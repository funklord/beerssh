/*
 * migrate.c -- does a live session survive being EXPORTED from one process
 * and IMPORTED into another, through the vendored libssh's transport
 * export (libssh-patch/, project.md sec 8.4)?
 *
 * authenticated.c settled the discipline by fork(), which carries libssh's
 * heap across and so serialises nothing. This is the real mechanism: the
 * receiving process is exec'd -- a fresh image holding none of the
 * parent's memory, and not even the parent's copy of the socket, which is
 * close-on-exec -- and gets the session only as a blob and a descriptor
 * passed over a unix socket by SCM_RIGHTS.
 *
 * What the parent leaves in flight before the move, so the import has to
 * carry it rather than meet a quiet connection:
 *
 *   - an echo libssh has already read into the channel's buffer and the
 *     caller has not taken;
 *   - an echo still in the kernel, sent and never polled for.
 *
 * What the child then proves:
 *
 *   - both echoes arrive, in order;
 *   - 128 KiB more goes through the same channel, in both directions,
 *     while the server rekeys every 16 KiB (sshd's RekeyLimit, set by
 *     run.sh) and this side every 24 KiB (carried in the export), so key
 *     exchanges in both directions run on the imported state;
 *   - the parent's detach after the child's acknowledgement sends nothing:
 *     the child's traffic happens after it.
 *
 * Modes, because a probe that can only pass proves nothing:
 *
 *   clean     the parent stops at the export, and detaches once told to
 *   steal     the parent reads the channel after exporting -- the commit
 *             point violated, which must break the child
 *
 * Usage: migrate <port> <user> <keyfile> <cipher> <mac|-> <clean|steal>
 */

#define _GNU_SOURCE

#include <libssh/libssh.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Every wait is bounded; the whole probe is under run.sh's timeout too. */
static const int BSSH_PROBE_READ_MS = 10000;
static const unsigned BSSH_PROBE_ALARM_S = 50;

/* ---- passing the descriptor and the state ----------------------------- */

/* One message: a length, the bytes, and optionally a descriptor. */
static int send_message(int sock, const void *data, uint32_t len, int fd) {
	struct iovec iov[2] = {
		{ .iov_base = &len, .iov_len = sizeof(len) },
		{ .iov_base = (void *)data, .iov_len = len },
	};
	char control[CMSG_SPACE(sizeof(int))];
	struct msghdr msg = { .msg_iov = iov, .msg_iovlen = 2 };

	if (fd >= 0) {
		memset(control, 0, sizeof(control));
		msg.msg_control = control;
		msg.msg_controllen = sizeof(control);
		struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
	}
	const ssize_t total = (ssize_t)(sizeof(len) + len);
	return sendmsg(sock, &msg, 0) == total ? 0 : -1;
}

/* The counterpart: a malloc'd payload, and the descriptor if one came. */
static int receive_message(int sock, char **data, uint32_t *len, int *fd) {
	char control[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { .iov_base = len, .iov_len = sizeof(*len) };
	struct msghdr msg = {
		.msg_iov = &iov, .msg_iovlen = 1,
		.msg_control = control, .msg_controllen = sizeof(control),
	};

	*fd = -1;
	if (recvmsg(sock, &msg, MSG_CMSG_CLOEXEC) != sizeof(*len)) return -1;
	for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
			memcpy(fd, CMSG_DATA(c), sizeof(int));
	}
	if (*len > (1u << 20)) return -1;
	*data = malloc(*len + 1);
	if (!*data) return -1;
	size_t got = 0;
	while (got < *len) {
		const ssize_t n = read(sock, *data + got, *len - got);
		if (n <= 0) return -1;
		got += (size_t)n;
	}
	(*data)[*len] = 0;
	return 0;
}

/* ---- reading a known reply off a channel ------------------------------ */

/* Reads until `want` has arrived in full, or fails naming what came. */
static int expect(ssh_channel channel, const char *want, const char *who) {
	const size_t len = strlen(want);
	char got[4096];
	size_t have = 0;

	if (len >= sizeof(got)) return -1;
	while (have < len) {
		const int n = ssh_channel_read_timeout(channel, got + have,
		                                       (uint32_t)(len - have), 0,
		                                       BSSH_PROBE_READ_MS);
		if (n <= 0) {
			fprintf(stderr, "%s: waiting for %.20s, read returned %d -- %s\n", who,
			         want, n, ssh_get_error(ssh_channel_get_session(channel)));
			return -1;
		}
		have += (size_t)n;
	}
	if (memcmp(got, want, len) != 0) {
		fprintf(stderr, "%s: expected %.20s, got %.20s\n", who, want, got);
		return -1;
	}
	return 0;
}

