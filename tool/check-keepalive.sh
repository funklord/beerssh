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
trap 'docker kill "$CONTAINER" >/dev/null 2>&1 || true; rm -rf "$STAGE"' EXIT INT TERM

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

echo "check-keepalive: building and running inside $IMAGE"

docker run --rm --name "$CONTAINER" --cap-add=NET_ADMIN -v "$STAGE":/in:ro "$IMAGE" sh -c '
set -e
apt-get update -qq >/dev/null 2>&1
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
	qt6-base-dev libgl1-mesa-dev libssh-dev libsixel-dev libssl-dev libsecret-1-dev \
	build-essential pkg-config python3 git openssh-server iptables >/dev/null 2>&1

mkdir -p /build && cd /build
tar xf /in/src.tar && tar xf /in/vterm.tar
# No patch stamp: vterm.tar is the submodule at its gitlink commit, so make
# applies vterm-patch/ here rather than being handed a patched tree with
# the step stamped away. git is installed above for it; git apply needs no
# repository, and an extracted tree is not one.
make -j"$(nproc)" test >/tmp/build.log 2>&1 || { tail -20 /tmp/build.log; exit 1; }

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
BSSH_TEST_SSH_PHASE=drop ./build-test/beerssh_test --live-ssh > $D/drop.log 2>&1 &
APP=$!
wait_for_file "$READY" 120 || { echo "session never came up"; cat $D/drop.log; exit 1; }
echo "check-keepalive: session up; black-holing port $PORT"
drop_port
wait $APP || true
restore_port
cat $D/drop.log
grep -q "^Totals: .* 0 failed" $D/drop.log

# ---- phase: it goes away, is noticed, and comes back ------------------
rm -f "$READY" "$NOTICED"
BSSH_TEST_SSH_PHASE=reconnect ./build-test/beerssh_test --live-ssh > $D/reconnect.log 2>&1 &
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
grep -q "^Totals: .* 0 failed" $D/reconnect.log

# ---- phase: the same, through a jump host ----------------------------
# A session reached through a bastion has TWO sockets, and the one the app
# never sees is the one carrying everything. The socket options are applied
# to the target socket by the transport; the tunnel opens its own, and
# until that one was given the same treatment a black-holed link left the
# session looking connected for as long as anyone cared to wait.
rm -f "$READY" "$NOTICED"
BSSH_TEST_SSH_PHASE=tunnel_black_hole ./build-test/beerssh_test --live-ssh > $D/tunnel.log 2>&1 &
APP=$!
wait_for_file "$READY" 120 || { echo "tunnelled session never came up"; cat $D/tunnel.log; exit 1; }
echo "check-keepalive: session up through a jump host; black-holing port $PORT"
drop_port
wait $APP || true
restore_port
cat $D/tunnel.log
grep -q "^Totals: .* 0 failed" $D/tunnel.log
'

echo "check-keepalive: a black-holed link is noticed, and the session comes back"
