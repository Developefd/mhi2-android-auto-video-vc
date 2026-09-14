# Development Story: From a Second Sink to a Usable Cockpit

This is the project narrative: not a polished claim that every experiment
worked, but the sequence of constraints, false starts, tests, and corrections
that shaped the current design.

It is reconstructed from the current hook/player sources, configuration
comments, package scripts, decompilation notes, DHU observations, dated car-test
records from the related `gal_dualscreen` worktree, and the recovered private
Claude/Antigravity conversation archive. The archive contains more than twenty
thousand deduplicated messages and records both failed hypotheses and their
corrections. It is used as research provenance, not copied wholesale into this
public wiki. Each conclusion below is labeled by its evidence level so later
work can distinguish a road-tested fact from an offline inference.

## Evidence language

| Label | Meaning |
|---|---|
| Source-verified | Present in this repository's code or scripts. |
| Decompiled | Derived from inspected target binary or Java bytecode. |
| DHU-observed | Reproduced with the Android Auto Desktop Head Unit. |
| Car-observed | Recorded on the target vehicle. |
| Open | Plausible or implemented, but still needs an on-car acceptance test. |

## Chronology at a glance

| Period | Main question | Durable outcome |
|---|---|---|
| 17 Aug 2026 | What can the MHI2 target actually render and route? | Established the hardware/firmware boundary, recovery rules, and need for binary-led investigation. |
| 22 Aug 2026 | How can Android Auto be persuaded to expose a second display? | Separated service registration, display metadata, AAP version, and input-source variables. |
| 22 Aug 2026 | Where are frames and their sink-specific behaviors handled? | Mapped VideoSink callbacks, vtables, payload extraction, and the shared-renderer hazard. |
| 22 Aug 2026 | Can the protocol and H.264 path be reproduced off-car? | Built DHU/H.264 negative controls, while retaining the distinction between DHU evidence and car evidence. |
| 23 Aug 2026 onward | Can it survive real device lifecycle and Cockpit routing? | Moved toward external rendering, bootstrap caching, ACK feedback, DMDT readiness, and car-test discipline. |
| Sep 2026 | Why do geometry and Exit still fail despite a visible stream? | Identified UiConfig field semantics and the display-unqualified Java focus-state boundary. |
| Sep 2026 | What did the separate Java/HMI experiments teach us? | Kept the JVM experiment explicitly optional and separated from the native video delivery path. |

The remainder follows this chronology. Each chapter states what was believed,
what falsified or narrowed it, and what became reproducible as a result.

## How this story is documented

The project accumulated three kinds of records: target logs and car
observations, small binary/decompilation probes, and host-side DHU/capture
experiments. They answer different questions. A useful public history should
not turn them into an undifferentiated pile of screenshots, packets, or private
conversation excerpts.

| Record | It can establish | It cannot establish alone |
|---|---|---|
| Current source and package scripts | The intended implementation and its safeguards | That the target accepted it in a real session |
| Target logs and a physical Cockpit observation | What happened on this unit in that test | That another firmware or phone will behave identically |
| DHU configuration and service-discovery capture | Wire-format hypotheses and one advertised-service effect | Actual QNX/GAL, DMDT, MOST, or vehicle HMI behavior |
| Captured Annex-B stream decoded off-target | That extracted bytes form decodable H.264 and expose startup ordering | That the target renderer presented each frame |
| Decompiled call paths | A likely ABI, ownership, or state-machine boundary | A safe patch without a runtime guard and car test |

This division is deliberate. It lets a future contributor reproduce a narrow
claim without obtaining private recordings or reproducing every historical
mistake.

## 0. Before writing hooks: establish a trustworthy test boundary

The earliest conversations did not begin with “how do we draw a second screen?”
They began with a more useful question: what can be asserted about this one
firmware, and what would be dangerous to change without recovery?

The August investigation established the project’s recurring safety rules:

