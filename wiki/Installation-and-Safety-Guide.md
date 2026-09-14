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

The installer expects this layout on the SD card, shown here with `/fs/sdb0` as
the card root:

```text
/fs/sdb0/
├── enable_hook.sh          from scripts/
├── disable_hook.sh         from scripts/
├── libgal_hook.so          built by make hook
├── stream-player           built by player/Makefile
├── config.txt              from player/
├── gal_dualscreen.conf     reviewed copy of scripts/gal_dualscreen.conf.example
├── lib/
│   └── libdmdt_flush.so    built by make hook
└── scripts/
    ├── hook_status.sh
    └── lib_app_mount.sh
```

`enable_hook.sh` creates `backups/` and `logs/` beside itself. The example
configuration documents lookup order and defaults. The active file is normally
`/fs/sdb0/gal_dualscreen.conf`, with a `/tmp` fallback or an explicit
`GAL_HOOK_CONF` override.

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

## Where the libraries go

| Library | On the SD card | Installed by `enable_hook.sh` to | Loaded by |
|---|---|---|---|
| `libgal_hook.so` | `libgal_hook.so` | `/mnt/app/eso/lib/gal_dualscreen/libgal_hook.so` | GAL, through `LD_PRELOAD=/eso/lib/gal_dualscreen/libgal_hook.so` in `smartphone_integrator.json` |
| `libdmdt_flush.so` | `lib/libdmdt_flush.so` | `/mnt/app/eso/lib/gal_dualscreen/libdmdt_flush.so` and `/mnt/app/eso/lib/libdmdt_flush.so` | `stream-player`, only into the `dmdt gs` commands it runs |

`/eso` resolves to `/mnt/app/eso`, so `/eso/lib/gal_dualscreen/` and
`/mnt/app/eso/lib/gal_dualscreen/` are the same directory. The installer also
copies `stream-player` and `config.txt` to `/mnt/app/navigation/`.

`stream-player` runs `dmdt gs` with `LD_PRELOAD` listing `libdmdt_flush.so`
under `/eso/lib/gal_dualscreen/`, `/mnt/app/eso/lib/gal_dualscreen/`,
`/mnt/app/eso/lib/`, `/fs/sdb0/lib/` and `/fs/sda0/lib/`. The loader skips
entries that do not exist without reporting it. If the library is in none of
them, installation still succeeds and `enable_hook.sh` only omits its
"Installed dmdt flush interposer" line, but the Cockpit is never switched:
`dmdt` exits without flushing its output into the pipe, so the player's output
keeps repeating `dmdt: display switch deferred; Kombi map is not ready`.

## Verify, reboot, and test

```sh
sh /fs/sdb0/scripts/hook_status.sh
```

Review its log evidence rather than treating `[PASS]` as visual proof. Reboot,
connect a phone, and perform the full [acceptance sequence](Engineering-Reference.md#acceptance-sequence): center display, cluster, in-app Exit, re-entry,
and explicit disconnect are separate checks.

## Roll back

Run the matching `disable_hook.sh` from the SD-card root. It restores the saved
supervisor configuration, deletes `/mnt/app/eso/lib/gal_dualscreen/` and
`/mnt/app/eso/lib/libdmdt_flush.so`, attempts to restore the Virtual Cockpit
route to displayable 33, and remounts storage read-only. It leaves
`/mnt/app/navigation/stream-player` and its `config.txt` in place. If the DMDT
restore reports failure, inspect the route before calling the unit fully
restored.

## JAR changes are a separate risk boundary

Do not bundle native-hook debugging with Java HMI changes. A bad Java payload
can remove the infotainment UI even when native installation is correct. See
[Companion HMI Integration](Companion-HMI-Integration.md).
