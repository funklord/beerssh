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
trap 'rm -rf "$STAGE"' EXIT

# Only what is committed, plus the vendored submodule: this checks what a
# push would actually give the runner, not what happens to be lying in
# the working tree.
git ls-files -z | tar --null -T - -cf "$STAGE/src.tar"
tar -cf "$STAGE/vterm.tar" vterm

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
