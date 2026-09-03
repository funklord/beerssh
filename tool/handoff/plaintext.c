/*
 * plaintext.c -- can a live, in-protocol TCP connection to a real sshd be
 * moved from one process to another without the peer noticing?
 *
 * This is the half of process migration (project.md sec 8.4) that has no
 * prior art to copy. CRIU solves the adjacent problem in the kernel with
 * TCP_REPAIR; nothing does it at library level. The two steps that decide
 * correctness are the DRAIN (bytes already in this process must cross with
 * the socket) and the COMMIT POINT (after it, exactly one process reads).
 *
 * Deliberately NO libssh. The SSH version exchange is plaintext and
 * stateful, which is exactly enough to make sshd itself the authority on
 * whether the connection stayed continuous: it sends KEXINIT only after a
 * valid banner exchange, and drops anything malformed. So a KEXINIT
 * arriving in the RECEIVING process is the peer saying "one unbroken
 * connection" -- which is the claim under test, and not one this program
 * gets to make about itself.
 *
 * The partial banner is the point. The parent deliberately reads only some
 * of it, leaving the rest in the kernel, so the handoff has to carry both
 * the socket AND the bytes already taken out of it. That is the shape of
 * the real problem: a libssh handoff must carry a partial packet the same
 * way.
 *
 * Three modes, because a probe that can only pass proves nothing:
 *
 *   clean       the handoff done correctly
 *   shutdown    parent calls shutdown() after passing the fd, which is the
 *               one thing it must never do -- close() is refcounted and
 *               invisible to the peer, shutdown() sends FIN
 *   doubleread  parent goes on reading after the commit point, stealing
 *               bytes the receiver needed
 *
 * The last two must FAIL. If they pass, this program cannot detect the
 * faults it exists to rule out.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

enum mode_t {
	MODE_CLEAN,
	MODE_SHUTDOWN,
	MODE_DOUBLEREAD
};

/* Every read is bounded: a probe that can hang is a probe nobody runs twice. */
static const int BSSH_PROBE_TIMEOUT_S = 5;

static int connect_to(int port) {
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;

	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((unsigned short)port);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}

	struct timeval tv;
	tv.tv_sec = BSSH_PROBE_TIMEOUT_S;
	tv.tv_usec = 0;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	return fd;
}

/*
 * Sends one fd plus the bytes this process has already taken off it.
 *
 * Both in ONE sendmsg, deliberately. Two messages could interleave with a
 * failure between them and leave the receiver holding a socket whose
 * already-read prefix it will never see -- which is the migration bug this
 * probe is meant to make impossible rather than merely unlikely.
 */
static int send_handoff(int channel, int payload_fd, const char *drained, size_t drained_len) {
	char control[CMSG_SPACE(sizeof(int))];
	memset(control, 0, sizeof(control));

	struct iovec io;
	io.iov_base = (void *)drained;
	io.iov_len = drained_len ? drained_len : 1;   /* sendmsg needs a byte */
	char zero = 0;
	if (!drained_len) io.iov_base = &zero;

	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &io;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(cmsg), &payload_fd, sizeof(int));

	ssize_t sent = sendmsg(channel, &msg, 0);
	return sent < 0 ? -1 : 0;
}

static int recv_handoff(int channel, int *payload_fd, char *drained, size_t cap, size_t *drained_len) {
	char control[CMSG_SPACE(sizeof(int))];
	memset(control, 0, sizeof(control));

	struct iovec io;
	io.iov_base = drained;
	io.iov_len = cap;

	struct msghdr msg;
	memset(&msg, 0, sizeof(msg));
	msg.msg_iov = &io;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	ssize_t got = recvmsg(channel, &msg, 0);
	if (got < 0) return -1;

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS) return -1;

	memcpy(payload_fd, CMSG_DATA(cmsg), sizeof(int));
	*drained_len = (size_t)got;
	return 0;
}

/* Reads until a newline or the cap, whichever comes first. */
static ssize_t read_line(int fd, char *buf, size_t cap) {
	size_t used = 0;
	while (used < cap) {
		ssize_t n = read(fd, buf + used, 1);
		if (n <= 0) return n == 0 ? (ssize_t)used : -1;
		used += (size_t)n;
		if (buf[used - 1] == '\n') break;
	}
	return (ssize_t)used;
}

