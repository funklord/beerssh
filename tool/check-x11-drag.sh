#!/bin/bash
# A pane dragged onto ANOTHER BeerSSH process's window is refused on X11
# (project.md sec 8.4), and the same drag onto nothing still tears out.
#
# Two processes on a private X server and a private session bus. A's pane
# is Alt-dragged onto B and released: A must keep its one window and B its
# one. Then the same drag onto the bare background: A must gain a window --
# the control that shows this can see a tear-out at all, without which an
# unchanged count would prove nothing. Measured when written: with the
# refusal removed, the first drag tore out (A had 2).
#
# Not part of `check`: it needs Xvfb, xdotool, xprop/xwininfo and
# dbus-run-session, and it drives a real pointer. Each start takes about
# half a minute here, the secret service activation timing out on a bus
# with no keyring (sec 8.4 records it).
#
# Bounded: every wait has a ceiling, and the trap kills both processes and
# the X server and removes the scratch directory however this exits.
set -u

cd "$(dirname "$0")/.." || exit 1
BIN=${BSSH_BIN:-build/beerssh}

for tool in Xvfb xdotool xprop xwininfo dbus-run-session; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "check-x11-drag: $tool is not installed, so nothing was checked" >&2
		exit 1
	fi
done
if [ ! -x "$BIN" ]; then
	echo "check-x11-drag: no $BIN -- build it first (make beerssh)" >&2
	exit 1
fi

# Under its own bus, so the processes do not autolaunch one that outlives it.
if [ -z "${BSSH_X11_DRAG_INNER:-}" ]; then
	BSSH_X11_DRAG_INNER=1 exec timeout 300 dbus-run-session -- "$0" "$@"
fi

SCRATCH=$(mktemp -d "${TMPDIR:-/tmp}/bssh-x11-drag.XXXXXX") || exit 1
A=; B=; XVFB=
cleanup() {
	for p in $A $B; do kill "$p" 2>/dev/null; done
	sleep 1
	for p in $A $B; do kill -9 "$p" 2>/dev/null; done
	[ -n "$XVFB" ] && kill "$XVFB" 2>/dev/null
	case "$SCRATCH" in "${TMPDIR:-/tmp}"/bssh-x11-drag.*) rm -rf "$SCRATCH" ;; esac
}
trap cleanup EXIT

D=
for n in 90 91 92 93 94 95 96 97 98 99; do
	[ -e "/tmp/.X11-unix/X$n" ] || { D=:$n; break; }
done
[ -n "$D" ] || { echo "check-x11-drag: no free display :90-:99" >&2; exit 1; }

Xvfb "$D" -screen 0 1600x900x24 -nolisten tcp >/dev/null 2>&1 & XVFB=$!
sleep 2
export DISPLAY=$D HOME=$SCRATCH XDG_CONFIG_HOME=$SCRATCH/config XDG_DATA_HOME=$SCRATCH/data

# A process's viewable top-level BeerSSH windows, by _NET_WM_PID.
pid_windows() {
	for w in $(xwininfo -root -children 2>/dev/null | awk '/BeerSSH"/{print $1}'); do
		xwininfo -id "$w" 2>/dev/null | grep -q IsViewable || continue
		[ "$(xprop -id "$w" _NET_WM_PID 2>/dev/null | awk '{print $3}')" = "$1" ] && echo "$w"
	done
}
windows() { pid_windows "$1" | wc -l; }
centre() { ( eval "$(xdotool getwindowgeometry --shell "$1")"; echo "$((X + WIDTH / 2)) $((Y + HEIGHT / 2))" ); }

"$BIN" --geometry 60x20+0+0 >/dev/null 2>&1 & A=$!
"$BIN" --geometry 60x20+900+0 >/dev/null 2>&1 & B=$!
for _ in $(seq 60); do
	[ "$(windows $A)" -ge 1 ] && [ "$(windows $B)" -ge 1 ] && break
	sleep 1
done
WA=$(pid_windows $A | head -1)
WB=$(pid_windows $B | head -1)
if [ -z "$WA" ] || [ -z "$WB" ]; then
	echo "check-x11-drag: the two windows never appeared" >&2
	exit 1
fi

# A second session in A, so A has something it is allowed to tear out.
xdotool windowfocus "$WA"
xdotool key --window "$WA" ctrl+shift+t
sleep 2

drag() {   # Alt-drag from A's centre to $1 $2, and let go
	local fx fy
	read -r fx fy < <(centre "$WA")
	xdotool mousemove "$fx" "$fy"; sleep 0.3
	xdotool keydown alt; sleep 0.1
	xdotool mousedown 1; sleep 0.2
	for step in 1 2 3 4 5 6 7 8; do
		xdotool mousemove "$((fx + ($1 - fx) * step / 8))" "$((fy + ($2 - fy) * step / 8))"
		sleep 0.15
	done
	xdotool mouseup 1; sleep 0.1
	xdotool keyup alt; sleep 1.5
}

status=0
read -r bx by < <(centre "$WB")
drag "$bx" "$by"
echo "check-x11-drag: onto B -- A has $(windows $A) window(s), B has $(windows $B)"
if [ "$(windows $A)" -ne 1 ] || [ "$(windows $B)" -ne 1 ]; then
	echo "check-x11-drag: FAILED -- a drop on another BeerSSH was not refused" >&2
	status=1
fi

drag 1500 850
echo "check-x11-drag: onto nothing -- A has $(windows $A) window(s)"
if [ "$(windows $A)" -ne 2 ]; then
	echo "check-x11-drag: FAILED -- the same drag onto nothing did not tear out," >&2
	echo "check-x11-drag:   so the check above saw nothing either way" >&2
	status=1
fi

[ "$status" = 0 ] && echo "check-x11-drag: passed"
exit "$status"
