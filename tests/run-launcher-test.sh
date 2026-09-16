#!/usr/bin/env bash
# The game browser (launcher): with no game argument - on a handheld the only
# way to start anything - krkr2 shows the file selector before the engine starts
# and starts whatever is picked there.
#
#   tests/run-launcher-test.sh [--build <dir>] [--keep]
#
# Three cases, all headless (the dialog draws on the host's SDL renderer):
#   * a game that is a directory:  the browser opens at the directory, the cursor
#     is moved onto the game and F2/pad-X takes the folder (ChooseFolder);
#   * a game that is an archive:  ENTER on the file selects it;
#   * cancelling (ESC) reports that nothing was selected and exits 0.
# Each case asserts that the picked game actually ran (its start-up script logs a
# marker) and that the last game is remembered for the next run.
set -u

build="build"
keep=0
while [ $# -gt 0 ]; do
	case "$1" in
		--build) build="$2"; shift 2 ;;
		--keep) keep=1; shift ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
done

root="$(cd "$(dirname "$0")/.." && pwd)"
cd "$root" || exit 2
krkr2="$build/krkr2"
[ -x "$krkr2" ] || { echo "no such binary: $krkr2" >&2; exit 2; }
command -v python3 >/dev/null || { echo "python3 is required" >&2; exit 2; }

work="$(mktemp -d)"
trap '[ "$keep" = 1 ] || rm -rf "$work"' EXIT

export SDL_VIDEODRIVER=offscreen
export SDL_AUDIODRIVER=dummy

# --- fixtures -----------------------------------------------------------------
# Both games are script-only (no Window): the engine ends such a game by itself,
# so the process exits 0 without a window ever appearing.
mkdir -p "$work/games/alpha" "$work/games/zeta"
cat >"$work/games/alpha/startup.tjs" <<'TJS'
Debug.message("launcher-test: alpha started");
TJS
cat >"$work/games/zeta/startup.tjs" <<'TJS'
Debug.message("launcher-test: zeta started");
TJS
python3 tests/make-xp3.py "$work/games/zeta" "$work/games/zeta.xp3" --quiet
rm -rf "$work/games/zeta"   # only the archive remains, so the listing is deterministic

pass=0; fail=0
declare -a results

# run_case <name> <keys> <want-marker> <want-exit|-> <want-selected>
run_case() {
	local name="$1" keys="$2" marker="$3" want_exit="$4" log="$work/$1.log"
	rm -f "$work/data/last-game.txt"
	timeout -k 5 60 env KRKR2_DIALOG_KEYS="$keys" "$krkr2" \
		--data-dir="$work/data" --browse "$work/games" >"$log" 2>&1
	local rc=$?
	local ok=1 why=""
	if [ "$want_exit" != "-" ] && [ "$rc" != "$want_exit" ]; then ok=0; why="exit $rc (want $want_exit)"; fi
	if [ "$ok" = 1 ] && [ -n "$marker" ] && ! grep -q "$marker" "$log"; then
		ok=0; why="the picked game did not run (/$(echo "$marker" | tr -d '\n')/)"
	fi
	if [ "$ok" = 1 ] && [ -z "$marker" ] && grep -q "launcher-test:" "$log"; then
		ok=0; why="a game ran although nothing was selected"
	fi
	if [ "$ok" = 1 ]; then
		pass=$((pass + 1)); results+=("PASS  $name")
	else
		fail=$((fail + 1)); results+=("FAIL  $name  ($why; log: $log)")
		sed -n '1,12p' "$log" >&2
	fi
}

# The listing is directories first, then files, so it reads:
#   0 = alpha (a game directory), 1 = zeta.xp3 (a game archive)
# A directory game is picked by entering it and taking the folder the cursor is
# now in (F2, or pad X - the Switch has no F keys), an archive by activating it.
run_case "launcher-directory-game" "enter,pad:x" "launcher-test: alpha started" 0
if grep -q "launcher-test: alpha started" "$work/launcher-directory-game.log"; then
	# the launcher remembers what it started, for the next run
	last="$(cat "$work/data/last-game.txt" 2>/dev/null)"
	if [ "$last" = "$work/games/alpha" ]; then
		pass=$((pass + 1)); results+=("PASS  launcher-remembers-last-game")
	else
		fail=$((fail + 1)); results+=("FAIL  launcher-remembers-last-game (got '${last:-none}')")
	fi
fi
run_case "launcher-archive-game"   "down,enter" "launcher-test: zeta started" 0
run_case "launcher-cancel"         "esc" "" 0

echo
echo "launcher test: $pass passed, $fail failed"
for line in "${results[@]}"; do echo "  $line"; done
[ "$fail" = 0 ]
