# Architecture Deep Dive

## Design goal

Stock GAL owns one shared center-display renderer. The hook adds a secondary
Android Auto video service without letting secondary playback reconfigure or
blank the center display. It forwards H.264 to a separate Virtual Cockpit
renderer.

```text
phone ──USB/AAP──► gal + libgal_hook.so ──TCP loopback──► stream-player
                         │                                      │
                         │                                      └─ EGL / GL_NV_draw_texture
                         └─ secondary service                         │
                                                                  dmdt: display 4 → context 70
```

## Ownership boundary

The essential architectural choice is not that there are two displays; it is
that there are two renderer owners. GAL owns the primary Android Auto video
renderer and the center display lifecycle. `stream-player` owns the secondary
decode/presentation lifecycle and is the only custom process that writes pixels
to the Cockpit route. The hook connects them, but must not make one owner act
as the other.

| Component | Owns | Must not own |
|---|---|---|
| Stock GAL | Primary sink, primary renderer, stock Android Auto behavior | Cockpit video presentation for the withheld secondary sink |
| `libgal_hook.so` | Secondary endpoint, ABI checks, encoded-frame handoff, phone ACK policy, player supervision | A replacement center renderer or Java HMI state |
| `vc_stream_out.c` | Loopback listener/client lifecycle and bounded bootstrap cache | Video decoding or DMDT routing |
| `stream-player` | FFmpeg decode, EGL/OpenKODE presentation, player-side ACK, Cockpit route restoration | Phone-protocol negotiation or GAL object manipulation |
| `libdmdt_flush.so` | Flushing `dmdt` output on `_exit()` so the player's `dmdt gs` readiness and route checks can read it | GAL or player state; it is preloaded only into `dmdt` |
| Java/LSD HMI | Center-canvas Exit and App-Connect state | Secondary H.264 transport or Cockpit frame pacing |

This split gives each failure a narrow owner. A phone that never opens the
secondary service is a negotiation/registration problem. Valid H.264 with no
Cockpit image is a player or DMDT problem. A correct Cockpit image with a bad
center Exit transition is an HMI problem.

## Runtime sequence

```text
GAL startup
  → preload resolves required symbols and verifies this firmware's ABI
  → observes primary VideoSink configuration
  → allocates/initializes an independent secondary sink + callback handler
  → registers the secondary service and its one 800×480 configuration

Phone session
  → secondary channel/service negotiation
  → secondary playbackStart
  → hook resets bootstrap state, opens stream + ACK listeners, starts player
  → codec config / H.264 access units arrive
  → hook extracts Annex-B payload and forwards it to loopback client

Presentation
  → player decodes and presents a frame
  → player routes Displayable 3 into Cockpit context 70 when ready
  → player emits one ACK byte
  → hook converts ACK byte into native media acknowledgement to phone

Teardown / recovery
  → playbackStop or new-session cleanup closes stream state and stops player
  → player or supervisor restores factory Displayable 33
```

The sequence contains deliberately separate readiness conditions. The player
may connect before it can decode; it may decode before the Kombi route is
usable; and a phone may be healthy while player ACK fallback is active. Logs
need to show which transition happened, not only that the process was running.

## Service registration and discovery

`libgal_hook.so` loads through `LD_PRELOAD`. During registration it observes
the primary video sink, creates a secondary endpoint, and registers it in GAL's
internal service routing structure. Secondary service ID `0` selects the first
free identifier after the primary service.

The hook can advertise secondary display metadata and an InputSourceService for
display ID 1. The project records that the latter, paired with AAP minor version
7, is required by the DHU two-display path. Both are firmware/protocol
assertions, not generic Android Auto settings.

`GAL_DUALSCREEN_SECOND_SINK=0` and `GAL_DUALSCREEN_INJECT_META=0` are
independent subtractive-diagnosis switches. Do not change both in one test.

### ABI guard and object construction

This is firmware-specific native work. Before modifying any GAL-owned routing,
the hook resolves the controller symbol and both relevant vtable symbols and
compares their fixed addresses with the MHI2 P4521 profile. A mismatch logs
`action=leave_stock_gal_untouched`; it is not an invitation to try the nearest
offset from another firmware.

