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
# --no-recursion, because `git ls-files` prints the SUBMODULE as a bare
# path -- `vterm` -- and tar given a directory name archives the whole
# tree under it. So the submodule arrived inside src.tar however the
# workflow was configured, which is a second way this reproduction was
# fixing the bug on its way in. Every other entry ls-files prints is a
# real file, so nothing else is lost by not descending.
#
# The option order matters: --no-recursion is POSITIONAL in GNU tar and
# affects only the arguments after it, so it has to precede -T.
git ls-files -z | tar -cf "$STAGE/src.tar" --null --no-recursion -T -

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

if [ "$WANTS_SUBMODULES" = yes ]; then
	tar -cf "$STAGE/vterm.tar" vterm
else
	echo "check-ci: the linux job does not ask for submodules, so neither does this"
	tar -cf "$STAGE/vterm.tar" --files-from /dev/null
fi

docker run --rm -v "$STAGE":/in:ro "$IMAGE" sh -c "
set -e
apt-get update -qq >/dev/null 2>&1
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq $PACKAGES \
	build-essential pkg-config python3 >/dev/null 2>&1

echo \"check-ci: Qt \$(pkg-config --modversion Qt6Core), libssh \$(pkg-config --modversion libssh)\"

mkdir -p /build && cd /build
tar xf /in/src.tar
tar xf /in/vterm.tar
# The vendored patches are already applied in the tree that was tarred;
# stamp them so the patch step, which wants a git repo, is skipped.
mkdir -p build-vterm && touch build-vterm/.patched

make style
make -j\"\$(nproc)\"
QT_QPA_PLATFORM=offscreen make test
"

echo "check-ci: the linux job passes on $IMAGE"
