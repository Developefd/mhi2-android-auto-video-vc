# Companion HMI Integration

## Scope

The native hook can receive and render the secondary stream, but it does not
replace Volkswagen's Java HMI policy. HMI patches affect navigation
mutual-exclusion rules, steering-wheel input, center-canvas transitions, and
overlay behavior. Keep their deployment and test record separate from the
native payload.

## Baseline and optional components

| Component | Purpose | Boundary |
|---|---|---|
| `MHI2_navignore` | Relaxes stock navigation mutual exclusion. | Baseline HMI change often paired with the native route. |
| `VCAndroidAuto_mapmode.jar` | Optional MFL zoom, D-pad routing, and duplicate-banner behavior. | VW Java/ASL-specific. |
| Native hook | Secondary service, streaming, player supervision, cluster route. | Does not make Java HMI changes. |

## Focus and Exit model

Android Auto in-app Exit is not a device disconnect. The desired result is that
the center display moves to App-Connect while Disconnect remains available and
the phone session plus cluster video stay active. The center transition is
driven by the Java HMI event chain, not merely by stopping a renderer or issuing
DMDT commands.

The dual-sink design makes this fragile because the Java callback does not carry
a display identifier. Current native design keeps primary focus on the stock
path and avoids letting secondary focus overwrite the global LSD focus state.
Every related hook or JAR change needs the on-car Exit/re-entry test.

## Java build compatibility

The target uses IBM J9-era class compatibility. Use the established IBM Java
toolchain and match the existing class-file level (major version 46 / `0x2e`)
for replacement classes. A modern `javac` build that succeeds on the host is
not evidence that the head unit can verify it.

Verify JAR contents, class version, and replacement class names before
installation. If the legacy compiler needs executable-stack repair on a modern
Linux host, repair only its development copy; never modify target binaries.
Keep a stock JAR restoration path available.

## Acceptance checks for HMI changes

1. Boot to normal center UI before connecting a phone.
2. Start Android Auto and verify primary and cluster behavior.
3. Exercise steering-wheel controls only if the optional JAR is installed.
4. Use in-app Exit, then re-enter Android Auto.
5. Use App-Connect Disconnect and verify both video paths terminate.
6. Reboot again and confirm the stock UI starts cleanly.
