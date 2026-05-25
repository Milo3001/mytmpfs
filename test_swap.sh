#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MODULE_NAME=my_tmpfs
MOUNT_POINT=/mnt/tmpfs
TEST_FILE=$MOUNT_POINT/swap_test.bin
PAGES=${PAGES:-2048}

if [[ ${EUID:-$(id -u)} -eq 0 ]]; then
    SUDO=""
elif command -v sudo >/dev/null 2>&1; then
    SUDO=sudo
else
    echo "Run this script as root or install sudo." >&2
    exit 1
fi

cleanup() {
    if mountpoint -q "$MOUNT_POINT"; then
        $SUDO umount "$MOUNT_POINT" || $SUDO umount -l "$MOUNT_POINT" 2>/dev/null || true
    fi
    if lsmod | awk '{print $1}' | grep -qx "$MODULE_NAME"; then
        $SUDO rmmod "$MODULE_NAME" || true
    fi
}

trap cleanup EXIT

echo "Building module and test program..."
make -C "$SCRIPT_DIR"
gcc -O2 -o "$SCRIPT_DIR/test_swap" "$SCRIPT_DIR/test_swap.c"

echo "Loading module..."
if lsmod | awk '{print $1}' | grep -qx "$MODULE_NAME"; then
    $SUDO rmmod "$MODULE_NAME"
fi
$SUDO insmod "$SCRIPT_DIR/my_tmpfs.ko"

echo "Preparing mount point $MOUNT_POINT..."
$SUDO mkdir -p "$MOUNT_POINT" || true
$SUDO mount -t my_tmpfs none "$MOUNT_POINT"

echo "Writing test file ($PAGES pages)..."
WRITE_CKSUM=$("$SCRIPT_DIR/test_swap" write "$TEST_FILE" "$PAGES")
echo "Wrote checksum: $WRITE_CKSUM"

echo "Forcing page eviction by dropping caches (requires root)..."
$SUDO sh -c 'sync; echo 3 > /proc/sys/vm/drop_caches'

echo "Reading back test file and computing checksum..."
READ_CKSUM=$("$SCRIPT_DIR/test_swap" read "$TEST_FILE" "$PAGES")
echo "Read checksum:  $READ_CKSUM"

if [[ "$WRITE_CKSUM" == "$READ_CKSUM" ]]; then
    echo "✓ swap integrity test passed"
    exit 0
else
    echo "✗ checksum mismatch: wrote $WRITE_CKSUM read $READ_CKSUM" >&2
    exit 1
fi
