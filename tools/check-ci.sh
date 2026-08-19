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
# Not run by `make test`: it needs docker and pulls an image. Run it
# before pushing anything that touches a platform or a library API.

set -eu

IMAGE=${BSSH_CI_IMAGE:-ubuntu:24.04}
WORKFLOW=.github/workflows/build.yml

if ! command -v docker >/dev/null 2>&1; then
	echo "check-ci: docker is not installed, so the runner cannot be reproduced." >&2
	echo "check-ci:   this is the one check that cannot be faked locally -- see sec 12.4." >&2
	exit 1
fi

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

STAGE=$(mktemp -d)
# INT and TERM as well as EXIT: an EXIT-only trap leaks the staging tree
# whenever this is interrupted or hits a timeout, which is exactly when a
# long docker run gets cut short.
trap 'rm -rf "$STAGE"' EXIT INT TERM

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
# runner gets. The build APPLIES vterm-patches/ to the submodule in
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
if [ "$WANTS_SUBMODULES" = yes ]; then
	git -C vterm archive --format=tar --prefix=vterm/ HEAD > "$STAGE/vterm.tar"
else
	echo "check-ci: the linux job does not ask for submodules, so neither does this"
	tar -cf "$STAGE/vterm.tar" --files-from /dev/null
fi

docker run --rm -v "$STAGE":/in:ro "$IMAGE" sh -c "
set -e
apt-get update -qq >/dev/null 2>&1
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq $PACKAGES \
	build-essential pkg-config python3 git >/dev/null 2>&1

echo \"check-ci: Qt \$(pkg-config --modversion Qt6Core), libssh \$(pkg-config --modversion libssh)\"

mkdir -p /build && cd /build
tar xf /in/src.tar
tar xf /in/vterm.tar
# No patch stamp here any more. vterm.tar is the submodule at its gitlink
# commit, so the patches are NOT applied and make has to apply them --
# which is the point, since that is what the runner does too. git is
# installed above for it: 'git apply' needs no repository, and the
# extracted tree is not one.

make style
make -j\"\$(nproc)\"
QT_QPA_PLATFORM=offscreen make test
"

echo "check-ci: the linux job passes on $IMAGE"

# The receipt the pre-push hook reads, naming the commit this run actually
# built -- which is HEAD, because that is what git archive staged above.
#
# The check writes its own receipt rather than the hook running the check,
# and that is not a preference. The hook used to run this script, and a
# push to GitHub then died with SIGPIPE while reporting that the check had
# passed: git opens the connection to the remote BEFORE running pre-push
# and sends the pack after, so five minutes of hook holds an idle socket
# that the far end closes. Measured -- the same push to a local bare repo
# succeeds with the same hook and the same run, twice, so it is the remote
# that goes away and not the check that breaks anything.
#
# In .git, so it is local to the clone and never committed: a receipt that
# survived a clone would vouch for a build that machine never did.
printf '%s\n' "$(git rev-parse HEAD)" > "$(git rev-parse --git-dir)/check-ci-passed"
