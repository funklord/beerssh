#!/bin/bash
# How long BeerSSH takes to show its first window where no keyring answers
# (project.md sec 11): with no session bus at all, and with a private bus
# whose secrets service is activatable but never comes up.
#
# Both took 27 seconds once. The settings page named the secret store while
# the main window was being built, and asking which store there is probed
# for a keyring the way D-Bus does -- waiting out its 25-second default, or
# autolaunching a bus nothing reaps. The probe is bounded and off the
# startup path now; measured when written, 0.42s in both cases.
#
# Not part of `check`: it needs Xvfb, xwininfo and dbus-run-session. Fails
# above BSSH_STARTUP_LIMIT seconds (default 5), and when a window never
# appears at all. Bounded: a 60-second poll per case, and everything it
# starts is killed and its scratch removed however it exits.
set -u

cd "$(dirname "$0")/.." || exit 1
BIN=${BSSH_BIN:-build/beerssh}
LIMIT=${BSSH_STARTUP_LIMIT:-5}

for tool in Xvfb xwininfo dbus-run-session bc; do
	if ! command -v "$tool" >/dev/null 2>&1; then
		echo "check-startup: $tool is not installed, so nothing was measured" >&2
		exit 1
	fi
done
if [ ! -x "$BIN" ]; then
	echo "check-startup: no $BIN -- build it first (make beerssh)" >&2
	exit 1
fi

# One case: start on a private display, poll for the first window.
measure() {
	local scratch xvfb app display start end found
	scratch=$(mktemp -d "${TMPDIR:-/tmp}/bssh-startup.XXXXXX") || return 1
	display=
	for n in 80 81 82 83 84 85 86 87 88 89; do
		[ -e "/tmp/.X11-unix/X$n" ] || { display=:$n; break; }
	done
	[ -n "$display" ] || { echo "check-startup: no free display :80-:89" >&2; return 1; }

	Xvfb "$display" -screen 0 1600x900x24 -nolisten tcp >/dev/null 2>&1 & xvfb=$!
	sleep 2
	start=$(date +%s.%N)
	DISPLAY=$display HOME=$scratch XDG_CONFIG_HOME=$scratch/config XDG_DATA_HOME=$scratch/data \
		"$BIN" --geometry 60x20+0+0 >/dev/null 2>&1 & app=$!
	found=no
	for _ in $(seq 600); do
		if DISPLAY=$display xwininfo -root -children 2>/dev/null | grep -q 'BeerSSH"'; then
			found=yes
			break
		fi
		sleep 0.1
	done
	end=$(date +%s.%N)

	kill "$app" 2>/dev/null; sleep 1; kill -9 "$app" 2>/dev/null
	kill "$xvfb" 2>/dev/null
	case "$scratch" in "${TMPDIR:-/tmp}"/bssh-startup.*) rm -rf "$scratch" ;; esac

	[ "$found" = yes ] || { echo "none"; return 0; }
	echo "$end - $start" | bc
}

if [ "${1:-}" = --measure ]; then
	measure
	exit $?
fi

status=0
check() {   # label, seconds
	if [ "$2" = none ]; then
		echo "check-startup: FAILED -- $1: no window in 60 seconds" >&2
		status=1
	elif [ "$(echo "$2 > $LIMIT" | bc)" = 1 ]; then
		echo "check-startup: FAILED -- $1: first window after ${2}s (limit ${LIMIT}s)" >&2
		status=1
	else
		echo "check-startup: $1: first window after ${2}s"
	fi
}

check "no session bus" "$(env -u DBUS_SESSION_BUS_ADDRESS timeout 90 "$0" --measure)"
check "private bus, no keyring" \
	"$(timeout 120 dbus-run-session -- "$0" --measure 2>/dev/null | tail -1)"

[ "$status" = 0 ] && echo "check-startup: passed"
exit "$status"
