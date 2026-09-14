# Compatibility Matrix

## Verified target

| Item | Verified profile |
|---|---|
| Head unit | Harman MIB2.5 High / MHI2 |
| SoC | Nvidia Tegra 30 (T30) |
| OS | QNX Neutrino 6.5.0 SP1 |
| Firmware | `MHI2_ER_VWG13_P4521_MU1367` |
| Vehicle test bed | Volkswagen Golf Mk7.5 with Virtual Cockpit |
| Cluster video path | 800×480 coded video, MOST150, display 4, context 70 |

The project hooks private C++ objects and firmware-specific virtual methods.
“MHI2” is not itself a compatibility promise. Rebuild and revalidate every
symbol, layout assumption, service registration path, and DMDT route before
using another firmware train.

## Explicitly unsupported or unknown

| Target | Status | Why |
|---|---|---|
| MHI2Q / i.MX6 | Unsupported | Different SoC, graphics stack, binary layout, and renderer. |
| Other MHI2 firmware | Unknown | ABI and protocol assumptions may differ. |
| Other VAG brands | Unknown | Native path may resemble MHI2, but HMI/JAR integration is brand-specific. |
| No Virtual Cockpit | Unsupported | No validated secondary MOST display route. |

## Important constraints

- The hook enforces 800×480; it is not a general display-resolution setting.
- `30` FPS is the advertised Android Auto rate, not a guaranteed physical
  presentation rate on every car or phone.
- Center and cluster displays have different lifecycle owners. A working center
  session does not prove that the cluster route is valid.
- Java HMI additions are optional to the native renderer but can be needed for
  the desired navigation and input experience.

## Before claiming compatibility

Record firmware, GAL build identity, `dmdt gs` output, hook log, phone model,
Android Auto version, and the exact deployment/config package. Then run the
full [acceptance sequence](Engineering-Reference.md#acceptance-sequence).