- MHI2/Tegra 30 is not interchangeable with MHI2Q/i.MX6.
- The target's QNX graphics and GAL layouts are private implementation detail;
  a host build or a similar firmware is not proof of target compatibility.
- System libraries under `/lib` and `/usr/lib` are not an acceptable deployment
  surface. Runtime injection must remain reversible and outside those paths.
- `smartphone_integrator` has a constrained environment array. An overfull
  array can silently discard `LD_PRELOAD`, creating a failure that looks like a
  protocol bug but is actually an installation bug.

This changed the working style for everything that followed: first make a
change observable and reversible, then make it functional. The hook log,
firmware checks, explicit environment accounting, and rollback script are all
descendants of that decision.

## 1. The original problem: one renderer, two desired displays

The practical goal was simple to describe: keep Android Auto working on the
center screen while presenting navigation video in the Virtual Cockpit. The
system is not simple. Stock Harman GAL was designed around one primary video
sink and one shared native renderer. A second video stream cannot merely be
added to `gal.json` and expected to become an independent display.

The first stable conclusion was architectural rather than cosmetic:

> The secondary stream must be visible to Android Auto without being handed to
> the center renderer that GAL already owns.

**Source-verified:** `video_sink_hook.c` intercepts service registration,
builds a second endpoint, and handles secondary media configuration without
forwarding it into the shared renderer. The log text documents why: forwarding
the secondary 800×480 configuration reconfigures the primary center path.

**Design consequence:** the normal output mode is `withhold`, not `gal`.
`gal` remains useful as a diagnostic comparison, but it is expected to damage
the center-display experience because it exercises the stock one-renderer path.

### Reproduce the boundary

1. Preserve a known-good package and configuration.
2. Run one test with `GAL_DUALSCREEN_OUTPUT=withhold`.
3. Run a separate, disposable comparison with `GAL_DUALSCREEN_OUTPUT=gal`.
4. Record primary-frame logs and the physical center display in both cases.

Do not use the comparison mode as a fix for a black Cockpit. A working primary
screen is part of the acceptance criteria.

## 2. Making the phone offer a second display

Adding a sink to GAL did not automatically make a phone send secondary video.
The project had to separate several coupled variables: service registration,
secondary display metadata, advertised Android Auto protocol minor version, and
the cluster input service.

The resulting insight was methodological:

> “No secondary video” is a negotiation failure until logs prove otherwise; it
> is not yet a decoder, socket, or DMDT problem.

**Source-verified:** configuration exposes independent switches for
`GAL_DUALSCREEN_SECOND_SINK`, `GAL_DUALSCREEN_INJECT_META`,
`GAL_DUALSCREEN_AAP_MINOR`, `GAL_DUALSCREEN_CLUSTER_INPUT`, and
`GAL_DUALSCREEN_MAIN_INPUT_ID`.

**DHU-observed / source-commented:** advertising AAP minor 7 together with the
cluster InputSourceService was required by the two-display test path. The
separate bisect switches exist because changing metadata and sink registration
together had previously made a failure impossible to attribute.

### Reproduce the negotiation test

Start from the normal profile, then change only one input per test:

```ini
GAL_DUALSCREEN_AAP_MINOR=7
GAL_DUALSCREEN_CLUSTER_INPUT=1
GAL_DUALSCREEN_MAIN_INPUT_ID=1
GAL_DUALSCREEN_SECOND_SINK=1
GAL_DUALSCREEN_INJECT_META=1
```

For each case, save `event=secondary.register`, discovery metadata, AAP setup,
media configuration, and the first primary/secondary frame logs. A failure
before secondary AAP setup cannot be diagnosed by changing FFmpeg settings.

### The DHU chapter: a reference instrument, not a substitute car

DHU was essential because it made service discovery observable and repeatable.
It was used to capture and decode the secondary-display advertisement, compare
subtractive configurations, and test which service shape made the client
advance to secondary setup. In that controlled setting, paired display/input
metadata mattered more than several initially plausible video-capability
changes.

