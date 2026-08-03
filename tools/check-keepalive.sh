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

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT INT TERM

git ls-files -z | tar --null -T - -cf "$STAGE/src.tar"
tar -cf "$STAGE/vterm.tar" vterm

echo "check-keepalive: building and running inside $IMAGE"

docker run --rm --cap-add=NET_ADMIN -v "$STAGE":/in:ro "$IMAGE" sh -c '
set -e
apt-get update -qq >/dev/null 2>&1
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
	qt6-base-dev libgl1-mesa-dev libssh-dev libsixel-dev libssl-dev libsecret-1-dev \
	build-essential pkg-config python3 openssh-server iptables >/dev/null 2>&1

mkdir -p /build && cd /build
tar xf /in/src.tar && tar xf /in/vterm.tar
mkdir -p build-vterm && touch build-vterm/.patched
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
export BSSH_TEST_SSH_PORT=$PORT
export BSSH_TEST_SSH_KEY=$D/home/.ssh/id
export BSSH_TEST_SSH_PHASE=drop
export BSSH_TEST_SSH_READY=$READY
export HOME=$D/home XDG_DATA_HOME=$D/home/.local/share USER=root
export QT_QPA_PLATFORM=offscreen

./build-tests/beerssh_test --live-ssh > $D/test.log 2>&1 &
APP=$!

# The test writes READY once its shell is up: the drop has to land on an
# established session, not mid-handshake.
for i in $(seq 1 120); do [ -f "$READY" ] && break; sleep 0.5; done
if [ ! -f "$READY" ]; then
	echo "check-keepalive: the session never came up"; cat $D/test.log; exit 1
fi

echo "check-keepalive: session up; black-holing port $PORT"
for chain in INPUT OUTPUT; do
	iptables -I $chain -p tcp --dport $PORT -j DROP
	iptables -I $chain -p tcp --sport $PORT -j DROP
done

wait $APP || true
cat $D/test.log
grep -q "^Totals: .* 0 failed" $D/test.log
'

echo "check-keepalive: a black-holed link is noticed inside sec 5.3's budget"
