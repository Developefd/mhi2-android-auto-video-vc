#include "vc_player_mgr.h"
#include "gal_hook.h"
#include "vc_stream_out.h"

#include <errno.h>
#ifdef __QNX__
#include <process.h>
#endif
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static pid_t g_player_pid = -1;

/*
 * A regular file, and nothing else.
 *
 * The execute bit is not usable as the test here: FAT32 does not carry one,
 * so a player on the SD card would be rejected -- which is why the check was
 * relaxed in the first place. But it was relaxed to
 * `(st_mode & S_IXUSR) || S_ISREG(st_mode)`, and a DIRECTORY named
 * stream-player satisfies the first half, because directories normally do
 * carry the user-execute bit. Such a path then wins the search and spawnv
 * fails on it, while a real binary further down the candidate list is never
 * reached. S_ISREG alone covers both filesystems; if the file turns out not
 * to be executable, spawnv reports that plainly.
 */
static int is_runnable(const struct stat *st)
{
    return S_ISREG(st->st_mode) != 0;
}

static const char *find_player_binary(void)
{
    int i;
    const char *custom = getenv("GAL_PLAYER_PATH");
    if (custom != NULL && *custom != '\0') {
        struct stat st;
        if (stat(custom, &st) == 0 && is_runnable(&st))
            return custom;
    }

    /* Check persistent /navigation SSD first, then SD card mounts and current dir */
    static const char *candidates[] = {
        "/mnt/app/eso/bin/stream-player",
        "/mnt/app/navigation/stream-player",
        "/navigation/stream-player",
        "/fs/sdb0/stream-player",
        "/fs/sda0/stream-player",
        "./stream-player",
        "/tmp/stream-player",
        NULL
    };

    for (i = 0; candidates[i] != NULL; ++i) {
        struct stat st;
        if (stat(candidates[i], &st) == 0 && is_runnable(&st))
            return candidates[i];
    }
    return NULL;
}

static int is_auto_player_enabled(void)
{
    const char *v = getenv("GAL_AUTORUN_PLAYER");
    if (v != NULL && (*v == '0' || strcmp(v, "false") == 0 || strcmp(v, "no") == 0))
        return 0;
    return 1;
}

void vc_player_start(void)
{
    const char *bin;
    pid_t pid;
    char *argv[3];
    char url[64];

    if (!is_auto_player_enabled()) return;
    if (g_player_pid > 0) {
        int status;
        if (waitpid(g_player_pid, &status, WNOHANG) == 0) {
            return; /* already running */
        }
        g_player_pid = -1;
    }

#ifdef __QNX__
    /* Terminate any orphan or lingering stream-player before spawning fresh instance */
    (void)system("slay -9 -f -q stream-player 2>/dev/null");
#endif
    bin = find_player_binary();
    if (bin == NULL) {
        gal_hook_logf("event=player.spawn result=skipped reason=binary_not_found");
        return;
    }

    /* Ensure IPL_CONFIG_DIR and LD_LIBRARY_PATH are set for player graphics initialization */
    setenv("IPL_CONFIG_DIR", "/etc/eso/production", 0);
    setenv("LD_LIBRARY_PATH", "/proc/boot:/lib:/lib/dll:/usr/lib:/mnt/app/root/lib-target:/mnt/app/usr/lib:/mnt/app/armle/lib:/mnt/app/armle/lib/dll:/mnt/app/armle/usr/lib:/eso/lib:/mnt/app/eso/lib:/fs/sdb0/lib", 1);
    unsetenv("LD_PRELOAD");

    /*
     * Built from vc_stream_out's own port rather than hardcoded. GAL_STREAM_PORT
     * is a documented knob in gal_dualscreen.conf, and with a literal 12346 here
     * setting it made the hook listen on one port while the player it spawned
     * dialled another -- a black cluster whose only trace is a connect failure
     * in the player's output, not in this log.
     */
    (void)snprintf(url, sizeof(url), "tcp://127.0.0.1:%u", vc_stream_out_port());
    argv[0] = (char *)"stream-player";
    argv[1] = url;
    argv[2] = NULL;

    {
        char *envp[] = {
            (char *)"IPL_CONFIG_DIR=/etc/eso/production",
            (char *)"LD_LIBRARY_PATH=/proc/boot:/lib:/lib/dll:/usr/lib:/mnt/app/root/lib-target:/mnt/app/usr/lib:/mnt/app/armle/lib:/mnt/app/armle/lib/dll:/mnt/app/armle/usr/lib:/eso/lib:/mnt/app/eso/lib:/fs/sdb0/lib",
            (char *)"PATH=/proc/boot:/bin:/usr/bin:/usr/sbin:/sbin:/eso/bin:/mnt/app/eso/bin:/fs/sdb0/bin:/mnt/app/navigation",
            NULL
        };
#ifdef __QNX__
        pid = (pid_t)spawnve(P_NOWAIT, bin, argv, envp);
#else
        pid = fork();
        if (pid == 0) {
            execve(bin, argv, envp);
            _exit(127);
        }
#endif
    }

    if (pid < 0) {
        gal_hook_logf("event=player.spawn result=failed path=%s errno=%d", bin, errno);
        g_player_pid = -1;
    } else {
        g_player_pid = pid;
        gal_hook_logf("event=player.spawn result=success path=%s pid=%ld url=%s", bin, (long)pid, url);
    }
}

