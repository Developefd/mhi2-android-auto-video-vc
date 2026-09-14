// opengl-render-qnx-stream-player — pipelined multithreaded stream renderer
//
// Dual-Target Unified Architecture:
// - QNX ARM: OpenKODE (libdisplayinit.so) + GLES2 hardware texture mapping (Tegra 3 proven)
// - x86/Linux: GLFW + GLES2 hardware texture mapping
//
// Features:
// - 6-slot lock-free frame pool (zero runtime heap allocations inside 30 FPS loop)
// - Monotonic target clock pacer locked to 30.00 FPS
// - Direct fast YUV420p -> RGBA CPU conversion with macroblock stride compensation
// - 4-line centered HUD overlay rendered directly into RGBA buffer (immune to GL state bugs)
// - Seamless dual-mode: local file replay (--loop) and direct socket streaming (tcp://...)
// - Factory-safe DMDT display context activation (Context 3) and clean restoration (Context 33)

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <malloc.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <stddef.h>

#ifdef __QNX__
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#else
#include <GLES2/gl2.h>
#include <GLFW/glfw3.h>
#endif

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
}

// ---------------- Configuration & Performance Knobs ----------------
static const int kBufferPoolSize    = 16;     // 16-slot frame pool for jitter smoothing & zero contention
static int   g_ffmpegThreadCount    = 2;      // Default: 2 worker threads for multi-core Tegra 3
static int   g_ffmpegThreadType     = FF_THREAD_FRAME; // Default: true multi-core parallel frame decode
static bool  g_ffmpegLowDelay       = false;  // false allows FF_THREAD_FRAME (+33ms 1-frame latency)
static bool  g_ffmpegFastDecode     = true;
static int   g_ffmpegSkipFrame      = AVDISCARD_DEFAULT;
static int   g_ffmpegSkipLoopFilter = AVDISCARD_NONREF; // Default: noref eliminates macroblock prediction blur during movement
static int   g_idleMaxBytes         = 0;      // Stationary frame threshold (default: 0 = disabled for pure 30 FPS)
static int   g_idleHeartbeatHz      = 0;      // Stationary heartbeat keepalive rate in Hz (default: 0 = disabled)
static bool  g_loopFile             = false;
static bool  g_verbose              = true;
#ifndef __QNX__
static char  g_dumpVideoPath[512]   = {0};
static int   g_dumpMaxFrames        = 0;
static int   g_dumpFrameCount       = 0;
static FILE* g_dumpVideoPipe        = NULL;
#endif

// ---------------- Logging ----------------
#define LOG(fmt, ...) do {     printf("[stream-player] " fmt "\n", ##__VA_ARGS__);     fflush(stdout); } while(0)

#define LOG_STATS(fmt, ...) do {     printf("[stream-player STATS] " fmt "\n", ##__VA_ARGS__);     fflush(stdout); } while(0)

#define LOGE(fmt, ...) do {     fprintf(stderr, "[stream-player ERROR] " fmt "\n", ##__VA_ARGS__);     fflush(stderr); } while(0)

static inline uint8_t clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static uint64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)(ts.tv_nsec / 1000ULL);
}

static double us_to_ms(uint64_t us) {
    return (double)us / 1000.0;
}

static double get_process_memory_mb() {
#ifdef __QNX__
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/as", getpid());
    int fd = open(path, O_RDONLY);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) == 0) {
            close(fd);
            return (double)st.st_size / (1024.0 * 1024.0);
        }
        close(fd);
    }
    return 0.0;
#else
    FILE* f = fopen("/proc/self/statm", "r");
    if (f) {
        long total_pages = 0, rss_pages = 0;
        if (fscanf(f, "%ld %ld", &total_pages, &rss_pages) == 2) {
            fclose(f);
            long page_size_kb = sysconf(_SC_PAGESIZE) / 1024;
            return (double)(rss_pages * page_size_kb) / 1024.0;
        }
        fclose(f);
    }
    return 0.0;
#endif
}

// ---------------- Frame Buffer Structures ----------------
struct RGBABuffer {
    uint8_t* pixels;
    int width;
    int height;
    size_t capacity;
    int packetBytes;
    bool isKeyframe;

    RGBABuffer() : pixels(NULL), width(0), height(0), capacity(0), packetBytes(0), isKeyframe(false) {}

    void ensureCapacity(int w, int h) {
        size_t total = (size_t)w * h + 2 * ((size_t)(w/2) * (h/2));
        if (capacity < total) {
            free(pixels);
            pixels = (uint8_t*)malloc(total);
            capacity = total;
        }
        width = w;
        height = h;
    }

    void release() { free(pixels); pixels = NULL; capacity = 0; }
};

// 6-Slot Ring Buffer Pool
struct FramePool {
    RGBABuffer slots[kBufferPoolSize];
    int writeSlot;
    int readySlot;
    int readSlot;

    FramePool() : writeSlot(0), readySlot(-1), readSlot(-1) {}

    RGBABuffer* getWriteBuffer(int w, int h) {
        slots[writeSlot].ensureCapacity(w, h);
        return &slots[writeSlot];
    }

    void publishWrite() {
        readySlot = writeSlot;
        writeSlot = (writeSlot + 1) % kBufferPoolSize;
        if (writeSlot == readSlot) {
            writeSlot = (writeSlot + 1) % kBufferPoolSize;
        }
    }

    RGBABuffer* getDisplayBuffer() {
        if (readySlot >= 0) {
            readSlot = readySlot;
            readySlot = -1;
        }
        if (readSlot >= 0) return &slots[readSlot];
        return NULL;
    }
};

static FramePool g_framePool;
static pthread_mutex_t g_frameMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_frameCond  = PTHREAD_COND_INITIALIZER;

static volatile bool g_running = true;
static volatile int g_streamSock = -1;
static volatile bool g_newFrameReady = false;
static uint64_t g_decodedFrameCount = 0;
static uint64_t g_lastDecodeDurationUs = 0;
static char g_videoSource[512] = "tcp://127.0.0.1:12346";
static bool g_isSocket = false;

int windowWidth  = 800;
int windowHeight = 480;

static GLfloat backgroundColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

// Full-screen Quad Geometry (Screen dimensions: 800x480)
static const GLfloat landscapeVertices[] = {
    -1.0f,  1.0f, 0.0f,
     1.0f,  1.0f, 0.0f,
     1.0f, -1.0f, 0.0f,
    -1.0f, -1.0f, 0.0f
};

static const GLfloat landscapeTexCoords[] = {
    0.0f, 0.0f,
    1.0f, 0.0f,
    1.0f, 1.0f,
    0.0f, 1.0f
};

#ifdef __QNX__
static void* g_displayInitHandle = NULL;
static EGLDisplay eglDisplay = EGL_NO_DISPLAY;
static EGLSurface eglSurface = EGL_NO_SURFACE;
static EGLContext eglContext = EGL_NO_CONTEXT;
#else
static GLFWwindow* g_glfwWindow = NULL;
#endif

#ifdef __QNX__
typedef void (*PFNGLDRAWTEXTURENVPROC)(GLuint texture, GLuint sampler,
                                       GLfloat x0, GLfloat y0,
                                       GLfloat x1, GLfloat y1,
                                       GLfloat z,
                                       GLfloat s0, GLfloat t0,
                                       GLfloat s1, GLfloat t1);
static PFNGLDRAWTEXTURENVPROC g_pfnDrawTextureNV = NULL;
#endif
static bool g_useShader = false;

static GLuint g_programObject = 0;
static GLint  g_posAttr = -1;
static GLint  g_texAttr = -1;
static GLint  g_texLocY = -1;
static GLint  g_texLocU = -1;
static GLint  g_texLocV = -1;
static GLuint g_texY = 0;
static GLuint g_texU = 0;
static GLuint g_texV = 0;

// Standard GLES2 RGBA Shaders (Proven 100% compatible with Tegra 3)
static const char* vertexShaderSource =
    "attribute vec2 position;    \n"
    "attribute vec2 texCoord;     \n"
    "varying vec2 v_texCoord;     \n"
    "void main()                  \n"
    "{                            \n"
    "   gl_Position = vec4(position, 0.0, 1.0); \n"
    "   v_texCoord = texCoord;   \n"
    "}                            \n";

