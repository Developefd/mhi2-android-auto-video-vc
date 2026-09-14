# Test Methodology and Evidence Levels

## Why the test protocol is strict

This project crosses phone protocol negotiation, private GAL ABI, QNX process
supervision, FFmpeg decode, GPU presentation, DMDT routing, MOST transport, and
Java HMI state. A successful log line validates one layer only. It must not be
used to infer success in every downstream layer.

## Evidence levels

| Level | Meaning | Example |
|---|---|---|
| Source-inspected | The current code contains a path. | Hook creates the TCP listener. |
| Offline-reproduced | Built or tested outside the car. | DHU negotiates a second display. |
| Log-observed | The target emitted an expected event. | Secondary service registration logged. |
| Visually verified | The intended cockpit/center result was observed. | Full-frame map shown in the cluster. |
| Road-validated | Result survived a controlled drive and lifecycle changes. | Map remains correct across Exit/re-entry. |

Only the last level supports an operational claim. Record negative outcomes as
carefully as positive ones; they often isolate the real compatibility boundary.

## One-variable procedure

1. Start from a known package and record its Git revision/checksum.
2. Change one variable: protocol metadata, focus behavior, geometry, player,
   or HMI layer—not several at once.
3. Reboot GAL/head unit as required so old process state cannot contaminate the
   result.
4. Use the same phone, route, Android Auto state, and observation checklist.
5. Save logs before the next case.
6. Return to the last known-good package before starting a new branch.

## Geometry testing

The coded canvas is fixed at 800×480. Outer margins affect the encoded content
rectangle; UiConfig field 2 affects UI `contentInsets`. These are not
interchangeable. The current tested profile uses zero outer margins and field-2
insets, keeping the video full-frame while constraining cards.

When `GAL_SECONDARY_UI_CONFIG_HEX` is set, it wins over scalar inset/theme
settings. Update the hex string whenever you change an inset or theme, or remove
the hex override deliberately to test the structured builder.

## Required capture set

- Firmware/train identity and GAL binary identity.
- Full deployment package hash and configuration file.
- Supervisor environment entries after installation.
- Hook and player logs from the complete session.
- Phone model, Android Auto version, and route state.
- Center and cluster observations for start, steady state, Exit, re-entry,
  disconnect, reverse-camera interaction where relevant, and a post-test reboot.

## Reproducible case record

Use one record for each hypothesis. It may be a Markdown file beside a test
package, an issue, or a private lab notebook; the important part is that the
inputs and observations survive longer than the current session.

```text
Case ID / date:
Question:
Claim level sought: source-inspected | offline-reproduced | log-observed |
                    visually verified | road-validated

Target:
  Firmware/train and GAL binary identity:
  Hook/player Git revision and package checksums:
  Phone model / Android Auto version / USB route:
  Companion HMI JAR identity, if installed:

One changed variable:
  Previous known-good value:
  New value:
  Exact UiConfig hex, if geometry is involved:

Expected evidence:
  Protocol/GAL:
  Player/decoder:
  DMDT/MOST:
  Center HMI:

Observed evidence:
  Relevant hook and player log timestamps:
  DMDT state before/after:
  Center result:
  Cockpit result:
  Reverse/Exit/re-entry/disconnect/reboot result, if in scope:

Conclusion:
Rollback performed and result:
Next single variable:
```

Avoid writing “worked” as the conclusion. State which layer worked, for how
long, and what observation supports it. A test that proves the player decoded
the stream but did not prove a Cockpit route is still a useful result.

## DHU and capture experiments

DHU, packet/service-discovery captures, and off-target H.264 decode are useful
reference instruments. Record their version, configuration, input capture, and
the exact narrowed question. Do not use them as a substitute for vehicle
acceptance: they cannot validate the target GAL ABI, DMDT/MOST route, Cockpit
power state, or the Volkswagen Java HMI transition.

For a stream capture, retain only the minimum needed for the result where
possible: capture duration/byte count, detected SPS/PPS/IDR ordering, decoder
outcome, and a checksum or private retention location. Do not publish private
conversations, vehicle identifiers, or large proprietary payloads merely to
make a test feel documented.