The secondary endpoint is assembled only after the hook has seen the primary
sink's configuration sequence. It uses a separately allocated `VideoSink`
storage block and a separately allocated callback-handler block. The latter is
constructed by the target's own hidden constructor, then checked against the
callback-handler vtable. This matters because the object at the sink callback
slot is a multiply-inheriting callback handler, not a generic “video renderer”
pointer. Treating either type as the other can corrupt a live GAL process.

The hook derives the service identifier from the router's free slots instead of
hard-coding one. It advertises exactly one matching 800×480 configuration at
the requested supported frame rate. If no matching target configuration exists,
registration stops rather than advertising an invented structure.

### Why the hook observes rather than rewrites the primary path

The primary sink provides the safest runtime reference for configuration and
registration order. It is observed to find the compatible controller/router
context; the implementation does not replace the primary sink, its callback
object, or its normal data path. This preserves the strongest available
regression test: unmodified primary Android Auto must still work.

## Output modes

| Mode | Purpose | Status |
|---|---|---|
| `withhold` | Do not forward secondary `playbackStart` to GAL; forward H.264 to `stream-player`. | Production path. |
| `gal` | Give secondary playback to stock GAL's shared renderer. | Diagnostic only; can blank the center display. |
| `own` / `own-ack` | Historical private-renderer experiments. | Not normal deployment; `own-ack` falls back safely. |

The current hook header calls `withhold` the only production path. This wiki
uses the implementation as authority over older prototype notes.

## Stream transport and bootstrap

The hook listens on TCP loopback (default `127.0.0.1:12346`) and starts the
player with the matching URL. It uses non-blocking I/O, a 2 MiB socket buffer,
and `TCP_NODELAY`. TCP avoids a pathname-backed UNIX socket that an inherited
library destructor could unlink during a child-process exit.

SPS/PPS and one bounded IDR are cached. A new player client receives bootstrap
data before live delta frames. Session teardown clears state so an old IDR is
never replayed into a new phone encoder session.

### Wire and cache rules

The hook receives a GAL buffer object plus a header length, not a bare H.264
pointer. Payload extraction accounts for the buffer's base/offset/limit and
the protocol header before checking the first bytes for Annex-B framing. The
first few frames are logged with framing evidence precisely because a decoder
option cannot repair the wrong payload boundary.

The server has three bootstrap states:

| State | What may be sent to a new player | Why |
|---|---|---|
| No codec config / no IDR | Nothing useful yet | Starting at a delta frame is not valid decoder bootstrap. |
| Codec config only | Parameter sets only | The player still waits for an IDR. |
| Codec config + bounded IDR | Both are replayed before live data | A late player can initialize without waiting for a later keyframe. |

Oversize keyframes are not silently treated as cacheable. They are logged and
not retained, avoiding an unbounded allocation or a stale partial cache. On a
new playback session, all cached codec/keyframe state is cleared before the new
client is served.

### Transport is deliberately local and single-client

The listener binds `127.0.0.1`, not an external interface. It is a local IPC
boundary, not a remote video service. One player client is supported at a time;
when the client stalls or disconnects, the server drops that client cleanly and
waits for a new one. Non-blocking sends use a bounded timeout rather than
letting GAL's frame callback wait indefinitely on a child process.

## Acknowledgement semantics

There are two mechanisms:

1. `MediaSinkBase::ackFrames` acknowledges secondary media to the phone.
2. `stream-player` writes a byte to `/tmp/gal_ack.sock` after presentation; the
   hook's polling thread converts received bytes into `ackFrames` calls.

When the player has connected to the ACK socket, this is the normal
render-paced path. The hook immediately ACKs if no ACK client is connected, and
also falls back to an immediate ACK after more than 500 ms without player
feedback while frames are arriving. Disabling `GAL_DUALSCREEN_ACK` is a
diagnostic experiment that can stall the phone session. Strict
one-phone-frame-to-one-rendered-frame pacing is therefore an intended
healthy-path property, not an unconditional guarantee under failure.

