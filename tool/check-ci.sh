#!/bin/sh
# Reproduce the Linux CI job locally, in the image CI actually uses.
#
# project.md sec 12.4. The runner is older than any machine this gets
# written on -- Ubuntu 24.04 ships Qt 6.4.2 and libssh 0.10.6 -- so an API
# added in a later version compiles perfectly here and breaks the build
# there. That has happened twice: Qt::ColorScheme (6.5+) in the platform
# interface, and ssh_channel_get_exit_state (libssh 0.11+) in the
# shell-exit path, which failed on the runner for two days before anybody
# ran this.
#
# The package list is READ OUT OF THE WORKFLOW rather than repeated here.
# That is the point: the failure this catches is a dependency that the
# runner image happens to carry and the workflow never names, so a second
# copy of the list would be a second thing to be wrong. It has already
# caught one -- libsecret-1-dev was satisfied for months by luck.
#
# Run by hand, `make check-ci`, and by nothing else. It gated every push
# until 2026-10-22, when the repository went public, GitHub's own linux job
# -- the thing this reproduces -- became free and was switched back on, and
# the holder decided: keep the check, stop running it (project.md sec 12.4).
# Use it to reproduce a CI failure locally, or before pushing something that
# leans on a platform or library API, to hear about it before CI does.

set -eu

IMAGE=${BSSH_CI_IMAGE:-ubuntu:24.04}
WORKFLOW=.github/workflows/build.yml

# Two engines run the same job, `docker` and `hull` (sec 12.4, the holder's
# word 2026-11-09). Both stay until they have agreed on enough commits for
# hull to replace docker; until then docker is the default.
#
# hull because docker cannot be stopped: the container belongs to the
# daemon, so killing this script left one compiling (the trap's comment
# below has the measurement). A hull machine goes with the `hull run` that
# started it, killed or not -- measured 2026-10-22, SIGKILL to the run and
# to its whole process group each leaving nothing on the host. And its
# store lives under $HOME, where docker's images and this script's staging
# share `/` with everything else; on the machine this was written on, `/`
# had 3 GB free and `/home` 59.
ENGINE=${BSSH_CI_ENGINE:-docker}
case "$ENGINE" in
docker)
	if ! command -v docker >/dev/null 2>&1; then
		echo "check-ci: docker is not installed, so the runner cannot be reproduced." >&2
		echo "check-ci:   try BSSH_CI_ENGINE=hull, or see sec 12.4." >&2
		exit 1
	fi
	;;
hull)
	# hull off the PATH, or a checkout of it beside this one, built: it is
	# not vendored, so this names where it usually is and HULL overrides.
	HULL=${HULL:-$(command -v hull || echo "$HOME/src/hull/build/hull")}
	if [ ! -x "$HULL" ]; then
		echo "check-ci: no hull at $HULL; set HULL to one, or use docker." >&2
		exit 1
	fi
	;;
*)
	echo "check-ci: BSSH_CI_ENGINE is docker or hull, not '$ENGINE'." >&2
	exit 1
	;;
esac

if [ ! -f "$WORKFLOW" ]; then
	echo "check-ci: $WORKFLOW is missing; nothing to reproduce." >&2
	exit 1
fi

# The apt-get line from the linux job, continuations joined, "sudo
# apt-get install -y" stripped. Fails loudly rather than installing an
# empty list, because "no packages" would build nothing and pass.
PACKAGES=$(
	awk '
		/^  linux:/       { in_job = 1; next }
		/^  [a-z_-]+:/    { in_job = 0 }
		!in_job           { next }
		# The install line, plus any continuation lines after it.
		/apt-get install -y/ { sub(/.*apt-get install -y[[:space:]]*/, ""); collecting = 1 }
		collecting {
			line = $0
			more = sub(/\\[[:space:]]*$/, "", line)
			gsub(/^[[:space:]]+|[[:space:]]+$/, "", line)
			out = out " " line
			if (!more) { collecting = 0 }
		}
		END { gsub(/^[[:space:]]+/, "", out); print out }
	' "$WORKFLOW"
)
if [ -z "$PACKAGES" ]; then
	echo "check-ci: could not find the linux job's apt-get line in $WORKFLOW." >&2
	echo "check-ci:   it moved or changed shape; fix this parser rather than" >&2
	echo "check-ci:   hard-coding the list, or the two will drift." >&2
	exit 1
fi

echo "check-ci: image    $IMAGE"
echo "check-ci: packages $PACKAGES"

# hull's stage is where the machine builds, so it goes on the disk under
# $HOME rather than in /tmp, and in a directory of the user's own group: a
# machine above an overlay cannot write under a group it does not map,
# which this tree's `users` is (EOVERFLOW, hull sec 7).
if [ "$ENGINE" = hull ]; then
	CACHE=${XDG_CACHE_HOME:-$HOME/.cache}
	mkdir -p "$CACHE"
	STAGE=$(mktemp -d "$CACHE/bssh-check-ci.XXXXXX")