static const char* fragmentShaderSource =
    "precision mediump float;\n"
    "varying vec2 v_texCoord;\n"
    "uniform sampler2D texY;\n"
    "uniform sampler2D texU;\n"
    "uniform sampler2D texV;\n"
    "void main()\n"
    "{\n"
    "    float y = texture2D(texY, v_texCoord).r;\n"
    "    float u = texture2D(texU, v_texCoord).r - 0.5;\n"
    "    float v = texture2D(texV, v_texCoord).r - 0.5;\n"
    "    float r = y + 1.402 * v;\n"
    "    float g = y - 0.34414 * u - 0.71414 * v;\n"
    "    float b = y + 1.772 * u;\n"
    "    gl_FragColor = vec4(r, g, b, 1.0);\n"
    "}\n";

// ---------------- 5x7 Bitmap Font ----------------
struct Glyph5x7 { char c; uint8_t rows[7]; };
static const Glyph5x7 FONT_5X7[] = {
    { ' ',{0x00,0x00,0x00,0x00,0x00,0x00,0x00} },
    { ':',{0x00,0x04,0x00,0x00,0x04,0x00,0x00} },
    { '.',{0x00,0x00,0x00,0x00,0x00,0x04,0x00} },
    { '(',{0x02,0x04,0x08,0x08,0x08,0x04,0x02} },
    { ')',{0x08,0x04,0x02,0x02,0x02,0x04,0x08} },
    { '%',{0x19,0x19,0x02,0x04,0x08,0x13,0x13} },
    { '-',{0x00,0x00,0x00,0x1F,0x00,0x00,0x00} },
    { '/',{0x01,0x02,0x04,0x08,0x10,0x00,0x00} },
    { '0',{0x0E,0x11,0x13,0x15,0x19,0x11,0x0E} },
    { '1',{0x04,0x0C,0x04,0x04,0x04,0x04,0x0E} },
    { '2',{0x0E,0x11,0x01,0x02,0x04,0x08,0x1F} },
    { '3',{0x1F,0x02,0x04,0x02,0x01,0x11,0x0E} },
    { '4',{0x02,0x06,0x0A,0x12,0x1F,0x02,0x02} },
    { '5',{0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E} },
    { '6',{0x06,0x08,0x10,0x1E,0x11,0x11,0x0E} },
    { '7',{0x1F,0x01,0x02,0x04,0x08,0x08,0x08} },
    { '8',{0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E} },
    { '9',{0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C} },
    { 'A',{0x0E,0x11,0x11,0x1F,0x11,0x11,0x11} },
    { 'B',{0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E} },
    { 'C',{0x0E,0x11,0x10,0x10,0x10,0x11,0x0E} },
    { 'D',{0x1C,0x12,0x11,0x11,0x11,0x12,0x1C} },
    { 'E',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F} },
    { 'F',{0x1F,0x10,0x10,0x1E,0x10,0x10,0x10} },
    { 'G',{0x0E,0x11,0x10,0x17,0x11,0x11,0x0F} },
    { 'H',{0x11,0x11,0x11,0x1F,0x11,0x11,0x11} },
    { 'I',{0x0E,0x04,0x04,0x04,0x04,0x04,0x0E} },
    { 'J',{0x07,0x02,0x02,0x02,0x02,0x12,0x0C} },
    { 'K',{0x11,0x12,0x14,0x18,0x14,0x12,0x11} },
    { 'L',{0x10,0x10,0x10,0x10,0x10,0x10,0x1F} },
    { 'M',{0x11,0x1B,0x15,0x15,0x11,0x11,0x11} },
    { 'N',{0x11,0x19,0x15,0x13,0x11,0x11,0x11} },
    { 'O',{0x0E,0x11,0x11,0x11,0x11,0x11,0x0E} },
    { 'P',{0x1E,0x11,0x11,0x1E,0x10,0x10,0x10} },
    { 'Q',{0x0E,0x11,0x11,0x11,0x15,0x12,0x0D} },
    { 'R',{0x1E,0x11,0x11,0x1E,0x14,0x12,0x11} },
    { 'S',{0x0E,0x11,0x10,0x0E,0x01,0x11,0x0E} },
    { 'T',{0x1F,0x04,0x04,0x04,0x04,0x04,0x04} },
    { 'U',{0x11,0x11,0x11,0x11,0x11,0x11,0x0E} },
    { 'V',{0x11,0x11,0x11,0x11,0x11,0x0A,0x04} },
    { 'W',{0x11,0x11,0x11,0x15,0x15,0x1B,0x11} },
    { 'X',{0x11,0x11,0x0A,0x04,0x0A,0x11,0x11} },
    { 'Y',{0x11,0x11,0x0A,0x04,0x04,0x04,0x04} },
    { 'Z',{0x1F,0x01,0x02,0x04,0x08,0x10,0x1F} },
    { 'c',{0x00,0x00,0x0E,0x11,0x10,0x11,0x0E} },
    { 'd',{0x01,0x01,0x0D,0x13,0x11,0x11,0x0F} },
    { 'e',{0x0E,0x11,0x1F,0x10,0x10,0x11,0x0E} },
    { 'm',{0x00,0x00,0x1A,0x15,0x15,0x11,0x11} },
    { 'n',{0x00,0x00,0x16,0x19,0x11,0x11,0x11} },
    { 'o',{0x00,0x00,0x0E,0x11,0x11,0x11,0x0E} },
    { 'p',{0x00,0x00,0x1E,0x11,0x1E,0x10,0x10} },
    { 'r',{0x00,0x00,0x16,0x19,0x10,0x10,0x10} },
    { 's',{0x00,0x00,0x0E,0x10,0x0E,0x01,0x1E} },
    { '\n',{0x00,0x00,0x00,0x00,0x00,0x00,0x00} }
};

static void draw_text_yuv(uint8_t* fb, int fbW, int fbH, int start_x, int start_y, const char* text, int dot_px) {
    if (!text || !fb) return;

    // Bright yellow in YUV (Y=226, U=17, V=153)
    uint8_t colorY = 226, colorU = 17, colorV = 153;

    uint8_t* planeY = fb;
    uint8_t* planeU = fb + (fbW * fbH);
    uint8_t* planeV = planeU + ((fbW / 2) * (fbH / 2));

    int max_line_len = 0, cur_line_len = 0, line_count = 1;
    for (const char* p = text; *p; ++p) {
        if (*p == '\n') {
            line_count++;
            if (cur_line_len > max_line_len) max_line_len = cur_line_len;
            cur_line_len = 0;
        } else cur_line_len++;
    }
    if (cur_line_len > max_line_len) max_line_len = cur_line_len;

    int box_w = max_line_len * 6 * dot_px + 8 * dot_px;
    int box_h = line_count * 8 * dot_px + 6 * dot_px;
    int bg_x0 = start_x - 4 * dot_px;
    int bg_y0 = start_y - 3 * dot_px;

    // Dark background plate (dim Y by 75%)
    for (int y = bg_y0; y < bg_y0 + box_h; ++y) {
        if (y < 0 || y >= fbH) continue;
        for (int x = bg_x0; x < bg_x0 + box_w; ++x) {
            if (x < 0 || x >= fbW) continue;
            planeY[y * fbW + x] >>= 2;
        }
    }

    int cur_x = start_x;
    int cur_y = start_y;
    for (const char* p = text; *p; ++p) {
        char c = *p;
        if (c == '\n') {
            cur_x = start_x;
            cur_y += 8 * dot_px;
            continue;
        }
        const uint8_t* glyph = NULL;
        for (size_t i = 0; i < sizeof(FONT_5X7)/sizeof(FONT_5X7[0]); ++i) {
            if (FONT_5X7[i].c == c) {
                glyph = FONT_5X7[i].rows;
                break;
            }
        }
        if (glyph) {
            for (int r = 0; r < 7; ++r) {
                uint8_t row = glyph[r];
                for (int col = 0; col < 5; ++col) {
                    if (row & (1 << (4 - col))) {
                        int dr_x = cur_x + col * dot_px;
                        int dr_y = cur_y + r * dot_px;
                        for (int dy = 0; dy < dot_px; ++dy) {
                            for (int dx = 0; dx < dot_px; ++dx) {
                                int fx = dr_x + dx;
                                int fy = dr_y + dy;
                                if (fx >= 0 && fx < fbW && fy >= 0 && fy < fbH) {
                                    planeY[fy * fbW + fx] = colorY;
                                    planeU[(fy / 2) * (fbW / 2) + (fx / 2)] = colorU;
                                    planeV[(fy / 2) * (fbW / 2) + (fx / 2)] = colorV;
                                }
                            }
                        }
                    }
                }
            }
        }
        cur_x += 6 * dot_px;
    }
}
// ---------------- QNX DMDT Context Switching ----------------
#ifdef __QNX__
struct Command { const char* command; const char* error_message; };
static bool g_initialCmdsExecuted = false;
static pthread_t g_sentinelThread;
static volatile bool g_sentinelRunning = false;
static volatile bool g_streamActive = false;
static pid_t g_parentPid = 0;
static const uint64_t kMinDecodedFramesBeforeDisplaySwitch = 30;