static int write_all(ssh_channel channel, const char *data) {
	const uint32_t len = (uint32_t)strlen(data);
	return ssh_channel_write(channel, data, len) == (int)len ? 0 : -1;
}

/* ---- the receiving process -------------------------------------------- */

static int receive(int sock) {
	char *blob = NULL;
	uint32_t blob_len = 0;
	int fd = -1;

	if (receive_message(sock, &blob, &blob_len, &fd) != 0 || fd < 0) {
		fprintf(stderr, "child: no state or no descriptor arrived\n");
		return 1;
	}

	ssh_session session = ssh_new();
	ssh_string state = ssh_string_new(blob_len);
	if (!session || !state || ssh_string_fill(state, blob, blob_len) != 0) return 1;
	memset(blob, 0, blob_len);
	free(blob);

	if (ssh_session_import_transport(session, fd, state) != SSH_OK) {
		fprintf(stderr, "child: import failed -- %s\n", ssh_get_error(session));
		return 1;
	}
	ssh_string_burn(state);
	ssh_string_free(state);
	ssh_channel channel = ssh_session_get_channel(session, 0);
	if (!channel) {
		fprintf(stderr, "child: the imported session has no channel\n");
		return 1;
	}
	printf("child: imported the session and its channel\n");

	/* Committed: the parent lets go, and everything after this is ours. */
	if (send_message(sock, "imported", 8, -1) != 0) return 1;
	char *ack = NULL;
	uint32_t ack_len = 0;
	int unused = -1;
	if (receive_message(sock, &ack, &ack_len, &unused) != 0) return 1;
	free(ack);

	/* The two echoes left in flight: one in the channel buffer, one in
	 * the kernel. */
	if (expect(channel, "two\nthree\n", "child") != 0) return 1;
	printf("child: both echoes left in flight arrived\n");

	/* Enough traffic to cross the server's and this side's rekey limits
	 * several times over. */
	char line[513];
	for (int i = 0; i < 256; i++) {
		memset(line, 'a' + i % 26, 511);
		line[511] = '\n';
		line[512] = 0;
		if (write_all(channel, line) != 0 || expect(channel, line, "child") != 0) {
			fprintf(stderr, "child: traffic failed at line %d\n", i);
			return 1;
		}
	}
	printf("child: 128 KiB each way through the moved session\n");

	ssh_channel_send_eof(channel);
	ssh_channel_close(channel);
	ssh_channel_free(channel);
	ssh_disconnect(session);
	ssh_free(session);
	fflush(stdout);
	return 0;
}

/* ---- the giving process ------------------------------------------------- */

static int authenticate(ssh_session session, const char *keyfile) {
	if (ssh_userauth_publickey_auto(session, NULL, NULL) == SSH_AUTH_SUCCESS) return 0;
	ssh_key key = NULL;
	if (ssh_pki_import_privkey_file(keyfile, NULL, NULL, NULL, &key) != SSH_OK) return -1;
	const int rc = ssh_userauth_publickey(session, NULL, key);
	ssh_key_free(key);
	return rc == SSH_AUTH_SUCCESS ? 0 : -1;
}

