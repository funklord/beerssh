#!/bin/sh
# Does the Debian package actually install and run? (project.md sec 12.2)
#
# `make deb` producing two .deb files says nothing about whether either of
# them works. The failure that matters is not a broken build -- that is
# loud -- but a package that installs onto the developer's own machine,
# where every library it needs is already present for other reasons, and
# then fails at the first launch on anybody else's. The Depends line is
# generated from the binary for exactly that reason, and this is what
# proves the generated line is complete.
#
# So: a container with nothing in it, apt asked to resolve the package's
# own declared dependencies, and then the app RUN. --self-test builds the
# real window, pumps the event loop and exits, which is the cheapest thing
# that fails if a library, a plugin or a font path is missing.
#
# Deliberately a different distribution from check-ci's: that one pins the
# CI runner's image, this one asks whether the package is portable.

set -eu

IMAGE=${BSSH_DEB_IMAGE:-debian:trixie}
DEB_DIR=${DEB_DIR:-build-deb}

if ! command -v docker >/dev/null 2>&1; then
	echo "check-deb: docker is not installed." >&2
	echo "check-deb:   a clean machine is the whole point of this check." >&2
	exit 1
fi

if ! ls "$DEB_DIR"/*.deb >/dev/null 2>&1; then
	echo "check-deb: no packages in $DEB_DIR. Run: make deb" >&2
	exit 1
fi

echo "check-deb: installing $(ls "$DEB_DIR"/*.deb | wc -l) package(s) into $IMAGE"

docker run --rm -v "$(cd "$DEB_DIR" && pwd)":/in:ro "$IMAGE" sh -c '
set -e
apt-get update -qq >/dev/null 2>&1
apt-get install -y -qq desktop-file-utils >/dev/null 2>&1

# apt rather than dpkg: dpkg -i would leave the dependencies unresolved
# and report success, which is the failure this exists to catch.
if ! apt-get install -y -qq /in/*.deb > /tmp/apt.log 2>&1; then
	echo "check-deb: the packages did not install" >&2
	tail -30 /tmp/apt.log >&2
	exit 1
fi

command -v beerssh >/dev/null || { echo "check-deb: no beerssh on PATH" >&2; exit 1; }

# The app, actually started. A missing library, a missing Qt platform
# plugin or an unreadable font directory all fail here and nowhere
# earlier.
if ! QT_QPA_PLATFORM=offscreen beerssh --self-test > /tmp/run.log 2>&1; then
	echo "check-deb: the installed binary did not run" >&2
	cat /tmp/run.log >&2
	exit 1
fi

# It must find the fonts through the INSTALLED layout, not a build tree.
# The line is one the app prints itself, so this checks the app agrees.
faces=$(grep -o "beerssh: [0-9]* font faces" /tmp/run.log | grep -o "[0-9]*" || echo 0)
if [ "$faces" -lt 1 ]; then
	echo "check-deb: the installed app found no bundled fonts" >&2
	cat /tmp/run.log >&2
	exit 1
fi
echo "check-deb: the app started and loaded $faces font faces"

desktop-file-validate /usr/share/applications/se.vibes.beerssh.desktop
echo "check-deb: the desktop entry validates"

# The icon the entry names has to be the icon that shipped, or the app has
# no icon anywhere it matters (sec 12.2).
icon=$(sed -n "s/^Icon=//p" /usr/share/applications/se.vibes.beerssh.desktop)
if ! ls /usr/share/icons/hicolor/*/apps/"$icon".* >/dev/null 2>&1; then
	echo "check-deb: the desktop entry names an icon that is not installed: $icon" >&2
	exit 1
fi
echo "check-deb: the icon it names is installed"

# And removing it leaves nothing behind, which is the other half of what a
# package is for.
apt-get remove -y -qq beerssh beerssh-fonts >/dev/null 2>&1

# Tested as a FILE. `command -v` consults the shell hash table, which
# still holds the path apt has just deleted -- this reported the package
# as leaving its binary behind when it had removed it correctly.
if [ -e /usr/bin/beerssh ]; then
	echo "check-deb: uninstalling left the binary behind" >&2
	exit 1
fi
echo "check-deb: it uninstalls cleanly"
'

echo "check-deb: the package installs, runs and uninstalls on a clean $IMAGE"
