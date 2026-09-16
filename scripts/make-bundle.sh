#!/usr/bin/env bash
# Build a relocatable bundle: the executable plus the libraries it needs that a
# target machine is not guaranteed to have, laid out so the whole directory can be
# copied to another machine of the same architecture:
#
#   krkr2-bundle/
#     krkr2          the executable, linked with a $ORIGIN/lib runpath
#     lib/*.so*      the libraries it loads from beside itself
#
#   scripts/make-bundle.sh --build <build-dir> --out <dir> [--quiet]
#
# The build must have been configured with -DKRKR2_BUNDLE=ON (that is what puts
# $ORIGIN/lib in the runpath and links the C++ runtime in statically).
#
# What it does NOT solve: the libc a binary needs is decided when it is built
# (a GLIBC_2.43 symbol does not exist in a glibc 2.41), and this port cannot link
# libc statically - it dlopen()s the GL/EGL driver, the audio backends and
# fontconfig, which a static libc breaks.  So a bundle built for a newer libc than
# the target has will still not start; build on (or in a container of) the oldest
# system you want to support.  The script prints the floor it baked in.
#
# Libraries that must come from the machine that runs it are not copied: the libc
# family and loader, the display/GL stack and its driver entry points, the desktop
# glue (glib, pango, cairo, fontconfig, freetype, ...), the compression/xml
# libraries every desktop already has, and the audio backends.  Everything else -
# ffmpeg, the image codecs, the archive readers, the script engine's dependencies
# - is copied, because those are exactly the ones whose shared-object names differ
# between distributions (libjpeg.so.8 vs .62, libavcodec.so.63 vs .61, ...).
set -u

build="build"
out="krkr2-bundle"
quiet=0
while [ $# -gt 0 ]; do
	case "$1" in
		--build) build="$2"; shift 2 ;;
		--out) out="$2"; shift 2 ;;
		--quiet) quiet=1; shift ;;
		-h|--help) sed -n '2,30p' "$0"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root" || exit 2
exe="$build/krkr2"
[ -x "$exe" ] || { echo "no executable at $exe (build it first)" >&2; exit 2; }
command -v ldd >/dev/null || { echo "ldd is required" >&2; exit 2; }
command -v readelf >/dev/null || { echo "readelf is required (binutils)" >&2; exit 2; }
if ! command -v patchelf >/dev/null; then
	cat >&2 <<'EOM'
patchelf is required (Debian/Ubuntu: apt install patchelf, Arch: pacman -S patchelf).
A bundled library's own dependencies are looked up through that library's runpath,
not the executable's ($ORIGIN is not inherited), so every library in the bundle
needs "$ORIGIN" written into it.
EOM
	exit 2
fi

say() { [ "$quiet" = 1 ] || echo "$@"; }

# ---------------------------------------------------------------------------
# Everything whose shared-object name a target machine provides itself.  Kept in
# one place so the copy and the final check cannot disagree.
# ---------------------------------------------------------------------------
# Every alternative ends in ".so", so the match is on the whole shared-object
# name and not on a prefix of it ("libm" must not match "libmodplug").
system_prefixes='^(ld-linux[^ ]*\.so|lib(c|m|pthread|dl|rt|resolv|nsl|util|thread_db|gcc_s|stdc\+\+)\.so|lib(X11|xcb|Xau|Xdmcp|Xext|Xfixes|Xrender|Xi|Xtst|Xrandr|Xcursor|Xinerama|SM|ICE|GL|EGL|GLX|GLdispatch|OpenGL|gbm|drm|wayland|fontconfig|freetype|expat|z|bz2|lzma|zstd|brotli|png16|harfbuzz|fribidi|datrie|thai|glib|gio|gobject|gmodule|gdk|pango|cairo|pixman|atk|ffi|pcre2|mount|blkid|systemd|selinux|acl|attr|uuid|cap|udev|dbus|asound|pulse|snd|jack|zmq|vulkan|xshmfence)\.so|libxcb-[a-z0-9]+\.so)'

is_system_lib() { printf '%s' "$1" | grep -qE "$system_prefixes"; }

