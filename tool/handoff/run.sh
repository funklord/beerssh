#!/bin/sh
# Runs the handoff probe against a real sshd.
#
# Same shape as tool/check-ssh.sh: a spare port bound to loopback, a
# generated host key, a config in a throwaway directory, and a trap that
# kills the listener AND its forked children on any exit. sshd forks per
# connection, so killing the listener alone leaves established sessions
# running -- check-ssh.sh learned that and says so.

set -eu

HERE=$(cd "$(dirname "$0")" && pwd)

SSHD=""
for candidate in /usr/sbin/sshd /usr/local/sbin/sshd "$(command -v sshd 2>/dev/null || true)"; do
	if [ -n "$candidate" ] && [ -x "$candidate" ]; then SSHD=$candidate; break; fi
done
if [ -z "$SSHD" ]; then
	echo "probe: no sshd found, so there is nothing real to test against." >&2
	exit 2
fi

DIR=$(mktemp -d)
PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')

cleanup() {
	if [ -f "$DIR/sshd.pid" ]; then
		pid=$(cat "$DIR/sshd.pid")
		pkill -P "$pid" 2>/dev/null || true
		kill "$pid" 2>/dev/null || true
	fi
	rm -rf "$DIR"
}
trap cleanup EXIT INT TERM

ssh-keygen -q -t ed25519 -f "$DIR/host_key" -N "" -C probe

cat > "$DIR/sshd_config" <<EOF
Port $PORT
ListenAddress 127.0.0.1
HostKey $DIR/host_key
PidFile $DIR/sshd.pid
StrictModes no
UsePAM no
PasswordAuthentication no
PermitRootLogin no
AuthorizedKeysFile $DIR/home/.ssh/authorized_keys
LogLevel DEBUG1
# Rekeys every 16 KiB from the server's side, so migrate.c's traffic after
# an import runs key exchanges on the imported state; the earlier probes
# move far less than that and never meet one.
RekeyLimit 16K
EOF

"$SSHD" -f "$DIR/sshd_config" -E "$DIR/sshd.log"

# Bounded wait: fifty tries at a tenth of a second, then give up loudly.
i=0
while [ $i -lt 50 ]; do
	if python3 -c "import socket,sys; s=socket.socket(); s.settimeout(0.2); \
sys.exit(0 if s.connect_ex(('127.0.0.1',$PORT))==0 else 1)" 2>/dev/null; then break; fi
	i=$((i+1))
	sleep 0.1
done
if [ $i -ge 50 ]; then
	echo "probe: sshd never came up on $PORT; its log follows." >&2
	cat "$DIR/sshd.log" >&2
	exit 1
fi
echo "probe: sshd on 127.0.0.1:$PORT"
echo

cc -O1 -Wall -Wextra -o "$DIR/plaintext" "$HERE/plaintext.c"
# The vendored libssh (project.md sec 8.4), which `make check-handoff`
# builds first and names here: the export this probe grows into lives in
# its patches, and Debian's libssh has none of them.
LIBSSH_PREFIX=${LIBSSH_PREFIX:?run this through make check-handoff, which builds libssh first}
cc -O1 -Wall -Wextra -DLIBSSH_STATIC -I"$LIBSSH_PREFIX/include" -o "$DIR/authenticated" \
	"$HERE/authenticated.c" "$LIBSSH_PREFIX/lib/libssh.a" -lcrypto -lz -lpthread
cc -O1 -Wall -Wextra -DLIBSSH_STATIC -I"$LIBSSH_PREFIX/include" -o "$DIR/migrate" \
	"$HERE/migrate.c" "$LIBSSH_PREFIX/lib/libssh.a" -lcrypto -lz -lpthread

# A key sshd will accept, and an authorized_keys naming it. Same shape as
# tool/check-ssh.sh: its own HOME so nothing touches the real one.
mkdir -p "$DIR/home/.ssh"
ssh-keygen -q -t ed25519 -f "$DIR/home/.ssh/id_probe" -N "" -C probe
cp "$DIR/home/.ssh/id_probe.pub" "$DIR/home/.ssh/authorized_keys"
chmod 700 "$DIR/home/.ssh"; chmod 600 "$DIR/home/.ssh/authorized_keys"

