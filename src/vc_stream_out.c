#include "vc_stream_out.h"
#include "gal_hook.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <unistd.h>

#define VC_STREAM_DEFAULT_PORT 12346
#define VC_ACK_SOCK_PATH       "/tmp/gal_ack.sock"

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_listen_fd = -1;
static int g_client_fd = -1;
static unsigned long g_frames_no_client = 0ul;
static int g_open_failed = 0;                 /* do not retry a hard failure */
static unsigned long g_sent_bytes = 0ul;
static unsigned long g_dropped_bytes = 0ul;
static unsigned long g_dropped_chunks = 0ul;
static unsigned char g_sps_pps_cache[512];
static size_t g_sps_pps_len = 0u;
static unsigned char g_keyframe_cache[524288]; /* 512KB persistent IDR keyframe cache */
static size_t g_keyframe_len = 0u;
static int g_client_has_keyframe = 0;

/* ACK Feedback Pipe file descriptors */
static int g_ack_listen_fd = -1;
static int g_ack_client_fd = -1;

static void drop_client(const char *reason, int err);

static int contains_idr_nal(const unsigned char *p, size_t len)
{
    size_t i;
    for (i = 0; i + 4 < len; ++i) {
        if (p[i] == 0 && p[i+1] == 0) {
            if (p[i+2] == 1) {
                unsigned char nal_type = p[i+3] & 0x1F;
                if (nal_type == 5) return 1;
            } else if (p[i+2] == 0 && p[i+3] == 1) {
                unsigned char nal_type = p[i+4] & 0x1F;
                if (nal_type == 5) return 1;
            }
        }
    }
    return 0;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static unsigned configured_port(void)
{
    const char *s = getenv("GAL_STREAM_PORT");
    if (s != NULL && *s != '\0') {
        char *end = NULL;
        unsigned long v = strtoul(s, &end, 10);
        if (end != NULL && *end == '\0' && v > 0ul && v < 65536ul)
            return (unsigned)v;
        gal_hook_logf("event=stream.config result=ignored reason=bad_port value=\"%s\"", s);
    }
    return VC_STREAM_DEFAULT_PORT;
}

/* Caller holds g_lock. */
static int ensure_listening(void)
{
    struct sockaddr_in addr;
    int on = 1;
    unsigned port;

    if (g_listen_fd >= 0) return 0;
    if (g_open_failed) return -1;

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) {
        g_open_failed = 1;
        gal_hook_logf("event=stream.open result=failed reason=socket errno=%d", errno);
        return -1;
    }
    (void)setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    {
        int sndbuf = 1 << 20;
        (void)setsockopt(g_listen_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof sndbuf);
    }
    if (set_nonblocking(g_listen_fd) != 0) {
        gal_hook_logf("event=stream.open result=failed reason=nonblock errno=%d", errno);
        close(g_listen_fd); g_listen_fd = -1; g_open_failed = 1;
        return -1;
    }

    port = configured_port();
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        gal_hook_logf("event=stream.open result=failed reason=bind port=%u errno=%d", port, errno);
        close(g_listen_fd); g_listen_fd = -1; g_open_failed = 1;
        return -1;
    }
    if (listen(g_listen_fd, 1) != 0) {
        gal_hook_logf("event=stream.open result=failed reason=listen port=%u errno=%d", port, errno);
        close(g_listen_fd); g_listen_fd = -1; g_open_failed = 1;
        return -1;
    }

    (void)fcntl(g_listen_fd, F_SETFD, FD_CLOEXEC);
    gal_hook_logf("event=stream.open result=success port=%u bind=127.0.0.1", port);
    return 0;
}

/*
 * Scan an access unit for SPS (NAL type 7) and PPS (NAL type 8).
 * Android Auto delivers these at the head of IDR keyframes in handleDataAvailable.
 * If present, retain them in g_sps_pps_cache so codec config is available even if
 * handleCodecConfig was never dispatched. Caller holds g_lock.
 */
