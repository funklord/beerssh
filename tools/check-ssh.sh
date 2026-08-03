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
wait_for_port() {
	i=0
	while [ $i -lt 50 ]; do
		if python3 -c "
import socket,sys
s=socket.socket()
s.settimeout(0.2)
sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)"; then return 0; fi
		i=$((i + 1))
		sleep 0.1
	done

	echo "check-ssh: sshd never came up on $PORT; its log follows." >&2
	cat "$DIR/sshd.log" >&2
	exit 1
}

wait_for_port

# HOME and XDG_DATA_HOME are both the throwaway tree: the tilde test must
# address the generated key, and the trust-on-first-use tests must write
# their known-hosts into a store made for this run rather than into the
# one belonging to whoever is running it.
run_phase() {
	BSSH_TEST_SSH_PORT=$PORT \
	BSSH_TEST_SSH_KEY="$DIR/home/.ssh/id_test" \
	BSSH_TEST_SSH_PHASE="$1" \
	HOME="$DIR/home" \
	XDG_DATA_HOME="$DIR/home/.local/share" \
	XDG_CONFIG_HOME="$DIR/home/.config" \
	QT_QPA_PLATFORM=offscreen \
		"$BIN_ABS" --live-ssh
}

BIN_ABS="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"

echo "check-ssh: phase 1 -- authentication, and trusting this host key"
run_phase trust

# A host key can only be seen to CHANGE across a restart on the SAME port:
# a different port is a different known-hosts entry and would be merely
# unknown. So the server is stopped, given a new identity, and put back.
echo "check-ssh: swapping the server's host key and restarting"
kill "$(cat "$DIR/sshd.pid")" 2>/dev/null || true
wait_gone=0
while [ $wait_gone -lt 50 ] && [ -f "$DIR/sshd.pid" ] && kill -0 "$(cat "$DIR/sshd.pid")" 2>/dev/null; do
	wait_gone=$((wait_gone + 1))
	sleep 0.1
done

rm -f "$DIR/hostkey" "$DIR/hostkey.pub"
ssh-keygen -q -t ed25519 -f "$DIR/hostkey" -N ''
"$SSHD" -f "$DIR/sshd_config" -E "$DIR/sshd.log"
wait_for_port

echo "check-ssh: phase 2 -- the same host, a different key"
run_phase rekeyed

echo "check-ssh: the transport authenticates, and refuses a changed host key"
