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
trap 'docker kill "$CONTAINER" >/dev/null 2>&1 || true; [ -n "$STAGE" ] && rm -rf "$STAGE"; true' EXIT
# INT and TERM only exit; `exit` runs the EXIT trap above, so the
# cleanup lives in exactly one place.
trap 'exit 130' INT TERM

if [ -n "${DEB_DIR:-}" ]; then
	echo "check-deb: using the packages already in $DEB_DIR"
else
	STAGE=$(mktemp -d)

	git archive --format=tar --prefix=src/ HEAD | tar -xf - -C "$STAGE"
	git -C vterm archive --format=tar --prefix=src/vterm/ HEAD \
		| tar -xf - -C "$STAGE"
	git -C softkeys archive --format=tar --prefix=src/softkeys/ \
		"$(git rev-parse HEAD:softkeys)" | tar -xf - -C "$STAGE"
	# libssh by its gitlink, unpatched, as check-ci stages it: the build
	# applies libssh-patch/ itself, and an export is not a git checkout,
	# so a missing submodule could not be fetched there (project.md 8.4).
	git -C libssh archive --format=tar --prefix=src/libssh/ \
		"$(git rev-parse HEAD:libssh)" | tar -xf - -C "$STAGE"
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
apt-get install -y -qq desktop-file-utils appstream lintian >/dev/null 2>&1

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

# The bash completion, on the same footing and for the same reason: it is
# generated at build time, so what is worth checking here is that the
# generation ran and the result reached the package.
#
# Sourced rather than grepped, because a completion that does not PARSE
# is one every new shell of everybody who installed it fails to load --
# and the failure lands in their prompt rather than anywhere near a
# build. `complete -p beerssh` then asks bash itself whether the file
# registered anything, which is the question a grep for the word
# "complete" cannot answer about a line that is commented out.
completion=/usr/share/bash-completion/completions/beerssh
if [ ! -f "$completion" ]; then
	echo "check-deb: the package has no bash completion" >&2
	exit 1
fi

if ! bash -c ". \"$completion\" && complete -p beerssh >/dev/null"; then
	echo "check-deb: the bash completion does not register a completion" >&2
	exit 1
fi
echo "check-deb: the bash completion is installed and registers"

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

# And the AppStream metadata beside it, which shipped unchecked until
# 2026-09-21 and did not validate: appstreamcli wanted a developer and a
# homepage, and nothing here ever asked it.
#
# NOTE: this whole script body is one single-quoted argument, so nothing
# below may contain an apostrophe. The first version of this comment did
# and ended the quote 60 lines early; bash -n caught it.
#
# The status alone cannot gate this. appstreamcli exits 3 for ANY issue,
# and one of them is url-homepage-missing, which cannot be answered while
# the repository is private because a homepage there would send a user to
# a 404. Absent is the correct value today and the warning is right to
# raise it.
#
# So the known issues are asserted as a SET rather than ignored: any error
# fails, and any warning that is not the homepage one fails. An ignore
# list would let the next warning in silently, which is how a gate gets
# switched off by instalments; naming the whole expected population means
# a NEW complaint breaks the build and the known one does not.
#
# When the repository goes public the URL goes in and the warning stops.
metainfo=/usr/share/metainfo/se.vibes.beerssh.metainfo.xml

# The tool has to BE here, and its absence has to fail.
#
# The first version of this check ran appstreamcli with a trailing
# || true and then grepped the output for E: and W: lines. appstreamcli
# is not in debian:trixie, so the command failed, the log held a shell
# error instead of findings, the greps matched nothing, and the check
# printed that the metadata validates -- a gate over an empty list,
# reporting success exactly as loudly as a real pass. It shipped that way
# for one commit. A guard whose failure is not the callers failure is not
# a guard.
command -v appstreamcli >/dev/null || {
	echo "check-deb: appstreamcli is missing, so nothing validated the" >&2
	echo "check-deb:   AppStream metadata. Refusing to report a pass." >&2
	exit 1
}

appstreamcli validate "$metainfo" > /tmp/metainfo.log 2>&1 || true

# And it has to have SAID something. An empty log means it ran and
# reported nothing, which for this tool means it did not read the file.
if [ ! -s /tmp/metainfo.log ]; then
	echo "check-deb: appstreamcli produced no output at all" >&2
	exit 1
fi

if grep -q "^E:" /tmp/metainfo.log; then
	echo "check-deb: the AppStream metadata has errors" >&2
	cat /tmp/metainfo.log >&2
	exit 1
fi

unexpected=$(grep "^W:" /tmp/metainfo.log | grep -vc "url-homepage-missing" || true)
if [ "$unexpected" -ne 0 ]; then
	echo "check-deb: an AppStream warning that is not the known missing" >&2
	echo "check-deb:   homepage (the repository is private):" >&2
	cat /tmp/metainfo.log >&2
	exit 1
fi
echo "check-deb: the AppStream metadata validates, bar the known homepage"