That work also produced one of the project's most important negative controls:
a modified local `gal.json` or a DHU-specific launch issue can prevent a normal
session before the hook's video path has run. Treating that as proof that the
hook broke primary Android Auto would have sent the investigation in the wrong
direction. The test procedure therefore grew two rules:

1. Keep a stock-or-known-good comparison for any DHU failure.
2. Do not promote a DHU result to a car claim until the exact target log and
   physical display observation exist.

There were also different protocol-era experiments. Earlier captures and later
four-service symmetry work describe an AAP 1.2-shaped reference configuration;
the current native hook's tested secondary negotiation exposes AAP minor 7 and
a cluster input service. They are historical evidence about message shape, not
interchangeable runtime recipes. The current configuration reference remains
the authoritative deployable setting.

### Four-service symmetry was a discovery lesson

One later DHU-derived experiment made the advertised display/input services
symmetric across the center and cluster identities. The practical lesson was
not “always inject four services”; it was that service discovery is a linked
topology. Adding a display identity without its matching input-side identity
can create a believable-looking payload that a client still declines.

This is why the native hook keeps registration, display metadata, and cluster
input independently configurable for bisection. It is also why a trace should
record the complete discovery response rather than only the line that says a
secondary sink was registered.

## 3. Divorcing transport from the stock renderer

Once secondary frames arrived, the project needed a consumer outside GAL. The
chosen split was intentionally plain: extract Annex-B H.264 access units,
forward them over TCP loopback, and decode in a separate process.

```text
secondary VideoSink
  → hook extracts payload after GAL buffer header
  → loopback listener at 127.0.0.1:12346
  → stream-player probes Annex-B H.264
  → FFmpeg decode
  → GLES2 / GL_NV_draw_texture
```

**Source-verified:** the listener is bound only to loopback; it uses
non-blocking I/O, `TCP_NODELAY`, and 2 MiB socket buffers. The player manager
builds its URL from the configured port, rather than relying on a second,
possibly stale hard-coded port.

### Why TCP remained the baseline

A UNIX-domain video socket looked attractive because it avoids the TCP stack.
It exposed a lifecycle problem: helper children inherited the preloaded library
and could run its destructor, unlinking a pathname-backed socket that belonged
to the live server. TCP loopback has no filesystem pathname to unlink.

This was an important change in the project's reasoning: transport was no
longer treated as a throughput-only choice. It also defines process ownership,
cleanup, restart behavior, and what happens when a helper process exits.

**Open:** an AF_UNIX implementation is only safe if it explicitly solves
descriptor inheritance and destructor ownership. It must be benchmarked against
the same car scenario; it is not a mechanical replacement.

### What frame capture settled

The initial external-renderer idea had a hard prerequisite: prove that the
callback delivered usable encoded bytes rather than an opaque GAL object. A
small capture path and the non-stripped DHU counterpart were used to recover
the relevant buffer boundary: base address, offset, limit, and the callback's
header-length argument. The output was then treated as Annex-B H.264 and
decoded independently.

This was a modest but decisive experiment. It separated three questions that
had been conflated: whether a secondary callback fires, whether the payload
calculation is correct, and whether a target decoder/presenter can consume it.
It also made SPS/PPS and IDR ordering visible rather than inferred from a black
screen. Public documentation retains the method and expected observations, not
the private captures themselves.

### Reverse engineering became safer after its mistakes were recorded

Several early object-model assumptions were wrong: a controller offset was an
embedded vtable-related value rather than the expected sink pointer, and a
callback-handler type was briefly confused with an implementation type. These
were corrected with runtime logging, cross-reference tracing, and a complete
search for duplicated checks—not by trusting one appealing decompilation.

The lasting insight is methodological. On a stripped, multiple-inheritance ARM
C++ binary, an address-looking value is not yet an object identity. Every ABI
claim needs a runtime guard, and a correction must be propagated to every
parallel path before a car test. That discipline is reflected in the hook's
firmware checks and explicit event logs.