void execute_initial_commands(bool force = false);
void check_and_reassert_context();
void switch_context_to_factory();

static bool kombi_map_ready() {
    struct stat st;
    if (stat("/dev/mlb/isoTX2", &st) != 0) return false;

    FILE* fp = popen("LD_PRELOAD=/eso/lib/gal_dualscreen/libdmdt_flush.so:/mnt/app/eso/lib/gal_dualscreen/libdmdt_flush.so:/mnt/app/eso/lib/libdmdt_flush.so:/fs/sdb0/lib/libdmdt_flush.so:/fs/sda0/lib/libdmdt_flush.so "
                     "LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib IPL_CONFIG_DIR=/etc/eso/production "
                     "/eso/bin/apps/dmdt gs 2>/dev/null", "r");
    if (!fp) return false;
    char line[256];
    bool ready = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "DISPLAYABLE_KOMBI_MAP_VIEW") ||
            (strstr(line, "context 70") && strstr(line, "33")) ||
            (strstr(line, "33") && strstr(line, "window"))) {
            ready = true;
            break;
        }
    }
    pclose(fp);
    return ready;
}

static void* sentinel_thread_func(void* arg) {
    (void)arg;
    LOG("sentinel: background watchdog thread started (monitoring parent PID %ld)", (long)g_parentPid);
    while (g_running && g_sentinelRunning) {
        for (int i = 0; i < 20 && g_running && g_sentinelRunning; ++i) {
            usleep(100000); // 100ms intervals, 2.0s total
            if (g_parentPid > 1) {
                if (kill(g_parentPid, 0) != 0 || getppid() != g_parentPid) {
                    LOG("sentinel: parent process (PID %ld) exited/changed! Triggering clean shutdown...", (long)g_parentPid);
                    g_running = false;
                    pthread_cond_broadcast(&g_frameCond);
                    int s = g_streamSock;
                    if (s >= 0) shutdown(s, SHUT_RDWR);
                    break;
                }
            }
        }
        if (!g_running || !g_sentinelRunning) break;
        check_and_reassert_context();
    }
    LOG("sentinel: background watchdog thread exiting");
    return NULL;
}

static void start_sentinel_thread() {
    if (g_sentinelRunning) return;
    g_sentinelRunning = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256 * 1024);
    if (pthread_create(&g_sentinelThread, &attr, sentinel_thread_func, NULL) != 0) {
        fprintf(stderr, "Failed to start sentinel thread\n");
        g_sentinelRunning = false;
    }
    pthread_attr_destroy(&attr);
}

static void stop_sentinel_thread() {
    if (!g_sentinelRunning) return;
    g_sentinelRunning = false;
    pthread_join(g_sentinelThread, NULL);
}

void switch_context_to_factory() {
    LOG("dmdt: reverting display context to factory (context 33)");
    system("LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib IPL_CONFIG_DIR=/etc/eso/production /eso/bin/apps/dmdt dc 70 33 2>/dev/null; "
           "LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib IPL_CONFIG_DIR=/etc/eso/production /eso/bin/apps/dmdt sc 4 70 2>/dev/null");
}

void execute_initial_commands(bool force) {
    if (g_initialCmdsExecuted && !force) return;
    /* Decode off-screen first. Normally the IDR arrives while Kombi is still
     * starting; if Kombi wins the race, this prevents exposing the first
     * decoder frames or a connection that immediately fails. */
    if (!g_initialCmdsExecuted &&
        g_decodedFrameCount < kMinDecodedFramesBeforeDisplaySwitch) return;
    static uint64_t lastReadyCheckUs = 0;
    static unsigned deferredChecks = 0;
    uint64_t now = now_us();
    if (now - lastReadyCheckUs < 1000000ULL) return;
    lastReadyCheckUs = now;
    if (!kombi_map_ready()) {
        ++deferredChecks;
        if (deferredChecks == 1 || deferredChecks % 10 == 0)
            LOG("dmdt: display switch deferred; Kombi map is not ready");
        return;
    }
    deferredChecks = 0;
    LOG("dmdt: activating display context (context 3)");
    struct Command commands[] = {
        { "LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib IPL_CONFIG_DIR=/etc/eso/production /eso/bin/apps/dmdt dc 70 3",  "Create display table with context 3 failed" },
        { "LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib IPL_CONFIG_DIR=/etc/eso/production /eso/bin/apps/dmdt sc 4 70", "Set display 4 (VC) to display table 70 failed" }
    };
    bool all_ok = true;
    for (size_t i = 0; i < sizeof(commands)/sizeof(commands[0]); ++i) {
        int ret = system(commands[i].command);
        if (ret != 0) {
            fprintf(stderr, "%s: %d\n", commands[i].error_message, ret);
            all_ok = false;
        } else {
            LOG("dmdt: '%s' ok", commands[i].command);
        }
    }
    if (all_ok) {
        g_initialCmdsExecuted = true;
        start_sentinel_thread();
    }
}

void check_and_reassert_context() {
    if (!g_streamActive) return;
    if (!g_initialCmdsExecuted) {
        execute_initial_commands();
        return;
    }
    /*
     * Context 70 Sentinel Watchdog:
     * When stream is actively flowing, periodically inspect whether Context 70
     * has been clobbered by OEM system (e.g. vehicle ignition turned ON, map viewer startup, or user cluster view change).
     * If dmdt gs reports "context 70 -> 33", automatically re-assert dc 70 3 and sc 4 70.
     * Uses libdmdt_flush.so interposer so dmdt flushes stdout upon _exit().
     */
    FILE* fp = popen("LD_PRELOAD=/eso/lib/gal_dualscreen/libdmdt_flush.so:/mnt/app/eso/lib/gal_dualscreen/libdmdt_flush.so:/mnt/app/eso/lib/libdmdt_flush.so:/fs/sdb0/lib/libdmdt_flush.so:/fs/sda0/lib/libdmdt_flush.so "
                     "LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib IPL_CONFIG_DIR=/etc/eso/production "
                     "/eso/bin/apps/dmdt gs 2>/dev/null", "r");
    if (!fp) return;
    char buf[512];
    bool clobbered = false;
    while (fgets(buf, sizeof(buf), fp)) {
        if (strstr(buf, "context 70") && strstr(buf, "33")) {
            clobbered = true;
            break;
        }
    }
    pclose(fp);
    if (clobbered && g_streamActive) {
        LOGE("sentinel: Context 70 clobbered by OEM system (found stock 33)! Re-asserting dc 70 3 && sc 4 70...");
        execute_initial_commands(true);
    }
}

void execute_final_commands() {
    stop_sentinel_thread();
    switch_context_to_factory();
    g_initialCmdsExecuted = false;
}
#else
static bool g_initialCmdsExecuted = false;
static volatile bool g_streamActive = false;
static pid_t g_parentPid = 0;
void execute_initial_commands(bool force = false) { (void)force; }
void execute_final_commands() {}
void check_and_reassert_context() {}
void switch_context_to_factory() {}
#endif

static void crash_signal_handler(int sig) {
    fprintf(stderr, "\n[stream-player CRASH] Caught fatal signal %d! Emergency restoring display context 33...\n", sig);
#ifdef __QNX__
    system("LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib IPL_CONFIG_DIR=/etc/eso/production /eso/bin/apps/dmdt dc 70 33 2>/dev/null");
    system("LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib IPL_CONFIG_DIR=/etc/eso/production /eso/bin/apps/dmdt sc 4 70 2>/dev/null");
#endif
    signal(sig, SIG_DFL);
    raise(sig);
}

static void signal_handler(int sig) {
    (void)sig;
    g_running = false;
    pthread_cond_broadcast(&g_frameCond);
    int s = g_streamSock;
    if (s >= 0) {
        shutdown(s, SHUT_RDWR);
    }
#ifndef __QNX__
    if (g_dumpVideoPipe) {
        pclose(g_dumpVideoPipe);
        g_dumpVideoPipe = NULL;
    }
#endif
}

