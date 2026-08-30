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

if ! command -v docker >/dev/null 2>&1; then
	echo "check-deb: docker is not installed." >&2
	echo "check-deb:   a clean machine is the whole point of this check." >&2
	exit 1
fi

# Built from HEAD in a throwaway export rather than in the tree.
#
# dpkg-buildpackage builds where it is run, so this check had no staging
# step at all. check-ci and check-keepalive were each given one; this was
# the third door into the same room, and the last of the three.
#
# Not theoretical. A run here was building while another session committed
# underneath it, and the package that came out was made of sources from
# both sides of that commit -- a tree that never existed, reported on as
# though it had. The other two scripts are immune because git archive
# reads the object store; this one had to be given the same footing.
#
# fonts/ is SYMLINKED rather than exported. It is hundreds of megabytes of
# upstream release archives, deliberately untracked (sec 9), so git
# archive cannot carry it and copying it would cost more than the build
# does. Read-only here, and dh_install follows it.
#
# DEB_DIR still overrides, for checking packages that are already built:
#   make deb && DEB_DIR=build/deb ./tool/check-deb.sh
# which is the faster loop when the question is about the container half
# rather than about the sources.
# A NAME AND A KILL, because the trap below cleans up what this SHELL owns
# and a container is not that. A container is a child of the docker daemon,
# so a `timeout` around this script, or a Ctrl-C, signals the shell and the
# daemon hears nothing: the container carries on, reparented and
# unsupervised, and `docker ps` shows it "Up" minutes later.
#
# Measured 2026-08-31 on check-ci: a ten-minute wrapper killed make and left
# an ubuntu:24.04 container compiling. Worse than a wasted run, because the
# receipt that gates a push is written after the container exits under
# make's supervision -- so the orphan finishes nothing that counts. A bound
# that stops the supervisor while the work continues is worse than no bound:
# it turns a slow success into an invisible one.
#
# `docker kill` on a container that has already exited returns non-zero, so
# every status here is discarded -- under `set -e` a failing trap command is
# not something to let decide this script's exit code.
#
# Set ABOVE the branch below, not inside it. The DEB_DIR path skips the
# staging tree entirely, and a name assigned only in the other branch
# would leave `docker run --name "$CONTAINER"` reading an unset
# variable under `set -u` -- which `sh -n` cannot see, because it is a
# runtime error and not a syntax one.
CONTAINER=bssh-check-deb-$$
STAGE=
trap 'docker kill "$CONTAINER" >/dev/null 2>&1 || true; [ -n "$STAGE" ] && rm -rf "$STAGE"; true' EXIT INT TERM

if [ -n "${DEB_DIR:-}" ]; then
	echo "check-deb: using the packages already in $DEB_DIR"
else
	STAGE=$(mktemp -d)

	git archive --format=tar --prefix=src/ HEAD | tar -xf - -C "$STAGE"
	git -C vterm archive --format=tar --prefix=src/vterm/ HEAD \
		| tar -xf - -C "$STAGE"
	ln -s "$PWD/fonts" "$STAGE/src/fonts"

	echo "check-deb: building $(git rev-parse --short HEAD) in a throwaway export"

	# DEB_DIR named rather than defaulted, so this knows where to look
	# without repeating the Makefile's spelling of it -- a path written in
	# two places is one that will disagree with itself, which is how this
	# check spent a while pointed at the directory above its packages.
	if ! ( cd "$STAGE/src" && make deb DEB_DIR=out ) >"$STAGE/build.log" 2>&1; then
		echo "check-deb: the package build failed in the export" >&2
		tail -30 "$STAGE/build.log" >&2
		exit 1
	fi
	DEB_DIR="$STAGE/src/out"
fi

if ! ls "$DEB_DIR"/*.deb >/dev/null 2>&1; then
	echo "check-deb: no packages in $DEB_DIR. Run: make deb" >&2
	exit 1
fi

echo "check-deb: installing $(ls "$DEB_DIR"/*.deb | wc -l) package(s) into $IMAGE"

docker run --rm --name "$CONTAINER" -v "$(cd "$DEB_DIR" && pwd)":/in:ro "$IMAGE" sh -c '
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

# The manual page. It is GENERATED at build time from the option table
# (sec 8.6), so what is worth checking is that the generation ran and the
# result installed -- a package that quietly shipped without one would
# still install, run and uninstall.
if [ ! -f /usr/share/man/man1/beerssh.1 ] && [ ! -f /usr/share/man/man1/beerssh.1.gz ]; then
	echo "check-deb: the package has no manual page" >&2
	exit 1
fi

# And that it describes the options this program takes, rather than an
# empty skeleton. --execute is the entry nothing else would produce.
if ! zcat -f /usr/share/man/man1/beerssh.1* | grep -q "execute"; then
	echo "check-deb: the manual page does not document the options" >&2
	exit 1
fi
echo "check-deb: the manual page is installed and documents the options"

# --help with no display, which is how it is read over SSH and in a build
# chroot -- and how the manual page above was generated. This died with
# "could not connect to display" until QApplication stopped being built
# before the printing options were answered.
if ! beerssh --help > /tmp/help.log 2>&1; then
	echo "check-deb: --help failed with no display" >&2
	cat /tmp/help.log >&2
	exit 1
fi
grep -q "^Usage: beerssh" /tmp/help.log || {
	echo "check-deb: --help printed something that is not a usage" >&2
	cat /tmp/help.log >&2
	exit 1
}
echo "check-deb: --help works with no display"

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
