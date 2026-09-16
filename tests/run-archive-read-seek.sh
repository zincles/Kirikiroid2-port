#!/bin/sh
#
# Build and run tests/archive-read-seek.cpp - the archive member read/seek
# verifier - against a CMake build tree.
#
#   tests/run-archive-read-seek.sh [--build <dir>] <archive> <member>:<original> [...]
#
# The verifier goes through the engine's own TVPOpenArchive(), i.e. exactly the
# path the game loader uses, and compares what the archive's streams deliver
# against the original files (size, CRC32 of a full read, CRC32 of slices read
# after forward/backward/from-end seeks, and reading at end of stream).
#
# Member names are storage names and may be given in any case with '/' or '\'
# separators; they are normalized the way the engine normalizes a name before
# the lookup (tTVPArchive::CreateStream() itself requires a normalized name).
#
# Only unencrypted archives can be checked this way: an encrypted XP3's content
# filter is registered by the game's patch script, which this verifier does not
# run, so it would compare ciphertext against the plain-text originals.  Use a
# game run for those (tests/encrypted-game).
#
# The engine has no test target, so the compiler flags and the link libraries
# are taken from the build tree: compile flags from compile_commands.json,
# libraries from build.ninja's krkr2 link line.
#
# examples:
#   tests/run-archive-read-seek.sh tests/7z-game/game.7z \
#       tone.wav:tests/smoke-game/tone.wav startup.tjs:tests/smoke-game/startup.tjs
#   tests/run-archive-read-seek.sh --build build-switch ...   (cross build: not runnable)

set -e

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/build"

while [ $# -gt 0 ]; do
	case "$1" in
	--build) BUILD=$2; shift 2 ;;
	--build=*) BUILD=${1#--build=}; shift ;;
	*) break ;;
	esac
done

if [ $# -lt 1 ]; then
	sed -n '3,12p' "$0" >&2
	exit 2
fi

BUILD=$(CDPATH= cd -- "$BUILD" && pwd)
SRC="$ROOT/tests/archive-read-seek.cpp"
OUT="$BUILD/archive-read-seek"

[ -f "$BUILD/build.ninja" ] || {
	echo "$0: $BUILD is not a ninja build tree (run cmake/ninja first)" >&2
	exit 2
}
[ -f "$BUILD/compile_commands.json" ] || {
	echo "$0: $BUILD/compile_commands.json is missing" >&2
	exit 2
}

FLAGS=$(python3 - "$BUILD/compile_commands.json" <<'PY'
import json, shlex, sys
entries = json.load(open(sys.argv[1]))
entry = next((e for e in entries if e['file'].endswith('/base/StorageIntf.cpp')), entries[0])
args = shlex.split(entry['command'])[1:]
keep, i = [], 0
while i < len(args):
    a = args[i]
    if a in ('-o', '-c', '-MF', '-MT', '-MQ'):   # output/dependency plumbing
        i += 2
        continue
    keep.append(a)
    i += 1
print(' '.join(keep))
PY
)

LIBS=$(sed -n 's/^  LINK_LIBRARIES = //p' "$BUILD/build.ninja" | head -1)
[ -n "$LIBS" ] || {
	echo "$0: cannot find the krkr2 link line in $BUILD/build.ninja" >&2
	exit 2
}
LIBS=$(echo "$LIBS" | sed "s| libkrkr2core.a| $BUILD/libkrkr2core.a|")

# The engine links with -Wl,--wrap=malloc,... (Application.cpp's allocator
# interception), and those objects are pulled into any binary that uses the
# engine's logging/exception paths - so the verifier needs the same flags.
# --dependency-file is dropped: it is ninja's, and the verifier is not a ninja
# target.
LINK_FLAGS=$(sed -n 's/^  LINK_FLAGS = //p' "$BUILD/build.ninja" | head -1 \
	| sed 's/-Wl,--dependency-file=[^ ]*//g')

CXX=${CXX:-$(sed -n 's/^CXX = //p' "$BUILD/build.ninja" | head -1)}
CXX=${CXX:-c++}

# rebuild when the verifier or either library changed
if [ ! -x "$OUT" ] || [ "$SRC" -nt "$OUT" ] || [ "$BUILD/libkrkr2core.a" -nt "$OUT" ]; then
	echo "# $CXX ... -o $OUT"
	# shellcheck disable=SC2086
	( cd "$BUILD" && $CXX $FLAGS $LINK_FLAGS -o "$OUT" "$SRC" $LIBS )
fi

ARCHIVE="$1"; shift
[ -f "$ARCHIVE" ] || ARCHIVE="$ROOT/$ARCHIVE"
exec "$OUT" "$ARCHIVE" "$@"