// ---------------- Shader Compilation & Setup ----------------
static void check_shader(GLuint shader, const char* name) {
    GLint status = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        char log[512] = {0};
        glGetShaderInfoLog(shader, sizeof(log)-1, NULL, log);
        fprintf(stderr, "GL: shader '%s' compile failed: %s\n", name, log);
    } else {
        LOG("GL: shader '%s' compiled ok", name);
    }
}

static void check_program(GLuint prog, const char* name) {
    GLint status = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        char log[512] = {0};
        glGetProgramInfoLog(prog, sizeof(log)-1, NULL, log);
        fprintf(stderr, "GL: program '%s' link failed: %s\n", name, log);
    } else {
        LOG("GL: program '%s' linked ok", name);
    }
}

void InitGL() {
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertexShaderSource, NULL);
    glCompileShader(vs);
    check_shader(vs, "vert-main");

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragmentShaderSource, NULL);
    glCompileShader(fs);
    check_shader(fs, "frag-main");

    g_programObject = glCreateProgram();
    glAttachShader(g_programObject, vs);
    glAttachShader(g_programObject, fs);
    glLinkProgram(g_programObject);
    check_program(g_programObject, "main");

    g_posAttr = glGetAttribLocation(g_programObject, "position");
    g_texAttr = glGetAttribLocation(g_programObject, "texCoord");
    g_texLocY = glGetUniformLocation(g_programObject, "texY");
    g_texLocU = glGetUniformLocation(g_programObject, "texU");
    g_texLocV = glGetUniformLocation(g_programObject, "texV");
    LOG("GL: attrib locations: pos=%d tex=%d", g_posAttr, g_texAttr);

    GLint linkStatus = 0;
    glGetProgramiv(g_programObject, GL_LINK_STATUS, &linkStatus);
    if (linkStatus) {
        g_useShader = true;
        LOG("GL: GLES2 shader pipeline ACTIVE");
    } else {
        LOGE("GL: shader link failed, probing for hardware blit extension...");
    }

#ifdef __QNX__
    g_pfnDrawTextureNV = (PFNGLDRAWTEXTURENVPROC)eglGetProcAddress("glDrawTextureNV");
    if (g_pfnDrawTextureNV) {
        LOG("GL: GL_NV_draw_texture hardware blitter available at %p", (void*)g_pfnDrawTextureNV);
    } else {
        LOG("GL: GL_NV_draw_texture NOT available");
    }

    if (!g_useShader && !g_pfnDrawTextureNV) {
        LOGE("FATAL: no rendering path (shader link failed AND glDrawTextureNV not found)");
        GLint numBinFmt = 0;
        glGetIntegerv(0x8DF9 /* GL_NUM_SHADER_BINARY_FORMATS */, &numBinFmt);
        LOGE("FATAL: GL_NUM_SHADER_BINARY_FORMATS = %d", numBinFmt);
    }
