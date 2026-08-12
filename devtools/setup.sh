#!/usr/bin/env bash
# devtools/setup.sh -- one-time setup for the QEMU dev/debug environment.
# Downloads kernel source + busybox, builds a minimal kernel with debug info,
# and packs a BusyBox initramfs. Each phase is idempotent: it is skipped when
# its output already exists and the relevant config has not changed.
#
# Must run on Linux (native, or a Linux env such as WSL2 / Docker / Lima).

set -euo pipefail

DEVTOOLS_DIR="$(cd "$(dirname "$0")" && pwd)"
. "$DEVTOOLS_DIR/config.defaults"
[ -f "$DEVTOOLS_DIR/config.local" ] && . "$DEVTOOLS_DIR/config.local"

NPROC=$(nproc 2>/dev/null || echo 4)

die() { echo "ERROR: $*" >&2; exit 1; }

stamp_hash() { cat "$@" 2>/dev/null | sha256sum | cut -c1-16; }
stamp_fresh() { [ -f "$1" ] && [ "$(cat "$1")" = "$2" ]; }

check_host() {
	[ "$(uname)" = "Linux" ] || die "Kernel and busybox must be built on Linux."
	for cmd in make gcc bc flex bison cpio curl; do
		command -v "$cmd" >/dev/null 2>&1 || \
			die "$cmd not found. Install prerequisites (see README)."
	done
}