# ---------------------------------------------------------------------------
# Collect the direct and indirect dependencies.
# ---------------------------------------------------------------------------
say "reading dependencies of $exe ..."
libs="$(ldd "$exe" | awk '$2 == "=>" && $3 ~ /^\// {print $3} $1 ~ /^\// {print $1}' | sort -u)"
[ -n "$libs" ] || { echo "could not read the dependencies (ldd failed)" >&2; exit 1; }

if ! readelf -d "$exe" | grep -q 'ORIGIN'; then
	echo "$exe has no \$ORIGIN runpath: configure with -DKRKR2_BUNDLE=ON" >&2
	exit 1
fi

rm -rf "$out"
mkdir -p "$out/lib"
cp "$exe" "$out/krkr2"

bundled=0
expected=""
while IFS= read -r lib; do
	base="$(basename "$lib")"
	# strip the trailing "(0x...)" that ldd appends in some formats
	base="${base%% *}"
	if is_system_lib "$base"; then
		expected="$expected $base"
		continue
	fi
	# the soname is what the loader looks for, and it is also the real file name
	# for a versioned library; copy the resolved file under its own name
	cp -Lf "$lib" "$out/lib/$base"
	bundled=$((bundled + 1))
done <<<"$libs"

# The executable's runpath is exactly the bundle's (a build against an SDL2 in a
# non-system prefix would otherwise also record that prefix, which shadows the
# bundled copy on the machine it was built on) ...
patchelf --set-rpath '$ORIGIN/lib:$ORIGIN' "$out/krkr2"
# ... and every bundled library gets "$ORIGIN" so that it finds *its* dependencies
# in lib/ as well: DT_RUNPATH is not inherited from the executable, so without
# this a transitive dependency (libxml2 through libavformat, say) is looked for in
# the system paths only and the bundle fails on the target machine.
patched=0
for lib in "$out"/lib/*.so*; do
	[ -e "$lib" ] || continue
	patchelf --set-rpath '$ORIGIN' "$lib"
	patched=$((patched + 1))
done
# ---------------------------------------------------------------------------
# Self-check: every dependency is either in the bundle or one of the names a
# desktop machine provides.  This is what makes the bundle complete, and it is
# cheap enough to always run.
# ---------------------------------------------------------------------------
check_needed() {
	local file="$1" context="$2" missing=""
	while IFS= read -r name; do
		[ -n "$name" ] || continue
		if [ -e "$out/lib/$name" ] || is_system_lib "$name"; then continue; fi
		missing="$missing $name"
	done < <(readelf -d "$file" 2>/dev/null |
		awk '/NEEDED/ {gsub(/[][]/, "", $5); print $5}')
	if [ -n "$missing" ]; then
		echo "$context needs libraries that are neither bundled nor system:$missing" >&2
		exit 1
	fi
}

check_needed "$out/krkr2" "the executable"
unpatched=""
for lib in "$out"/lib/*.so*; do
	[ -e "$lib" ] || continue
	readelf -d "$lib" 2>/dev/null | grep -q 'ORIGIN' || unpatched="$unpatched $(basename "$lib")"
done
if [ -n "$unpatched" ]; then
	echo "these bundled libraries have no \$ORIGIN runpath:$unpatched" >&2
	exit 1
fi
for lib in "$out"/lib/*.so*; do
	[ -e "$lib" ] || continue
	check_needed "$lib" "$(basename "$lib")"
done

# The libc floor this build imposes, and the runpath it will use.
glibc_max="$(readelf --version-info "$out/krkr2" 2>/dev/null |
	grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -Vu | tail -1)"
runpath="$(readelf -d "$out/krkr2" | awk '/RUNPATH|RPATH/ {sub(/.*\[/, ""); sub(/\].*/, ""); print}')"

say ""
say "bundle: $out"
say "  krkr2            $(stat -c %s "$out/krkr2") bytes"
say "  lib/             $bundled libraries ($patched patched), $(du -sh "$out/lib" | cut -f1)"
say "  runpath          $runpath"
say "  from the machine $(printf '%s' "$expected" | wc -w) libraries (libc, GL, desktop, audio)"
say ""
say "  needs at least ${glibc_max:-unknown} on the machine that runs it;"
say "  copy the whole $out directory and run ./krkr2 inside it."
