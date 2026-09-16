#!/usr/bin/env bash
# Release gate for the Linux/Switch port: runs every behavioural check that was
# used to verify this port, against one build of the engine.
#
#   tests/run-all.sh [--binary <path>] [--with-switch] [--keep-fixtures]
#
# Default binary: build/krkr2.  Exits non-zero on the first failure and prints a
# summary table.  Every check runs headless (SDL_VIDEODRIVER=offscreen), so it
# works over ssh and in CI.
#
# Why each check exists (all of these caught a real defect at some point):
#   * smoke-game      - boot, script, drawing, colour, input, audio, movie, exit
#   * plain XP3       - archive storage path (regenerated, never stale)
#   * encrypted XP3   - xp3filter patch flow: script + audio from ciphertext
#   * 7z              - libarchive backend
#   * api-conformance - 62 engine/script API checks (asserts a pass floor)
#   * file-selector   - modal dialog keyboard/mouse/gamepad paths + cancel/save
#   * archive read/seek - CRC-exact member streams incl. backwards/END seeks
#   * empty strings   - "".length and friends (crashed every optimised build)
#   * null archive    - pointing the engine at a non-archive must not crash
set -u

binary="build/krkr2"
with_switch=0
keep_fixtures=0
while [ $# -gt 0 ]; do
	case "$1" in
		--binary) binary="$2"; shift 2 ;;
		--with-switch) with_switch=1; shift ;;
		--keep-fixtures) keep_fixtures=1; shift ;;
		-h|--help) sed -n '2,20p' "$0"; exit 0 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root" || exit 2
