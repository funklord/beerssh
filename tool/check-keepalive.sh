#!/bin/sh
# Dead-peer detection against a link whose packets really vanish
# (project.md sec 5.3, sec 12.3).
#
# A closed socket is not a dropped link. A peer that goes away cleanly
# sends a FIN and is noticed immediately, which is the case that was never
# in doubt; what sec 5.3's socket options exist for is the case where
# packets simply stop arriving and nothing says so. Simulating that needs
# packets dropped rather than refused, which needs iptables, which needs
# NET_ADMIN -- hence a container.
#
# Everything is built inside it rather than mounted from the host, so this
# does not quietly depend on the developer's Qt matching the image's.
#
# Measured when this was written: 62s with the socket options, and still
# connected after 240s without them.

set -eu

IMAGE=${BSSH_KEEPALIVE_IMAGE:-ubuntu:24.04}

if ! command -v docker >/dev/null 2>&1; then
	echo "check-keepalive: docker is not installed." >&2
	echo "check-keepalive:   a real black hole needs iptables, which needs NET_ADMIN." >&2
	exit 1
fi

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
CONTAINER=bssh-check-keepalive-$$
STAGE=$(mktemp -d)
trap 'docker kill "$CONTAINER" >/dev/null 2>&1 || true; rm -rf "$STAGE"' EXIT
# INT and TERM only exit; `exit` runs the EXIT trap above, so the
# cleanup lives in exactly one place.
trap 'exit 130' INT TERM

# Only what is committed, for the reason check-ci.sh carries at length:
# `git ls-files` names tracked PATHS and the tar that follows reads them off
# the DISK, so every uncommitted edit to a tracked file rode into the
# container. git archive reads the tree object and cannot emit uncommitted
# content. It also omits submodules, so vterm comes separately.
git archive --format=tar HEAD > "$STAGE/src.tar"

# From the submodule's own HEAD, unpatched -- the build applies
# vterm-patch/ in place, so the working tree is never what a checkout
# gives. Unconditional here, unlike check-ci: that script asks the workflow
# whether the runner wants submodules because its job is to reproduce the
# runner exactly, empty vterm/ included. This one builds the app to test
# dead-peer detection and always needs libvterm.
git -C vterm archive --format=tar --prefix=vterm/ HEAD > "$STAGE/vterm.tar"

# softkeys at the commit HEAD's gitlink names, which is what a checkout gives.
git -C softkeys archive --format=tar --prefix=softkeys/ \
	"$(git rev-parse HEAD:softkeys)" > "$STAGE/softkeys.tar"

echo "check-keepalive: building and running inside $IMAGE"

docker run --rm --name "$CONTAINER" --cap-add=NET_ADMIN -v "$STAGE":/in:ro "$IMAGE" sh -c '
set -e
apt-get update -qq >/dev/null 2>&1
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
	qt6-base-dev libgl1-mesa-dev libssh-dev libsixel-dev libssl-dev libsecret-1-dev libxcb1-dev \
	build-essential pkg-config python3 git openssh-server iptables >/dev/null 2>&1

mkdir -p /build && cd /build
tar xf /in/src.tar && tar xf /in/vterm.tar && tar xf /in/softkeys.tar
# No patch stamp: vterm.tar is the submodule at its gitlink commit, so make
# applies vterm-patch/ here rather than being handed a patched tree with
# the step stamped away. git is installed above for it; git apply needs no
# repository, and an extracted tree is not one.
make -j"$(nproc)" test >/tmp/build.log 2>&1 || { tail -20 /tmp/build.log; exit 1; }

# Named ONCE, and checked, because it was written out three times and all
# three were wrong. The singular-directory pass of 2026-08-25 rewrote
# `build-tests` to `build-test` here and in check-ssh.sh while the
# Makefile that creates the directory kept the plural, so every phase
# below ran a binary that does not exist -- failing after a 60-second
# wait with "session never came up", which is a true sentence about the
# wrong thing. Neither script runs under `make test` or check-ci, so
# nothing said so for thirteen days.
BIN=./build-tests/beerssh_test
if [ ! -x "$BIN" ]; then
	echo "check-keepalive: $BIN was not built by make test -- the build"
	echo "check-keepalive:   directory has moved and this path is stale."
	exit 1
fi

D=/tmp/k && mkdir -p $D/home/.ssh && mkdir -p /run/sshd
ssh-keygen -q -t ed25519 -f $D/hostkey -N ""
ssh-keygen -q -t ed25519 -f $D/home/.ssh/id -N ""
cp $D/home/.ssh/id.pub $D/authorized_keys
chmod 600 $D/authorized_keys $D/home/.ssh/id