void vc_player_stop(void)
{
    if (g_player_pid > 0) {
        int status;
        int i;
        gal_hook_logf("event=player.stop pid=%ld action=SIGTERM", (long)g_player_pid);
        kill(g_player_pid, SIGTERM);
        /*
         * Poll for clean process exit up to 300ms (10ms intervals).
         * If the player terminates cleanly and restores DMDT, this loop
         * returns immediately without arbitrary delays.
         */
        for (i = 0; i < 30; ++i) {
            if (waitpid(g_player_pid, &status, WNOHANG) != 0) {
                gal_hook_logf("event=player.stop pid=%ld result=clean_exit elapsed_ms=%d",
                              (long)g_player_pid, (i + 1) * 10);
                g_player_pid = -1;
                return;
            }
            usleep(10000);
        }
        /* If still running after 300ms, force termination */
        gal_hook_logf("event=player.stop pid=%ld action=SIGKILL", (long)g_player_pid);
        kill(g_player_pid, SIGKILL);
        for (i = 0; i < 10; ++i) {
            if (waitpid(g_player_pid, &status, WNOHANG) != 0) break;
            usleep(10000);
        }
        if (i == 10) {
            gal_hook_logf("event=player.stop result=unreaped pid=%ld",
                          (long)g_player_pid);
        }
        g_player_pid = -1;
        /* Emergency DMDT restore if player had to be forcefully terminated */
#ifdef __QNX__
        (void)system("LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib IPL_CONFIG_DIR=/etc/eso/production /eso/bin/apps/dmdt dc 70 33 2>/dev/null; "
                     "LD_LIBRARY_PATH=/eso/lib:/lib:/usr/lib IPL_CONFIG_DIR=/etc/eso/production /eso/bin/apps/dmdt sc 4 70 2>/dev/null");
#endif
    }
}

void vc_player_restart(void)
{
    vc_player_stop();
    usleep(50000); /* 50ms pause before respawn */
    vc_player_start();
}

int vc_player_is_running(void)
{
    int status;
    pid_t ret;
    if (g_player_pid <= 0) return 0;
    ret = waitpid(g_player_pid, &status, WNOHANG);
    if (ret == 0) return 1; /* Process is still active */
    
    /* Process has terminated or no longer exists; clean up state */
    g_player_pid = -1;
    return 0;
}

void vc_player_supervisor_tick(long frame_silence_ms, long ack_silence_ms)
{
    static long s_cooldown_ticks = 0;
    /* Only supervise if a player process is currently alive */
    if (!vc_player_is_running()) {
        s_cooldown_ticks = 0;
        return;
    }

    if (s_cooldown_ticks > 0) {
        s_cooldown_ticks--;
        return;
    }

    /*
     * Condition for hang detection:
     * 1. Frames are actively arriving from the phone (frame_silence_ms <= 2000).
     * 2. The player has not produced any render ACK for >= 15000ms (15 seconds).
     */
    if (frame_silence_ms >= 0 && frame_silence_ms <= 2000) {
        if (ack_silence_ms >= 15000) {
            gal_hook_logf("event=player.supervisor action=kill_hung_player ack_silence_ms=%ld pid=%ld",
                          ack_silence_ms, (long)g_player_pid);
            s_cooldown_ticks = 20; /* 20 * 500ms = 10s anti-flapping cooldown */
            vc_player_restart();
        }
    }
}