else
	STAGE=$(mktemp -d)
fi
CONTAINER=bssh-check-ci-$$
RUN_PID=
# INT and TERM as well as EXIT: an EXIT-only trap leaks the staging tree
# whenever this is interrupted or hits a timeout, which is exactly when a
# long docker run gets cut short.
#
# That reasoning was right and its remedy was half the size of the problem.
# A trap cleans up what this SHELL owns; the container is a child of the
# docker daemon, which hears nothing when the shell is signalled. So the
# interruption this trap was written for left the staging tree tidied and
# an ubuntu:24.04 container still compiling.
#
# Measured 2026-08-31: `timeout 600 sg docker -c "make check-ci"` killed
# make at ten minutes and `docker ps` showed the container Up for another
# ten. Worse than a wasted run, because the receipt that gates a push is
# written after the container exits under make's supervision -- so the
# orphan finishes nothing that counts, and the next run competes with it
# for the same cores. A bound that stops the supervisor while the work
# continues is worse than no bound: it turns a slow success into an
# invisible one.
#
# The name is what makes the kill possible at all. Docker's generated
# names are random, so an orphan from a previous run and the container
# this run is waiting on are indistinguishable in `docker ps` -- there is
# no safe way to clean up by hand, and none to write down here.
#
# `docker kill` on an already-exited container returns non-zero, so the
# status is discarded: under `set -e` a failing trap command should not
# get to decide this script's exit code.
#
# hull's half is the kill, which is enough: the machine goes with its run.
# `rm -rf` is of the directory mktemp made above and nothing else.
trap 'if [ "$ENGINE" = docker ]; then docker kill "$CONTAINER" >/dev/null 2>&1 || true;
      elif [ -n "$RUN_PID" ]; then kill -KILL "$RUN_PID" 2>/dev/null || true; fi
      rm -rf "$STAGE"' EXIT
# INT and TERM only exit; `exit` runs the EXIT trap above, so the
# cleanup lives in exactly one place.
trap 'exit 130' INT TERM

# Only what is committed: this checks what a push would actually give the
# runner, not what happens to be lying in the working tree.
#
# `git archive HEAD` rather than `git ls-files | tar`, and the difference
# is the whole claim above. ls-files names TRACKED PATHS; the tar that
# follows then reads those paths off the DISK, so every uncommitted edit
# to a tracked file went into the container while the comment here said
# it did not. Measured rather than argued: a marker line appended to a
# tracked file and left uncommitted arrived in the tar, and did not
# arrive in `git archive HEAD` of the same file.
#
# git archive reads the tree object, so it can only produce committed
# content. It also omits submodules entirely, which is what the old
# --no-recursion flag was for: ls-files prints the submodule as a bare
# path -- `vterm` -- and tar given a directory name archives everything
# under it, so the submodule used to arrive inside src.tar however the
# workflow was configured. That is now a property of the tool instead of
# a flag somebody has to remember to keep in front of -T.
# The commit being staged, captured HERE rather than read again at the end.
#
# `git archive HEAD` resolves HEAD once, now. The receipt at the bottom used
# to resolve it a second time when the run finished, and in a tree with more
# than one writer those are not the same commit: measured 2026-08-26, a run
# staged 43f0e2c, another session committed 8001a5b five minutes later while
# the container was building, and the receipt named 8001a5b -- a commit that
# had never been inside it. The pre-push hook would then have passed it as
# verified. One resolution, used twice.
STAGED=$(git rev-parse HEAD)

git archive --format=tar HEAD > "$STAGE/src.tar"

# And the submodule ONLY IF THE WORKFLOW ASKS FOR IT, read out of the
# workflow the same way the package list above is, and for the same
# reason -- a second copy of a decision is a copy that can be wrong.
#
# This used to hand the container vterm/ unconditionally, and that made
# the check blind to the one failure it should have caught first: the
# linux job's checkout did not ask for submodules, so on the runner
# vterm/ was empty, the object list expanded to nothing and the build
# died on `ar rcs build-vterm/libvterm.a` with no members. Every run of
# that job had failed since it was written while this reproduction
# passed, because this reproduction was quietly fixing the bug on its
# way in.
WANTS_SUBMODULES=$(python3 - "$WORKFLOW" <<'PY'
import re
import sys

text = open(sys.argv[1]).read()

# The linux job's own block: from "  linux:" to the next job at that indent.
block = re.search(r"^  linux:\n(.*?)(?=^  \S+:|\Z)", text, re.S | re.M)
body = block.group(1) if block else ""

print("yes" if re.search(r"^\s*submodules:\s*(recursive|true)\s*$", body, re.M)
      else "no")
PY
)

