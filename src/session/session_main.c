/*
 * beerssh-session: the server side of a session that outlives its connection
 * (project.md sec 5.6), run on the host by whatever carries the connection.
 *
 *     beerssh-session version       says the protocol it speaks, and exits
 *     beerssh-session attach NAME   joins stdin and stdout to session NAME,
 *                                   starting it first if there is none
 *
 * The first is the probe an SSH client negotiates with: a host without this
 * program answers "command not found" and stays plain SSH. The second is what
 * an exec channel runs. It ends when the connection does, and the session goes
 * on without it, so the next attach finds the same shell.
 *
 * The session's own process leaves the connection behind on purpose: new
 * session, stdio on /dev/null, or the drop that closes the channel would take
 * it too. What ends it is what ends bssh_session_host_run().
 */

#define _GNU_SOURCE

#include "session/session_host.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PROTOCOL_VERSION 1
#define LOG_BYTES (8u * 1024u * 1024u)
#define LOG_MARKS 4096u
#define LINGER_MS (10 * 60 * 1000)
#define START_WAIT_MS 3000

static int name_is_safe(const char *name) {
	if (!*name || strlen(name) > 64) return 0;
	for (const char *at = name; *at; ++at) {
		const char c = *at;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
		      || c == '_'))
			return 0;
	}
	return 1;
}

/*
 * A directory only this user can reach. /tmp is anybody's, so a directory found
 * there is believed only if it is ours and closed to everyone else.
 */
static int socket_directory(char *path, size_t capacity) {
	const char *runtime = getenv("XDG_RUNTIME_DIR");
	const int len = runtime && *runtime
	                        ? snprintf(path, capacity, "%s/beerssh-session", runtime)
	                        : snprintf(path, capacity, "/tmp/beerssh-session-%u", (unsigned)geteuid());
	if (len < 0 || (size_t)len >= capacity) return -1;

	if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;

	struct stat info;
	if (lstat(path, &info) != 0 || !S_ISDIR(info.st_mode)) return -1;
	if (info.st_uid != geteuid() || (info.st_mode & 0077)) return -1;
	return 0;
}

static int connect_to(const char *path) {
	struct sockaddr_un address;

	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(address.sun_path)) return -1;
	strcpy(address.sun_path, path);

	const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static const char *login_shell(void) {
	/* sshd always sets it, and a static binary has no passwd lookup to trust. */
	const char *shell = getenv("SHELL");
	return shell && *shell ? shell : "/bin/sh";
}

/* Leaves the connection behind and becomes the session. Does not return. */
static void become_session(const char *path) {
	if (setsid() < 0) _exit(1);
	if (fork() != 0) _exit(0);

	const int null = open("/dev/null", O_RDWR);
	if (null < 0) _exit(1);
	dup2(null, STDIN_FILENO);
	dup2(null, STDOUT_FILENO);
	dup2(null, STDERR_FILENO);
	if (null > STDERR_FILENO) close(null);

	const char *home = getenv("HOME");
	if (home && chdir(home) != 0) _exit(1);

	uint8_t *bytes = malloc(LOG_BYTES);
	bssh_output_log_mark_t *marks = malloc(LOG_MARKS * sizeof(*marks));
	if (!bytes || !marks) _exit(1);

	const char *shell = login_shell();
	char *const argv[] = { (char *)shell, (char *)"-l", NULL };

	bssh_session_host_options_t options;
	options.socket_path = path;
	options.argv = argv;
	options.log_bytes = bytes;
	options.log_capacity = LOG_BYTES;
	options.log_marks = marks;
	options.log_marks_capacity = LOG_MARKS;
	options.linger_ms = LINGER_MS;

	bssh_session_host_run(&options);
	_exit(0);
}

static int start_and_connect(const char *path) {
	/* A socket file nobody answers is a host that was killed: not a session. */
	if (unlink(path) != 0 && errno != ENOENT) return -1;

	const pid_t child = fork();
	if (child < 0) return -1;
	if (child == 0) become_session(path);

	/* It forks and leaves at once, and nothing else here would reap it. */
	waitpid(child, NULL, 0);

	for (int waited = 0; waited < START_WAIT_MS; waited += 10) {
		const int fd = connect_to(path);
		if (fd >= 0) return fd;

		const struct timespec pause = { 0, 10 * 1000 * 1000 };
		nanosleep(&pause, NULL);
	}
	return -1;
}

/* All of it, or -1. The far side may take its time; this has nothing else to do. */
static int write_all(int fd, const uint8_t *bytes, size_t len) {
	while (len) {
		const ssize_t wrote = write(fd, bytes, len);
		if (wrote < 0) {
			if (errno == EINTR) continue;
			return -1;
		}
		bytes += wrote;
		len -= (size_t)wrote;
	}
	return 0;
}

/* stdin to the session and the session to stdout, until either ends. */
static int join(int session) {
	struct pollfd fds[2] = { { STDIN_FILENO, POLLIN, 0 }, { session, POLLIN, 0 } };
	static uint8_t chunk[65536];

	for (;;) {
		if (poll(fds, 2, -1) < 0) {
			if (errno == EINTR) continue;
			return 1;
		}
		for (int i = 0; i < 2; ++i) {
			if (!fds[i].revents) continue;

			const ssize_t got = read(fds[i].fd, chunk, sizeof(chunk));
			if (got < 0 && errno == EINTR) continue;
			if (got <= 0) return 0;
			if (write_all(i == 0 ? session : STDOUT_FILENO, chunk, (size_t)got) != 0) return 0;
		}
	}
}

static int attach(const char *name) {
	if (!name_is_safe(name)) {
		fprintf(stderr, "beerssh-session: a name is letters, digits, - and _\n");
		return 2;
	}

	char directory[PATH_MAX];
	if (socket_directory(directory, sizeof(directory)) != 0) {
		fprintf(stderr, "beerssh-session: no private directory for sessions\n");
		return 1;
	}

	char path[PATH_MAX + 80];
	snprintf(path, sizeof(path), "%s/%s", directory, name);

	int session = connect_to(path);
	if (session < 0) session = start_and_connect(path);
	if (session < 0) {
		fprintf(stderr, "beerssh-session: could not start session %s\n", name);
		return 1;
	}
	return join(session);
}

int main(int argc, char *argv[]) {
	if (argc == 2 && !strcmp(argv[1], "version")) {
		printf("beerssh-session %d\n", PROTOCOL_VERSION);
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "attach")) return attach(argv[2]);

	fprintf(stderr, "usage: beerssh-session version | attach NAME\n");
	return 2;
}