## 4. The startup problem: a decoder cannot begin with a P-frame

Early playback failures were not always rendering bugs. H.264 clients that join
midstream need SPS/PPS and a current IDR before they can decode later predicted
frames. Starting the player after frames were already flowing made startup
timing a first-class problem.

**Source-verified:** the stream server caches codec configuration and one IDR
(up to 512 KiB), sends them to a newly connected client, withholds pre-IDR delta
frames, and clears the session cache on teardown. The player manager starts the
player from secondary `playbackStart`; it does not wait for Cockpit routing.

**Car-observed in related test records:** a first client could disconnect while
FFmpeg was opening after a partial write. The later design gives cached
bootstrap data a timeout-aware delivery path and treats a timeout as a clean
reconnect boundary, rather than silently dropping a potentially referenced
frame and continuing with corrupted prediction state.

### Reproduce startup

1. Start a fresh GAL session.
2. Confirm the player connects before the first displayed Cockpit frame.
3. Capture codec-config caching, keyframe caching/replay, and the first
   Annex-B framing log.
4. Disconnect/reconnect the player without reusing the phone session.
5. Confirm the new client receives bootstrap data before delta frames.

Expected diagnostic distinctions:

| Evidence | Meaning |
|---|---|
| No cached/replayed IDR | Decoder may be waiting for a current session reset point. |
| Partial-write timeout | Client must reconnect cleanly; do not keep its prediction chain. |
| Annex-B framing failure | Payload/header extraction is wrong; player tuning will not fix it. |
| Repeated player spawn | Inspect prior session teardown or abrupt phone disconnect. |

## 5. The Cockpit is not merely another window

The player can render correctly and still not be visible. The Cockpit is routed
through DMDT and MOST150, with a context/displayable relationship separate from
the center renderer.

**Source-verified:** the player delays the visible switch until it has decoded
frames and the Kombi map is ready. It then runs:

```sh
dmdt dc 70 3
dmdt sc 4 70
```

Display ID `4` is the DMDT hardware argument for the Cockpit. Some `dmdt gs`
output presents a different index. Passing that index can be accepted without
creating the desired route, which is why shell success is insufficient evidence.

The reverse path matters equally. Clean player exit restores displayable `33`
in context `70`; forced player termination has an emergency restoration path.

### Reproduce routing

1. Confirm the player has decoded frames before running DMDT commands.
2. Record `dmdt gs` before activation.
3. Activate `dc 70 3`, then `sc 4 70`.
4. Confirm both the expected route state and the physical Cockpit output.
5. Stop the player and confirm displayable 33 is restored.

This sequence separates decoder availability, DMDT route state, and physical
MOST display output—the three observations that were previously easy to merge
into one misleading “black screen” symptom.

## 6. Geometry: the black border was not an EGL scaling bug

The map needed a usable card safe area without shrinking the 800×480 video
canvas. Several early explanations mixed up outer video margins, Android Auto
UiConfig fields, and DHU crop settings. The investigation became reproducible
only after those paths were tested separately.

**Decompiled + car-observed:** UiConfig field 1 participates in the encoded
content rectangle. Supplying values there centered the card but produced black
borders, because the phone encoded a smaller image inside the same canvas.
Field 3 preserved full-frame video but did not constrain the card as required.
Field 2 became `contentInsets`, which constrained the card while keeping the
H.264 picture full-screen.

The car-tested progression was:

| Case | Parameters | Observation | Decision |
|---|---|---|---|
| Field 1 | 40,40,127,127 | Centered card, black video border | Reject for full-frame video. |
| Field 3 | Tested inset payload | Full frame, wrong card constraint | Reject for this purpose. |
| Field 2 | 60,40,127,127; DPI 140 | Full frame, centered card | Establish baseline. |
| Field 2 | 80,80,127,127; DPI 125 | Full frame, safe area not exact center | Tune vertical clearance. |
| Field 2 | 80,100,125,125; DPI 125 | Full frame, desired safe area | Current reference profile. |