# lintian over the installed packages, which is the standard check for
# this format and was not run at all. It is the tool that WOULD have
# caught the metainfo shipping invalid: its one complaint about this
# package is appstream-metadata-validation-failed, raised because
# appstreamcli exits non-zero for any issue including the homepage that
# cannot be answered while the repository is private.
#
# Same shape as above: errors fail, and the expected warnings are named
# as a set so a NEW tag breaks the build while the known one does not.
command -v lintian >/dev/null || {
	echo "check-deb: lintian is missing, so nothing checked the packages" >&2
	exit 1
}

lintian /in/*.deb > /tmp/lintian.log 2>&1 || true

if grep -q "^E:" /tmp/lintian.log; then
	echo "check-deb: lintian reports errors" >&2
	cat /tmp/lintian.log >&2
	exit 1
fi

# The expected tags, each named with the reason it is expected. This is a
# POPULATION, not an ignore list: every entry has to be justified here, and
# a tag that is not in it fails the build.
#
#   appstream-metadata-validation-failed
#       the homepage cannot be answered while the repository is private
#   duplicate-font-file
#       sec 9.2 ships a curated, PINNED set on purpose rather than
#       depending on the distributions font packages, which is the whole
#       point of pinning: version skew is what it exists to avoid
#   debug-file-with-no-debug-symbols
#       NOT deliberate and NOT settled. dh_auto_build runs the Makefile,
#       which wraps qmake, so Debian build flags never reach
#       QMAKE_CXXFLAGS_RELEASE and nothing emits -g -- so the -dbgsym
#       package ships a debug file with nothing in it. Fixing it is a
#       packaging decision with a trade: -g globally also inflates a
#       from-source make install, and scoping it to packaging needs a
#       hook the Makefile does not have. Recorded in project.md sec 12.2
#       as a live finding, tolerated here so the gate can run at all,
#       and this line is what somebody deletes when it is settled.
expected="appstream-metadata-validation-failed|duplicate-font-file|debug-file-with-no-debug-symbols"
strays=$(grep "^W:" /tmp/lintian.log | grep -Evc "$expected" || true)
if [ "$strays" -ne 0 ]; then
	echo "check-deb: a lintian warning outside the expected set:" >&2
	cat /tmp/lintian.log >&2
	exit 1
fi
echo "check-deb: lintian reports only the expected tags"

# The icon the entry names has to be the icon that shipped, or the app has
# no icon anywhere it matters (sec 12.2).
icon=$(sed -n "s/^Icon=//p" /usr/share/applications/se.vibes.beerssh.desktop)
if ! ls /usr/share/icons/hicolor/*/apps/"$icon".* >/dev/null 2>&1; then
	echo "check-deb: the desktop entry names an icon that is not installed: $icon" >&2
	exit 1
fi
echo "check-deb: the icon it names is installed"

# beersshd, the SFTP server, packaged apart from the terminal (sec 19).
# --help and its manual page come from one option table, so both are read
# here: --host-key is in the options and nowhere else on the page, where
# --root is in the synopsis too and would pass an empty OPTIONS section.
if ! beersshd --version > /tmp/sshd.log 2>&1; then
	echo "check-deb: the installed beersshd did not run" >&2
	cat /tmp/sshd.log >&2
	exit 1
fi
if ! zcat -f /usr/share/man/man1/beersshd.1* 2>/dev/null | grep -q "host.-key"; then
	echo "check-deb: beersshd has no manual page documenting its options" >&2
	exit 1
fi
echo "check-deb: beersshd runs and its manual page is installed"

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

# Separate packages means neither needs the other: with the terminal gone
# the server is still installed and still runs. A Depends on beerssh would
# have taken it out with the remove above.
if ! /usr/bin/beersshd --version >/dev/null 2>&1; then
	echo "check-deb: removing beerssh took beersshd with it" >&2
	exit 1
fi
apt-get remove -y -qq beersshd >/dev/null 2>&1
if [ -e /usr/bin/beersshd ]; then
	echo "check-deb: uninstalling left beersshd behind" >&2
	exit 1
fi
echo "check-deb: both uninstall cleanly, and neither needs the other"
' &
# `wait` rather than a plain call, because a shell trap CANNOT preempt a
# foreground command -- it runs only once that command returns. Measured:
# a script sleeping in the foreground ignores SIGTERM until the sleep ends,
# while the same script backgrounding it and waiting fires its trap at once.
# So the kill below was unreachable in the one case it was written for. It
# ran on the normal exit path, where the container has already gone and
# killing it is a no-op, and never on the interruption path.
#
# It is worth knowing what this still does NOT buy. An outer
# `timeout N sg docker -c "make check-ci"` signals `sg`, and nothing
# downstream hears it: measured, a `timeout 75` sat at 2m45s with the
# script and the container both running underneath. Signal THIS script, or
# kill the container by the name above -- which is what the name is for.
wait "$!"

echo "check-deb: the package installs, runs and uninstalls on a clean $IMAGE"
