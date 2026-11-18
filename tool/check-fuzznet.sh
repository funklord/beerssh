#!/bin/sh
# beersshd as a fuzznet host, end to end (project.md sec 5.6). Through
# beersshd alone: an estate root, then a device for a client paired in one
# step with --fuzznet-pair, which prints its image -- and then
# beersshd serving, a device on its UDP port asking the session helper its
# version over fuzznet's words and the stream inside them, and the helper's
# answer coming back. Then the same without --sessions, which must answer
# nothing: a paired host gets no helper unless beersshd was told to give one.
#
# Under beerssh's own numbers, service 1 and product 4 (fuzznet's
# chain/service.h), which beersshd gives fuzznet unless told others.
#
# What stops it: every pairing step exits by itself under a 20 s ceiling, each
# server is started under `timeout 60` and killed by the trap, and the probe
# has its own 20 s deadline. The scratch is this script's own, removed on
# every path out; the socket's directory is a short one of its own, since a
# Unix socket's path has a length limit a deep scratch can exceed.
set -u

bin=${1:?usage: check-fuzznet.sh path/to/beersshd path/to/beerssh-session path/to/probe}
helper=${2:?usage: check-fuzznet.sh path/to/beersshd path/to/beerssh-session path/to/probe}
probe=${3:?usage: check-fuzznet.sh path/to/beersshd path/to/beerssh-session path/to/probe}
dir=$(mktemp -d)
server=
finish() {
	[ -n "$server" ] && kill "$server" 2>/dev/null && wait "$server" 2>/dev/null
	rm -rf "$dir"
}
trap finish EXIT
trap 'exit 1' INT TERM

# beersshd and the helper side by side, as --sessions finds it. The host's
# store is left for fuzznet to make, which it does since its 59b67817.
mkdir "$dir/bin" "$dir/s" || exit 1
cp "$bin" "$dir/bin/beersshd" && cp "$helper" "$dir/bin/beerssh-session" || exit 1
bin=$dir/bin/beersshd

# One step; its last line of output in $out, and the script ends on a failure.
run() {
	if ! timeout 20 "$bin" --fuzznet "$@" >"$dir/out" 2>"$dir/err"; then
		echo "check-fuzznet: beersshd --fuzznet $* failed: $(head -c 400 "$dir/err")" >&2
		exit 1
	fi
	out=$(tail -n 1 "$dir/out")
}

run --fuzznet-dir="$dir/server" --new-root

# The pairing in one step, as a person does it: a device made and paired
# off to one side and its image printed (src/sshd/fuzznet_pair.h). And
# refused for a directory holding no host, rather than making one there.
if timeout 60 "$bin" --fuzznet-pair --fuzznet-dir="$dir/nobody" >/dev/null 2>&1; then
	echo "check-fuzznet: FAILED -- --fuzznet-pair paired with no host to pair to" >&2
	exit 1
fi
# And with no directory named, as BeerSSH asks over SSH: fuzznet's default
# store, $XDG_DATA_HOME/fuzznet, the one a host started without one keeps.
if ! XDG_DATA_HOME="$dir/xdg" timeout 20 "$bin" --fuzznet --new-root >/dev/null 2>"$dir/err" \
   || ! XDG_DATA_HOME="$dir/xdg" timeout 60 "$bin" --fuzznet-pair 2>"$dir/err" | grep -q '^FZD1:'; then
	echo "check-fuzznet: FAILED -- no pairing in fuzznet's default store: $(head -c 300 "$dir/err")" >&2
	exit 1
fi
if ! image=$(timeout 60 "$bin" --fuzznet-pair --fuzznet-dir="$dir/server" 2>"$dir/err"); then
	echo "check-fuzznet: beersshd --fuzznet-pair failed: $(head -c 400 "$dir/err")" >&2
	exit 1
fi
case "$image" in
FZD1:*) ;;
*) echo "check-fuzznet: FAILED -- no device image, got: $(printf '%s' "$image" | head -c 200)" >&2; exit 1 ;;
esac

port=$((28000 + $$ % 2000))

# beersshd serving, with `$@` beside fuzznet's options, until it answers on
# its socket or 10 s pass.
serve() {
	timeout 60 "$bin" --fuzznet --fuzznet-dir="$dir/server" --socket="$dir/s/sock" \
		--udp-port="$port" --no-log-file "$@" >"$dir/server.log" 2>&1 &
	server=$!
	for _ in $(seq 1 50); do
		[ -S "$dir/s/sock" ] && return 0
		sleep 0.2
	done
	echo "check-fuzznet: beersshd --fuzznet did not start: $(tail -c 400 "$dir/server.log")" >&2
	exit 1
}
stop() {
	kill "$server" 2>/dev/null
	wait "$server" 2>/dev/null
	server=
	rm -f "$dir/s/sock"
}

# Twice to the same server: the second is a client come back after a drop,
# a new stream from offset 0 while the host still holds the first.
serve --sessions
for attempt in first second; do
	said=$(timeout 30 "$probe" "$image" 127.0.0.1 "$port" 2>"$dir/probe.err")
	if [ "$said" != "beerssh-session 1" ]; then
		stop
		echo "check-fuzznet: FAILED -- the $attempt time, the helper's version did not come" \
		     "back over fuzznet: \"$said\"; $(head -c 300 "$dir/probe.err")" >&2
		exit 1
	fi
done
stop

serve
if said=$(timeout 30 "$probe" "$image" 127.0.0.1 "$port" 2>/dev/null); then
	stop
	echo "check-fuzznet: FAILED -- beersshd without --sessions answered: \"$said\"" >&2
	exit 1
fi
stop

echo "check-fuzznet: paired with beersshd --fuzznet-pair; the helper answered over fuzznet with" \
     "--sessions, again to a new stream, and nothing answered without"
