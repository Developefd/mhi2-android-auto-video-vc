# Engineering Reference

This page records current tested behavior. It complements the explanatory
[Architecture Deep Dive](Architecture-Deep-Dive.md) with details that are easy
to lose during iterative reverse engineering.

## Car-tested reference profile

```ini
GAL_SECONDARY_DPI=125
GAL_SECONDARY_INSETS=80,100,125,125
GAL_SECONDARY_UI_THEME=2
GAL_SECONDARY_UI_CONFIG_HEX=5a0c120808501064187d207d2002
GAL_PLAYER_DEBLOCK=none
```

This is the car-tested profile documented in the related `gal_dualscreen`
worktree; it is not the same as this repository's checked-in example fallback,
which uses DPI 140 and different inset bytes. Confirm the deployed file before
calling either profile “current.”

The hook advertises an 800×480, 30-FPS coded frame with zero outer margins.
The active UiConfig field-2 payload describes the safe area as
`top=80, bottom=100, left=125, right=125`. In the tested phone build this
constrains Android Auto cards while preserving full-screen video. DPI is a
normal video-configuration scalar; the hex string is serialized protobuf data.

### UiConfig fields

| Field | Runtime role |
|---:|---|
| 1 | Content/crop rectangle; non-zero values can create black borders. |
| 2 | `contentInsets`; tested safe-area path for cards and overlays. |
| 3 | `stableInsets`; not the tested card-constraint path. |
| 4 | UI theme: `0` automatic, `1` light, `2` dark. |

Do not describe field 4 as `CropMargins`; that interpretation was disproved.
If exact hex is supplied, it takes precedence over structured inset/theme
settings. The hook accepts payloads up to 15 bytes because of the firmware's
inline `std::string` representation.

## Session and rendering lifecycle

1. GAL starts and the preload hook registers a secondary VideoSink endpoint.
2. The phone opens the secondary channel and sends H.264 access units.
3. The hook caches SPS/PPS and a bounded IDR, then forwards synchronized data
   to `stream-player` over `127.0.0.1:12346`.
4. The player decodes with FFmpeg, renders through `glDrawTextureNV`, and
   switches Displayable 3 to Context 70 only after stream and Kombi readiness.
5. After `eglSwapBuffers()`, the player writes one byte to
   `/tmp/gal_ack.sock`; the hook releases the corresponding phone ACK.
6. On teardown, stale IDR state is cleared so old reference data cannot enter a
   new encoder session.

The player delays only cluster routing commands. Center-display Exit is an HMI
event path, not a substitute for DMDT screen-buffer commands.

## Android Auto Exit behavior

The in-app Exit control should leave the center Android Auto canvas and present
App-Connect while keeping the phone session, secondary sink, and Virtual
Cockpit navigation alive. App-Connect Disconnect separately ends the session.

Primary mode-2 focus requests must continue through the stock Java/HMI chain so
`CANVAS_LEFT` is emitted. Secondary mode-1/mode-2 requests are acknowledged on
the secondary sink and must not alter the display-unqualified LSD focus state.
Calling `dmdt sb 0` is not sufficient to reproduce the HMI transition.

## Acceptance sequence

1. Start Android Auto and confirm center and Virtual Cockpit video.
2. Tap Android Auto's in-app Exit.
3. Confirm the center shows App-Connect with Disconnect available.
4. Confirm the phone remains connected and Virtual Cockpit navigation remains.
5. Re-enter Android Auto and confirm center video resumes.
6. Use App-Connect Disconnect and confirm both streams tear down.

Record firmware, package/config checksum, hook log, player log, and phone/app
version with every car test.

## Known limits and planned work

- TCP loopback remains the transport; AF_UNIX migration is planned.
- Decoding remains FFmpeg-based; NvSS/NvMedia investigation is planned.
- Dynamic focus and Kombi-tab-aware encoding suspension remain open.
- GPS uncertainty clamping remains experimental and requires road validation.