### ACK state table

| ACK listener state | Hook action for secondary frames | Interpretation |
|---|---|---|
| No player ACK client | Immediate native ACK | Keep the phone session alive while player is absent. |
| ACK client connected and sending feedback | ACK once per received feedback byte | Healthy render-paced operation. |
| ACK client connected but silent for >500 ms while frames arrive | Log fallback and immediate ACK | Rendering evidence is lost, but phone flow control fails open. |
| ACK feature disabled for diagnosis | No normal paced ACK policy | Can stall/terminate the phone stream; not an operational mode. |

The byte is intentionally an acknowledgement token, not a frame number. The
design assumes ordered local stream/presentation behavior and uses it to bound
phone pressure. It does not claim a forensic one-to-one mapping after a player
crash, reconnect, or fallback transition.

## Rendering and DMDT lifecycle

`stream-player` decodes Annex-B H.264 with FFmpeg and presents through
`GL_NV_draw_texture`. It waits for decoded frames and Kombi readiness before:

```sh
dmdt dc 70 3
dmdt sc 4 70
```

`4` is the DMDT Virtual Cockpit display ID. The index printed by `dmdt gs` is
not interchangeable. On teardown, displayable `33` is restored in context
`70`; a forced-player termination has an emergency restore path.

### Player supervision and environment isolation

The hook starts one `stream-player` process when secondary playback begins. It
chooses a regular file from an ordered set of target paths (or an explicit
override), builds the player URL from the same configured stream port used by
the listener, and passes a purpose-built environment. In particular it removes
`LD_PRELOAD` from the child so the hook's destructor/interposition state cannot
be inherited accidentally by the renderer.

Stopping prefers `SIGTERM` and gives the player a short opportunity to restore
DMDT itself. If the process remains alive, the manager kills it and issues an
emergency restoration command. This is not a guarantee that every display
manager failure is recoverable; it is a last-resort attempt to return the
Cockpit to factory Displayable 33 instead of leaving a custom route active.

### Decode and presentation path

`stream-player` consumes Annex-B H.264 through FFmpeg. It uses the Tegra
GLES/OpenKODE path and `GL_NV_draw_texture` for presentation because the target
graphics stack does not provide a usable online shader-compiler route. It
switches/reasserts the Cockpit route only after its own readiness checks; a
socket connection alone must not seize context 70.

The project does not claim that FFmpeg software decode, a configured thread
count, or a single EGL success proves sustained in-car performance. Those are
implementation choices to be measured in the full primary+secondary workload.

## Focus and center-screen Exit

Primary focus stays on the stock HMI path. Secondary focus can be answered on
the secondary sink without changing the global LSD focus state that governs
center-canvas Exit. See [Engineering Reference](Engineering-Reference.md#android-auto-exit-behavior).

## Failure containment and observability

The hook favors a stock fallback over speculative recovery in the process that
owns Android Auto. Symbol/firmware mismatch stops secondary construction;
stream/ACK absence becomes a logged fail-open acknowledgement condition rather
than a deadlocked phone session; and player termination tries to restore the
factory Cockpit route.

Useful log checkpoints, in order, are:

1. `event=firmware.verify result=success` — target ABI guard passed.
2. `event=secondary.register result=success` — secondary endpoint exists.
3. `event=aap.playback ... role=secondary` — phone entered secondary playback.
4. `event=aap.frame.framing ... annex_b=yes` — extracted bytes look like video.
5. `event=stream.client result=connected` — player attached to the stream.
6. player decode/presentation evidence and `event=ack.client` — healthy
   end-to-end pacing evidence.
7. DMDT and physical Cockpit observation — visible routing evidence.

Skipping a checkpoint invites a wrong-layer fix. Use [Test Methodology](Test-Methodology.md)
to capture the complete evidence bundle and [Troubleshooting and Diagnostics](Troubleshooting-and-Diagnostics.md)
to diagnose a missing checkpoint.