#endif

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    GLuint texs[3];
    glGenTextures(3, texs);
    g_texY = texs[0]; g_texU = texs[1]; g_texV = texs[2];
    for (int i=0; i<3; ++i) {
        glBindTexture(GL_TEXTURE_2D, texs[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    glViewport(0, 0, windowWidth, windowHeight);
    glClearColor(backgroundColor[0], backgroundColor[1], backgroundColor[2], backgroundColor[3]);
    LOG("GL: hardware RGBA texture & shaders initialized successfully");
}

// ---------------- H.264 Video Decoder Thread ----------------
static void* DecoderThreadFunc(void* arg) {
    (void)arg;
    LOG("decoder: background worker thread active");

    bool is_socket = g_isSocket;
    bool is_unix = (strncmp(g_videoSource, "unix://", 7) == 0 ||
                    (strncmp(g_videoSource, "/tmp/", 5) == 0 && strstr(g_videoSource, ".sock") != NULL));
    char unix_path[512] = "/tmp/gal_video.sock";
    char host[512] = "127.0.0.1";
    int  port = 12346;

    if (is_unix) {
        if (strncmp(g_videoSource, "unix://", 7) == 0) {
            snprintf(unix_path, sizeof(unix_path), "%s", g_videoSource + 7);
        } else {
            snprintf(unix_path, sizeof(unix_path), "%s", g_videoSource);
        }
        LOG("network: target stream unix://%s", unix_path);
    } else if (g_isSocket) {
        const char* p = g_videoSource + 6;
        char* colon = (char*)strchr(p, ':');
        if (colon) {
            size_t host_len = colon - p;
            if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
            strncpy(host, p, host_len);
            host[host_len] = '\0';
            port = atoi(colon + 1);
        } else {
            snprintf(host, sizeof(host), "%s", p);
        }
        LOG("network: target stream tcp://%s:%d", host, port);
    } else {
        LOG("file: reading local H.264 file '%s' (loop=%d)", g_videoSource, g_loopFile ? 1 : 0);
    }

    const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) { LOGE("avcodec: H.264 decoder not found"); return NULL; }

    AVCodecParserContext* parser = av_parser_init(codec->id);
    if (!parser) { LOGE("avcodec: parser init failed"); return NULL; }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) { LOGE("avcodec: context alloc failed"); av_parser_close(parser); return NULL; }

    codecCtx->thread_count = g_ffmpegThreadCount;
    codecCtx->thread_type = g_ffmpegThreadType;
    if (g_ffmpegLowDelay) {
        codecCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    }
    if (g_ffmpegFastDecode) codecCtx->flags2 |= AV_CODEC_FLAG2_FAST;
    codecCtx->skip_frame = (AVDiscard)g_ffmpegSkipFrame;
    codecCtx->skip_loop_filter = (AVDiscard)g_ffmpegSkipLoopFilter;

    const char* deblock_name = "DEFAULT";
    if (g_ffmpegSkipLoopFilter == AVDISCARD_ALL) deblock_name = "ALL (Deblocking DISABLED, saves ~25-30% CPU)";
    else if (g_ffmpegSkipLoopFilter == AVDISCARD_NONE) deblock_name = "NONE (Deblocking ENABLED)";
    else if (g_ffmpegSkipLoopFilter == AVDISCARD_NONREF) deblock_name = "NONREF (Deblocking skipped on non-reference frames)";

    const char* thread_type_name = "FRAME (Multi-core parallel)";
    if (g_ffmpegThreadType == FF_THREAD_SLICE) thread_type_name = "SLICE (Single-slice fallback)";
    else if (g_ffmpegThreadType == (FF_THREAD_FRAME | FF_THREAD_SLICE)) thread_type_name = "FRAME+SLICE";

    LOG("decoder: configuring FFmpeg (threads: %d, type: %s, low_delay: %d, fast: %d, skip_loop_filter: %s, idle_bytes: %d, heartbeat: %dHz)",
        g_ffmpegThreadCount, thread_type_name, g_ffmpegLowDelay ? 1 : 0, g_ffmpegFastDecode ? 1 : 0, deblock_name,
        g_idleMaxBytes, g_idleHeartbeatHz);

    if (avcodec_open2(codecCtx, codec, NULL) < 0) {
        LOGE("avcodec: open failed");
        avcodec_free_context(&codecCtx);
        av_parser_close(parser);
        return NULL;
    }

    AVPacket* packet = av_packet_alloc();
    AVFrame*  frame  = av_frame_alloc();
    uint8_t* inbuf = (uint8_t*)av_malloc(262144 + AV_INPUT_BUFFER_PADDING_SIZE);
    if (!inbuf) {
        LOGE("avcodec: inbuf allocation failed");
        av_frame_free(&frame);
        av_packet_free(&packet);
        avcodec_free_context(&codecCtx);
        av_parser_close(parser);
        return NULL;
    }

    bool hasDecodedKeyframe = false;

    while (g_running) {
        int sock = -1;
        FILE* infile = NULL;

        if (is_socket) {
            if (is_unix) {
                sock = socket(AF_UNIX, SOCK_STREAM, 0);
                if (sock < 0) { usleep(100000); continue; }

                struct sockaddr_un serv_un;
                memset(&serv_un, 0, sizeof(serv_un));
                serv_un.sun_family = AF_UNIX;
                strncpy(serv_un.sun_path, unix_path, sizeof(serv_un.sun_path) - 1);

                g_streamSock = sock;
                struct timeval tv;
                tv.tv_sec = 2; tv.tv_usec = 0;
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));

                LOG("socket: connecting to unix://%s...", unix_path);
                if (connect(sock, (struct sockaddr*)&serv_un, sizeof(serv_un)) < 0) {
                    close(sock);
                    usleep(200000);
                    continue;
                }
                LOG("socket: connected successfully to unix://%s!", unix_path);
                hasDecodedKeyframe = false;
            } else {
                sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock < 0) { usleep(100000); continue; }

                struct sockaddr_in serv_addr;
                memset(&serv_addr, 0, sizeof(serv_addr));
                serv_addr.sin_family = AF_INET;
                serv_addr.sin_port = htons(port);
                inet_pton(AF_INET, host, &serv_addr.sin_addr);

                g_streamSock = sock;
                int flag = 1;
                setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

                int rcv_buf = 2097152; // 2MB socket receive buffer
                setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcv_buf, sizeof(rcv_buf));

                struct timeval tv;
                tv.tv_sec = 2; tv.tv_usec = 0;
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));

                LOG("socket: connecting to %s:%d...", host, port);
                if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
                    close(sock);
                    usleep(200000);
                    continue;
                }
                LOG("socket: connected successfully to %s:%d!", host, port);
                hasDecodedKeyframe = false;
            }
        } else {
            infile = fopen(g_videoSource, "rb");
            if (!infile) {
                LOGE("file: failed to open '%s'", g_videoSource);
                break;
            }
        }

        while (g_running) {
            ssize_t bytes_read = 0;
            if (is_socket) {
                bytes_read = recv(sock, inbuf, 131072, 0);
                if (bytes_read < 0) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                        // SO_RCVTIMEO expired (idle pause / reverse gear) -- keep connection alive and wait!
                        continue;
                    }
                    LOG("socket: recv error (errno=%d), connection reset", errno);
                    hasDecodedKeyframe = false;
                    break;
                } else if (bytes_read == 0) {
                    LOG("socket: connection closed by server (EOF), will reconnect");
                    hasDecodedKeyframe = false;
                    break;
                }
            } else {
                bytes_read = fread(inbuf, 1, 32768, infile);
                if (bytes_read <= 0) {
                    if (g_loopFile && g_running) {
                        rewind(infile);
                        continue;
                    }
                    LOG("file: reached EOF");
                    break;
                }
            }

            uint8_t* data = inbuf;
            size_t data_size = bytes_read;

            while (data_size > 0 && g_running) {
                int len = av_parser_parse2(parser, codecCtx,
                                          &packet->data, &packet->size,
                                          data, data_size,
                                          AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
                if (len < 0) break;
                data += len;
                data_size -= len;

                if (packet->size > 0) {
                    uint64_t t_dec_start = now_us();
                    int ret = avcodec_send_packet(codecCtx, packet);
                    if (ret < 0) continue;

                    while (ret >= 0 && g_running) {
                        ret = avcodec_receive_frame(codecCtx, frame);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                        if (ret < 0) break;

                        uint64_t dec_dur = now_us() - t_dec_start;

                        int w = frame->width;
                        int h = frame->height;

                        bool isKey = false;
#ifdef AV_FRAME_FLAG_KEY
                        if ((frame->flags & AV_FRAME_FLAG_KEY) != 0) isKey = true;
#endif
                        if (frame->key_frame != 0) isKey = true;
                        if (frame->pict_type == AV_PICTURE_TYPE_I) isKey = true;

                        // Keyframe Gate: Discard pre-keyframe P-frames to eliminate initial macroblock mosaic
                        if (!hasDecodedKeyframe) {
                            if (!isKey) {
                                av_frame_unref(frame);
                                continue;
                            }
                            hasDecodedKeyframe = true;
                            LOG("decoder: first IDR/I keyframe received (%dx%d)! Stream stabilized.", w, h);
                        }

                        // Direct memcpy to contiguous YUV buffer
                        pthread_mutex_lock(&g_frameMutex);
                        RGBABuffer* buf = g_framePool.getWriteBuffer(w, h);
                        buf->packetBytes = packet->size;
                        buf->isKeyframe = isKey;
                        
                        uint8_t* dstY = buf->pixels;
                        uint8_t* dstU = dstY + (w * h);
                        uint8_t* dstV = dstU + ((w / 2) * (h / 2));

                        // Copy Y plane
                        for (int y = 0; y < h; ++y) {
                            memcpy(dstY + y * w, frame->data[0] + y * frame->linesize[0], w);
                        }
                        // Copy U plane
                        for (int y = 0; y < h / 2; ++y) {
                            memcpy(dstU + y * (w / 2), frame->data[1] + y * frame->linesize[1], w / 2);
                        }
                        // Copy V plane
                        for (int y = 0; y < h / 2; ++y) {
                            memcpy(dstV + y * (w / 2), frame->data[2] + y * frame->linesize[2], w / 2);
                        }

                        g_framePool.publishWrite();
                        g_newFrameReady = true;
                        g_decodedFrameCount++;
                        g_lastDecodeDurationUs = dec_dur;

                        pthread_cond_signal(&g_frameCond);
                        pthread_mutex_unlock(&g_frameMutex);

                        av_frame_unref(frame);

                        // Monotonic 30.00 FPS Target Clock Pacing for file playback
                        if (!is_socket) {
                            static uint64_t s_nextTargetUs = 0;
                            const uint64_t kFrameIntervalUs = 33333ULL;
                            uint64_t now = now_us();
                            if (s_nextTargetUs == 0 || now > s_nextTargetUs + 100000ULL) {
                                s_nextTargetUs = now + kFrameIntervalUs;
                            } else {
                                if (now < s_nextTargetUs) {
                                    usleep((useconds_t)(s_nextTargetUs - now));
                                }
                                s_nextTargetUs += kFrameIntervalUs;
                            }
                        }
                    }
                }
            }
        }

        if (sock >= 0) { g_streamSock = -1; close(sock); sock = -1; }
        if (infile) { fclose(infile); infile = NULL; }
        if (!g_running) break;
        if (!is_socket && !g_loopFile) break;
    }

    av_free(inbuf);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&codecCtx);
    av_parser_close(parser);
    LOG("decoder: worker thread exited cleanly");
    return NULL;
}

static int check_and_acquire_instance_lock() {
    int fd = open("/tmp/stream-player.lock", O_CREAT | O_RDWR, 0666);
    if (fd < 0) return 0; // If /tmp is inaccessible, proceed without lock
    struct flock fl;
    for (int retry = 0; retry < 5; ++retry) {
        memset(&fl, 0, sizeof(fl));
        fl.l_type = F_WRLCK;
        fl.l_whence = SEEK_SET;
        if (fcntl(fd, F_SETLK, &fl) == 0) {
            return fd; // Acquired exclusive process lock
        }
        if (errno == ENOSYS || errno == EINVAL) {
            // Filesystem (e.g. QNX /dev/shmem) does not support POSIX advisory locks
            return fd;
        }
        usleep(20000); // 20ms retry in case previous instance is exiting
    }
    close(fd);
    return -1; // Another instance is actively running!
}

