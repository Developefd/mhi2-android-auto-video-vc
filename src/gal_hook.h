#ifndef GAL_HOOK_H
#define GAL_HOOK_H

#include <stddef.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gal_secondary_config {
    unsigned width;
    unsigned height;
    unsigned fps;
    unsigned codec;       /* 1 = H.264/AVC in the GAL video protocol */
    unsigned displayable_id; /* custom QNX EGL window; VcMOST uses 3 */
    unsigned vc_display;     /* dmdt sc/sb display id for the VC: 4, NOT the
                              * 1 that `dmdt gs` prints as its index */
    unsigned context_id;     /* Display Manager context; this unit uses 70 */
    unsigned restore_displayable_id; /* stock Kombi map window, 33 */
    unsigned service_id;    /* 0 = select the first free ID after primary */
    unsigned display_id;    /* AAP display_id; cluster is 1 */
    unsigned dpi;
    unsigned viewing_distance;
    unsigned pixel_aspect_ratio_e4;
    unsigned insets_top;
    unsigned insets_bottom;
    unsigned insets_left;
    unsigned insets_right;
    unsigned ui_theme;
    unsigned ui_config_payload_len;
    unsigned char ui_config_payload[16];
} gal_secondary_config;

int gal_hook_init(const gal_secondary_config *config);
void gal_hook_log(const char *message);
void gal_hook_logf(const char *format, ...);
void gal_hook_debugf(const char *format, ...);
int gal_hook_is_enabled(void);
int gal_hook_is_debug(void);
/*
 * Independent bisect switches for the 2026-08-22 SERVICE_DISCOVERY_MISSING
 * result: the phone stopped answering discovery once the hook both added a
 * second video service AND injected unknown display fields. Those two
 * changes were confounded. Both default ON, so behaviour is unchanged
 * unless explicitly disabled.
 *   GAL_DUALSCREEN_SECOND_SINK=0  -> do not build/register the second sink
 *   GAL_DUALSCREEN_INJECT_META=0  -> do not write the display_id/type fields
 */
int gal_hook_second_sink_enabled(void);
int gal_hook_inject_meta_enabled(void);
/*
 * AAP minor version to advertise. Defaults to 2, the stock value, so an
 * unset environment changes nothing. GAL_DUALSCREEN_AAP_MINOR=7 makes the
 * receiver claim 1.7, which is what Google's DHU negotiates.
 */
unsigned gal_hook_aap_minor(void);
int gal_hook_aap_minor_overridden(void);
/*
 * Advertise a cluster InputSourceService (display id 1) beside the
 * secondary video sink. Proven necessary against DHU; defaults OFF so it
 * can be enabled deliberately together with --aap-minor.
 */
int gal_hook_cluster_input_enabled(void);
/*
 * Output mode: what to do with the secondary sink's video.
 *
 *   withhold  Do not forward playbackStart to GAL. The sink stays registered
 *             and frames arrive via handleDataAvailable, are ACKed, and are
 *             forwarded over TCP to stream-player (127.0.0.1:12346).
 *             Main screen untouched. This is the only production path.
 *
 *   gal       Forward playbackStart to GAL. Stock path; reconfigures the
 *             one shared CVideoRenderer and blanks the main screen.
 *             Kept for diagnostic comparison only.
 */
enum {
    GAL_OUTPUT_GAL = 0,
    GAL_OUTPUT_WITHHOLD
};
int gal_hook_output_mode(void);
/*
 * Acknowledge decoded frames back to the phone ourselves.
 *
 * MediaSinkBase::ackFrames(session, count) is what tells the phone a frame
 * was consumed. In withhold mode GAL never acks the secondary stream, so
 * the hook acks every frame itself. Defaults ON, GAL_DUALSCREEN_ACK=0
 * disables (useful when measuring how long the phone tolerates silence).
 */
int gal_hook_frame_ack_enabled(void);
int gal_hook_focus_mirror_enabled(void);
int gal_hook_main_input_id_enabled(void);
const char *gal_hook_output_mode_name(void);
const gal_secondary_config *gal_hook_config(void);

#ifdef __cplusplus
}
#endif

#endif
