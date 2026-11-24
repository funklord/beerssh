#!/bin/sh
# beersshd as a ROOT fuzznet daemon, end to end (project.md sec 5.6, "beersshd
# as a root fuzznet daemon, sshd's shape"). Root's alone: the drop to a user
# cannot be tried by anybody else, so `make test`, which runs as a user, has
# check-fuzznet.sh, and this runs where there is root -- check-ci's machine
# before it hands the suite to its user, and GitHub's runner under sudo.
#
# A user made for the run, a host and a device paired as check-fuzznet.sh
# pairs them, and beersshd serving as root with a map of devices to users.
# The helper beside beersshd is a stand-in that writes down who it ran as
# and then runs the real one, so what is checked is the uid, groups and
# environment the session really started with, not a log line saying so.
#
#   - root with --sessions and no --users refuses to start;
#   - a device with no line is refused, and its key is logged -- which is
#     how this script, like an administrator, learns the key to write;
#   - a map others may write, and a line naming root, are refused;
#   - the device's line added, without a restart, its session runs as the
#     user: their uid and gid, never 0, their HOME, USER and SHELL.
#
# What stops it: each pairing step under `timeout 20`, the server under
# `timeout 120` and killed by the trap, each probe under `timeout 30`. The
# trap also removes the user it made and the scratch, on every path out.
set -u

usage='usage: check-fuzznet-root.sh path/to/beersshd path/to/beerssh-session path/to/probe'
bin=${1:?$usage}
helper=${2:?$usage}
probe=${3:?$usage}

if [ "$(id -u)" != 0 ]; then
	echo "check-fuzznet-root: needs root, to start a session as another user; skipped" >&2
	exit 77
fi

dir=$(mktemp -d)
user=bsshfz$$
server=
made_user=
finish() {
	[ -n "$server" ] && kill "$server" 2>/dev/null && wait "$server" 2>/dev/null
	[ -n "$made_user" ] && userdel -r "$user" >/dev/null 2>&1
	rm -rf "$dir"
}
trap finish EXIT
trap 'exit 1' INT TERM

fail() {
	echo "check-fuzznet-root: FAILED -- $*" >&2
	[ -f "$dir/server.log" ] && echo "check-fuzznet-root: the server said: $(tail -c 600 "$dir/server.log")" >&2
	exit 1
}

if ! useradd -m -s /bin/sh "$user"; then
	echo "check-fuzznet-root: could not make a user to log in as" >&2
	exit 1
fi
made_user=1
uid=$(id -u "$user")
gid=$(id -g "$user")
home=$(getent passwd "$user" | cut -d: -f6)

# The scratch is root's, but the session runs as the user: it must reach
# the programs and the place it writes down who it was.
chmod 711 "$dir"
mkdir "$dir/bin" "$dir/s" "$dir/who" "$dir/etc" || exit 1
chmod 755 "$dir/bin" "$dir/etc"
chmod 777 "$dir/who"
cp "$bin" "$dir/bin/beersshd" && cp "$helper" "$dir/bin/beerssh-session.real" || exit 1
cat > "$dir/bin/beerssh-session" <<EOF
#!/bin/sh
printf 'uid=%s gid=%s groups=%s home=%s user=%s shell=%s\n' "\$(id -u)" "\$(id -g)" "\$(id -G)" \
	"\$HOME" "\$USER" "\$SHELL" > "$dir/who/\$\$"
exec "$dir/bin/beerssh-session.real" "\$@"
EOF
chmod 755 "$dir/bin/beerssh-session" "$dir/bin/beerssh-session.real" "$dir/bin/beersshd"
bin=$dir/bin/beersshd
map=$dir/etc/fuzznet-users
: > "$map"
chmod 600 "$map"

if ! timeout 20 "$bin" --fuzznet --fuzznet-dir="$dir/server" --new-root >/dev/null 2>"$dir/err"; then
	fail "no estate root: $(head -c 300 "$dir/err")"
fi
if ! image=$(timeout 60 "$bin" --fuzznet-pair --fuzznet-dir="$dir/server" 2>"$dir/err"); then
	fail "no pairing: $(head -c 300 "$dir/err")"
fi
case "$image" in
FZD1:*) ;;
*) fail "no device image" ;;
esac

port=$((30000 + $$ % 2000))

# Root serving sessions with no map would give every paired device a root shell.
timeout 20 "$bin" --fuzznet --fuzznet-dir="$dir/server" --sessions >/dev/null 2>"$dir/err"
code=$?
if [ "$code" != 2 ] || ! grep -q -- 'needs --users' "$dir/err"; then
	fail "root took --sessions with no map (exit $code): $(head -c 300 "$dir/err")"
fi

timeout 120 "$bin" --fuzznet --fuzznet-dir="$dir/server" --socket="$dir/s/sock" --udp-port="$port" \
	--no-log-file --sessions --users="$map" >"$dir/server.log" 2>&1 &
server=$!
for _ in $(seq 1 50); do
	[ -S "$dir/s/sock" ] && break
	sleep 0.2
done
[ -S "$dir/s/sock" ] || fail "beersshd did not start as root"

ask() {
	timeout 30 "$probe" "$image" 127.0.0.1 "$port" 2>/dev/null
}

# No line for the device: nothing answers, and the log names its key.
said=$(ask) && fail "a device with no line in the map was answered: \"$said\""
key=$(sed -n 's/.*no fuzznet session for \([0-9a-f]\{64\}\): no line.*/\1/p' "$dir/server.log" | head -n 1)
[ -n "$key" ] || fail "the refusal did not log the device's key"

# A map others may write is no map at all.
printf '%s %s\n' "$key" "$user" > "$map"
chmod 620 "$map"
said=$(ask) && fail "a map its group may write was obeyed: \"$said\""
grep -q 'only uid 0 may write' "$dir/server.log" || fail "a group-writable map was not refused for it"
chmod 600 "$map"

# Root is never a fuzznet login.
printf '%s root\n' "$key" > "$map"
said=$(ask) && fail "a line naming root was answered: \"$said\""
grep -q 'root is never a fuzznet login' "$dir/server.log" || fail "a line naming root was not refused for it"

# The device's own line, and no restart: the map is read for each session.
printf '# the device paired for this run\n%s %s\n' "$key" "$user" > "$map"
said=$(ask)
[ "$said" = "beerssh-session 1" ] || fail "the mapped device's session did not answer: \"$said\""
who=$(cat "$dir"/who/* 2>/dev/null | tail -n 1)
[ -n "$who" ] || fail "the helper never ran"
case "$who" in
"uid=$uid gid=$gid "*) ;;
*) fail "the session did not run as $user ($uid:$gid): $who" ;;
esac
case "$who" in
*"groups=0 "* | *" 0 "* | *"groups=0") fail "the session kept root's group: $who" ;;
esac
case "$who" in
*" home=$home user=$user shell=/bin/sh") ;;
*) fail "the session's environment is not $user's: $who" ;;
esac
grep -q "fuzznet session for $key as $user" "$dir/server.log" || fail "the session was not logged"

echo "check-fuzznet-root: root without a map refused; an unmapped device, a writable map and a line" \
     "naming root refused; the mapped device's session ran as $user ($who)"
