/*
 * authenticated.c -- does the handoff discipline hold for an ENCRYPTED,
 * authenticated, channel-open libssh session?
 *
 * plaintext.c proved the substrate on a plaintext banner exchange: a socket
 * and the bytes already drained off it can cross a process boundary with
 * sshd noticing nothing. What it could not touch is the case that matters,
 * where there are cipher contexts, MAC keys, sequence numbers and an open
 * channel on both sides.
 *
 * This separates the two questions that were tangled together:
 *
 *   1. does the DISCIPLINE hold under encryption -- one reader after the
 *      commit point, no shutdown(), nothing lost?
 *   2. can libssh's state be SERIALISED?
 *
 * Only the first is asked here, and fork() is what makes that possible:
 * the child inherits libssh's entire heap along with the fd, so the state
 * "transfers" without a serialiser existing. If an authenticated session
 * survives that, question 1 is answered yes for the real case and what
 * remains is purely question 2 -- laborious rather than unknown.
 *
 * fork() is NOT being proposed as the mechanism. It cannot move a session
 * into an already-running process, which is the actual feature. It is a
 * way to hold question 2 still while question 1 is settled.
 *
 * Two modes, because a probe that can only pass proves nothing:
 *
 *   clean       the parent stops touching the session at the commit point
 *   doubleread  the parent keeps reading the channel afterwards
 *
 * The second must FAIL, or this cannot see the fault it exists to rule out.
 */

#define _GNU_SOURCE

#include <libssh/libssh.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const long BSSH_PROBE_TIMEOUT_S = 5;

/* Runs one command down an already-open session and returns its output. */
static int run_command(ssh_session session, const char *command, char *out, int cap) {
	ssh_channel channel = ssh_channel_new(session);
	if (!channel) return -1;

	if (ssh_channel_open_session(channel) != SSH_OK) {
		ssh_channel_free(channel);
		return -1;
	}
	if (ssh_channel_request_exec(channel, command) != SSH_OK) {
		ssh_channel_close(channel);
		ssh_channel_free(channel);
		return -1;
	}

	const int got = ssh_channel_read(channel, out, (uint32_t)cap - 1, 0);
	if (got >= 0) out[got] = 0;

	ssh_channel_send_eof(channel);
	ssh_channel_close(channel);
	ssh_channel_free(channel);
	return got;
}

int main(int argc, char **argv) {
	if (argc < 5) {
		fprintf(stderr, "usage: authed <port> <user> <keyfile> <clean|doubleread>\n");
		return 2;
	}

	const int port = atoi(argv[1]);
	const char *user = argv[2];
	const char *keyfile = argv[3];
	const int doubleread = strcmp(argv[4], "doubleread") == 0;

	ssh_session session = ssh_new();
	if (!session) return 1;

	const char *host = "127.0.0.1";
	long timeout = BSSH_PROBE_TIMEOUT_S;
	int no = 0;
	ssh_options_set(session, SSH_OPTIONS_HOST, host);
	ssh_options_set(session, SSH_OPTIONS_PORT, &port);
	ssh_options_set(session, SSH_OPTIONS_USER, user);
	ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &timeout);
	ssh_options_set(session, SSH_OPTIONS_STRICTHOSTKEYCHECK, &no);
	ssh_options_set(session, SSH_OPTIONS_KNOWNHOSTS, "/dev/null");
	ssh_options_set(session, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, "/dev/null");

	/*
	 * Compression off, and stated rather than defaulted. A migratable
	 * session cannot carry a zlib stream across a process boundary: zlib
	 * has deflateCopy but no serialisation at all, so this is a constraint
	 * the real design has to declare, not a convenience here.
	 */
	ssh_options_set(session, SSH_OPTIONS_COMPRESSION, "no");

	if (ssh_connect(session) != SSH_OK) {
		fprintf(stderr, "probe: connect failed -- %s\n", ssh_get_error(session));
		ssh_free(session);
		return 1;
	}

	if (ssh_userauth_publickey_auto(session, NULL, NULL) != SSH_AUTH_SUCCESS) {
		/* The generated key is not in an agent, so offer it by name. */
		ssh_key key = NULL;
		if (ssh_pki_import_privkey_file(keyfile, NULL, NULL, NULL, &key) != SSH_OK) {
			fprintf(stderr, "probe: cannot read %s\n", keyfile);
			ssh_disconnect(session);
			ssh_free(session);
			return 1;
		}
		const int rc = ssh_userauth_publickey(session, NULL, key);
		ssh_key_free(key);
		if (rc != SSH_AUTH_SUCCESS) {
			fprintf(stderr, "probe: auth failed -- %s\n", ssh_get_error(session));
			ssh_disconnect(session);
			ssh_free(session);
			return 1;
		}
	}

	char before[256];
	if (run_command(session, "echo before-handoff", before, sizeof(before)) < 0) {
		fprintf(stderr, "probe: the session does not work before the handoff\n");
		ssh_disconnect(session);
		ssh_free(session);
		return 1;
	}
	printf("parent: authenticated, and the session works: %s", before);

	fflush(stdout);
	pid_t child = fork();
	if (child < 0) return 1;

	if (child == 0) {
		/*
		 * ---- the RECEIVING process ----
		 *
		 * Same ssh_session pointer, inherited: cipher contexts, MAC keys,
		 * sequence numbers and the socket all came across with the memory
		 * image. Nothing was serialised, which is the point -- this asks
		 * whether the DISCIPLINE holds, not whether a serialiser could.
		 */
		char after[256];
		const int got = run_command(session, "echo after-handoff", after, sizeof(after));
		if (got < 0) {
			fprintf(stderr, "child: the session did not survive the handoff -- %s\n",
			         ssh_get_error(session));
			_exit(1);
		}
		if (strncmp(after, "after-handoff", 13) != 0) {
			fprintf(stderr, "child: wrong output after the handoff: %.40s\n", after);
			_exit(1);
		}
		printf("child: ran a command on the moved session: %s", after);

		/*
		 * _exit, not return: the atexit handlers and any libssh cleanup
		 * would run in a process that is deliberately sharing state with
		 * one that is still alive.
		 */
		fflush(stdout);
		_exit(0);
	}

	/* ---- the ORIGINATING process ---- */
	if (doubleread) {
		/*
		 * The commit point violated under encryption. Reading here
		 * consumes packets the child needs AND advances this side's
		 * receive sequence number, so the two processes disagree about
		 * where the stream is and the MAC stops verifying.
		 */
		char stolen[256];
		const int n = run_command(session, "echo stolen", stolen, sizeof(stolen));
		printf("parent: kept using the session after the commit point (rc=%d)\n", n);
	}

	int status = 0;
	waitpid(child, &status, 0);
	const int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;

	/*
	 * The parent must NOT ssh_disconnect() here: that writes a disconnect
	 * message down a connection it has given away, which is the encrypted
	 * form of the shutdown() fault plaintext.c demonstrated. It leaks the
	 * ssh_session deliberately and says so.
	 */
	printf("result: child %s\n", ok ? "COMPLETED the session" : "FAILED");
	fflush(stdout);
	_exit(ok ? 0 : 1);
}
