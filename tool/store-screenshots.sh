#!/bin/sh
# The store listing's screenshots, taken on an emulator rather than on
# whatever phone happens to be plugged in (the holder, 2026-10-23).
#
# `make android-screenshots` (tool/android.mk) captures on the one device
# adb sees, or the one ANDROID_SERIAL names, and starts nothing itself. On
# the holder's phone that meant a lock screen, a PIN, the camera cutout,
# One UI ignoring SystemUI demo mode, and the real app restarted under
# them. An emulator has none of those, so the pictures come out the same
# every time and the phone is left alone.
#
# What this starts, and what stops it: ONE emulator, headless, on a port
# of its own (STORE_EMULATOR_PORT, 5584), refused if that port is already
# taken. The EXIT trap asks it to quit (`adb emu kill`), waits a bounded
# time for the process, and SIGKILLs it if it is still there -- so a
# failed run does not leave an emulator holding a few GB of memory. Every
# wait below has a ceiling.
#
#   make store-screenshots                 every view in STORE_VIEWS
#   STORE_AVD=other make store-screenshots another AVD (x86_64 only)

set -eu

AVD=${STORE_AVD:-bssh_api34}
PORT=${STORE_EMULATOR_PORT:-5584}
SERIAL=emulator-$PORT
SDK=${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}
ADB=$SDK/platform-tools/adb
EMULATOR=$SDK/emulator/emulator

for tool in "$ADB" "$EMULATOR"; do
	if [ ! -x "$tool" ]; then
		echo "store-screenshots: $tool is not there; set ANDROID_SDK_ROOT." >&2
		exit 1
	fi
done
if ! "$EMULATOR" -list-avds 2>/dev/null | grep -qx "$AVD"; then
	echo "store-screenshots: no AVD named $AVD; set STORE_AVD." >&2
	exit 1
fi

# A port somebody else's emulator holds is theirs: refused, not reused.
if "$ADB" devices | grep -q "^$SERIAL[[:space:]]"; then
	echo "store-screenshots: $SERIAL is already running; stop it or set STORE_EMULATOR_PORT." >&2
	exit 1
fi

LOG=$(mktemp)
EMU_PID=

# Alive and not a zombie. `kill -0` answers yes for a zombie, and the
# emulator is this script's own child, so once it exits it stays one
# until reaped -- running-code.md has the rule.
alive() {
	[ -n "$1" ] && [ -r "/proc/$1/status" ] && ! grep -q '^State:[[:space:]]*Z' "/proc/$1/status"
}

stop_emulator() {
	if alive "$EMU_PID"; then
		timeout 20 "$ADB" -s "$SERIAL" emu kill >/dev/null 2>&1 || true
		waited=0
		while alive "$EMU_PID" && [ "$waited" -lt 30 ]; do
			sleep 1
			waited=$((waited + 1))
		done
		if alive "$EMU_PID"; then
			echo "store-screenshots: the emulator did not quit; killing it" >&2
			kill -9 -- "-$EMU_PID" 2>/dev/null || kill -9 "$EMU_PID" 2>/dev/null || true
		fi
	fi
	rm -f "$LOG"
}
trap stop_emulator EXIT
trap 'exit 130' INT TERM

echo "store-screenshots: starting $AVD headless on $SERIAL"
# A session of its own, so the fallback below can SIGKILL the whole group:
# `emulator` execs into qemu-system, which starts a netsimd child in its
# process group (measured 2026-10-23), and killing qemu alone would leave
# that behind.
setsid "$EMULATOR" -avd "$AVD" -port "$PORT" -no-window -no-audio -no-boot-anim \
	-no-snapshot-save -gpu swiftshader_indirect > "$LOG" 2>&1 &
EMU_PID=$!

waited=0
until [ "$(timeout 5 "$ADB" -s "$SERIAL" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = 1 ]; do
	if ! alive "$EMU_PID"; then
		echo "store-screenshots: the emulator exited while booting:" >&2
		tail -5 "$LOG" >&2
		exit 1
	fi
	if [ "$waited" -ge 300 ]; then
		echo "store-screenshots: $AVD did not finish booting in 300s" >&2
		exit 1
	fi
	sleep 5
	waited=$((waited + 5))
done
echo "store-screenshots: booted in about ${waited}s"

# A cold boot under software rendering puts up "System UI isn't
# responding" a little after boot_completed (seen 2026-10-22). Given time
# to settle, then any system dialog is asked to close; the target's own
# focus check refuses to capture if one is still in front.
sleep 20
"$ADB" -s "$SERIAL" shell am broadcast -a android.intent.action.CLOSE_SYSTEM_DIALOGS >/dev/null 2>&1 || true
"$ADB" -s "$SERIAL" shell input keyevent KEYCODE_HOME >/dev/null 2>&1 || true

# The emulator's ABI, and the kit for it found by the fragment, which
# looks for one when QT_ANDROID_ROOT is not set -- so a phone's arm64 kit
# in the environment is put aside for this run.
unset QT_ANDROID_ROOT
ANDROID_SERIAL=$SERIAL ANDROID_ABI=x86_64 \
	timeout 2400 make --no-print-directory android-screenshots ANDROID_ABI=x86_64
