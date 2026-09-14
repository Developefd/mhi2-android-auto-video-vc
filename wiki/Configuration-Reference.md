# Configuration Reference

The complete commented template is
[`scripts/gal_dualscreen.conf.example`](../scripts/gal_dualscreen.conf.example).
This page groups settings by their operational meaning. Unknown or experimental
keys should remain unset until a bounded test requires them.

## Precedence and loading

At startup the hook reads the configuration file from the configured path,
normally `/fs/sdb0/gal_dualscreen.conf`, then falls back to `/tmp` when needed.
`GAL_HOOK_CONF` can select another location. Values injected by
`enable_hook.sh` into the supervisor environment take precedence over file
values. Keep both sources in the test record.

## Core switches

| Key | Default/normal value | Meaning |
|---|---|---|
| `GAL_DUALSCREEN_ENABLE` | `1` | Master hook enable. |
| `GAL_DUALSCREEN_DEBUG` | `1` during development | Enables verbose frame evidence. |
| `GAL_DUALSCREEN_OUTPUT` | `withhold` | Keeps secondary playback away from stock GAL's shared renderer. |
| `GAL_STREAM_ENABLE` | `1` | Enables H.264 forwarding to the external player. |
| `GAL_STREAM_PORT` | `12346` | Loopback listener; player receives the same port automatically. |

## Android Auto discovery

| Key | Normal test value | Meaning |
|---|---|---|
| `GAL_DUALSCREEN_AAP_MINOR` | `7` | Advertised Android Auto protocol minor version. |
| `GAL_DUALSCREEN_CLUSTER_INPUT` | `1` | Advertises cluster InputSourceService. |
| `GAL_DUALSCREEN_MAIN_INPUT_ID` | `1` | Tags GAL's primary InputSourceService as display ID 0. |
| `GAL_SECONDARY_AAP_DISPLAY_ID` | `1` | Android Auto cluster display identifier. |
| `GAL_SECONDARY_SERVICE_ID` | `0` | First free service after primary. |
| `GAL_DUALSCREEN_SECOND_SINK` | `1` | Register secondary sink; set `0` only for bisecting. |
| `GAL_DUALSCREEN_INJECT_META` | `1` | Inject display metadata; set `0` only for bisecting. |

Changing protocol-minor, cluster-input, sink registration, and metadata together
does not isolate a discovery failure. Change one input per case.

## Cluster route and video geometry

| Key | Normal value | Meaning |
|---|---|---|
| `GAL_VC_DISPLAYABLE_ID` | `3` | Player-created cluster displayable. |
| `GAL_VC_CONTEXT` | `70` | Virtual Cockpit DMDT context. |
| `GAL_VC_DISPLAY` | `4` | DMDT Cockpit display ID; not the `dmdt gs` index. |
| `GAL_VC_RESTORE_DISPLAYABLE_ID` | `33` | Stock Kombi map on teardown. |
| `GAL_SECONDARY_WIDTH/HEIGHT` | `800` / `480` | Fixed coded canvas. |
| `GAL_SECONDARY_FPS` | `30` | Advertised video rate. |
| `GAL_SECONDARY_DPI` | profile-specific | UI scale hint, not physical panel DPI. |
| `GAL_SECONDARY_VIEWING_DISTANCE` | `700` | Separate display hint. |

`GAL_SECONDARY_INSETS` is ordered `top,bottom,left,right`. If
`GAL_SECONDARY_UI_CONFIG_HEX` is present, the exact bytes override structured
insets and theme. This is the most common geometry-testing trap.

## Player controls

`GAL_PLAYER_PATH` overrides player discovery. `GAL_AUTORUN_PLAYER=0` prevents
automatic spawn. The player also reads thread, low-delay, idle-byte, heartbeat,
and deblock controls. Treat `GAL_PLAYER_DEBLOCK=none` as a deliberate decoder
choice, not a generic performance default.

## Logging and safe experimentation

`GAL_HOOK_LOG` selects a log path but costs an environment entry if injected.
The normal fallback is `/tmp/gal_dualscreen.log`. `GAL_DUALSCREEN_ACK=0` is a
diagnostic switch only: it suppresses the normal phone-frame acknowledgement and
can intentionally stall the session.