static void extract_inline_sps_pps(const unsigned char *p, size_t len)
{
    size_t i;
    size_t sps_start = (size_t)-1;
    size_t pps_end = (size_t)-1;
    int found_sps = 0;
    int found_pps = 0;

    for (i = 0; i + 4 < len; ++i) {
        if (p[i] == 0 && p[i+1] == 0) {
            size_t sc_len = 0;
            unsigned char nal_type = 0;
            if (p[i+2] == 1) {
                sc_len = 3;
                nal_type = p[i+3] & 0x1F;
            } else if (p[i+2] == 0 && p[i+3] == 1) {
                sc_len = 4;
                nal_type = p[i+4] & 0x1F;
            }
            if (sc_len > 0) {
                if (nal_type == 7 && !found_sps) {
                    sps_start = i;
                    found_sps = 1;
                } else if (nal_type == 8 && found_sps && !found_pps) {
                    found_pps = 1;
                } else if (found_sps && found_pps && nal_type != 7 && nal_type != 8) {
                    pps_end = i;
                    break;
                }
            }
        }
    }
    if (found_sps && found_pps) {
        if (pps_end == (size_t)-1) pps_end = len;
        if (sps_start < pps_end && (pps_end - sps_start) <= sizeof(g_sps_pps_cache)) {
            g_sps_pps_len = pps_end - sps_start;
            memcpy(g_sps_pps_cache, p + sps_start, g_sps_pps_len);
            gal_hook_logf("event=stream.codec_config result=extracted_inline bytes=%u",
                          (unsigned)g_sps_pps_len);
        }
    }
}

/* Send entire buffer on non-blocking socket with select timeout. Caller holds g_lock. */
static int send_all_timeout(int fd, const unsigned char *buf, size_t len, int timeout_ms)
{
    size_t left = len;
    const unsigned char *p = buf;
    while (left > 0u) {
        ssize_t n = send(fd, p, left, MSG_NOSIGNAL);
        if (n > 0) {
            p += (size_t)n;
            left -= (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            struct timeval tv;
            fd_set wfds;
            int sret;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            sret = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (sret > 0 && FD_ISSET(fd, &wfds)) {
                continue;
            }
            return -1; /* timed out or stalled */
        }
        return -1; /* hard socket error */
    }
    return 0;
}

/* Caller holds g_lock. Never blocks: if nobody is waiting, we stay unconnected. */
static void try_accept(void)
{
    int fd;
    if (g_client_fd >= 0 || g_listen_fd < 0) return;
    fd = accept(g_listen_fd, NULL, NULL);
    if (fd < 0) return;                       /* EWOULDBLOCK: nobody there */
    if (set_nonblocking(fd) != 0) { close(fd); return; }
    {
        int on = 1;
        int buf_size = 2097152;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
        (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof buf_size);
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof buf_size);
    }
    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);

    g_client_fd = fd;
    g_sent_bytes = 0ul;
    g_dropped_bytes = 0ul;
    g_dropped_chunks = 0ul;
    gal_hook_log("event=stream.client result=connected");

    /*
     * Replay cached SPS/PPS codec configuration immediately upon connection.
     * This eliminates the race condition where stream-player connects after
     * handleCodecConfig was dispatched, allowing FFmpeg to identify the stream
     * and initialize its decoder without waiting for a new keyframe.
     */
    if (g_sps_pps_len > 0u) {
        if (send_all_timeout(g_client_fd, g_sps_pps_cache, g_sps_pps_len, 1000) == 0) {
            g_sent_bytes += (unsigned long)g_sps_pps_len;
            gal_hook_logf("event=stream.client action=replayed_codec_config bytes=%u", (unsigned)g_sps_pps_len);
        } else {
            drop_client("replay_codec_config_failed", errno);
            return;
        }
    }
    if (g_keyframe_len > 0u) {
        if (send_all_timeout(g_client_fd, g_keyframe_cache, g_keyframe_len, 1000) == 0) {
            g_sent_bytes += (unsigned long)g_keyframe_len;
            gal_hook_logf("event=stream.client action=replayed_keyframe bytes=%u", (unsigned)g_keyframe_len);
            g_client_has_keyframe = 1;
        } else {
            drop_client("replay_keyframe_failed", errno);
            return;
        }
    } else {
        g_client_has_keyframe = 0;
    }
}

void vc_stream_out_begin_stream(void)
{
    pthread_mutex_lock(&g_lock);
    /*
     * An IDR is only valid for the stream/session that produced it. Replaying
     * the previous phone session cached IDR lets FFmpeg start immediately,
     * but its following P-frames reference a different encoder state and show
     * corruption until the next current-session IDR. Keep any codec config
     * already delivered for the new session, but require a fresh IDR.
     */
    drop_client("new_stream", 0);
    g_keyframe_len = 0u;
    g_client_has_keyframe = 0;
    gal_hook_log("event=stream.bootstrap result=reset reason=new_playback_session");
    pthread_mutex_unlock(&g_lock);
}