The reference exact payload is:

```text
5a0c120808501064187d207d2002
```

It represents UiConfig field 2 with `top=80`, `bottom=100`, `left=125`,
`right=125`, plus field 4 with dark theme (`2`). It is 14 bytes, which fits the
firmware's small-string unknown-field path.

### The reproducibility trap

When `GAL_SECONDARY_UI_CONFIG_HEX` exists, it wins. Editing
`GAL_SECONDARY_INSETS` or `GAL_SECONDARY_UI_THEME` without changing the hex
string does not alter the emitted bytes. Every geometry test must record both
the readable values and the exact payload, then verify `event=video.insets` in
the log.

## 7. ACK pacing: performance behavior is also failure behavior

The stock renderer normally owns media acknowledgement. In `withhold` mode it
does not, so the hook has to preserve phone flow control itself.

The evolving insight was:

> Rendering feedback is useful only if its failure does not turn into a phone
> session failure.

**Source-verified:** `stream-player` sends one byte after presentation to
`/tmp/gal_ack.sock`; the hook polls that socket and calls GAL's native
`MediaSinkBase::ackFrames` for each received byte. If the player is not
connected, or feedback remains silent for more than 500 ms while frames arrive,
the hook ACKs immediately as a failsafe.

The healthy path is render-paced. The failure path intentionally sacrifices
strict pacing to preserve the Android Auto session. Therefore measure both the
ACK client and fallback events; counting received video frames alone cannot
prove a healthy presentation loop.

## 8. Reverse gear exposed a timing boundary

The rear camera/parking transition can pause the display consumer while the
phone continues to produce video. An aggressive socket timeout converted a
short context transition into a disconnected client and a decoder restart.

**Source-verified:** live socket writes use a 250 ms `select()` allowance before
the client is dropped. This lets ordinary RVC/OPS pauses drain without holding a
dead connection forever.

**Car-observed in related drive records:** short reverse transitions could be
absorbed; longer failures still need logs and a fresh-IDR/reconnect strategy.
Do not infer a universal reverse-gear guarantee from the timeout alone.

## 9. In-app Exit revealed the Java HMI boundary

The most subtle failure was not video transport. Android Auto's in-app Exit
must leave the center canvas and display App-Connect while keeping the phone
connected and Cockpit navigation alive. Stopping primary playback, or issuing
DMDT center commands, did not recreate that HMI state.

**Decompiled:** primary focus mode 2 reaches LSD's
`RequestHandler.performVideoFocusRequestNotification(2)`. The desired
`CANVAS_LEFT` event is emitted only when a single global focus state says the
center was previously projected. The Java callback has no sink/display ID.

The old design allowed the secondary sink to affect that shared state, so a
later primary Exit could be acknowledged but have its actual HMI event
suppressed. The evidence changed the interception point:

- primary focus follows the original native path;
- valid secondary mode 1/2 is acknowledged on its own sink with native AAP
  response `0x8008`;
- secondary focus is not sent into the display-unqualified LSD state machine;
- unknown modes retain the complete stock path.

**Open:** the split is source- and package-checked, but each firmware and phone
combination still needs the full car acceptance sequence. The correct test is
not “did the renderer stop?” It is: Exit → App-Connect with Disconnect →
Cockpit continues → re-enter → explicit Disconnect tears both paths down.

## 10. The companion Java work: valuable, separate, and high risk

The native hook is only one half of the user-facing system. The MIB2 HMI also
contains a Java/ASL layer which decides mutual exclusion, canvas ownership,
steering-wheel behavior, banner policy, and how the center screen leaves
projection. That led to a companion line of experiments: improve the HMI
experience without pretending that a JAR is part of the native frame path.

