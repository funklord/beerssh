#!/bin/sh
# Run the SSH transport against a real sshd (project.md sec 12.3).
#
# Everything in tests/ otherwise runs against bssh_fake_ssh_session, which
# is what makes the suite runnable with no server -- and what leaves the
# libssh binding itself unexercised. A fake proves the emulator handles
# bytes; it cannot prove an option reached libssh, or that libssh did what
# the option name suggests. IdentityFile was wired up, shipped, and given
# a widget on both front doors before anything checked that a named key
# actually authenticates.
#
# Everything here is thrown away: its own host key, its own user key, its
# own HOME, and an sshd on a spare port bound to loopback that accepts
# that one key and no password. Nobody's real ~/.ssh is read or written.

set -eu

BIN=${BSSH_TEST_BIN:-./build-tests/beerssh_test}

if [ ! -x "$BIN" ]; then
	echo "check-ssh: $BIN is not built. Run: make test" >&2
	exit 1
fi

SSHD=""
for candidate in /usr/sbin/sshd /usr/local/sbin/sshd "$(command -v sshd 2>/dev/null || true)"; do
	if [ -n "$candidate" ] && [ -x "$candidate" ]; then SSHD=$candidate; break; fi
done

if [ -z "$SSHD" ]; then
	echo "check-ssh: no sshd found, so there is nothing real to test against." >&2
	echo "check-ssh:   install openssh-server, or skip this check." >&2
	exit 1
fi

DIR=$(mktemp -d)
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')

cleanup() {
	if [ -f "$DIR/sshd.pid" ]; then kill "$(cat "$DIR/sshd.pid")" 2>/dev/null || true; fi
	rm -rf "$DIR"
}
trap cleanup EXIT INT TERM

mkdir -p "$DIR/home/.ssh"
ssh-keygen -q -t ed25519 -f "$DIR/hostkey" -N ''
ssh-keygen -q -t ed25519 -f "$DIR/home/.ssh/id_test" -N '' -C bssh-check-ssh
chmod 700 "$DIR/home/.ssh"
chmod 600 "$DIR/home/.ssh/id_test"
cp "$DIR/home/.ssh/id_test.pub" "$DIR/authorized_keys"
chmod 600 "$DIR/authorized_keys"

# Loopback only, this key only, no passwords: the "without the key" test
# is only meaningful if there is genuinely no other way in.
cat > "$DIR/sshd_config" <<EOF
Port $PORT
ListenAddress 127.0.0.1
HostKey $DIR/hostkey
PidFile $DIR/sshd.pid
AuthorizedKeysFile $DIR/authorized_keys
PasswordAuthentication no
KbdInteractiveAuthentication no
PubkeyAuthentication yes
UsePAM no
StrictModes no
EOF

echo "check-ssh: sshd on 127.0.0.1:$PORT, one generated key, no passwords"
"$SSHD" -f "$DIR/sshd_config" -E "$DIR/sshd.log"

# Wait for the listener rather than sleeping a guessed interval.
i=0
while [ $i -lt 50 ]; do
	if python3 -c "
import socket,sys
s=socket.socket()
s.settimeout(0.2)
sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)"; then break; fi
	i=$((i + 1))
	sleep 0.1
done

if [ $i -ge 50 ]; then
	echo "check-ssh: sshd never came up; its log follows." >&2
	cat "$DIR/sshd.log" >&2
	exit 1
fi

# HOME is the throwaway tree so the tilde test addresses the generated key
# and not anything belonging to the person running this.
BSSH_TEST_SSH_PORT=$PORT \
BSSH_TEST_SSH_KEY="$DIR/home/.ssh/id_test" \
HOME="$DIR/home" \
QT_QPA_PLATFORM=offscreen \
	"$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")" --live-ssh

echo "check-ssh: the transport authenticates against a real server"
