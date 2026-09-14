#!/bin/bash
set -e

MIB_IP="${1:-10.252.75.46}"
WITH_JARS=0
if [ "${2:-}" = "--with-jars" ]; then
    WITH_JARS=1
elif [ -n "${2:-}" ]; then
    echo "Usage: $0 [MIB_IP] [--with-jars]"
    exit 2
fi
SSH_OPTS="-o ConnectTimeout=5 -o PubkeyAcceptedKeyTypes=+ssh-rsa -o MACs=hmac-sha1 -o KexAlgorithms=+diffie-hellman-group1-sha1 -o HostKeyAlgorithms=+ssh-rsa -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
DIST_DIR="$(cd "$(dirname "$0")/../dist/sdcard_hook" && pwd)"

echo "==> Deploying to MIB2.5 High head unit at $MIB_IP..."

if [ ! -d "$DIST_DIR" ]; then
    echo "ERROR: Package directory $DIST_DIR does not exist. Run 'make package' first."
    exit 1
fi

echo "==> Step 1: Stopping running player/gal and remounting /fs/sdb0 read-write..."
ssh $SSH_OPTS root@"$MIB_IP" "export PATH=/proc/boot:/bin:/usr/bin:/sbin:/usr/sbin:/fs/sdb0/bin:\$PATH; slay -9 stream-player 2>/dev/null || true; slay -9 gal 2>/dev/null || true; mount -uw /fs/sdb0; mkdir -p /fs/sdb0/lib /fs/sdb0/scripts /fs/sdb0/logs 2>/dev/null || true"

echo "==> Step 2: SCP copying updated binaries and scripts to /fs/sdb0/..."
scp -O $SSH_OPTS "$DIST_DIR"/libgal_hook.so "$DIST_DIR"/stream-player* "$DIST_DIR"/*.sh "$DIST_DIR"/*.conf* "$DIST_DIR"/*.txt root@"$MIB_IP":/fs/sdb0/
if [ "$WITH_JARS" -eq 1 ]; then
    echo "==> Optional: copying Android Auto JARs..."
    scp -O $SSH_OPTS "$DIST_DIR"/*.jar root@"$MIB_IP":/fs/sdb0/
else
    echo "==> Keeping existing car/SD-card JARs (use --with-jars to update them)."
fi
scp -O $SSH_OPTS "$DIST_DIR"/lib/libdmdt_flush.so root@"$MIB_IP":/fs/sdb0/lib/libdmdt_flush.so
scp -r -O $SSH_OPTS "$DIST_DIR"/scripts/* root@"$MIB_IP":/fs/sdb0/scripts/

echo "==> Step 3: Setting permissions..."
ssh $SSH_OPTS root@"$MIB_IP" "export PATH=/proc/boot:/bin:/usr/bin:/sbin:/usr/sbin:\$PATH; chmod 755 /fs/sdb0/*.sh /fs/sdb0/scripts/*.sh /fs/sdb0/stream-player* /fs/sdb0/lib/libdmdt_flush.so 2>/dev/null || true"

echo "==> Step 4: Installing updated hook into /mnt/app via enable_hook.sh..."
ssh $SSH_OPTS root@"$MIB_IP" "cd /fs/sdb0 && ./enable_hook.sh"
if [ "$WITH_JARS" -eq 1 ]; then
    echo "==> Optional: installing updated LSD override JAR..."
    ssh $SSH_OPTS root@"$MIB_IP" "cd /fs/sdb0 && ./scripts/install_vc_mapmode_jar.sh"
fi

echo "==> Step 5: Verifying configuration on car..."
ssh $SSH_OPTS root@"$MIB_IP" "grep -E 'INSETS|CROP|DPI' /fs/sdb0/gal_dualscreen.conf"

echo ""
echo "==> SUCCESS: Deployment complete to $MIB_IP!"
echo "    Please reboot the head unit (hold power button 10s or reboot via engineering menu)."