static void load_player_config() {
    const char* env_val = getenv("GAL_PLAYER_DEBLOCK");
    if (!env_val) env_val = getenv("GAL_DEBLOCK");
    if (!env_val) env_val = getenv("GAL_SKIP_LOOP_FILTER");

    char conf_val[64] = {0};

    if (!env_val) {
        static const char* candidate_paths[] = {
            "/fs/sdb0/gal_dualscreen.conf",
            "/fs/sda0/gal_dualscreen.conf",
            "/tmp/gal_dualscreen.conf",
            "/eso/lib/gal_dualscreen/gal_dualscreen.conf",
            "./gal_dualscreen.conf",
            NULL
        };
        for (int i = 0; candidate_paths[i]; ++i) {
            FILE* f = fopen(candidate_paths[i], "r");
            if (!f) continue;
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                char* p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') continue;
                if (strncmp(p, "GAL_PLAYER_DEBLOCK=", 19) == 0 ||
                    strncmp(p, "GAL_DEBLOCK=", 12) == 0 ||
                    strncmp(p, "GAL_SKIP_LOOP_FILTER=", 21) == 0) {
                    char* eq = strchr(p, '=');
                    if (eq) {
                        char* val = eq + 1;
                        while (*val == ' ' || *val == '\t') val++;
                        char* end = val + strlen(val) - 1;
                        while (end >= val && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t')) {
                            *end = '\0';
                            end--;
                        }
                        strncpy(conf_val, val, sizeof(conf_val) - 1);
                        break;
                    }
                }
            }
            fclose(f);
            if (conf_val[0] != '\0') {
                env_val = conf_val;
                break;
            }
        }
    }

    if (env_val) {
        if (strcmp(env_val, "all") == 0 || strcmp(env_val, "0") == 0 ||
            strcasecmp(env_val, "false") == 0 || strcasecmp(env_val, "off") == 0) {
            g_ffmpegSkipLoopFilter = AVDISCARD_ALL;
        } else if (strcmp(env_val, "none") == 0 || strcmp(env_val, "1") == 0 ||
                   strcasecmp(env_val, "true") == 0 || strcasecmp(env_val, "on") == 0) {
            g_ffmpegSkipLoopFilter = AVDISCARD_NONE;
        } else if (strcmp(env_val, "noref") == 0) {
            g_ffmpegSkipLoopFilter = AVDISCARD_NONREF;
        }
    }

    // Parse candidate config files for additional performance settings
    static const char* conf_files[] = {
        "/fs/sdb0/gal_dualscreen.conf",
        "/fs/sda0/gal_dualscreen.conf",
        "/tmp/gal_dualscreen.conf",
        "/eso/lib/gal_dualscreen/gal_dualscreen.conf",
        "./gal_dualscreen.conf",
        NULL
    };
    for (int i = 0; conf_files[i]; ++i) {
        FILE* f = fopen(conf_files[i], "r");
        if (!f) continue;
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char* p = line;
            while (*p == ' ' || *p == '	') p++;
            if (*p == '#' || *p == '\r' || *p == '\n' || *p == '\0') continue;
            char* eq = strchr(p, '=');
            if (!eq) continue;
            *eq = '\0';
            char* key = p;
            char* val = eq + 1;
            while (*val == ' ' || *val == '	') val++;
            char* end = val + strlen(val) - 1;
            while (end >= val && (*end == '\r' || *end == '\n' || *end == ' ' || *end == '	')) {
                *end = '\0';
                end--;
            }
            if (strcmp(key, "GAL_PLAYER_THREADS") == 0) {
                int t = atoi(val);
                if (t >= 1 && t <= 8) g_ffmpegThreadCount = t;
            } else if (strcmp(key, "GAL_PLAYER_THREAD_TYPE") == 0) {
                if (strcasecmp(val, "frame") == 0) g_ffmpegThreadType = FF_THREAD_FRAME;
                else if (strcasecmp(val, "slice") == 0) g_ffmpegThreadType = FF_THREAD_SLICE;
                else if (strcasecmp(val, "auto") == 0) g_ffmpegThreadType = FF_THREAD_FRAME | FF_THREAD_SLICE;
            } else if (strcmp(key, "GAL_PLAYER_LOW_DELAY") == 0) {
                g_ffmpegLowDelay = (atoi(val) != 0);
            } else if (strcmp(key, "GAL_PLAYER_IDLE_BYTES") == 0) {
                g_idleMaxBytes = atoi(val);
            } else if (strcmp(key, "GAL_PLAYER_HEARTBEAT_HZ") == 0) {
                g_idleHeartbeatHz = atoi(val);
            }
        }
        fclose(f);
        break; // Stop at first found config file
    }

    // Environment variables override config file
    const char* env_threads = getenv("GAL_PLAYER_THREADS");
    if (env_threads) {
        int t = atoi(env_threads);
        if (t >= 1 && t <= 8) g_ffmpegThreadCount = t;
    }
    const char* env_ttype = getenv("GAL_PLAYER_THREAD_TYPE");
    if (env_ttype) {
        if (strcasecmp(env_ttype, "frame") == 0) g_ffmpegThreadType = FF_THREAD_FRAME;
        else if (strcasecmp(env_ttype, "slice") == 0) g_ffmpegThreadType = FF_THREAD_SLICE;
        else if (strcasecmp(env_ttype, "auto") == 0) g_ffmpegThreadType = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    const char* env_lowdelay = getenv("GAL_PLAYER_LOW_DELAY");
    if (env_lowdelay) {
        g_ffmpegLowDelay = (atoi(env_lowdelay) != 0);
    }
    const char* env_idle_bytes = getenv("GAL_PLAYER_IDLE_BYTES");
    if (env_idle_bytes) {
        g_idleMaxBytes = atoi(env_idle_bytes);
    }
    const char* env_hb = getenv("GAL_PLAYER_HEARTBEAT_HZ");
    if (env_hb) {
        g_idleHeartbeatHz = atoi(env_hb);
    }
}

// ---------------- Application Entry Point ----------------

#if defined(__QNX__) || defined(__linux__)
static int g_ack_fd = -1;
static void write_ack() {
    if (g_ack_fd < 0) {
        g_ack_fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (g_ack_fd >= 0) {
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, "/tmp/gal_ack.sock", sizeof(addr.sun_path) - 1);
            if (connect(g_ack_fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
                close(g_ack_fd);
                g_ack_fd = -1;
                return;
            }
            int flags = fcntl(g_ack_fd, F_GETFL, 0);
            fcntl(g_ack_fd, F_SETFL, flags | O_NONBLOCK);
        }
    }
    if (g_ack_fd >= 0) {
        char b = 1;
        int n = send(g_ack_fd, &b, 1, 0);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            close(g_ack_fd);
            g_ack_fd = -1;
        }
    }
}
#else
static void write_ack() {}
#endif

int main(int argc, char* argv[]) {
    g_parentPid = getppid();
    int lock_fd = check_and_acquire_instance_lock();
    if (lock_fd < 0) {
        LOG("stream-player: another instance is already running, exiting immediately to prevent duplicate renderers");
        return 0;
    }
    load_player_config();

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--loop") == 0) {
            g_loopFile = true;
        } else if (strcmp(argv[i], "--overlay") == 0 || strcmp(argv[i], "--no-overlay") == 0) {
            /* Retained as accepted no-op options; the on-video metrics HUD was removed. */
        } else if (strcmp(argv[i], "--no-deblock") == 0) {
            g_ffmpegSkipLoopFilter = AVDISCARD_ALL;
        } else if (strcmp(argv[i], "--deblock") == 0) {
            g_ffmpegSkipLoopFilter = AVDISCARD_NONE;
        } else if (strncmp(argv[i], "--skip-loop-filter=", 19) == 0) {
            const char* opt = argv[i] + 19;
            if (strcmp(opt, "all") == 0) g_ffmpegSkipLoopFilter = AVDISCARD_ALL;
            else if (strcmp(opt, "none") == 0) g_ffmpegSkipLoopFilter = AVDISCARD_NONE;
            else if (strcmp(opt, "noref") == 0) g_ffmpegSkipLoopFilter = AVDISCARD_NONREF;
        } else if (strncmp(argv[i], "--dot-px=", 9) == 0) {
            /* Backward-compatible no-op; the on-video metrics HUD was removed. */
        } else if (strncmp(argv[i], "--threads=", 10) == 0) {
            int t = atoi(argv[i] + 10);
            if (t >= 1 && t <= 8) g_ffmpegThreadCount = t;
        } else if (strncmp(argv[i], "--thread-type=", 14) == 0) {
            const char* opt = argv[i] + 14;
            if (strcmp(opt, "frame") == 0) g_ffmpegThreadType = FF_THREAD_FRAME;
            else if (strcmp(opt, "slice") == 0) g_ffmpegThreadType = FF_THREAD_SLICE;
            else if (strcmp(opt, "auto") == 0) g_ffmpegThreadType = FF_THREAD_FRAME | FF_THREAD_SLICE;
        } else if (strcmp(argv[i], "--low-delay") == 0) {
            g_ffmpegLowDelay = true;
        } else if (strcmp(argv[i], "--no-low-delay") == 0) {
            g_ffmpegLowDelay = false;
        } else if (strncmp(argv[i], "--idle-bytes=", 13) == 0) {
            g_idleMaxBytes = atoi(argv[i] + 13);
        } else if (strncmp(argv[i], "--heartbeat-hz=", 15) == 0) {
            g_idleHeartbeatHz = atoi(argv[i] + 15);
#ifndef __QNX__
        } else if (strncmp(argv[i], "--dump-video=", 13) == 0) {
            snprintf(g_dumpVideoPath, sizeof(g_dumpVideoPath), "%s", argv[i] + 13);
        } else if (strncmp(argv[i], "--dump-frames=", 14) == 0) {
            g_dumpMaxFrames = atoi(argv[i] + 14);
#endif
        } else if (strncmp(argv[i], "--url=", 6) == 0) {
            snprintf(g_videoSource, sizeof(g_videoSource), "%s", argv[i] + 6);
        } else if (argv[i][0] != '-') {
            snprintf(g_videoSource, sizeof(g_videoSource), "%s", argv[i]);
        }
    }

    bool is_unix_sock = (strncmp(g_videoSource, "unix://", 7) == 0 ||
                         (strncmp(g_videoSource, "/tmp/", 5) == 0 && strstr(g_videoSource, ".sock") != NULL));
    g_isSocket = (strncmp(g_videoSource, "tcp://", 6) == 0 || is_unix_sock);
    LOG("stream-player starting (source: %s, loop: %d, pool: %d)",
        g_videoSource, g_loopFile ? 1 : 0, kBufferPoolSize);

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGSEGV, crash_signal_handler);
    signal(SIGABRT, crash_signal_handler);
    signal(SIGBUS,  crash_signal_handler);
    signal(SIGILL,  crash_signal_handler);
    signal(SIGFPE,  crash_signal_handler);

