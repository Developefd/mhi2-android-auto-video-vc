# Shared by enable_hook.sh and disable_hook.sh: where the preload target
# lives on the internal /mnt/app partition, and the mount-guard cleanup
# for it.
#
# Under /eso/lib specifically, not /navigation: this firmware's own
# smartphone_integrator.json already has a "mirrorlink" child using
# LD_PRELOAD=/eso/lib/libsystemtime_hack.so, direct proof (not just
# analogy) that this exact framework relies on LD_PRELOAD from this exact
# path working reliably for a supervised child. /navigation was considered
# and rejected: it is also on /mnt/app and would likely work the same way,
# but it holds map/POI data content that a future navigation update could
# resync or clean, silently deleting an unrelated subdirectory placed
# inside it. /eso/lib is vendor framework code, not managed content, and
# is not going to be swept by anything that isn't a full firmware update.
APP_MOUNT=/mnt/app
PRELOAD_WRITE_DIR="$APP_MOUNT/eso/lib/gal_dualscreen"
PRELOAD_SO_ALIAS=/eso/lib/gal_dualscreen/libgal_hook.so
PRELOAD_SO_REAL="$PRELOAD_WRITE_DIR/libgal_hook.so"
APP_WRITABLE=0

# Callers should call this from their own finish()/trap handler, after
# handling whatever else that script's finish() is responsible for (e.g.
# SYSTEM_MOUNT cleanup, which is script-specific and not handled here).
app_mount_finish()
{
    if [ "$APP_WRITABLE" -eq 1 ]; then
        # Do not swallow a failed remount. This runs from a trap, so the
        # exit status goes nowhere -- but leaving the internal partition
        # writable is not a cosmetic problem on a device that loses power
        # by having its ignition switched off, and the operator is the
        # only one who can act on it. The flag is cleared only on success,
        # so a caller that runs this twice will try again.
        if mount -ur "$APP_MOUNT" 2>/dev/null; then
            APP_WRITABLE=0
        else
            echo "WARNING: could not remount $APP_MOUNT read-only." >&2
            echo "         It is still writable. Run:  mount -ur $APP_MOUNT" >&2
            echo "         before switching the car off." >&2
        fi
    fi
}