fails=0
for mode in clean shutdown doubleread; do
	echo "--- plaintext handoff, mode: $mode ---"
	if timeout 30 "$DIR/plaintext" "$PORT" "$mode"; then
		verdict=passed
	else
		verdict=failed
	fi
	echo "mode $mode: $verdict"
	echo

	# clean must pass; the other two exist to prove the probe can see a
	# broken handoff, so a PASS there is the probe failing at its job.
	case "$mode:$verdict" in
		clean:failed) fails=$((fails+1)) ;;
		shutdown:passed) echo "!! shutdown was not detected" >&2; fails=$((fails+1)) ;;
		doubleread:passed) echo "!! doubleread was not detected" >&2; fails=$((fails+1)) ;;
	esac
done

# ---- the encrypted case ----------------------------------------------
#
# The plaintext runs above prove the substrate. These ask whether the same
# discipline holds once there are cipher contexts, MAC keys, sequence
# numbers and an open channel, which is the case that matters.
for mode in clean doubleread; do
	echo "--- authenticated libssh session, mode: $mode ---"
	if timeout 60 "$DIR/authenticated" "$PORT" "$(id -un)" "$DIR/home/.ssh/id_probe" "$mode"; then
		verdict=passed
	else
		verdict=failed
	fi
	echo "authenticated $mode: $verdict"
	echo

	case "$mode:$verdict" in
		clean:failed) echo "!! an authenticated session did not survive" >&2; fails=$((fails+1)) ;;
		doubleread:passed) echo "!! doubleread was not detected under encryption" >&2; fails=$((fails+1)) ;;
	esac
done

# ---- the real move: export, exec, import -----------------------------
#
# The session crosses as a blob and a descriptor into an exec'd process
# (migrate.c), once per cipher family libssh treats differently: chacha's
# nonce comes from the sequence number, GCM's counter lives in OpenSSL,
# CTR with an ETM MAC sends its length in clear, and CTR with a plain MAC
# decrypts a packet's first block before the rest arrives.
newkeys_before=$(grep -c 'SSH2_MSG_NEWKEYS received' "$DIR/sshd.log" || true)
for spec in "chacha20-poly1305@openssh.com -" \
            "aes256-gcm@openssh.com -" \
            "aes128-ctr hmac-sha2-256-etm@openssh.com" \
            "aes256-ctr hmac-sha2-512"; do
	set -- $spec
	echo "--- moved by export and import: $1 ${2#-} ---"
	if timeout 60 "$DIR/migrate" "$PORT" "$(id -un)" "$DIR/home/.ssh/id_probe" "$1" "$2" clean; then
		verdict=passed
	else
		verdict=failed
	fi
	echo "migrate $1: $verdict"

	# A pass is only evidence if the key exchanges really ran on the
	# imported state. Counted from sshd's own log, the peer's word: each
	# NEWKEYS it receives is one exchange completed. One is the initial
	# exchange, before the move; 128 KiB after it, against a 16 KiB limit,
	# has to bring several more.
	newkeys=$(grep -c 'SSH2_MSG_NEWKEYS received' "$DIR/sshd.log" || true)
	exchanges=$((newkeys - newkeys_before))
	newkeys_before=$newkeys
	echo "migrate $1: sshd completed $exchanges key exchange(s) on this connection"
	echo
	if [ "$verdict" = passed ] && [ "$exchanges" -lt 4 ]; then
		echo "!! too few key exchanges to have rekeyed after the move ($1)" >&2
		fails=$((fails+1))
	fi
	[ "$verdict" = passed ] || { echo "!! the session did not survive the move ($1)" >&2; fails=$((fails+1)); }
done

# And the commit point violated: the exporter reads on after the export.
# This must fail, or the probe cannot see the fault.
echo "--- moved by export and import, exporter keeps reading ---"
if timeout 60 "$DIR/migrate" "$PORT" "$(id -un)" "$DIR/home/.ssh/id_probe" \
	chacha20-poly1305@openssh.com - steal; then
	echo "!! a read after the export was not detected" >&2
	fails=$((fails+1))
else
	echo "migrate steal: failed, as it must"
fi
echo

echo "=============================="
if [ $fails -eq 0 ]; then
	echo "probe: the handoff works, and both fault modes are detectable"
else
	echo "probe: $fails outcome(s) were not what the design predicts"
fi
exit $fails