# And from the submodule's own HEAD, for the reason src.tar is: `tar -cf
# vterm.tar vterm` read the working tree, which here is never what the
# runner gets. The build APPLIES vterm-patch/ to the submodule in
# place, so the tree carries 38 modified lines at all times, and handing
# those to the container made a green run mean "HEAD plus whatever is
# lying in vterm/" rather than HEAD. It also hid the patch step: the
# container was given an already-patched tree and then stamped the step
# as done, so the one rule that could break when upstream moves was the
# one rule this reproduction never ran.
#
# git archive against the submodule gives what the runner's checkout
# gives -- the gitlink commit, unpatched -- and `make` then applies the
# patches in the container exactly as it does on the runner.
#
# softkeys is not patched, so its own HEAD would do as well on a quiet
# tree -- but a session developing softkeys from inside this one moves
# that HEAD, and the runner checks out the GITLINK. So it is staged from
# the commit the staged tree names, $STAGED:softkeys, not from whatever
# the submodule has checked out.
if [ "$WANTS_SUBMODULES" = yes ]; then
	git -C vterm archive --format=tar --prefix=vterm/ HEAD > "$STAGE/vterm.tar"
	git -C softkeys archive --format=tar --prefix=softkeys/ \
		"$(git rev-parse "$STAGED:softkeys")" > "$STAGE/softkeys.tar"
	# libssh by its gitlink too, unpatched, so the container applies
	# libssh-patch/ itself -- the proof the patches still apply from
	# clean, which the working tree cannot give (sec 8.4).
	git -C libssh archive --format=tar --prefix=libssh/ \
		"$(git rev-parse "$STAGED:libssh")" > "$STAGE/libssh.tar"
else
	echo "check-ci: the linux job does not ask for submodules, so neither does this"
	tar -cf "$STAGE/vterm.tar" --files-from /dev/null
	tar -cf "$STAGE/softkeys.tar" --files-from /dev/null
	tar -cf "$STAGE/libssh.tar" --files-from /dev/null
fi

# What the job runs, the same for both engines, from the image with the
# packages already in it. Quoted, so nothing here is expanded by this shell.
cat > "$STAGE/run.sh" <<'RUN'
set -e
echo "check-ci: Qt $(pkg-config --modversion Qt6Core), libssh vendored (libssh/)"

mkdir -p /build && cd /build
tar xf /in/src.tar
tar xf /in/vterm.tar
tar xf /in/softkeys.tar
tar xf /in/libssh.tar
# No patch stamp here any more. vterm.tar is the submodule at its gitlink
# commit, so the patches are NOT applied and make has to apply them --
# which is the point, since that is what the runner does too. git is
# installed above for it: 'git apply' needs no repository, and the
# extracted tree is not one.

# But the runner's tree IS one, and the style gate asks git what the tree
# holds: in a checkout a submodule is one gitlink, while in a walk of an
# exported tree it is every file in it. Since libssh joined vterm and
# softkeys (2026-10-23), the walk counted enough vendored files that the
# gate's collapse floor fired on a tree that passes on the runner. So the
# tree is given the runner's shape: each submodule a repository holding
# what was exported, the top one indexing them as gitlinks. Nothing is
# committed at the top; ls-files reads the index. apply-patches.sh is
# written for a submodule that is its own repository, as here.
for d in vterm softkeys libssh; do
	[ -n "$(ls -A "$d" 2>/dev/null)" ] || continue
	(cd "$d" && git init -q && git add -A &&
	 git -c user.name=check-ci -c user.email=check-ci@invalid \
		commit -q -m 'exported by check-ci')
done
git init -q
git add -A  # warns once per submodule: an embedded repository is the point

make style

# The curated set, as the linux job restores it from the fonts job's
# cache: the host's fonts/, mounted read-only, linked where a checkout has
# it. The tests find it by a path compiled in (tests.pro), so it is there
# before the build. Without it seven tests of the set skip.
if [ -n "$(ls -A /fonts 2>/dev/null)" ]; then
	ln -s /fonts fonts
	echo "check-ci: fonts from the host's fonts/"
else
	echo "check-ci: fonts/ not fetched on the host, so the set's tests skip"
fi

# Kept so the WARNINGS can be counted, and printed either way so the
# log a person reads is the same one it counted. Redirected rather than
# piped to tee: a pipeline reports its LAST command's status, which is
# how a failing build gets swallowed into a clean-looking run, and this
# script's own comments record that trap twice already.
if ! make -j"$(nproc)" > /build/build.log 2>&1; then
	cat /build/build.log
	exit 1
fi