#ifdef __QNX__
    // Keep libdisplayinit.so resident for the entire application lifetime!
    g_displayInitHandle = dlopen("libdisplayinit.so", RTLD_LAZY);
    if (!g_displayInitHandle) {
        fprintf(stderr, "Error loading libdisplayinit.so: %s\n", dlerror());
        return 1;
    }

    void (*display_init)(int, int) = (void (*)(int, int))dlsym(g_displayInitHandle, "display_init");
    if (display_init) display_init(0, 0);

    eglDisplay = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (eglDisplay == EGL_NO_DISPLAY) {
        fprintf(stderr, "EGL: no display\n");
        return 1;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(eglDisplay, &major, &minor)) {
        fprintf(stderr, "EGL: eglInitialize failed (0x%x)\n", eglGetError());
        return 1;
    }

    EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RED_SIZE, 1, EGL_GREEN_SIZE, 1, EGL_BLUE_SIZE, 1, EGL_ALPHA_SIZE, 1,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLConfig configs[5];
    EGLint num_configs = 0;
    EGLNativeWindowType windowEgl = 0;
    int kdWindow = 0;

    eglChooseConfig(eglDisplay, config_attribs, configs, 5, &num_configs);
    if (num_configs == 0) {
        fprintf(stderr, "EGL: no suitable config found\n");
        return 1;
    }

    void (*display_create_window)(EGLDisplay, EGLConfig, int, int, int, EGLNativeWindowType*, int*) =
        (void (*)(EGLDisplay, EGLConfig, int, int, int, EGLNativeWindowType*, int*))dlsym(g_displayInitHandle, "display_create_window");

    if (display_create_window) {
        // Displayable 3 = Center display on Virtual Cockpit
        display_create_window(eglDisplay, configs[0], windowWidth, windowHeight, 3, &windowEgl, &kdWindow);
        LOG("display: window created %dx%d (win=%p, kd=%d)", windowWidth, windowHeight, (void*)windowEgl, kdWindow);
    }

    eglSurface = eglCreateWindowSurface(eglDisplay, configs[0], windowEgl, 0);
    if (eglSurface == EGL_NO_SURFACE) {
        fprintf(stderr, "EGL: eglCreateWindowSurface failed (0x%x)\n", eglGetError());
        return 1;
    }

    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint context_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    eglContext = eglCreateContext(eglDisplay, configs[0], EGL_NO_CONTEXT, context_attribs);
    if (eglContext == EGL_NO_CONTEXT) {
        fprintf(stderr, "EGL: eglCreateContext failed (0x%x)\n", eglGetError());
        return 1;
    }

    if (!eglMakeCurrent(eglDisplay, eglSurface, eglSurface, eglContext)) {
        fprintf(stderr, "EGL: eglMakeCurrent failed (0x%x)\n", eglGetError());
        return 1;
    }
    // Non-blocking buffer swap (interval 0):
    // Prevents eglSwapBuffers() from hanging on lost hardware VSYNC when
    // the Virtual Cockpit display powers off during vehicle ignition OFF.
    eglSwapInterval(eglDisplay, 0);
#else
    if (!glfwInit()) {
        LOGE("GLFW: failed to initialize");
        return 1;
    }
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    g_glfwWindow = glfwCreateWindow(windowWidth, windowHeight, "VW Virtual Cockpit - Unified GLES2 Player", NULL, NULL);
    if (!g_glfwWindow) {
        LOGE("GLFW: failed to create window");
        glfwTerminate();
        return 1;
    }
    glfwSetWindowCloseCallback(g_glfwWindow, [](GLFWwindow* w) {
        (void)w;
        g_running = false;
        pthread_cond_broadcast(&g_frameCond);
    });
    glfwMakeContextCurrent(g_glfwWindow);
    glfwSwapInterval(0); // Driven by our monotonic target clock
#endif

    InitGL();

    // Pre-fill slot 0 with a blank black frame (Y=16, U=128, V=128)
    RGBABuffer* initBuf = g_framePool.getWriteBuffer(windowWidth, windowHeight);
    memset(initBuf->pixels, 16, (size_t)windowWidth * windowHeight);
    memset(initBuf->pixels + (size_t)windowWidth * windowHeight, 128, ((size_t)windowWidth * windowHeight) / 2);
    g_framePool.publishWrite();

    pthread_attr_t decAttr;
    pthread_attr_init(&decAttr);
    pthread_attr_setstacksize(&decAttr, 1024 * 1024);

    pthread_t decThread;
    if (pthread_create(&decThread, &decAttr, DecoderThreadFunc, NULL) != 0) {
        LOGE("decoder: failed to create background thread");
        pthread_attr_destroy(&decAttr);
        return 1;
    }
    pthread_attr_destroy(&decAttr);

    int prevFbW = 0, prevFbH = 0;
    bool firstFrame = true;

    uint64_t lastFpsUs = now_us();
    uint64_t frameCount = 0;
    double renderFps = 0.0;
    double decodeFps = 0.0;
    uint64_t lastDecCount = 0;
    double renderLatencyMs = 0.0;

    struct rusage lastUsage;
    getrusage(RUSAGE_SELF, &lastUsage);
    uint64_t lastCpuSampleUs = now_us();
    double cpuPct = 0.0;
    double processMemMb = 0.0;

    uint64_t lastFrameUs = now_us();
    bool waitingMessageShown = false;

    while (g_running) {
        pthread_mutex_lock(&g_frameMutex);
        while (!g_newFrameReady && g_running) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000L; // 100ms timeout
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_frameCond, &g_frameMutex, &ts);

            if (!g_newFrameReady) {
                uint64_t idleUs = now_us() - lastFrameUs;

                // Session display context: preserved throughout driving and reverse gear.
                // Factory Context 33 is restored on clean shutdown when gal terminates (sentinel watchdog).

                // Note: Self-termination on stream idle removed so player stays alive
                // during reverse gear / pauses. The supervisor in libgal_hook.so will kill
                // and respawn if and only if the player is genuinely hung.

                if (idleUs >= 1000000ULL && !waitingMessageShown && !g_initialCmdsExecuted) {
                    RGBABuffer* buf = g_framePool.getDisplayBuffer();
                    if (buf && buf->pixels) {
                        draw_text_yuv(buf->pixels, buf->width, buf->height,
                                      274, 232, "WAITING FOR SIGNAL...", 2);
                        waitingMessageShown = true;
                        break;
                    }
                }
            }
        }
        if (!g_running) {
            pthread_mutex_unlock(&g_frameMutex);
            break;
        }
        if (g_newFrameReady) {
            g_newFrameReady = false;
            lastFrameUs = now_us();
            waitingMessageShown = false;
            if (!g_streamActive && !firstFrame) {
                g_streamActive = true;
                execute_initial_commands(true);
            }
            if (!g_initialCmdsExecuted) execute_initial_commands();
        }

        uint64_t frameStartUs = now_us();

        RGBABuffer* dispBuf = g_framePool.getDisplayBuffer();
        if (dispBuf && dispBuf->pixels) {
            static uint64_t s_lastRenderUs = 0;
            bool isIdle = (g_idleMaxBytes > 0 && dispBuf->packetBytes > 0 &&
                           dispBuf->packetBytes <= g_idleMaxBytes && !dispBuf->isKeyframe);
            uint64_t heartbeatIntervalUs = (g_idleHeartbeatHz > 0) ? (1000000ULL / (uint64_t)g_idleHeartbeatHz) : 0ULL;
            uint64_t timeSinceRenderUs = frameStartUs - s_lastRenderUs;

            // Idle render bypass: when stationary, only render on heartbeat keepalive (~333ms at 3Hz)
            if (isIdle && heartbeatIntervalUs > 0 && s_lastRenderUs > 0 && timeSinceRenderUs < heartbeatIntervalUs) {
                pthread_mutex_unlock(&g_frameMutex);
                continue; // Skip texture upload & swap; previous surface stays presented cleanly
            }
            s_lastRenderUs = frameStartUs;

            int w = dispBuf->width;
            int h = dispBuf->height;

            uint8_t* pY = dispBuf->pixels;
            uint8_t* pU = pY + (w * h);
            uint8_t* pV = pU + ((w / 2) * (h / 2));

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, g_texY);
            if (firstFrame || w != prevFbW || h != prevFbH) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, pY);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_LUMINANCE, GL_UNSIGNED_BYTE, pY);
            }

            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, g_texU);
            if (firstFrame || w != prevFbW || h != prevFbH) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w/2, h/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, pU);
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w/2, h/2, GL_LUMINANCE, GL_UNSIGNED_BYTE, pU);
            }

            glActiveTexture(GL_TEXTURE2);
            glBindTexture(GL_TEXTURE_2D, g_texV);
            if (firstFrame || w != prevFbW || h != prevFbH) {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w/2, h/2, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, pV);
                

                
                prevFbW = w;
                prevFbH = h;
                firstFrame = false;
                g_streamActive = true;
                execute_initial_commands();
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w/2, h/2, GL_LUMINANCE, GL_UNSIGNED_BYTE, pV);
            }
            
            pthread_mutex_unlock(&g_frameMutex);

            // Render to screen (GPU YUV Shader)
            glViewport(0, 0, windowWidth, windowHeight);
            glClear(GL_COLOR_BUFFER_BIT);

            if (g_useShader) {
                glUseProgram(g_programObject);
                glUniform1i(g_texLocY, 0);
                glUniform1i(g_texLocU, 1);
                glUniform1i(g_texLocV, 2);

                glVertexAttribPointer(g_posAttr, 3, GL_FLOAT, GL_FALSE, 0, landscapeVertices);
                glVertexAttribPointer(g_texAttr, 2, GL_FLOAT, GL_FALSE, 0, landscapeTexCoords);
                glEnableVertexAttribArray(g_posAttr);
                glEnableVertexAttribArray(g_texAttr);

                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

                glDisableVertexAttribArray(g_posAttr);
                glDisableVertexAttribArray(g_texAttr);
            } else {
                static int s_noDrawWarn = 0;
                if (s_noDrawWarn++ < 3)
                    LOGE("render: SHADER PATH FAILED -- GPU YUV requires shaders! (Screen is blank)");
            }

