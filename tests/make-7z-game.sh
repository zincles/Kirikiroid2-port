#!/bin/sh
#
# Regenerates the .7z fixtures used by the 7z archive backend's tests from
# tests/smoke-game, mirroring tests/make-xp3.py (which builds the XP3 fixture
# from the same directory).
#
#   tests/7z-game/game.7z           the game archive: the smoke game's files at
#                                   the archive root, startup.tjs included, with
#                                   7-Zip's default options (solid blocks)
#   tests/7z-game/game-nonsolid.7z  the same with -ms=off (one folder per member)
#   tests/7z-game/game-paths.7z     the same bytes under nested mixed-case names
#                                   (Data/Startup.TJS, Sound/Sub/Tone.WAV, ...),
#                                   which the engine has to find through its
#                                   case-insensitive, slash-normalising lookup
#
# Requires the official 7-Zip CLI (pacman: 7zip, debian: 7zip/p7zip-full).
#
#   sh tests/make-7z-game.sh

set -e

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SRC="$ROOT/tests/smoke-game"
DEST="$ROOT/tests/7z-game"

command -v 7z >/dev/null 2>&1 || {
	echo "$0: the 7z CLI is not installed (pacman -S 7zip)" >&2
	exit 2
}

mkdir -p "$DEST"
rm -f "$DEST/game.7z" "$DEST/game-nonsolid.7z" "$DEST/game-paths.7z"

# The game archive: from inside the game's directory, so the entries land at the
# archive root (7z keeps the given relative path otherwise).
( cd "$SRC" && 7z a -bso0 -bsp0 "$DEST/game.7z" ./* >/dev/null )
( cd "$SRC" && 7z a -bso0 -bsp0 -ms=off "$DEST/game-nonsolid.7z" ./* >/dev/null )

# A nested, mixed-case layout, staged in a temporary tree.
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$STAGE/Data" "$STAGE/Sound/Sub"
cp "$SRC/startup.tjs" "$STAGE/Data/Startup.TJS"
cp "$SRC/tone.wav" "$STAGE/Sound/Sub/Tone.WAV"
cp "$SRC/tone.mp3" "$STAGE/Sound/Tone.MP3"
( cd "$STAGE" && 7z a -bso0 -bsp0 "$DEST/game-paths.7z" Data Sound >/dev/null )

for f in game.7z game-nonsolid.7z game-paths.7z; do
	printf '\n== %s\n' "$f"
	7z l "$DEST/$f" | tail -n 8
done
