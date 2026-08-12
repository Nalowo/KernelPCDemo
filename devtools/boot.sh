#!/usr/bin/env bash
# devtools/boot.sh -- boot QEMU with the dev kernel and initramfs.
# The project root is shared into the guest via 9p at $GUEST_MNT (default /mnt/host).
#
# Usage:
#   devtools/boot.sh              # interactive shell
#   devtools/boot.sh --gdb        # start paused, wait for GDB on :PORT
#   devtools/boot.sh --test CMD   # run CMD in guest, exit with its status
#   devtools/boot.sh -- EXTRA...  # pass extra args straight to QEMU

set -euo pipefail

DEVTOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$DEVTOOLS_DIR/.." && pwd)"
. "$DEVTOOLS_DIR/config.defaults"
[ -f "$DEVTOOLS_DIR/config.local" ] && . "$DEVTOOLS_DIR/config.local"

die() { echo "ERROR: $*" >&2; exit 1; }

GDB_MODE=0
TEST_CMD=""
EXTRA_QEMU_ARGS=()
while [ $# -gt 0 ]; do
	case "$1" in
		--gdb)  GDB_MODE=1; shift ;;
		--test) shift; TEST_CMD="${1:?--test requires a command}"; shift ;;
		--)     shift; EXTRA_QEMU_ARGS=("$@"); break ;;
		-h|--help) echo "Usage: $0 [--gdb] [--test CMD] [-- QEMU_ARGS...]"; exit 0 ;;
		*)      die "Unknown option: $1" ;;
	esac
done

BZIMAGE="$KERNEL_BUILD/arch/x86/boot/bzImage"
[ -f "$BZIMAGE" ] || die "Kernel not built. Run devtools/setup.sh first."
[ -f "$INITRAMFS_CPIO" ] || die "Initramfs not built. Run devtools/setup.sh first."
command -v "$QEMU_BIN" >/dev/null 2>&1 || die "$QEMU_BIN not found. Install QEMU."

# nokaslr -> stable kernel addresses for GDB breakpoints
KCMD="console=ttyS0 loglevel=7 nokaslr"
if [ -n "$TEST_CMD" ]; then
	KCMD="$KCMD kmod.cmd64=$(printf '%s' "$TEST_CMD" | base64 | tr -d '\n')"
fi

QEMU_ARGS=(
	-kernel "$BZIMAGE"
	-initrd "$INITRAMFS_CPIO"
	-nographic
	-m "$QEMU_MEM"
	-smp "$QEMU_SMP"
	-no-reboot
	-virtfs "local,id=$MOUNT_TAG,path=$PROJECT_ROOT,security_model=none,mount_tag=$MOUNT_TAG"
	-append "$KCMD"
)

# KVM acceleration when available (present and writable, e.g. WSL2 with nested virt).
# Falls back to TCG software emulation otherwise.
if [ -w /dev/kvm ] 2>/dev/null; then
	QEMU_ARGS+=(-enable-kvm -cpu host)
fi

if [ "$GDB_MODE" -eq 1 ]; then
	QEMU_ARGS+=(-gdb "tcp::$QEMU_GDB_PORT" -S)
	echo "QEMU paused, waiting for GDB on localhost:$QEMU_GDB_PORT"
	echo "Attach from another terminal:  make gdb-attach   (or devtools/gdb.sh)"
fi

QEMU_ARGS+=("${EXTRA_QEMU_ARGS[@]+"${EXTRA_QEMU_ARGS[@]}"}")

if [ -n "$TEST_CMD" ]; then
	exec "$QEMU_BIN" "${QEMU_ARGS[@]}" < /dev/null
else
	exec "$QEMU_BIN" "${QEMU_ARGS[@]}"
fi