The distinction matters operationally. A hook failure normally affects GAL;
an invalid bootclasspath override can affect the center HMI itself. They have
different rollback paths, different processes, and different evidence needed
for acceptance.

### The J9 compatibility lesson

The target's IBM J9 runtime accepts an older-compatible class format in the
established companion build. An experimental 15 FPS transport-rate change and
other decompiled-class edits showed why “it compiles on a modern host” is a
dangerous success criterion. Recompilation can alter verifier-relevant control
flow or introduce stack-map metadata that the target rejects. The visible
failure mode can be loss of the expected vehicle UI rather than a friendly Java
error.

The durable workflow is intentionally conservative:

1. Start from a known-good companion JAR, not from a decompiler's source as an
   assumed equivalent.
2. Make one bytecode-level behavior change.
3. Build with the established legacy-compatible toolchain and inspect every
   replacement class's major version.
4. Preserve the original JAR and a boot-safe restoration method.
5. Test boot, primary Android Auto, Cockpit output, Exit, re-entry, Disconnect,
   and reboot before treating an HMI change as a usable feature.

The 15 FPS work remains a useful performance experiment, not a dependency of
the native hook. It belongs to the companion project and must be documented
there with its own baseline, payload identity, and car result.

### Focus mirroring was the wrong abstraction

An early instinct was to make the secondary display behave like a mirror of the
primary focus sequence. It was attractive because it seemed to keep both sinks
in sync. The later Exit investigation showed the hidden assumption was wrong:
the HMI callback that maintains the relevant focus state does not identify a
sink. Mirroring a secondary event can therefore change the state that a later
primary Exit needs.

This is a general lesson for the project. “Same protocol event” does not mean
“same state-machine owner.” The secondary sink must receive the response it
needs for Android Auto flow control, but it cannot be allowed to impersonate
the primary UI in a display-unqualified HMI state machine. That is why the
current native design distinguishes local secondary acknowledgement from the
stock primary HMI route.

## 11. Lifecycle hardening: the car keeps changing underneath the process

The first functional pipeline was not yet a usable vehicle feature. Car use
introduced transitions that a desk setup scarcely represents: reverse-camera
priority, ignition off while MIB remains awake, a Cockpit splash that stops
consuming video, phone unplug/replug, and a display manager that may restore
factory content independently of the player.

These cases changed the definition of success. A displayed frame is a milestone;
a clean return to OEM content after a pause or process fault is a safety
property.

### Reverse and transient backpressure

During reverse/parking transitions, the display consumer may stop draining for
a short period. The video socket must therefore distinguish a short consumer
pause from a dead client. The bounded `select()` allowance in the current live
write path embodies that judgement. It is long enough to tolerate the observed
transition class, but bounded so a genuinely stuck client cannot retain a
session forever.

The test should not reduce this to “reverse worked once.” Capture the duration,
player log, hook stream log, ACK behavior, route state, and the first picture
after returning to Drive. If recovery depends on a new keyframe, record that as
a separate protocol/lifecycle result rather than masking it with a restart.

### Ignition and route restoration

The Cockpit can stop consuming the custom display during its own power or
splash-state transition while the head unit remains awake. A graphics call that
blocks waiting for a display refresh can then freeze a render thread even
though the Android Auto session still exists. Separately, factory routing may
reassert displayable 33 while the custom process is still alive.

The project response is intentionally defensive: avoid treating a presentation
stall as permission to keep a stale route, restore stock displayable 33 during
normal or emergency teardown, and require an ignition/reboot observation in
the acceptance checklist. The precise timeout policy remains target-specific;
the principle is not. OEM recovery must win over a frozen custom image.

### Supervision is part of the feature

Several failures came not from video code but from the seams around it:
environment-array limits that could omit `LD_PRELOAD`, child-process inheritance
that changed cleanup ownership, DMDT commands whose useful output was
TTY-sensitive, and scripts that reported a state they had not verified.

