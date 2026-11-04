#!/bin/bash
# Every window comes back, and New Window is a new shell (project.md sec 8.4).
#
# Real programs on a private X server and a private session bus, because
# what is being checked is the space between processes, where no test in
# one process can stand. Two saved windows: one start must bring both back
# as processes of their own, New Window from one must be neither of them,
# ending all three with a signal must leave two layouts and not three, and
# a second start must bring the same two back.
#
# Not part of `check`: it needs Xvfb, xdotool, xprop/xwininfo and
# dbus-run-session.
#
# Bounded: every wait has a ceiling, the whole run is under timeout, and
# the trap kills what it can name and the X server and removes the scratch
# directory however this exits. Started processes are found by _NET_WM_PID
# on this private display, so nothing else on the machine is touched.
set -u

cd "$(dirname "$0")/.." || exit 1
BIN=${BSSH_BIN:-build/beerssh}

for tool in Xvfb xdotool xprop xwininfo dbus-run-session; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "check-windows: $tool is not installed, so nothing was checked" >&2
		exit 1
	fi
done
if [ ! -x "$BIN" ]; then
	echo "check-windows: no $BIN -- build it first (make beerssh)" >&2
	exit 1
fi
BIN=$(readlink -f "$BIN")

if [ -z "${BSSH_CHECK_WINDOWS_INNER:-}" ]; then
	BSSH_CHECK_WINDOWS_INNER=1 exec timeout 300 dbus-run-session -- "$0" "$@"
fi

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/bssh-windows.XXXXXX") || exit 1
XVFB=
pids() {
	for w in $(window_ids); do
		xprop -id "$w" _NET_WM_PID 2>/dev/null | awk '{print $3}'
	done | sort -u
}
cleanup() {
	for p in $(pids); do kill "$p" 2>/dev/null; done
	sleep 1
	for p in $(pids); do kill -9 "$p" 2>/dev/null; done
	[ -n "$XVFB" ] && kill "$XVFB" 2>/dev/null
	case "$SCRATCH" in "${TMPDIR:-/tmp}"/bssh-windows.*) rm -rf "$SCRATCH" ;; esac
}
trap cleanup EXIT

D=
for n in 80 81 82 83 84 85 86 87 88 89; do
	[ -e "/tmp/.X11-unix/X$n" ] || { D=:$n; break; }
done
[ -n "$D" ] || { echo "check-windows: no free display :80-:89" >&2; exit 1; }

Xvfb "$D" -screen 0 1600x900x24 -nolisten tcp >/dev/null 2>&1 & XVFB=$!
sleep 2
export DISPLAY=$D HOME=$SCRATCH XDG_CONFIG_HOME=$SCRATCH/config XDG_DATA_HOME=$SCRATCH/data

CONFIG=$XDG_CONFIG_HOME/beerssh
mkdir -p "$CONFIG" "$HOME/.ssh"
cat > "$HOME/.ssh/beerssh_hosts" <<'HOSTS'
next_id=3

[alpha-1]
name=alpha
host=alpha.invalid
user=nobody

[beta-2]
name=beta
host=beta.invalid
user=nobody
HOSTS
printf '[layout]\nhosts=1\npanes=1\nactive=0\n' > "$CONFIG/layout.ini"
printf '[layout]\nhosts=2\npanes=1\nactive=0\n' > "$CONFIG/layout-1.ini"

window_ids() {
	for w in $(xwininfo -root -children 2>/dev/null | awk '/"beerssh"|BeerSSH"/{print $1}'); do
		xwininfo -id "$w" 2>/dev/null | grep -q IsViewable && echo "$w"
	done
}
count() { window_ids | wc -l; }
titles() {
	for w in $(window_ids); do
		xprop -id "$w" WM_NAME 2>/dev/null | sed 's/^[^"]*"//; s/"$//'
	done | sort
}
wait_for_count() {   # wait_for_count N: true once N windows are up and settled
	for _ in $(seq 60); do
		[ "$(count)" -eq "$1" ] && { sleep 2; [ "$(count)" -eq "$1" ] && return 0; }
		sleep 1
	done
	return 1
}
layouts() { ls "$CONFIG" | grep -E '^layout(-[0-9]+)?\.ini$' | wc -l; }
fail() { echo "check-windows: FAILED -- $1" >&2; titles | sed 's/^/check-windows:   window: /' >&2; exit 1; }

"$BIN" >/dev/null 2>&1 &
wait_for_count 2 || fail "a start showed $(count) window(s), not the 2 saved"
[ "$(pids | wc -l)" -eq 2 ] || fail "2 windows but $(pids | wc -l) process(es)"
titles | grep -q alpha || fail "the first saved window did not come back"
titles | grep -q beta || fail "the second saved window did not come back"
echo "check-windows: a start brought back both windows, one process each"

W=$(window_ids | head -1)
xdotool windowfocus "$W"; sleep 0.5
xdotool key --window "$W" ctrl+shift+n
wait_for_count 3 || fail "New Window left $(count) window(s), not 3"
[ "$(titles | grep -c alpha)" -eq 1 ] || fail "New Window brought back the alpha window"
[ "$(titles | grep -c beta)" -eq 1 ] || fail "New Window brought back the beta window"
echo "check-windows: New Window is a window of its own, not a saved one"

# Ended rather than closed, as a logout or a shutdown ends them: no window
# gets a closeEvent, so what comes back is what was saved along the way. A
# tab first, so there is something to save; the seeded files say nowhere
# where a window was, which is how the save is seen to have happened.
xdotool key --window "$W" ctrl+shift+t
sleep 3
for p in $(pids); do kill "$p"; done
for _ in $(seq 20); do [ "$(count)" -eq 0 ] && break; sleep 1; done
[ "$(count)" -eq 0 ] || fail "$(count) window(s) would not end"
sleep 1
[ "$(layouts)" -eq 2 ] || fail "ending left $(layouts) layouts, not 2"
grep -q geometry "$CONFIG"/layout*.ini || fail "no window saved where it was as it went"
echo "check-windows: ending them left the two with hosts in, as saved along the way"

"$BIN" >/dev/null 2>&1 &
wait_for_count 2 || fail "the second start showed $(count) window(s), not 2"
titles | grep -q alpha && titles | grep -q beta || fail "the second start lost a window"
echo "check-windows: passed"
