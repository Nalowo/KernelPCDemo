#!/usr/bin/env bash
# devtools/test.sh -- boot QEMU, insmod the built module, dump dmesg, rmmod, exit.
# Exits non-zero if insmod/rmmod fail, so it is usable in CI.

set -euo pipefail

DEVTOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$DEVTOOLS_DIR/.." && pwd)"
. "$DEVTOOLS_DIR/config.defaults"
[ -f "$DEVTOOLS_DIR/config.local" ] && . "$DEVTOOLS_DIR/config.local"

die() { echo "ERROR: $*" >&2; exit 1; }

KO=$(ls "$PROJECT_ROOT"/build/*.ko 2>/dev/null | head -1) || true
[ -n "${KO:-}" ] || die "No .ko in build/. Run devtools/build.sh first."
KOFILE=$(basename "$KO")
# Module name = filename without .ko, with '-' turned into '_' (kernel rule).
MOD=$(basename "$KOFILE" .ko | tr '-' '_')

CMD="insmod $GUEST_MNT/build/$KOFILE && dmesg | tail -n 20 && rmmod $MOD && echo TEST_OK"
exec "$DEVTOOLS_DIR/boot.sh" --test "$CMD"