void vc_stream_out_set_codec_config(const void *data, size_t bytes)
{
    int changed;
    if (data == NULL || bytes == 0u || bytes > sizeof(g_sps_pps_cache)) return;
    pthread_mutex_lock(&g_lock);
    changed = (bytes != g_sps_pps_len ||
               memcmp(g_sps_pps_cache, data, bytes) != 0);
    memcpy(g_sps_pps_cache, data, bytes);
    g_sps_pps_len = bytes;
    if (changed) {
        g_keyframe_len = 0u;
        g_client_has_keyframe = 0;
    }
    gal_hook_logf("event=stream.codec_config result=cached bytes=%u", (unsigned)bytes);
    /* If a client is already connected, forward the new codec config immediately */
    if (g_client_fd >= 0) {
        if (send_all_timeout(g_client_fd, g_sps_pps_cache, g_sps_pps_len, 1000) == 0) {
            g_sent_bytes += (unsigned long)g_sps_pps_len;
        } else {
            drop_client("codec_config_send_failed", errno);
        }
    }
    pthread_mutex_unlock(&g_lock);
}

/* Caller holds g_lock. */
static void drop_client(const char *reason, int err)
{
    if (g_client_fd < 0) return;
    close(g_client_fd);
    g_client_fd = -1;
    g_client_has_keyframe = 0;
    gal_hook_logf("event=stream.client result=disconnected reason=%s errno=%d sent=%lu dropped=%lu",
                  reason, err, g_sent_bytes, g_dropped_bytes);
}

int vc_stream_out_open(void)
{
    int rc;
    pthread_mutex_lock(&g_lock);
    rc = ensure_listening();
    pthread_mutex_unlock(&g_lock);
    return rc;
}

int vc_stream_out_write(const void *data, size_t bytes)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t left = bytes;
    int rc = 0;

    if (data == NULL || bytes == 0u) return 0;

    pthread_mutex_lock(&g_lock);

    int is_idr = contains_idr_nal(p, bytes);
    /* Retain latest IDR Keyframe for instant client bootstrap */
    if (is_idr) {
        if (bytes <= sizeof(g_keyframe_cache)) {
            memcpy(g_keyframe_cache, data, bytes);
            g_keyframe_len = bytes;
        } else {
            /* Do not leave an older IDR available for replay. Following
             * frames may reference this newer decoder reset point, so an old
             * cached IDR would not be a valid bootstrap for a late client. */
            g_keyframe_len = 0u;
            gal_hook_logf("event=stream.keyframe result=dropped_oversize bytes=%u max=%u",
                          (unsigned)bytes, (unsigned)sizeof(g_keyframe_cache));
        }
        /* Inline SPS/PPS belongs to this IDR and supersedes stale config. */
        extract_inline_sps_pps(p, bytes);
    }

    if (ensure_listening() != 0) { pthread_mutex_unlock(&g_lock); return -1; }
    try_accept();
    if (g_client_fd < 0) {
        /*
         * No consumer attached, so this frame goes nowhere. Say so, or a
         * capture shows the secondary happily receiving and forwarding
         * frames with nothing to indicate the far end never existed.
         * Rate limited because this is normal whenever player is not running.
         */
        ++g_frames_no_client;
        if (g_frames_no_client == 1ul || g_frames_no_client % 300ul == 0ul)
            gal_hook_logf("event=stream.write result=no_client frames=%lu bytes=%lu",
                          g_frames_no_client, (unsigned long)bytes);
        pthread_mutex_unlock(&g_lock);
        return -1;
    }

    /*
     * Keyframe Gate:
     * Never forward P-frames to a client that hasn not received an IDR keyframe yet.
     * Prevents macroblock mosaic / decoding corruptions during initial connection.
     */
    if (!g_client_has_keyframe) {
        if (is_idr) {
            g_client_has_keyframe = 1;
        } else {
            pthread_mutex_unlock(&g_lock);
            return 0; /* Withhold pre-keyframe delta frame */
        }
    }

    {
        size_t sent_here = 0u;
        while (left > 0u) {
            ssize_t n = send(g_client_fd, p, left, MSG_NOSIGNAL);
            if (n > 0) {
                p += (size_t)n; left -= (size_t)n; sent_here += (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
                struct timeval tv;
                fd_set wfds;
                int sret;
                FD_ZERO(&wfds);
                FD_SET(g_client_fd, &wfds);
                /*
                 * 250ms timeout: allows display context / reverse camera transitions
                 * to catch up without false disconnects.
                 */
                tv.tv_sec = 0;
                tv.tv_usec = 250000;
                sret = select(g_client_fd + 1, NULL, &wfds, NULL, &tv);
                if (sret > 0 && FD_ISSET(g_client_fd, &wfds)) continue;
                g_dropped_bytes += (unsigned long)left;
                g_dropped_chunks += 1ul;
                drop_client(sent_here > 0u ?
                            "partial_write_timeout" : "write_timeout",
                            errno);
                rc = -1;
                break;
            }
            drop_client(n == 0 ? "eof" : "send_error", errno);
            rc = -1;
            break;
        }
    }
    if (rc == 0) g_sent_bytes += (unsigned long)bytes;
    pthread_mutex_unlock(&g_lock);
    return rc;
}

