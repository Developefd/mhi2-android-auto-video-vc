#ifndef VC_PLAYER_MGR_H
#define VC_PLAYER_MGR_H

#ifdef __cplusplus
extern "C" {
#endif

void vc_player_start(void);
void vc_player_stop(void);
void vc_player_restart(void);
int  vc_player_is_running(void);

/*
 * Supervisor watchdog:
 * Called periodically from the ACK polling thread.
 * If frames are actively arriving from Android Auto, but stream-player has not
 * produced any render ACK for >= 15 seconds, forcefully terminates the stuck
 * player with SIGKILL, restores display context 33, and spawns a fresh player.
 */
void vc_player_supervisor_tick(long frame_silence_ms, long ack_silence_ms);

#ifdef __cplusplus
}
#endif

#endif
