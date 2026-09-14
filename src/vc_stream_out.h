#ifndef VC_STREAM_OUT_H
#define VC_STREAM_OUT_H

#include <stddef.h>

/*
 * Forwards the secondary sink's H.264 elementary stream out of gal over a
 * local Unix domain socket (/tmp/gal_video.sock), so a separate process can
 * decode and display it.
 *
 * Every call is non-blocking and every failure is silent-but-counted. A
 * hook running inside gal must never stall or kill the process it is
 * living in, so when there is no reader, or the reader is not keeping up,
 * frames are dropped rather than buffered or waited on.
 */

int  vc_stream_out_open(void);                       /* idempotent */
void vc_stream_out_begin_stream(void);                /* reset per-session bootstrap */
int  vc_stream_out_write(const void *data, size_t bytes);
void vc_stream_out_set_codec_config(const void *data, size_t bytes);
void vc_stream_out_close(void);
int  vc_stream_out_is_connected(void);
const char *vc_stream_out_path(void);
unsigned vc_stream_out_port(void);

typedef void (*vc_ack_callback_fn)(unsigned rendered_count);
int  vc_stream_out_ack_init(void);
void vc_stream_out_ack_close(void);
void vc_stream_out_poll_ack(vc_ack_callback_fn cb);
int  vc_stream_out_ack_client_connected(void);

#endif