int main(int argc, char **argv) {
	alarm(BSSH_PROBE_ALARM_S);

	if (argc == 3 && strcmp(argv[1], "receive") == 0)
		return receive(atoi(argv[2]));

	if (argc < 7) {
		fprintf(stderr, "usage: migrate <port> <user> <keyfile> <cipher> <mac|-> <clean|steal>\n");
		return 2;
	}
	const int port = atoi(argv[1]);
	const char *user = argv[2], *keyfile = argv[3], *cipher = argv[4], *mac = argv[5];
	const int steal = strcmp(argv[6], "steal") == 0;

	ssh_session session = ssh_new();
	if (!session) return 1;
	long timeout = 5;
	int no = 0;
	uint64_t rekey = 24 * 1024;
	ssh_options_set(session, SSH_OPTIONS_HOST, "127.0.0.1");
	ssh_options_set(session, SSH_OPTIONS_PORT, &port);
	ssh_options_set(session, SSH_OPTIONS_USER, user);
	ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
	ssh_options_set(session, SSH_OPTIONS_STRICTHOSTKEYCHECK, &no);
	ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, "/dev/null");
	ssh_options_set(session, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, "/dev/null");
	ssh_options_set(session, SSH_OPTIONS_COMPRESSION, "no");
	ssh_options_set(session, SSH_OPTIONS_CIPHERS_C_S, cipher);
	ssh_options_set(session, SSH_OPTIONS_CIPHERS_S_C, cipher);
	if (strcmp(mac, "-") != 0) {
		ssh_options_set(session, SSH_OPTIONS_HMAC_C_S, mac);
		ssh_options_set(session, SSH_OPTIONS_HMAC_S_C, mac);
	}
	ssh_options_set(session, SSH_OPTIONS_REKEY_DATA, &rekey);

	if (ssh_connect(session) != SSH_OK || authenticate(session, keyfile) != 0) {
		fprintf(stderr, "parent: cannot connect -- %s\n", ssh_get_error(session));
		return 1;
	}

	ssh_channel channel = ssh_channel_new(session);
	if (!channel || ssh_channel_open_session(channel) != SSH_OK ||
	    ssh_channel_request_exec(channel, "cat") != SSH_OK) {
		fprintf(stderr, "parent: cannot start cat -- %s\n", ssh_get_error(session));
		return 1;
	}
	if (write_all(channel, "one\n") != 0 || expect(channel, "one\n", "parent") != 0) return 1;
	printf("parent: %s session running cat\n", cipher);

	/* Left in flight: "two" read by libssh into the channel buffer and
	 * not taken; "three" still in the kernel. */
	if (write_all(channel, "two\n") != 0) return 1;
	if (ssh_channel_poll_timeout(channel, BSSH_PROBE_READ_MS, 0) <= 0) {
		fprintf(stderr, "parent: the second echo never arrived\n");
		return 1;
	}
	if (write_all(channel, "three\n") != 0) return 1;

	/* The export: retried while a key exchange or a packet is in progress,
	 * WITHOUT reading the channel, which would take the echoes. */
	ssh_string state = NULL;
	int rc;
	for (int tries = 0; (rc = ssh_session_export_transport(session, &state)) == SSH_AGAIN; tries++) {
		if (tries > 100) {
			fprintf(stderr, "parent: export kept answering SSH_AGAIN -- %s\n",
			         ssh_get_error(session));
			return 1;
		}
		usleep(20000);
	}
	if (rc != SSH_OK) {
		fprintf(stderr, "parent: export failed -- %s\n", ssh_get_error(session));
		return 1;
	}
	printf("parent: exported %zu bytes of state\n", ssh_string_len(state));

	/* The receiver is exec'd, and must not inherit the socket: it gets it
	 * only from the message. */
	const int ssh_fd = ssh_get_fd(session);
	fcntl(ssh_fd, F_SETFD, FD_CLOEXEC);
	int pair[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return 1;
	fcntl(pair[0], F_SETFD, FD_CLOEXEC);
	fflush(stdout);
	const pid_t child = fork();
	if (child < 0) return 1;
	if (child == 0) {
		char fdstr[16];
		snprintf(fdstr, sizeof(fdstr), "%d", pair[1]);
		execl("/proc/self/exe", argv[0], "receive", fdstr, (char *)NULL);
		_exit(127);
	}
	close(pair[1]);

	if (send_message(pair[0], ssh_string_data(state), (uint32_t)ssh_string_len(state),
	                 ssh_fd) != 0) {
		fprintf(stderr, "parent: could not send the state\n");
		return 1;
	}
	ssh_string_burn(state);
	ssh_string_free(state);

	if (steal) {
		/* The fault: this process goes on reading after the export, taking
		 * echoes the child needs and advancing a sequence number the child
		 * also holds. */
		/* Read until "three" is taken: "two" comes from this process's own
		 * copy of the channel buffer and costs the child nothing, so
		 * stopping there would leave the fault unexercised. */
		char stolen[64];
		size_t taken = 0;
		while (taken < 10) {
			const int n = ssh_channel_read_timeout(channel, stolen + taken,
			                                       (uint32_t)(sizeof(stolen) - 1 - taken),
			                                       0, 2000);
			if (n <= 0) break;
			taken += (size_t)n;
		}
		printf("parent: read %zu bytes of the channel after exporting\n", taken);
	}

	char *reply = NULL;
	uint32_t reply_len = 0;
	int unused = -1;
	int committed = receive_message(pair[0], &reply, &reply_len, &unused) == 0 &&
	                strcmp(reply, "imported") == 0;
	free(reply);
	if (committed) {
		/* Let go: close this copy, and make sure nothing is ever sent. */
		ssh_session_detach_transport(session);
		ssh_channel_free(channel);
		ssh_disconnect(session);
		ssh_free(session);
		send_message(pair[0], "released", 8, -1);
	}

	int status = 0;
	waitpid(child, &status, 0);
	const int ok = committed && WIFEXITED(status) && WEXITSTATUS(status) == 0;
	printf("result: the moved session %s\n", ok ? "WORKED" : "FAILED");
	return ok ? 0 : 1;
}
