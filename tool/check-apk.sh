#!/bin/sh
# Does the APK actually install and run? (project.md sec 12.2)
#
# The same question check-deb.sh asks of the Debian package, on the
# platform where it is harder to answer and easier to get wrong. An APK
# that builds tells you nothing: a missing Qt plugin, a missing JAR, an
# ABI that does not match the device or a manifest the system rejects all
# produce a perfectly good-looking file that fails on first launch.
#
# Two of those have already happened in this project. The networkinformation
# plugin shipped without its JAR and fell back to a "dummy" backend that
# reported nothing; the foreground service broke a lifecycle contract and
# Android killed the process on the first connection failure. Both were
# invisible until the thing ran on a device.
#
# Installs OVER whatever is there rather than replacing it, deliberately:
# a check that wiped the host list is a check nobody runs twice.

set -eu

APK=${1:-}

if [ -z "$APK" ] || [ ! -f "$APK" ]; then
	echo "check-apk: no APK given. Run: make check-apk" >&2
	exit 1
fi

if ! command -v adb >/dev/null 2>&1; then
	echo "check-apk: adb is not on PATH." >&2
	echo "check-apk:   this needs a device or emulator to install onto." >&2
	exit 1
fi

devices=$(adb devices | grep -cw device || true)
if [ "$devices" -eq 0 ]; then
	echo "check-apk: no device on adb. Start an emulator or plug a phone in." >&2
	exit 1
fi

PACKAGE=${BSSH_PACKAGE:-se.vibes.beerssh}

echo "check-apk: installing $(basename "$APK") ($(du -h "$APK" | cut -f1))"
if ! adb install -r "$APK" > /tmp/bssh-install.log 2>&1; then
	if grep -q INSTALL_FAILED_UPDATE_INCOMPATIBLE /tmp/bssh-install.log; then
		# Not a broken package: the device holds a build signed with a
		# different key. The usual case is a debug-signed build and a
		# release-signed one, which Android will never let replace each
		# other.
		echo "check-apk: this device has $PACKAGE installed under a DIFFERENT signing key." >&2
		echo "check-apk:   Android will not replace one signature with another." >&2
		echo "check-apk:   Uninstall it first -- and note that removes its hosts," >&2
		echo "check-apk:   known-hosts and saved passwords:" >&2
		echo "check-apk:     adb uninstall $PACKAGE" >&2
		exit 1
	fi
	echo "check-apk: the APK did not install" >&2
	cat /tmp/bssh-install.log >&2
	exit 1
fi

adb shell am force-stop "$PACKAGE" >/dev/null 2>&1 || true
adb logcat -c >/dev/null 2>&1 || true

echo "check-apk: launching it"
adb shell monkey -p "$PACKAGE" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1

# Long enough for Qt to start, load its plugins and put a window up. A
# crash on launch happens well inside this.
i=0
while [ $i -lt 20 ]; do
	i=$((i + 1))
	sleep 1
	pid=$(adb shell pidof "$PACKAGE" 2>/dev/null | tr -d '\r')
	[ -n "$pid" ] && break
done

if [ -z "${pid:-}" ]; then
	echo "check-apk: it did not start" >&2
	adb logcat -d 2>&1 | grep -iE "fatal|AndroidRuntime|beerssh" | tail -20 >&2
	exit 1
fi

# Still alive a moment later: a Qt app that fails to find a plugin exits
# after its process has already appeared, so an immediate check passes on
# an app that is already gone.
sleep 4
pid_again=$(adb shell pidof "$PACKAGE" 2>/dev/null | tr -d '\r')
if [ -z "$pid_again" ]; then
	echo "check-apk: it started and then died" >&2
	adb logcat -d 2>&1 | grep -iE "fatal|AndroidRuntime|beerssh" | tail -20 >&2
	exit 1
fi

if adb logcat -d 2>&1 | grep -q "FATAL EXCEPTION"; then
	echo "check-apk: it is running, but something threw on the way up" >&2
	adb logcat -d 2>&1 | grep -A6 "FATAL EXCEPTION" | head -12 >&2
	exit 1
fi

# The window, not just the process. An app whose activity never resolved
# leaves a live process and nothing on screen.
if ! adb shell dumpsys window 2>/dev/null | grep -q "$PACKAGE"; then
	echo "check-apk: the process is up but no window belongs to it" >&2
	exit 1
fi

# And the bundled fonts, which is the packaging question the APK is
# actually answering: the app prints what it found (sec 9.4).
faces=$(adb logcat -d 2>&1 | grep -o "beerssh: [0-9]* font faces" | tail -1 | grep -o "[0-9]*" || true)
if [ -z "$faces" ] || [ "$faces" -lt 1 ]; then
	echo "check-apk: it started but found no bundled fonts -- assets are missing" >&2
	exit 1
fi

echo "check-apk: running as pid $pid_again, window up, $faces font faces from assets"

adb shell am force-stop "$PACKAGE" >/dev/null 2>&1 || true
echo "check-apk: the APK installs and runs on the attached device"
