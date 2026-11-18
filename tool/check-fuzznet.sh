#!/bin/sh
# beersshd as a fuzznet host (project.md sec 5.6): the whole pairing a client
# needs, through `beersshd --fuzznet` alone -- an estate root on one side, a
# device's prekey on the other, the card, its acceptance and the device image a
# client holds. Every step is fuzznet's; what is checked is that beersshd links
# and runs fuzznet's node as fuzznetd would.
#
# Service 1, product 1 is fuzznet's test pair, as its own live checks use:
# beerssh has no product number of its own yet (asked of fuzznet 2026-11-18).
#
# What stops it: every command exits by itself and each has a 20 s ceiling;
# nothing is served and nothing listens. The scratch is this script's own and
# removed on every path out.
set -u

bin=${1:?usage: check-fuzznet.sh path/to/beersshd}
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT INT TERM
pair="--fuzznet-service=1 --fuzznet-product=1"

# Made first: fuzznet given a directory that is not there says it created an
# identity in it and keeps none (reported to fuzznet 2026-11-18).
mkdir "$dir/server" "$dir/client" || exit 1

# One step; its last line of output in $out, and the script ends on a failure.
run() {
	# shellcheck disable=SC2086
	if ! timeout 20 "$bin" --fuzznet "$@" $pair >"$dir/out" 2>"$dir/err"; then
		echo "check-fuzznet: beersshd --fuzznet $* failed: $(head -c 400 "$dir/err")" >&2
		exit 1
	fi
	out=$(tail -n 1 "$dir/out")
}

run --fuzznet-dir="$dir/server" --new-root
run --fuzznet-dir="$dir/client" --prekey
run --fuzznet-dir="$dir/server" --pair="$out"
run --fuzznet-dir="$dir/client" --accept="$out"
run --fuzznet-dir="$dir/client" --device-image="$out"
image=$out

case "$image" in
FZD1:*) echo "check-fuzznet: paired through beersshd --fuzznet; the client's image is ${#image} characters" ;;
*) echo "check-fuzznet: FAILED -- no device image, got: $(printf '%s' "$image" | head -c 200)" >&2; exit 1 ;;
esac
