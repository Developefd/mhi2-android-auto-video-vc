# Installation and Safety Guide

## Non-negotiable rules

1. Keep a recovery method and an untouched copy of
   `smartphone_integrator.json` before changing anything.
2. Never replace or edit `/lib` or `/usr/lib`.
3. Install custom preload libraries only under the managed `/mnt/app/eso/`
   location used by the scripts.
4. Keep the supervisor `envs` array at ten entries or fewer. Exceeding the
   limit can cause the entire array, including `LD_PRELOAD`, to be ignored.
5. Preserve LF line endings for every shell script deployed to QNX.
6. Change one protocol, geometry, or HMI variable per car test.

## Prepare deployment media

Place the built hook, `stream-player`, scripts, `libdmdt_flush.so`, and a
reviewed `gal_dualscreen.conf` in the package layout expected by the installer.
The example configuration documents lookup order and defaults. The active file
is normally `/fs/sdb0/gal_dualscreen.conf`, with a `/tmp` fallback or an
explicit `GAL_HOOK_CONF` override.

Before installation, preserve the exact payload and record source revision,
firmware, phone, Android Auto version, and intended test case.

## Install

On the head unit, run the packaged installer from the SD-card root:

```sh
cd /fs/sdb0
sh ./enable_hook.sh
```

The script makes a backup, installs the preload target onto persistent app
storage, validates the edited supervisor configuration, and restores mounts to
their safe state. Re-running it reconstructs the injection from the original
backup rather than accumulating duplicate environment entries.

Use command-line overrides only as recorded test variables. They consume
supervisor entries and take precedence over the configuration file.

## Verify, reboot, and test

```sh
sh /fs/sdb0/scripts/hook_status.sh
```

Review its log evidence rather than treating `[PASS]` as visual proof. Reboot,
connect a phone, and perform the full [acceptance sequence](Engineering-Reference.md#acceptance-sequence): center display, cluster, in-app Exit, re-entry,
and explicit disconnect are separate checks.

## Roll back

Run the matching `disable_hook.sh` from the SD-card root. It restores the saved
supervisor configuration, removes the preload payload, attempts to restore the
Virtual Cockpit route to displayable 33, and remounts storage read-only. If the
DMDT restore reports failure, inspect the route before calling the unit fully
restored.

## JAR changes are a separate risk boundary

Do not bundle native-hook debugging with Java HMI changes. A bad Java payload
can remove the infotainment UI even when native installation is correct. See
[Companion HMI Integration](Companion-HMI-Integration.md).