#ifdef __QNX__
            if (!eglSwapBuffers(eglDisplay, eglSurface)) {
                EGLint err = eglGetError();
                if (err == EGL_CONTEXT_LOST) {
                    LOGE("render: EGL_CONTEXT_LOST (display power down or surface invalidated)");
                }
            }
            write_ack();
#else
            if (!g_dumpVideoPipe) {
                const char* dumpPath = getenv("GAL_PLAYER_DUMP_VIDEO");
                if ((!dumpPath || strlen(dumpPath) == 0) && g_dumpVideoPath[0] != '\0') {
                    dumpPath = g_dumpVideoPath;
                }
                const char* dumpFramesEnv = getenv("GAL_PLAYER_DUMP_FRAMES");
                if (dumpFramesEnv && g_dumpMaxFrames == 0) {
                    g_dumpMaxFrames = atoi(dumpFramesEnv);
                }
                if (dumpPath && strlen(dumpPath) > 0) {
                    char cmd[1024];
                    snprintf(cmd, sizeof(cmd),
                             "ffmpeg -y -f rawvideo -vcodec rawvideo -pix_fmt rgba -s %dx%d -r 30 -i - -vf vflip -c:v libx264 -pix_fmt yuv420p -preset fast \"%s\" >/dev/null 2>&1",
                             windowWidth, windowHeight, dumpPath);
                    g_dumpVideoPipe = popen(cmd, "w");
                    if (g_dumpVideoPipe) {
                        LOG("video dump: recording to %s (%dx%d @ 30 FPS, target_frames: %d)", dumpPath, windowWidth, windowHeight, g_dumpMaxFrames);
                    } else {
                        LOGE("video dump: failed to open ffmpeg pipe for %s", dumpPath);
                    }
                }
            }
            if (g_dumpVideoPipe) {
                static uint8_t* s_dumpBuf = NULL;
                if (!s_dumpBuf) {
                    s_dumpBuf = (uint8_t*)malloc((size_t)windowWidth * windowHeight * 4);
                }
                glReadPixels(0, 0, windowWidth, windowHeight, GL_RGBA, GL_UNSIGNED_BYTE, s_dumpBuf);
                fwrite(s_dumpBuf, 1, (size_t)windowWidth * windowHeight * 4, g_dumpVideoPipe);
                g_dumpFrameCount++;
                if (g_dumpMaxFrames > 0 && g_dumpFrameCount >= g_dumpMaxFrames) {
                    LOG("video dump: reached target %d frames, terminating", g_dumpFrameCount);
                    g_running = false;
                }
            }
            glfwSwapBuffers(g_glfwWindow);
            glfwPollEvents();
            write_ack();
#endif
        } else {
            pthread_mutex_unlock(&g_frameMutex);
        }

        frameCount++;
        uint64_t frameEndUs = now_us();
        renderLatencyMs = us_to_ms(frameEndUs - frameStartUs);

        if ((frameEndUs - lastFpsUs) >= 1000000ULL) {
            uint64_t elapsedUs = frameEndUs - lastFpsUs;
            renderFps = (double)frameCount * 1000000.0 / (double)elapsedUs;

            uint64_t curDec = g_decodedFrameCount;
            decodeFps = (double)(curDec - lastDecCount) * 1000000.0 / (double)elapsedUs;
            lastDecCount = curDec;
            frameCount = 0;
            lastFpsUs = frameEndUs;

            struct rusage curUsage;
            getrusage(RUSAGE_SELF, &curUsage);
            uint64_t userUs = (curUsage.ru_utime.tv_sec - lastUsage.ru_utime.tv_sec) * 1000000ULL +
                              (curUsage.ru_utime.tv_usec - lastUsage.ru_utime.tv_usec);
            uint64_t sysUs  = (curUsage.ru_stime.tv_sec - lastUsage.ru_stime.tv_sec) * 1000000ULL +
                              (curUsage.ru_stime.tv_usec - lastUsage.ru_stime.tv_usec);
            uint64_t cpuElapsedUs = frameEndUs - lastCpuSampleUs;
            if (cpuElapsedUs > 0) {
                cpuPct = (double)(userUs + sysUs) * 100.0 / (double)cpuElapsedUs;
            }
            lastUsage = curUsage;
            lastCpuSampleUs = frameEndUs;

            processMemMb = get_process_memory_mb();

            if (g_verbose) {
                LOG_STATS("DISP: %5.1f fps | DEC: %5.1f fps (%4.1fms) | LAT: %4.1fms | CPU: %5.1f%% | MEM: %5.1f MB",
                          renderFps, decodeFps, us_to_ms(g_lastDecodeDurationUs),
                          renderLatencyMs, cpuPct, processMemMb);
            }
        }
    }

    // Instantly restore factory display context before anything else!
    execute_final_commands();

    // Ensure decoder thread is unblocked from recv()
    g_running = false;
    int s = g_streamSock;
    if (s >= 0) {
        shutdown(s, SHUT_RDWR);
    }

#ifndef __QNX__
    if (g_dumpVideoPipe) {
        pclose(g_dumpVideoPipe);
        g_dumpVideoPipe = NULL;
    }
#endif
    LOG("shutting down: waiting for decoder thread");
    pthread_join(decThread, NULL);

#ifdef __QNX__
    if (eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (eglSurface != EGL_NO_SURFACE) eglDestroySurface(eglDisplay, eglSurface);
        if (eglContext != EGL_NO_CONTEXT) eglDestroyContext(eglDisplay, eglContext);
        eglTerminate(eglDisplay);
    }
    if (g_displayInitHandle) {
        // Do not call display_deinit() on exit: Tegra driver deadlocks on semaphore fc522a2c
        // while the display pipeline is active. The OS reclaims resources automatically.
        dlclose(g_displayInitHandle);
    }
#else
    if (g_glfwWindow) {
        glfwDestroyWindow(g_glfwWindow);
        glfwTerminate();
    }
#endif

    LOG("stream-player terminated cleanly");
    return 0;
}