# --- Phase 1: kernel source ---
download_kernel() {
	if [ -d "$KERNEL_SRC" ]; then
		echo "[1/3] Kernel source present (skip): $KERNEL_SRC"
		return
	fi
	echo "[1/3] Downloading kernel $KERNEL_VERSION ..."
	mkdir -p "$CACHE_DIR"
	local xz="$CACHE_DIR/linux-${KERNEL_VERSION}.tar.xz"
	local gz="$CACHE_DIR/linux-${KERNEL_VERSION}.tar.gz"

	# Fetch from the GitHub mirror (gregkh/linux). Keeps every stable tag and is
	# reachable on networks where the kernel.org Fastly CDN is filtered/blocked.
	fetch_github() {
		echo "      Fetching from GitHub mirror (larger & slower, please wait)..."
		curl -fL --progress-bar -o "$gz" "$KERNEL_URL_GITHUB" || return 1
		tar -xzf "$gz" -C "$CACHE_DIR"
		# Normalise whatever top-level dir the archive used to $KERNEL_SRC.
		if [ ! -d "$KERNEL_SRC" ]; then
			local top
			top=$(tar -tzf "$gz" | head -1 | cut -d/ -f1)
			[ -n "$top" ] && [ -d "$CACHE_DIR/$top" ] && mv "$CACHE_DIR/$top" "$KERNEL_SRC"
		fi
	}

	if [ -f "$xz" ]; then
		tar -xf "$xz" -C "$CACHE_DIR"
	elif [ "$KERNEL_PREFER_MIRROR" = "1" ]; then
		fetch_github || die "GitHub mirror download failed. Check the version tag exists and your network."
	elif curl -fL --progress-bar -o "$xz" "$KERNEL_URL_PRIMARY"; then
		tar -xf "$xz" -C "$CACHE_DIR"
	else
		echo "      kernel.org CDN did not serve it (unreachable/filtered, or EOL)."
		fetch_github || die "Failed to download linux-${KERNEL_VERSION} from kernel.org and GitHub.
      Check the version exists (https://kernel.org, prefer an LTS: 6.18.x, 6.12.x)
      and your network. To always skip the CDN, set in devtools/config.local:
        KERNEL_PREFER_MIRROR=1"
	fi
	[ -d "$KERNEL_SRC" ] || die "Kernel source not found after extraction."
	echo "      Done: $KERNEL_SRC"
}

# --- Phase 2: build the kernel ---
build_kernel() {
	local bzimage="$KERNEL_BUILD/arch/x86/boot/bzImage"
	local stamp="$KERNEL_BUILD/.config-stamp"

	# Config fragments merged on top of defconfig. The debug fragment is added
	# only when KERNEL_DEBUG=1.
	local fragments=("$DEVTOOLS_DIR/kernel.config")
	if [ "${KERNEL_DEBUG:-0}" = "1" ]; then
		fragments+=("$DEVTOOLS_DIR/kernel.debug.config")
	fi

	local expected
	expected=$( { echo "debug=${KERNEL_DEBUG:-0}"; \
		cat "${fragments[@]}" "$DEVTOOLS_DIR/config.defaults"; } \
		| sha256sum | cut -c1-16)

	if [ -f "$bzimage" ] && stamp_fresh "$stamp" "$expected"; then
		echo "[2/3] Kernel already built (config unchanged, skip)"
		ln -sfn "$KERNEL_SRC" "$KERNEL_BUILD/source"
		return
	fi

	if [ "${KERNEL_DEBUG:-0}" = "1" ]; then
		echo "[2/3] Building kernel $KERNEL_VERSION (DEBUG profile: KASAN/lockdep/...) ..."
	else
		echo "[2/3] Building kernel $KERNEL_VERSION (lean profile) ..."
	fi
	mkdir -p "$KERNEL_BUILD"

	# defconfig, then merge our fragment(s) on top.
	make -C "$KERNEL_SRC" O="$KERNEL_BUILD" defconfig
	(cd "$KERNEL_SRC" && \
		scripts/kconfig/merge_config.sh -O "$KERNEL_BUILD" \
			"$KERNEL_BUILD/.config" "${fragments[@]}")

	# -std=gnu11: GCC 15+ defaults to C23 where bool/true/false are keywords,
	#   which clashes with the kernel's own typedefs.
	# -Wno-error: demote CONFIG_WERROR's -Werror so newer-GCC warnings on
	#   intentional kernel patterns don't abort the build.
	local stdflag="-std=gnu11 -Wno-error"
	make -C "$KERNEL_SRC" O="$KERNEL_BUILD" -j"$NPROC" \
		KCFLAGS="$stdflag" KCPPFLAGS="$stdflag" HOSTCFLAGS="$stdflag" bzImage
	make -C "$KERNEL_SRC" O="$KERNEL_BUILD" -j"$NPROC" \
		KCFLAGS="$stdflag" KCPPFLAGS="$stdflag" HOSTCFLAGS="$stdflag" modules_prepare
	# GDB scripts: creates $KERNEL_BUILD/vmlinux-gdb.py (+ generated constants).
	# This is tied to the `all` target, so a plain `bzImage` build skips it --
	# hence the explicit call, or GDB debugging fails to source vmlinux-gdb.py.
	make -C "$KERNEL_SRC" O="$KERNEL_BUILD" -j"$NPROC" \
		KCFLAGS="$stdflag" KCPPFLAGS="$stdflag" HOSTCFLAGS="$stdflag" scripts_gdb
	# Let out-of-tree modules resolve vmlinux symbols without building in-tree ones.
	cp "$KERNEL_BUILD/vmlinux.symvers" "$KERNEL_BUILD/Module.symvers"

	echo "$expected" > "$stamp"
	echo "      Done: $bzimage (vmlinux ready for GDB)"
}

# --- Phase 3: busybox + initramfs ---
build_busybox() {
	[ -f "$BUSYBOX_SRC/busybox" ] && { echo "      Busybox present (skip)"; return; }
	local tarball="$CACHE_DIR/busybox-${BUSYBOX_VERSION}.tar.bz2"
	if [ ! -d "$BUSYBOX_SRC" ]; then
		echo "      Downloading busybox $BUSYBOX_VERSION ..."
		[ -f "$tarball" ] || curl -fL --progress-bar -o "$tarball" "$BUSYBOX_URL"
		tar -xf "$tarball" -C "$CACHE_DIR"
	fi
	echo "      Building busybox (static) ..."
	make -C "$BUSYBOX_SRC" defconfig
	sed -i -e 's/# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
	       -e 's/CONFIG_TC=y/# CONFIG_TC is not set/' \
		"$BUSYBOX_SRC/.config"
	make -C "$BUSYBOX_SRC" -j"$NPROC"
}

build_initramfs() {
	local stamp="$CACHE_DIR/.initramfs-stamp"
	local expected
	expected=$(stamp_hash "$DEVTOOLS_DIR/initramfs/init")
	if [ -f "$INITRAMFS_CPIO" ] && stamp_fresh "$stamp" "$expected"; then
		echo "[3/3] Initramfs already built (skip)"
		return
	fi
	echo "[3/3] Creating initramfs ..."
	build_busybox

	rm -rf "$INITRAMFS_DIR"
	mkdir -p "$INITRAMFS_DIR"
	make -C "$BUSYBOX_SRC" CONFIG_PREFIX="$INITRAMFS_DIR" install
	mkdir -p "$INITRAMFS_DIR"/{proc,sys,dev,tmp,etc}

	cp "$DEVTOOLS_DIR/initramfs/init" "$INITRAMFS_DIR/init"
	chmod +x "$INITRAMFS_DIR/init"

	(cd "$INITRAMFS_DIR" && find . | cpio -o -H newc 2>/dev/null | gzip > "$INITRAMFS_CPIO")
	echo "$expected" > "$stamp"
	echo "      Done: $INITRAMFS_CPIO ($(du -h "$INITRAMFS_CPIO" | cut -f1))"
}

check_host
download_kernel
build_kernel
build_initramfs

echo ""
echo "Setup complete. Next:"
echo "  devtools/build.sh   # or: make qemu-build"
echo "  devtools/boot.sh    # or: make qemu-boot"
