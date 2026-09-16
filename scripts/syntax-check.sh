#!/bin/sh
# Syntax-check one source file with exactly the flags this build uses for it.
#
#   scripts/syntax-check.sh src/core/visual/FontImpl.cpp
#
# Reads build/compile_commands.json, strips the -o/-c pairing and runs the
# same command with -fsyntax-only. Safe to run concurrently (does not touch
# the build directory).
set -e
src="$1"
[ -n "$src" ] || { echo "usage: $0 <source.cpp>" >&2; exit 2; }
root="$(cd "$(dirname "$0")/.." && pwd)"
cc_json="$root/build/compile_commands.json"
[ -f "$cc_json" ] || { echo "missing $cc_json; run: cmake -S . -B build" >&2; exit 2; }

cmd="$(jq -r --arg f "$(cd "$(dirname "$src")" && pwd)/$(basename "$src")" \
	'.[] | select(.file == $f) | .command' "$cc_json" | head -1)"
[ -n "$cmd" ] || { echo "no compile command for $src (is it in the build?)" >&2; exit 2; }

cmd="$(printf '%s' "$cmd" | sed -E 's/ -o [^ ]+//; s/ -c / -fsyntax-only /')"
cd "$(jq -r '.[0].directory' "$cc_json")"
exec sh -c "$cmd"