These are not peripheral housekeeping bugs. On a closed embedded system they
determine whether a test is interpretable and whether a rollback is credible.
The project consequently keeps the hook external to system library paths,
records its configuration, logs explicit lifecycle events, and treats the
installer/rollback scripts as production code that needs testing.

## 12. A ledger of rejected explanations

Keeping failed explanations visible is more valuable than it may appear. They
are the shortest way to stop a later experiment from repeating the same
plausible mistake.

| Attractive explanation | What narrowed or rejected it | Resulting practice |
|---|---|---|
| “Registering a second sink is enough.” | The phone needs compatible discovery topology and negotiation evidence. | Capture full service discovery and bisect metadata independently. |
| “DHU success proves the car will work.” | DHU cannot exercise target GAL ABI, QNX routing, MOST, or Java HMI. | Label DHU results as offline evidence only. |
| “A successful DMDT command means the Cockpit changed.” | A wrong display identifier can return without the desired visible route. | Check route state and physical output. |
| “A black Cockpit means decoding failed.” | It may be discovery, payload extraction, IDR bootstrap, DMDT, or rendering. | Locate the failure by layer before tuning the decoder. |
| “Forwarding secondary playback is the simplest integration.” | The shared renderer can reconfigure/blank the center path. | Withhold secondary playback from stock GAL and render externally. |
| “No secondary renderer means no acknowledgement is needed.” | The phone ends a stream when frames are not acknowledged. | Tie ACKs to healthy presentation and fail open on player failure. |
| “Insets are just margins.” | Different UiConfig fields affect encoded crop, safe area, and theme differently. | Record exact bytes and physical visual result. |
| “Secondary focus can mirror primary focus.” | Global HMI focus state lacks a display discriminator. | Keep primary HMI route stock; locally acknowledge valid secondary focus. |
| “A host-valid Java build is a target-valid JAR.” | IBM J9 verification and old class compatibility are separate constraints. | Build/inspect with the established toolchain and retain a recovery JAR. |
| “A fast local socket is automatically the better transport.” | Pathname cleanup and inherited-destructor ownership broke lifecycle assumptions. | Prefer the simpler TCP lifecycle until AF_UNIX ownership is engineered. |

## 13. Reproducing a claim, not merely a build

A reproducible project record needs more than source files. A future test case
should be understandable even when the original phone, car, and conversation
archive are unavailable. The minimum evidence bundle is:

| Item | Why it belongs in the record |
|---|---|
| Git revision and package checksums | Identifies the exact hook/player/scripts under test. |
| Firmware/train identity and GAL binary identity | Establishes whether the ABI guard is meaningful. |
| Full runtime configuration, including exact UiConfig hex | Makes protocol and geometry inputs repeatable. |
| Phone model, Android Auto version, and connection route | Makes client behavior attributable. |
| Hook/player logs covering startup through teardown | Preserves layer-by-layer evidence. |
| DMDT before/after observations plus center/Cockpit outcome | Separates route state from visual state. |
| Explicit negative result and rollback observation | Makes failures useful and proves recovery. |

Use the case template in [Test Methodology](Test-Methodology.md). The public
wiki intentionally records the method and conclusions, not raw personal
messages, private captures, vehicle identifiers, or large proprietary binary
material.

## 14. What the story changes about future work

The project should now be developed as a chain of narrow, testable claims:

1. Establish the exact firmware, package revision, phone, and configuration.
2. Choose one layer: discovery, sink behavior, transport, decoder, DMDT,
   geometry, ACK, or HMI.
3. State the expected evidence before changing code.
4. Change one variable, reboot when state could persist, and capture logs.
5. Classify the result as source, offline, log, visual, or road evidence.
6. Preserve rejected hypotheses. They prevent the same attractive mistake from
   becoming “common knowledge” again.

The current implementation is not the end of the story. Its value is that the
next question has a narrower boundary than the last one.