# The suite runs as an ordinary user, as on the runner. As root, four
# tests of a write that fails (profile_store_test) skip, because root
# writes into a read-only directory regardless: both engines skipped them
# on 2026-11-10 while GitHub ran them. So root builds the test binary too,
# and the user's `make test` finds every target current and only runs
# the suite, which writes into a sandbox under /tmp. Nothing it writes
# lands in /build, which under hull is a host directory: a file there
# owned by a mapped uid is one the host cannot remove afterwards.
test_bin=$(make -s --eval='check-ci-test-bin: ; @echo $(TEST_BIN)' check-ci-test-bin)
if ! make -j"$(nproc)" "$test_bin" >> /build/build.log 2>&1; then
	cat /build/build.log
	exit 1
fi
cat /build/build.log
useradd -m check-ci
if ! setpriv --reuid=check-ci --regid=check-ci --init-groups \
	env HOME=/home/check-ci USER=check-ci QT_QPA_PLATFORM=offscreen \
	GIT_CONFIG_COUNT=1 GIT_CONFIG_KEY_0=safe.directory GIT_CONFIG_VALUE_0='*' \
	make test > /build/test.log 2>&1; then
	cat /build/test.log
	exit 1
fi
cat /build/test.log

# `|| true` because grep -c prints 0 and EXITS 1 when nothing matched,
# which under set -e ends the run at the moment there is nothing to
# report. That is this workspace's own recorded trap, met here.
warnings=$(grep -c 'warning:' /build/build.log /build/test.log 2>/dev/null \
	| awk -F: '{ total += $2 } END { print total + 0 }' || true)

echo "check-ci: the build emitted $warnings warning(s)"
if [ "$warnings" -gt 0 ]; then
	echo 'check-ci:   this build was warning-free on 2026-09-22 -- see sec 12.3.'
	echo 'check-ci:   REPORTED rather than refused: making a warning fatal is a'
	echo "check-ci:   build-policy change and the holder's to make."
	grep -h 'warning:' /build/build.log /build/test.log | sort -u | head -20
fi
RUN

# The image: the runner's, with the workflow's packages and what the build
# itself needs. Under docker they are installed in every run. Under hull a
# `(build ...)` step bakes them into a stored image once, on disk; the
# list is part of the step, so a changed workflow changes the key, and the
# step has a network, so hull runs it again once its refresh period has
# passed -- a day, so the image is never older than a day's updates, which
# answers the stale-cache question docker's own cached base never did.
SETUP="apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y -qq $PACKAGES build-essential pkg-config python3 git >/dev/null"

# fonts/ is fetched, not tracked, so it is not in the archive; the job
# gets the host's copy read-only, or none (sec 9.2, `make fonts`).
FONTS_VOLUME=
[ -d fonts ] && FONTS_VOLUME="$(pwd)/fonts"

if [ "$ENGINE" = docker ]; then
	docker run --rm --name "$CONTAINER" -v "$STAGE":/in:ro \
		${FONTS_VOLUME:+-v "$FONTS_VOLUME":/fonts:ro} "$IMAGE" \
		sh -c "$SETUP 2>&1 && sh /in/run.sh" &
else
	HULL_IMAGE=beerssh/check-ci
	# Relative paths in a spec are the spec's directory's. The machine's
	# writes to its root are memory (an overlay's upper is a tmpfs), so the
	# build goes to /build, a directory on disk; /in is read-only as under
	# docker. (network none): the suite needs only loopback, which a machine
	# of its own namespace has, and a test that reached further is a test
	# the runner may not reproduce either.
	mkdir "$STAGE/in" "$STAGE/build"
	for f in src.tar vterm.tar softkeys.tar libssh.tar run.sh; do
		mv "$STAGE/$f" "$STAGE/in/"
	done
	# A bare name is docker's library, which hull spells out.
	case "$IMAGE" in */*) BASE=$IMAGE;; *) BASE=docker.io/library/$IMAGE;; esac
	FONTS_HULL=
	[ -n "$FONTS_VOLUME" ] && FONTS_HULL="
    (volume (host \"$FONTS_VOLUME\") (guest \"/fonts\") (mode ro))"
	cat > "$STAGE/hull.conf" <<HULL
(build
  (base "$BASE")
  (steps
    (step
      (name "packages")
      (network true)
      (refresh "1d")
      (run "$SETUP"))))
(machine
  (source "$HULL_IMAGE")
  (mutable overlay)
  (network none)
  (volumes
    (volume (host "./in") (guest "/in") (mode ro))
    (volume (host "./build") (guest "/build") (mode rw))$FONTS_HULL)
  (command "sh" "/in/run.sh"))
HULL
	"$HULL" build -f "$STAGE/hull.conf" "$HULL_IMAGE"
	"$HULL" run -f "$STAGE/hull.conf" &
	RUN_PID=$!
fi
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

echo "check-ci: the linux job passes on $IMAGE under $ENGINE, for $(git rev-parse --short "$STAGED")"