unsigned vc_stream_out_port(void)
{
    return configured_port();
}

const char *vc_stream_out_path(void)
{
    return NULL;
}

int vc_stream_out_is_connected(void)
{
    int c;
    pthread_mutex_lock(&g_lock);
    c = (g_client_fd >= 0);
    pthread_mutex_unlock(&g_lock);
    return c;
}

/* ---------------- Player-ACK Feedback Pipe ---------------- */

int vc_stream_out_ack_init(void)
{
    struct sockaddr_un addr;
    const char *sock_path = VC_ACK_SOCK_PATH;

    if (g_ack_listen_fd >= 0) return 0;

    g_ack_listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_ack_listen_fd < 0) {
        gal_hook_logf("event=ack.open result=failed reason=socket errno=%d", errno);
        return -1;
    }
    if (set_nonblocking(g_ack_listen_fd) != 0) {
        close(g_ack_listen_fd); g_ack_listen_fd = -1;
        return -1;
    }
    (void)unlink(sock_path);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(g_ack_listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        gal_hook_logf("event=ack.open result=failed reason=bind path=%s errno=%d", sock_path, errno);
        close(g_ack_listen_fd); g_ack_listen_fd = -1;
        return -1;
    }
    if (listen(g_ack_listen_fd, 1) != 0) {
        gal_hook_logf("event=ack.open result=failed reason=listen path=%s errno=%d", sock_path, errno);
        close(g_ack_listen_fd); g_ack_listen_fd = -1;
        return -1;
    }
    (void)fcntl(g_ack_listen_fd, F_SETFD, FD_CLOEXEC);
    gal_hook_logf("event=ack.open result=success path=%s", sock_path);
    return 0;
}

void vc_stream_out_ack_close(void)
{
    if (g_ack_client_fd >= 0) {
        close(g_ack_client_fd);
        g_ack_client_fd = -1;
    }
    if (g_ack_listen_fd >= 0) {
        close(g_ack_listen_fd);
        g_ack_listen_fd = -1;
        (void)unlink(VC_ACK_SOCK_PATH);
    }
}

int vc_stream_out_ack_client_connected(void)
{
    return (g_ack_client_fd >= 0);
}

void vc_stream_out_poll_ack(vc_ack_callback_fn cb)
{
    unsigned char buf[64];
    ssize_t n;

    if (g_ack_listen_fd < 0) {
        if (vc_stream_out_ack_init() != 0) return;
    }

    if (g_ack_client_fd < 0) {
        int cfd = accept(g_ack_listen_fd, NULL, NULL);
        if (cfd >= 0) {
            if (set_nonblocking(cfd) == 0) {
                (void)fcntl(cfd, F_SETFD, FD_CLOEXEC);
                g_ack_client_fd = cfd;
                gal_hook_log("event=ack.client result=connected");
            } else {
                close(cfd);
            }
        }
    }

    if (g_ack_client_fd >= 0) {
        while ((n = recv(g_ack_client_fd, buf, sizeof(buf), 0)) > 0) {
            if (cb) cb((unsigned)n);
        }
        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            close(g_ack_client_fd);
            g_ack_client_fd = -1;
            gal_hook_log("event=ack.client result=disconnected");
        }
    }
}

void vc_stream_out_close(void)
{
    pthread_mutex_lock(&g_lock);
    drop_client("shutdown", 0);
    if (g_listen_fd >= 0) {
        close(g_listen_fd);
        g_listen_fd = -1;
    }
    g_open_failed = 0;
    g_sps_pps_len = 0u;
    g_keyframe_len = 0u;
    g_client_has_keyframe = 0;
    pthread_mutex_unlock(&g_lock);

    vc_stream_out_ack_close();
    gal_hook_log("event=stream.close result=success");
}