int main(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: handoff <port> <clean|shutdown|doubleread>\n");
		return 2;
	}

	const int port = atoi(argv[1]);
	enum mode_t mode = MODE_CLEAN;
	if (strcmp(argv[2], "shutdown") == 0) mode = MODE_SHUTDOWN;
	else if (strcmp(argv[2], "doubleread") == 0) mode = MODE_DOUBLEREAD;

	int fd = connect_to(port);
	if (fd < 0) {
		fprintf(stderr, "probe: cannot reach 127.0.0.1:%d -- %s\n", port, strerror(errno));
		return 1;
	}

	/*
	 * The DRAIN, done badly on purpose: take only part of the banner and
	 * leave the rest in the kernel. A handoff that carries the socket but
	 * not this prefix loses it, and the loss shows up as a protocol error
	 * in the receiving process rather than here.
	 */
	char drained[64];
	ssize_t partial = read(fd, drained, 10);
	if (partial <= 0) {
		fprintf(stderr, "probe: sshd sent no banner\n");
		close(fd);
		return 1;
	}
	printf("parent: drained %zd bytes of banner before the handoff\n", partial);

	int channel[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, channel) != 0) return 1;

	/*
	 * Flushed before the fork, or the parent's buffered stdout is
	 * duplicated into the child and every line printed so far appears
	 * twice. Cosmetic in a probe and not cosmetic in the evidence: a
	 * transcript that shows the drain happening twice is one a reader has
	 * to discount, and discounting output is how a real anomaly gets
	 * discounted with it.
	 */
	fflush(stdout);

	pid_t child = fork();
	if (child < 0) return 1;

	if (child == 0) {
		/* ---- the RECEIVING process ---- */
		close(channel[0]);
		close(fd);                      /* inherited; the real one arrives by SCM_RIGHTS */

		int moved = -1;
		char prefix[64];
		size_t prefix_len = 0;
		if (recv_handoff(channel[1], &moved, prefix, sizeof(prefix), &prefix_len) != 0) {
			fprintf(stderr, "child: no fd arrived -- %s\n", strerror(errno));
			return 1;
		}
		printf("child: received the socket and %zu drained bytes\n", prefix_len);

		char rest[512];
		ssize_t got = read_line(moved, rest, sizeof(rest));
		if (got <= 0) {
			fprintf(stderr, "child: the moved socket gave nothing -- %s\n",
			         got == 0 ? "peer closed" : strerror(errno));
			return 1;
		}

		char banner[600];
		memcpy(banner, prefix, prefix_len);
		memcpy(banner + prefix_len, rest, (size_t)got);
		banner[prefix_len + (size_t)got] = 0;

		if (strncmp(banner, "SSH-", 4) != 0) {
			fprintf(stderr, "child: reassembled banner is not a banner: %.40s\n", banner);
			return 1;
		}
		printf("child: banner reassembled whole: %.*s\n",
		        (int)strcspn(banner, "\r\n"), banner);

		/*
		 * Now speak. sshd answers a valid banner with KEXINIT and drops
		 * anything malformed, so what happens next is the PEER's verdict
		 * on whether the connection stayed continuous across the move.
		 */
		const char *ours = "SSH-2.0-bssh_handoff_probe\r\n";
		if (write(moved, ours, strlen(ours)) < 0) {
			fprintf(stderr, "child: cannot write to the moved socket -- %s\n", strerror(errno));
			return 1;
		}

		unsigned char packet[16];
		ssize_t p = read(moved, packet, sizeof(packet));
		if (p < 6) {
			fprintf(stderr, "child: no KEXINIT after the handoff (%zd bytes) -- "
			                 "sshd did not treat this as one connection\n", p);
			return 1;
		}

		/* uint32 length, byte padding_length, then payload[0] = message type. */
		const unsigned char type = packet[5];
		if (type != 20) {
			fprintf(stderr, "child: first packet is type %u, not KEXINIT(20)\n", type);
			return 1;
		}

		printf("child: sshd replied KEXINIT across the handoff\n");
		return 0;
	}

	/* ---- the ORIGINATING process ---- */
	close(channel[1]);

	if (send_handoff(channel[0], fd, drained, (size_t)partial) != 0) {
		fprintf(stderr, "parent: sendmsg failed -- %s\n", strerror(errno));
		return 1;
	}
	printf("parent: passed the socket and its drained prefix\n");

	if (mode == MODE_DOUBLEREAD) {
		/*
		 * The commit point violated. The parent has handed the socket
		 * over and goes on reading anyway, so bytes the receiver needs
		 * are consumed here instead. This must break the child.
		 */
		char stolen[64];
		ssize_t n = read(fd, stolen, sizeof(stolen));
		printf("parent: STOLE %zd bytes after the commit point\n", n);
	}

	if (mode == MODE_SHUTDOWN) {
		/*
		 * The one thing a departing process must never do. close() only
		 * drops a reference and the peer sees nothing; shutdown() sends
		 * FIN down a connection somebody else now owns.
		 */
		shutdown(fd, SHUT_RDWR);
		printf("parent: called shutdown() on a socket it had given away\n");
	}

	close(fd);
	close(channel[0]);

	int status = 0;
	waitpid(child, &status, 0);

	const int child_ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	printf("result: child %s\n", child_ok ? "COMPLETED the session" : "FAILED");
	return child_ok ? 0 : 1;
}