PORT=2222
cat > $D/sshd_config <<EOF
Port $PORT
ListenAddress 127.0.0.1
HostKey $D/hostkey
PidFile $D/sshd.pid
AuthorizedKeysFile $D/authorized_keys
PasswordAuthentication no
PubkeyAuthentication yes
UsePAM no
StrictModes no
EOF
/usr/sbin/sshd -f $D/sshd_config -E $D/sshd.log
sleep 1

READY=$D/ready
NOTICED=$D/noticed
export BSSH_TEST_SSH_PORT=$PORT
export BSSH_TEST_SSH_KEY=$D/home/.ssh/id
export BSSH_TEST_SSH_READY=$READY
export BSSH_TEST_SSH_NOTICED=$NOTICED
export HOME=$D/home XDG_DATA_HOME=$D/home/.local/share USER=root
export QT_QPA_PLATFORM=offscreen

drop_port() {
	for chain in INPUT OUTPUT; do
		iptables -I $chain -p tcp --dport $PORT -j DROP
		iptables -I $chain -p tcp --sport $PORT -j DROP
	done
}

restore_port() {
	for chain in INPUT OUTPUT; do
		iptables -D $chain -p tcp --dport $PORT -j DROP 2>/dev/null || true
		iptables -D $chain -p tcp --sport $PORT -j DROP 2>/dev/null || true
	done
}

wait_for_file() {
	for i in $(seq 1 "$2"); do [ -f "$1" ] && return 0; sleep 0.5; done
	return 1
}

# ---- phase: the link goes away and stays away -------------------------
rm -f "$READY" "$NOTICED"
BSSH_TEST_SSH_PHASE=drop "$BIN" --live-ssh > $D/drop.log 2>&1 &
APP=$!
wait_for_file "$READY" 120 || { echo "session never came up"; cat $D/drop.log; exit 1; }
echo "check-keepalive: session up; black-holing port $PORT"
drop_port
wait $APP || true
restore_port
cat $D/drop.log
# A positive PASS count, not just "0 failed".
#
# NOTE: this script body is a single-quoted argument, so nothing here may
# contain an apostrophe. The first version of this note did and ended the
# quote early; bash -n caught it, as it did for the same mistake in
# check-deb.sh two hours earlier.
#
# The old pattern was "^Totals: .* 0 failed", which a run where every
# case skipped satisfies: QtTest prints "0 passed, 0 failed, 1 skipped"
# and the grep matches. The gate could pass having verified nothing --
# evidence.md on a tally and a status being halves of one result, neither
# of them the answer alone.
#
# Skips are NORMAL here and stay allowed: each phase runs a filtered
# subset, so the cases belonging to other phases skip. Measured on a real
# run -- 18 passed with 5 skipped, then 8 with 15, then 3 with 20 --
# which is why this does not forbid them. Requiring zero skipped would
# have reddened a working gate for something that is not a defect.
#
# Stronger still would be a minimum count per phase, catching a phase
# that silently runs fewer cases than it used to. That needs expected
# numbers which drift, so it is not done here.
grep -qE "^Totals: [1-9][0-9]* passed, 0 failed" $D/drop.log

# ---- phase: it goes away, is noticed, and comes back ------------------
rm -f "$READY" "$NOTICED"
BSSH_TEST_SSH_PHASE=reconnect "$BIN" --live-ssh > $D/reconnect.log 2>&1 &
APP=$!
wait_for_file "$READY" 120 || { echo "session never came up"; cat $D/reconnect.log; exit 1; }
echo "check-keepalive: session up; dropping the link again"
drop_port

# Restored only once the drop has been NOTICED. Earlier would prove
# nothing about detection; never would only prove the backoff can count.
if wait_for_file "$NOTICED" 300; then
	echo "check-keepalive: drop noticed; restoring the link"
	restore_port
else
	echo "check-keepalive: the drop was never noticed"
	restore_port
fi

wait $APP || true
cat $D/reconnect.log
grep -qE "^Totals: [1-9][0-9]* passed, 0 failed" $D/reconnect.log

# ---- phase: the same, through a jump host ----------------------------
# A session reached through a bastion has TWO sockets, and the one the app
# never sees is the one carrying everything. The socket options are applied
# to the target socket by the transport; the tunnel opens its own, and
# until that one was given the same treatment a black-holed link left the
# session looking connected for as long as anyone cared to wait.
rm -f "$READY" "$NOTICED"
BSSH_TEST_SSH_PHASE=tunnel_black_hole "$BIN" --live-ssh > $D/tunnel.log 2>&1 &
APP=$!
wait_for_file "$READY" 120 || { echo "tunnelled session never came up"; cat $D/tunnel.log; exit 1; }
echo "check-keepalive: session up through a jump host; black-holing port $PORT"
drop_port
wait $APP || true
restore_port
cat $D/tunnel.log
grep -qE "^Totals: [1-9][0-9]* passed, 0 failed" $D/tunnel.log
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

echo "check-keepalive: a black-holed link is noticed, and the session comes back"