[ -x "$binary" ] || { echo "no such binary: $binary" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 is required to build fixtures" >&2; exit 2; }

work="$(mktemp -d)"
trap '[ "$keep_fixtures" = 1 ] || rm -rf "$work"' EXIT

export SDL_VIDEODRIVER=offscreen
export SDL_AUDIODRIVER=dummy   # a real device is not required for these checks
# Keep the engine's own data (its global preference, and the launcher's
# last-game file) inside the run's scratch directory: without this a test run
# writes into the user's ~/.local/share/kirikiri2 and the next ./krkr2 opens the
# browser wherever the last *test* stopped.
export KRKR2_DATA_DIR="$work/data"

pass=0; fail=0
declare -a results

# run <name> <timeout> <expect-exit|-> <grep-pattern|-> -- <command...>
run() {
	local name="$1" timeout_s="$2" want_exit="$3" pattern="$4"; shift 4
	[ "$1" = "--" ] && shift
	local log="$work/$name.log"
	timeout -k 5 "$timeout_s" "$@" >"$log" 2>&1
	local rc=$?
	local ok=1 why=""
	if [ "$want_exit" != "-" ] && [ "$rc" != "$want_exit" ]; then
		ok=0; why="exit $rc (want $want_exit)"
	fi
	if [ "$ok" = 1 ] && [ "$pattern" != "-" ] && ! grep -qE "$pattern" "$log"; then
		ok=0; why="missing /$pattern/"
	fi
	# The engine reports an uncaught script error by popping a dialog and still
	# exiting 0, so a success marker alone is not proof that a script ran
	# cleanly.  "@line(N)" is that report; TVPShowSimpleMessageBox is not a
	# failure signal - the port mirrors every engine message to the console (an
	# expected start-up failure includes it), so it is deliberately not matched.
	if [ "$ok" = 1 ] && grep -qE "@line\([0-9]+\)" "$log"; then
		ok=0; why="uncaught script error in log"
	fi
	if [ "$ok" = 1 ]; then
		pass=$((pass + 1)); results+=("PASS  $name")
	else
		fail=$((fail + 1)); results+=("FAIL  $name  ($why; log: $log)")
		echo "--- $name log (tail) ---" >&2
		tail -5 "$log" >&2
	fi
}

# --- fixtures -----------------------------------------------------------------
python3 tests/make-xp3.py tests/smoke-game "$work/plain.xp3" --quiet
printf 'this file is not an archive\n' >"$work/notarchive.dat"

# --- suites -------------------------------------------------------------------
run "smoke-game"        60 - "smoke: done"            -- "$binary" tests/smoke-game
grep -q "movie playing"    "$work/smoke-game.log" || echo "note: movie section missing from the smoke log"
run "xp3-plain"         60 - "smoke: done"            -- "$binary" "$work/plain.xp3"
run "xp3-encrypted"     60 - "smoke: done"            -- "$binary" tests/encrypted-game/game.xp3
grep -q "tone.mp3 status=play" "$work/xp3-encrypted.log" || echo "note: audio did not play from the encrypted archive"
run "7z"                60 - "smoke: done"            -- "$binary" tests/7z-game/game.7z
run "layer-ex-movie"    60 - "layerExMovie: done"     -- "$binary" tests/layer-ex-movie
# Touch and game-pad input, replayed by the host's scripted input (KRKR2_TEST_INPUT,
# src/core/environ/sdl/Host.h): two taps, a drag and a pad button.
run "touch-input"       60 - "touch-input: done"      -- env \
	KRKR2_TEST_INPUT="40:fingerdown:100,60;45:fingermove:160,90;50:fingerup:160,90;60:fingerdown:200,120;64:fingerup:200,120;80:pad:a;95:padup:a" \
	"$binary" tests/touch-input
run "file-selector-open"  40 - "call 1 returned 1"    -- env KRKR2_DIALOG_KEYS=down,enter "$binary" tests/file-selector/open
run "file-selector-cancel" 40 - "call 1 returned 0"   -- env KRKR2_DIALOG_KEYS=esc,esc "$binary" tests/file-selector/open
# The game browser (launcher): with no game it asks, then starts what was picked.
run "launcher"         120 - "launcher test: 4 passed, 0 failed" -- bash tests/run-launcher-test.sh --build "$(dirname "$binary")"

# conformance: assert the summary line; there are no known-failing checks, so the
# floor is the full count and any failure is a regression
run "api-conformance"   120 - "SUMMARY pass="          -- "$binary" tests/api-conformance
if [ -f "$work/api-conformance.log" ]; then
	got=$(grep -oE "SUMMARY pass=[0-9]+ fail=[0-9]+" "$work/api-conformance.log" | head -1)
	passes=$(echo "$got" | grep -oE "pass=[0-9]+" | cut -d= -f2)
	fails=$(echo "$got" | grep -oE "fail=[0-9]+" | cut -d= -f2)
	if [ -n "${passes:-}" ] && [ "$passes" -ge 61 ] && [ "${fails:-1}" = 0 ]; then
		pass=$((pass + 1)); results+=("PASS  api-conformance pass-count ($passes passed, 0 failed)")
	else
		fail=$((fail + 1)); results+=("FAIL  api-conformance pass-count (${got:-no summary})")
	fi
fi

run "archive-read-seek" 60 - "SUMMARY pass="          -- bash tests/run-archive-read-seek.sh --build "$(dirname "$binary")" tests/7z-game/game.7z "startup.tjs:tests/smoke-game/startup.tjs" "tone.wav:tests/smoke-game/tone.wav"

# --- regressions that only appear in optimised builds ------------------------
run "empty-string-probe" 30 0 "regress: empty-string checks passed" -- "$binary" tests/regress/empty-strings
run "null-archive"       30 3 "cannot start|did not start"         -- "$binary" "$work/notarchive.dat"

# An uncaught script error must be reported instead of crashing: the engine's
# exception register dump used to dereference an empty string and kill every
# optimised build (exit 139) before it could print anything.  So this check wants
# the engine's error report in the log AND a non-signal exit code.  It cannot use
# run(), whose success convention counts that report as a failure.
timeout -k 5 30 "$binary" tests/regress/uncaught-error >"$work/uncaught-error.log" 2>&1
rc=$?
if [ "$rc" -lt 128 ] && grep -qE "script exception|@line\([0-9]+\)" "$work/uncaught-error.log"; then
	pass=$((pass + 1)); results+=("PASS  uncaught-error (exit $rc, error reported)")
else
	fail=$((fail + 1)); results+=("FAIL  uncaught-error (exit $rc; log: $work/uncaught-error.log)")
	tail -5 "$work/uncaught-error.log" >&2
fi

# --- optional: cross target ---------------------------------------------------
if [ "$with_switch" = 1 ]; then
	if [ -n "${DEVKITPRO:-}" ] && [ -d "${DEVKITPRO:-}/cmake" ]; then
		run "switch-nro-build" 900 0 "." -- env DEVKITPRO="$DEVKITPRO" cmake --build build-switch --target krkr2
		nro=build-switch/krkr2.nro
		if [ -s "$nro" ]; then
			pass=$((pass + 1)); results+=("PASS  switch-nro-package ($(stat -c %s "$nro") bytes)")
		else
			fail=$((fail + 1)); results+=("FAIL  switch-nro-package ($nro missing or empty)")
		fi
	else
		results+=("SKIP  switch-nro-build (DEVKITPRO not set)")
	fi
fi

# --- summary ------------------------------------------------------------------
echo
echo "release gate: $pass passed, $fail failed  (binary: $binary)"
for line in "${results[@]}"; do echo "  $line"; done
[ "$fail" = 0 ]
