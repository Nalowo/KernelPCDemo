#!/usr/bin/env bash
# devtools/gdb.sh -- attach GDB to a guest started with boot.sh --gdb.
# Breaks at do_init_module (the common entry for any module's init), so the
# debugger stops the moment you insmod inside the guest.

set -euo pipefail

DEVTOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$DEVTOOLS_DIR/config.defaults"
[ -f "$DEVTOOLS_DIR/config.local" ] && . "$DEVTOOLS_DIR/config.local"

die() { echo "ERROR: $*" >&2; exit 1; }

VMLINUX="$KERNEL_BUILD/vmlinux"
[ -f "$VMLINUX" ] || die "vmlinux not found. Run devtools/setup.sh first."
command -v gdb >/dev/null 2>&1 || die "gdb not found. Install gdb."

# Optional TUI (source/regs panes): `TUI=1 make gdb-attach`. Off by default --
# TUI + `target remote` + lx-symbols output tends to garble the curses panes
# (Ctrl-L redraws), and there is no source at the reset vector / in idle. You
# can also toggle TUI live in any session with Ctrl-X A.
GDB_TUI_ARGS=()
if [ "${TUI:-0}" = "1" ]; then
	GDB_TUI_ARGS=(-ex "set pagination off" -ex "tui enable" -ex "layout src")
fi

# -iex runs BEFORE gdb loads vmlinux, so auto-load safe-path is in effect when
# gdb auto-sources the adjacent vmlinux-gdb.py (otherwise it declines it and
# lx-symbols is unavailable). Breaks at do_init_module -- the common entry for
# any module's init -- then continues, so it stops on the next insmod.
exec gdb -iex "set auto-load safe-path /" "$VMLINUX" \
	-ex "set architecture i386:x86-64" \
	"${GDB_TUI_ARGS[@]}" \
	-ex "target remote :$QEMU_GDB_PORT" \
	-ex "break do_init_module" \
	-ex "continue"
