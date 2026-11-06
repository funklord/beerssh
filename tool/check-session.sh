#!/bin/sh
# A shell surviving a real SSH drop (project.md sec 5.6): beerssh-session in a
# throwaway sshd container, the connection killed with a job running, and the
# same shell found on the next one, with everything written meanwhile.
#
# What stops it: sshd inside the container exits after 300 s whatever happens,
# the client has a deadline on every read, and the trap kills the container by
# name. A killed script leaves `docker ps --filter name=bssh-session-check`.
set -u

bin=${1:?usage: check-session.sh path/to/beerssh-session}
name=bssh-session-check-$$
port=$((20000 + $$ % 20000))
dir=$(mktemp -d)
here=$(dirname "$0")

finish() {
	docker kill "$name" >/dev/null 2>&1
	rm -f "$dir/key" "$dir/key.pub"
	rmdir "$dir"
}
trap finish EXIT INT TERM

ssh-keygen -q -t ed25519 -N '' -f "$dir/key" || exit 1
docker run -d --rm --name "$name" -p "127.0.0.1:$port:22" alpine:3.21 sh -c \
	'apk add -q openssh >/dev/null && adduser -D fold && passwd -u fold >/dev/null 2>&1
	 mkdir -p /home/fold/.ssh && ssh-keygen -A >/dev/null && touch /tmp/ready
	 exec timeout 300 /usr/sbin/sshd -D -e' >/dev/null || exit 1

for i in $(seq 1 60); do
	docker exec "$name" test -e /tmp/ready 2>/dev/null && break
	sleep 1
done

docker cp "$bin" "$name:/tmp/beerssh-session" || exit 1
docker cp "$dir/key.pub" "$name:/home/fold/.ssh/authorized_keys" || exit 1
docker exec "$name" sh -c 'chown -R fold /home/fold/.ssh; chmod 700 /home/fold/.ssh
	chmod 600 /home/fold/.ssh/authorized_keys; chmod 755 /tmp/beerssh-session' || exit 1

timeout 90 python3 -I "$here/check-session.py" "$dir/key" "$port"
status=$?

sleep 1
left=$(docker exec "$name" ps -o args 2>/dev/null | grep -c '[b]eerssh-session')
echo "sessions left on the host after exit: $left"
[ "$left" = 0 ] || status=1
[ "$status" = 0 ] && echo "check-session: passed" || echo "check-session: FAILED"
exit "$status"
