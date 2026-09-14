# mhi2-android-auto-video-vc

This wiki documents the experimental dual-screen Android Auto video path for
Harman MHI2.5 High units. It is written for the verified Volkswagen profile
`MHI2_ER_VWG13_P4521_MU1367`; other firmware must be treated as unverified
until its symbols, offsets, and display routing have been checked.

## Start here

1. Read [Compatibility Matrix](Compatibility-Matrix.md).
2. Read [Installation and Safety Guide](Installation-and-Safety-Guide.md).
3. Follow [Build Environment](Build-Environment.md) to produce the native hook
   and `stream-player`.
4. Use [Architecture Deep Dive](Architecture-Deep-Dive.md) for the native path.
5. Read [Development Story](Development-Story.md) for the chronological
   hypothesis → test → evidence → design-change narrative.
6. Use [Engineering Reference](Engineering-Reference.md) for current geometry,
   lifecycle, and Exit-flow behavior.
7. Use [Test Methodology](Test-Methodology.md) before changing protocol,
   geometry, focus, or HMI behavior.
8. Read [Design Decisions and Lessons](Design-Decisions.md) for the important
   non-obvious constraints behind the implementation.
9. Consult [Configuration Reference](Configuration-Reference.md) before editing
   a runtime key.
10. Run [Troubleshooting and Diagnostics](Troubleshooting-and-Diagnostics.md)
   before changing code or firmware.

## System boundary

```text
Android phone → Android Auto USB → MIB2 gal → libgal_hook.so
                                      │
                                      └→ TCP loopback → stream-player
                                          ├→ MOST150 / Displayable 3 / Context 70
                                          └← /tmp/gal_ack.sock (paced ACK)
```

The native project owns the GAL hook, secondary sink, transport, backpressure,
renderer, and player supervision. The Java HMI layer is separate; see
[Companion HMI Integration](Companion-HMI-Integration.md).

## Safety

Keep a verified recovery path and backups before deployment. Never replace
files in `/lib` or `/usr/lib`. Custom libraries belong under `/mnt/app/eso/`,
and the supervisor environment array must remain within the MIB2 limit of ten
entries.

## Related work

Thanks to these related projects and their maintainers:

- [VcMOSTRenderMqb](https://github.com/OneB1t/VcMOSTRenderMqb) — MOST150
  and Tegra/OpenKODE rendering foundation.
- [MHI2_navignore](https://github.com/harman-f/MHI2_navignore) — Java HMI
  mutual-exclusion workaround.
- [mib2-android-auto-vc](https://github.com/adi961/mib2-android-auto-vc)
  — companion HMI patch.
- [MIB SDK](https://gitlab.com/andrewleech/mibsdk) — QNX cross-compilation
  toolchain.
