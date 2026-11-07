#!/bin/sh
# Drives an EMULATOR through what a fold does to the app, again and again, and
# fails if Qt's main thread stays in its wait for a surface (project.md sec 5.6).
#
#   ANDROID_SERIAL=emulator-5602 tool/check-surface-wait.sh [cycles]
#
# First, by hand: a foldable AVD or any other, BeerSSH on screen, a tab showing
# steady output (`while date; do sleep 0.2; done` in a shell). Painting is what
# reaches the wait, so an idle window proves nothing. About 1 cycle in 12 blocked
# before the fix; each is about 12 s. Refuses a real phone: it changes the display
# size and the screen state, and a shared phone is somebody's.
#
# Stops by itself after the cycles, or at the first block. It leaves the display
# reset, except when it stops on a block, where the state is the evidence.

A=${ADB:-$HOME/Android/Sdk/platform-tools/adb}
P=se.vibes.beerssh
cycles=${1:-40}

case "$ANDROID_SERIAL" in
emulator-*) ;;
*) echo "check-surface-wait: ANDROID_SERIAL must name an emulator" >&2; exit 2;;
esac

pid=$($A shell pidof $P | tr -d '\r')
[ -n "$pid" ] || { echo "check-surface-wait: BeerSSH is not running" >&2; exit 2; }
$A logcat -c

# The switch count and wait channel of the thread with the most switches among
# those Qt names for the main loop: the others are GL workers and the watchdog.
main_thread() {
	$A shell "run-as $P sh -c 'for t in /proc/$pid/task/*; do [ \"\$(cat \$t/comm)\" = qtMainLoopThrea ] && echo \$(grep -m1 ^voluntary \$t/status | cut -f2) \$(cat \$t/wchan); done'" |
		tr -d '\r' | sort -n | tail -1
}

# In a wait twice over, and still there past the watchdog's 4 s and its 2 s poll.
check() {
	first=$(main_thread); sleep 1; second=$(main_thread)
	echo "  $1: $first | $second"
	case "$first$second" in
	*futex*futex*)
		sleep 9; third=$(main_thread)
		case "$third" in
		*futex*) echo "check-surface-wait: BLOCKED: $third" >&2; exit 1;;
		esac
		echo "  freed: $third";;
	esac
}

i=0
while [ $i -lt "$cycles" ]; do
	i=$((i + 1))
	case $((i % 5)) in 0) gap=0;; 1) gap=0.1;; 2) gap=0.3;; 3) gap=0.8;; *) gap=2;; esac
	mode=$((i % 3))
	echo "cycle $i: gap $gap, mode $mode"
	[ $mode = 1 ] && $A shell input keyevent HOME
	$A emu fold >/dev/null
	[ $mode = 2 ] && $A shell input keyevent SLEEP
	$A shell "wm size 840x2289; wm density 340"
	sleep $gap
	[ $mode = 0 ] && $A shell input keyevent SLEEP
	sleep $gap
	check folded
	$A emu unfold >/dev/null
	$A shell input keyevent WAKEUP
	$A shell "wm size reset; wm density reset; wm dismiss-keyguard"
	sleep $gap
	[ $mode = 1 ] && $A shell input keyevent HOME
	check opened
	$A shell monkey -p $P -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
	sleep 1
	check front
done

[ "$($A shell pidof $P | tr -d '\r')" = "$pid" ] || {
	echo "check-surface-wait: the process changed" >&2
	exit 1
}
freed=$($A logcat -d | grep -c 'spare surface to [1-9]')
echo "check-surface-wait: passed, $cycles cycles, $freed time(s) freed"
