#!/bin/sh
# Run the SSH transport against a real sshd (project.md sec 12.3).
#
# Everything in test/ otherwise runs against bssh_fake_ssh_session, which
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

# The Makefile passes this, and there is deliberately no default any
# more. There was one, it said ./build-test/beerssh_test, and the build
# directory is build-tests -- so from 2026-08-25 until 2026-09-07 this
# script told everyone who ran it to `make test` and then failed again
# for the same reason, in a circle. `make check-ssh` could not run at
# all for thirteen days and nothing said so, because this check is in
# neither `make test` nor check-ci.
#
# The singular-directory pass that day rewrote `tests` to `test` and
# reached into `build-tests`, which is a build output directory the
# Makefile names and that pass did not own. Its commit message cites
# `make style` and `make test` both passing, which is true and says
# nothing about a check that runs under neither.
#
# So the path is written once, in the Makefile, and this refuses rather
# than guessing a second copy of it.
BIN=${BSSH_TEST_BIN:-}

if [ -z "$BIN" ]; then
	echo "check-ssh: BSSH_TEST_BIN is not set, and this no longer guesses." >&2
	echo "check-ssh:   Run: make check-ssh" >&2
	echo "check-ssh:   or:  BSSH_TEST_BIN=./build-tests/beerssh_test $0" >&2
	exit 1
fi

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
	BSSH_TEST_SSH_READY="${BSSH_TEST_SSH_READY:-}" \
	BSSH_TEST_SSH_NOTICED="${BSSH_TEST_SSH_NOTICED:-}" \
	BSSH_TEST_OPENSSH_KNOWN_HOSTS="${BSSH_TEST_OPENSSH_KNOWN_HOSTS:-}" \
	BSSH_TEST_OPENSSH_SNAPSHOT="${BSSH_TEST_OPENSSH_SNAPSHOT:-}" \
	HOME="$DIR/home" \
	XDG_DATA_HOME="$DIR/home/.local/share" \
	XDG_CONFIG_HOME="$DIR/home/.config" \
	QT_QPA_PLATFORM=offscreen \
		"$BIN_ABS" --live-ssh
}

BIN_ABS="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"

# Let OpenSSH record this host in its own format -- hashed, because that
# is what the system default does -- so the read-only consultation can be
# checked against a file this app did not write.
OPENSSH_KH="$DIR/home/.ssh/known_hosts"
ssh -i "$DIR/home/.ssh/id_test" -p "$PORT" \
    -o UserKnownHostsFile="$OPENSSH_KH" \
    -o StrictHostKeyChecking=accept-new \
    -o IdentitiesOnly=yes 127.0.0.1 true >/dev/null 2>&1 || true

if [ -s "$OPENSSH_KH" ]; then
	echo "check-ssh: OpenSSH recorded the host ($(grep -c '^|1|' "$OPENSSH_KH") hashed entries)"
	BSSH_TEST_OPENSSH_KNOWN_HOSTS="$OPENSSH_KH"
	BSSH_TEST_OPENSSH_SNAPSHOT="$(cat "$OPENSSH_KH")"
	export BSSH_TEST_OPENSSH_KNOWN_HOSTS BSSH_TEST_OPENSSH_SNAPSHOT
fi

echo "check-ssh: phase 1 -- authentication, and trusting this host key"
run_phase trust

# The decisive check on what beerssh wrote: ssh(1) itself, in a mode
# that prompts for nothing, so it can only succeed on a match. This has
# to happen BEFORE the host key is swapped below -- afterwards ssh would
# refuse for the right reason and prove nothing about the entry.
if [ -s "$OPENSSH_KH" ]; then
	echo "check-ssh: asking ssh(1) to trust the entry beerssh wrote"
	if ssh -i "$DIR/home/.ssh/id_test" -p "$PORT" \
	       -o UserKnownHostsFile="$OPENSSH_KH" \
	       -o StrictHostKeyChecking=yes \
	       -o IdentitiesOnly=yes 127.0.0.1 true >/dev/null 2>&1; then
		echo "check-ssh: ssh(1) accepted it"
	else
		echo "check-ssh: ssh(1) would not use the entry beerssh wrote" >&2
		exit 1
	fi
fi

# ---- the jump host dies under a live session ------------------------
# Killing this sshd kills both ends -- it is bastion and target at once --
# and that is the case worth constructing: the news has to CROSS the
# tunnel. A session that cannot be told apart from a healthy one is worse
# than one that failed, and that is what this was before the pump learned
# to shut its local end down on the way out.
#
# It goes here because it needs a live server and established trust, and
# because the key swap below has to restart the server anyway.
echo "check-ssh: phase 3 -- the jump host dies while a session is up"
READY="$DIR/ready"
NOTICED="$DIR/noticed"
rm -f "$READY" "$NOTICED"
export BSSH_TEST_SSH_READY="$READY" BSSH_TEST_SSH_NOTICED="$NOTICED"

run_phase tunnel_drop > "$DIR/tunnel_drop.log" 2>&1 &
DROP_APP=$!

waited=0
while [ $waited -lt 300 ] && [ ! -f "$READY" ]; do
	waited=$((waited + 1))
	sleep 0.1
done

if [ ! -f "$READY" ]; then
	echo "check-ssh: the tunnelled session never came up" >&2
	cat "$DIR/tunnel_drop.log" >&2
	kill $DROP_APP 2>/dev/null || true
	exit 1
fi

# The CHILDREN first, and that is the whole trick: sshd forks a process
# per connection, so killing the listener alone leaves every established
# session running -- including the one this phase is about. The first
# version of this did exactly that and "proved" a session survives its
# jump host dying, when nothing had died at all.
echo "check-ssh: session up through the jump host; killing the server"
LISTENER=$(cat "$DIR/sshd.pid")
pkill -P "$LISTENER" 2>/dev/null || true
kill "$LISTENER" 2>/dev/null || true

wait $DROP_APP || true
cat "$DIR/tunnel_drop.log"
grep -q "^Totals: .* 0 failed" "$DIR/tunnel_drop.log"

# The server is already down, so the swap below only has to give it a new
# identity and start it again.
echo "check-ssh: giving the server a new host key and restarting"

# A host key can only be seen to CHANGE across a restart on the SAME port:
# a different port is a different known-hosts entry and would be merely
# unknown. So the server is stopped, given a new identity, and put back.
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
