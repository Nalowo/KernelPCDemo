#!/usr/bin/env bash
# devtools/build.sh -- build THIS module against the QEMU kernel.
# Thin wrapper that points the project Makefile at the kernel built by setup.sh.

set -euo pipefail

DEVTOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$DEVTOOLS_DIR/.." && pwd)"
. "$DEVTOOLS_DIR/config.defaults"
[ -f "$DEVTOOLS_DIR/config.local" ] && . "$DEVTOOLS_DIR/config.local"

die() { echo "ERROR: $*" >&2; exit 1; }

[ -d "$KERNEL_BUILD" ] || die "Kernel not built. Run devtools/setup.sh first."
[ -f "$KERNEL_BUILD/Module.symvers" ] || die "Kernel not prepared. Run devtools/setup.sh first."

echo "Building module against kernel $KERNEL_VERSION ..."
make -C "$PROJECT_ROOT" KDIR="$KERNEL_BUILD" "$@"
