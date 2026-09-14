# Troubleshooting and Diagnostics

## Start with evidence

Run the status helper from the deployment media:

```sh
sh /fs/sdb0/scripts/hook_status.sh
```

It resolves the hook log from SD-card and `/tmp` candidates, prints key events,
and checks that the hook reported startup, ABI verification, primary/secondary
registration, discovery metadata, AAP setup, and cluster routing. Those checks
prove that a log event occurred; they are not visual proof on the cluster.

## Diagnostic order

1. Confirm firmware and package revision match the test record.
2. Confirm `event=init` reports `enabled=1` and ABI verification passed.
3. Confirm primary registration before diagnosing secondary registration.
4. Confirm secondary registration and discovery metadata before blaming video
   transport or rendering.
5. Confirm `event=stream.open` and player spawn/connection before debugging
   FFmpeg or DMDT.
6. Inspect DMDT activation and the physical cluster display.
7. Test Exit, re-entry, and disconnect separately from first-frame display.

## Common findings

| Symptom | Evidence to seek | Next action |
|---|---|---|
| No hook log | Missing `event=init` | Inspect preload path and supervisor environment-entry count. |
| No secondary session | No secondary registration, discovery, or AAP setup | Use one bisect switch at a time; verify AAP minor and cluster-input settings. |
| Center screen blanks | Output mode is `gal` | Return to `withhold`; stock GAL has one shared renderer. |
| No player connection | Stream opens but no player event | Check player path, artifact type, port config, and inherited environment. |
| Stock/black cluster | Player starts but no route activation | Check Kombi readiness and DMDT display 4/context 70/displayable 3. |
| Cockpit never switches; player output keeps repeating `dmdt: display switch deferred; Kombi map is not ready` | No `libdmdt_flush.so` in `/mnt/app/eso/lib/gal_dualscreen/`, `/mnt/app/eso/lib/` or the card's `lib/`, and `enable_hook.sh` printed no "Installed dmdt flush interposer" line | Put the library at `lib/libdmdt_flush.so` on the card and re-run `enable_hook.sh`; see [Where the libraries go](Installation-and-Safety-Guide.md#where-the-libraries-go). |
| Player appears hung | ACK/frame silence and supervisor events | Preserve logs, then use supervisor or rollback to restore the route. |
| Phone stream stops | Frame ACK disabled or protocol changed | Restore default ACK behavior before performance experiments. |
| Exit leaves wrong UI | Stream stops but HMI event is absent | Treat as focus/LSD state problem; DMDT alone cannot fix it. |

## Safe log collection

Copy the complete hook log before rebooting or changing configuration. Retain
player output where possible. Every report should include firmware, package
checksum, complete config, injected environment values, phone/Android Auto
version, exact actions, and observed screen state.

## Performance claims

Do not carry old FPS, CPU, or “zero drop” figures forward as release guarantees.
They apply only to the firmware, phone, content, temperature, route, and build
that produced them. Measure and attach logs for the actual build under test.
