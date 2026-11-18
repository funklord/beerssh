/*
 * beersshd as a fuzznet host (fuzznet_node.h, project.md sec 5.6).
 *
 * WHAT A HOST SAYS IS A STREAM (src/session/stream.h): fuzznet's words go
 * once and in no promised order, so each host that speaks has a reliable
 * stream here, and what it carries is what an SSH exec channel carries to
 * the session helper. Its first line is the command -- "beerssh-session
 * version" or "beerssh-session attach NAME", word for word the two an exec
 * request may name (helper_channel.h) -- and everything after it is the
 * helper's input; the helper's output goes back the same way.
 *
 * OFF UNLESS ASKED, `--sessions`, as on the SSH side: a paired host that may
 * speak (`remote/permit/said`) would otherwise get a shell as this user
 * without anybody having said it should.
 *
 * ONE LOOP, fuzznet's: the helpers' descriptors are polled in it (`fds`,
 * `ready`), and everything that sends runs from `tick`, which fuzznet calls
 * after every pass and at least about once a second -- so a timeout the
 * stream asks for can be up to a second late, which costs a slow resend and
 * nothing else.
 */

#define _GNU_SOURCE

#include "fuzznet_node.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../posix_compat.h"
#include "../session/stream.h"
#include "chain/service.h"
#include "node/daemon.h"

/* Hosts speaking at once; a sixteenth more is not heard until one ends. */
#define LINKS_MAX 16
/* A word's payload, less the stream's header, with room to spare. */
#define SEGMENT_DATA 960u
#define RING 65536u
#define TO_HELPER 65536u
#define COMMAND_MAX 128u
/* A link whose helper has gone and whose host has said nothing for this long. */
#define IDLE_MS 600000u

typedef struct link {
	int used;
	uint8_t host[FZN_PUBKEY_LEN];
	bssh_stream_t stream;
	uint8_t ring[RING];
	uint64_t heard_ms;
	/* The stream this one replaced, whose late segments are not a third. */
	uint64_t retired;
	int has_retired;

	/* The command line, until its newline. */
	char command[COMMAND_MAX];
	size_t command_len;
	int started;
	/* Ended from inside the stream's own call, so gone at the next tick. */
	int dead;

	pid_t pid;
	int io;
	int input_shut;
	int output_ended;
	uint8_t to_helper[TO_HELPER];
	size_t to_helper_len;
} link_t;

static struct {
	int sessions;
	char helper[PATH_MAX];
	link_t links[LINKS_MAX];
} node;

static uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* The helper's own rule for a name (src/session/session_main.c). */
static int name_is_safe(const char *name, size_t len) {
	if (len == 0 || len > 64) return 0;
	for (size_t i = 0; i < len; ++i) {
		const char c = name[i];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
		      || c == '-' || c == '_'))
			return 0;
	}
	return 1;
}

static void end_link(link_t *link) {
	if (link->io >= 0) close(link->io);
	if (link->pid > 0) {
		kill(link->pid, SIGHUP);
		while (waitpid(link->pid, NULL, 0) < 0 && errno == EINTR) {
		}
	}
	memset(link, 0, sizeof(*link));
	link->io = -1;
}

/*
 * The helper with the words the command named, its stdin and stdout one
 * socket here, and nothing else of this process's held open in it.
 */
static int start_helper(link_t *link, char *const argv[]) {
	int io[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, io) != 0) return 0;
	if (bssh_set_fd_flags(io[0], BSSH_CLOEXEC | BSSH_NONBLOCK) != 0 || bssh_no_sigpipe(io[0]) != 0) {
		close(io[0]);
		close(io[1]);
		return 0;
	}
	struct rlimit limit;
	const int highest = getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY
	                            && limit.rlim_cur < 65536
	                    ? (int)limit.rlim_cur
	                    : 65536;
	const pid_t pid = fork();
	if (pid < 0) {
		close(io[0]);
		close(io[1]);
		return 0;
	}
	if (pid == 0) {
		if (dup2(io[1], STDIN_FILENO) < 0 || dup2(io[1], STDOUT_FILENO) < 0) _exit(127);
		/* stderr stays the node's, whose log is where a helper's trouble belongs. */
		for (int fd = STDERR_FILENO + 1; fd < highest; ++fd) close(fd);
		signal(SIGPIPE, SIG_DFL);
		execv(node.helper, argv);
		_exit(127);
	}
	close(io[1]);
	link->pid = pid;
	link->io = io[0];
	return 1;
}

/* The command line, whole: the helper started, or the link ended. */
static void command_given(link_t *link) {
	static const char prefix[] = "beerssh-session ";
	char *const command = link->command;
	const size_t len = link->command_len;
	char name[65];

	if (len > sizeof(prefix) - 1 && !memcmp(command, prefix, sizeof(prefix) - 1)) {
		const char *rest = command + sizeof(prefix) - 1;
		const size_t rest_len = len - (sizeof(prefix) - 1);
		if (rest_len == 7 && !memcmp(rest, "version", 7)) {
			char *const argv[] = { (char *)"beerssh-session", (char *)"version", NULL };
			if (start_helper(link, argv)) {
				link->started = 1;
				return;
			}
		} else if (rest_len > 7 && !memcmp(rest, "attach ", 7)
		           && name_is_safe(rest + 7, rest_len - 7)) {
			memcpy(name, rest + 7, rest_len - 7);
			name[rest_len - 7] = '\0';
			char *const argv[] = { (char *)"beerssh-session", (char *)"attach", name, NULL };
			if (start_helper(link, argv)) {
				link->started = 1;
				return;
			}
		}
	}
	fprintf(stderr, "beersshd: refused a fuzznet session's command\n");
	link->dead = 1;
}

/* The host's stream, in order: the command first, then the helper's input. */
static void delivered(void *context, const uint8_t *bytes, size_t len) {
	link_t *link = context;
	if (link->dead) return;
	while (len && !link->dead && !link->started) {
		const uint8_t c = *bytes++;
		--len;
		if (c == '\n') {
			command_given(link);
			continue;
		}
		if (link->command_len == COMMAND_MAX) {
			link->dead = 1;
			return;
		}
		link->command[link->command_len++] = (char)c;
	}
	if (!len || link->dead) return;
	if (len > TO_HELPER - link->to_helper_len) {
		/*
		 * The helper is not reading what the host sends: keystrokes and
		 * acknowledgements, which a working helper never falls this far
		 * behind on. Ended rather than dropped, since a stream with a
		 * hole in it is not the session's protocol any more.
		 */
		fprintf(stderr, "beersshd: a fuzznet session's helper stopped reading\n");
		link->dead = 1;
		return;
	}
	memcpy(link->to_helper + link->to_helper_len, bytes, len);
	link->to_helper_len += len;
}

/*
 * The host's link for the stream `epoch` names. A host that begins a stream
 * again -- reconnecting after a drop -- names a new one, and its old link
 * ends: the helper it attached is an attachment, and the session it showed
 * goes on in the session's own process, for the new attach to find.
 */
static link_t *link_for(const uint8_t host[FZN_PUBKEY_LEN], uint64_t epoch) {
	link_t *free_one = NULL;
	uint64_t retired = 0;
	int has_retired = 0;
	for (size_t i = 0; i < LINKS_MAX; ++i) {
		link_t *link = &node.links[i];
		if (link->used && !memcmp(link->host, host, FZN_PUBKEY_LEN)) {
			if (link->stream.epoch == epoch) return link;
			if (link->has_retired && link->retired == epoch) return NULL;
			retired = link->stream.epoch;
			has_retired = 1;
			end_link(link);
		}
		if (!link->used && !free_one) free_one = link;
	}
	if (!free_one) return NULL;
	memset(free_one, 0, sizeof(*free_one));
	free_one->used = 1;
	free_one->io = -1;
	memcpy(free_one->host, host, FZN_PUBKEY_LEN);
	free_one->retired = retired;
	free_one->has_retired = has_retired;
	bssh_stream_init(&free_one->stream, free_one->ring, RING, SEGMENT_DATA, RING, epoch);
	return free_one;
}

static void said(void *ctx, const uint8_t host[FZN_PUBKEY_LEN], const uint8_t *bytes, size_t len) {
	(void)ctx;
	/* An empty word is a device saying where it is, and carries no segment. */
	if (!node.sessions || len == 0) return;
	uint64_t epoch;
	if (bssh_stream_segment_epoch(bytes, len, &epoch) != 0) return;
	link_t *link = link_for(host, epoch);
	if (!link) return;
	const uint64_t now = now_ms();
	if (bssh_stream_receive(&link->stream, bytes, len, now, delivered, link) == 0)
		link->heard_ms = now;
}

static size_t fds(void *ctx, int *out, size_t cap) {
	(void)ctx;
	size_t n = 0;
	for (size_t i = 0; i < LINKS_MAX && n < cap; ++i) {
		link_t *link = &node.links[i];
		/* Read only while the stream has room: the helper waits otherwise. */
		if (link->used && !link->dead && link->io >= 0 && !link->output_ended
		    && bssh_stream_pending(&link->stream) < RING)
			out[n++] = link->io;
	}
	return n;
}

static void ready(void *ctx, int fd) {
	(void)ctx;
	for (size_t i = 0; i < LINKS_MAX; ++i) {
		link_t *link = &node.links[i];
		if (!link->used || link->io != fd) continue;
		uint8_t buffer[4096];
		size_t room = RING - bssh_stream_pending(&link->stream);
		if (room > sizeof(buffer)) room = sizeof(buffer);
		if (!room) return;
		const ssize_t got = read(fd, buffer, room);
		if (got > 0) bssh_stream_write(&link->stream, buffer, (size_t)got);
		else if (got == 0 || (errno != EAGAIN && errno != EINTR)) link->output_ended = 1;
		return;
	}
}

static void tick(void *ctx, uint64_t wall) {
	(void)ctx;
	(void)wall;
	const uint64_t now = now_ms();
	for (size_t i = 0; i < LINKS_MAX; ++i) {
		link_t *link = &node.links[i];
		if (!link->used) continue;
		if (link->dead) {
			end_link(link);
			continue;
		}

		while (link->io >= 0 && link->to_helper_len) {
			const ssize_t put = send(link->io, link->to_helper, link->to_helper_len,
			                         BSSH_MSG_NOSIGNAL);
			if (put <= 0) break;
			memmove(link->to_helper, link->to_helper + put, link->to_helper_len - (size_t)put);
			link->to_helper_len -= (size_t)put;
		}

		uint8_t segment[SEGMENT_DATA + BSSH_STREAM_HEADER_MAX];
		size_t n;
		while ((n = bssh_stream_poll(&link->stream, now, segment, sizeof(segment))) > 0)
			fzn_daemon_say(link->host, segment, n);

		if (link->pid > 0 && waitpid(link->pid, NULL, WNOHANG) == link->pid) link->pid = 0;
		/* Its helper gone and all of what it wrote taken, or a host long silent. */
		const int finished = link->started && link->pid == 0 && link->output_ended
		                     && bssh_stream_pending(&link->stream) == 0;
		if (finished || now - link->heard_ms > IDLE_MS) end_link(link);
	}
}

/* The helper beside this program, as `beersshd --sessions` finds it. */
static const char *helper_beside(void) {
	char self[PATH_MAX];
	const ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (len <= 0) return "can't find where this program is, to find beerssh-session beside it";
	self[len] = '\0';
	char *slash = strrchr(self, '/');
	if (!slash) return "can't find where this program is, to find beerssh-session beside it";
	*slash = '\0';
	/* Its desktop name, then its name in an Android app's native library directory. */
	static const char *const names[] = { "beerssh-session", "libbeerssh_session.so" };
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
		if (snprintf(node.helper, sizeof(node.helper), "%s/%s", self, names[i])
		    >= (int)sizeof(node.helper))
			return "this program's path is too long";
		if (access(node.helper, X_OK) == 0) return NULL;
	}
	return "--sessions needs beerssh-session installed beside beersshd";
}

static const char *option(void *ctx, const char *name, const char *value) {
	(void)ctx;
	(void)value;
	if (strcmp(name, "--sessions") != 0) return "unknown option";
	const char *why = helper_beside();
	if (why) return why;
	node.sessions = 1;
	return NULL;
}

static const fzn_daemon_option_t options[] = {
	{ "--sessions", 0,
	  "carry BeerSSH's sessions that survive a dropped connection to paired hosts that may "
	  "speak here (remote/permit/said), as beersshd --sessions does over SSH" },
};

static const fzn_daemon_ext_t extension = {
	.options = options,
	.n_options = sizeof(options) / sizeof(options[0]),
	.option = option,
	.fds = fds,
	.ready = ready,
	.tick = tick,
	.said = said,
};

/* `prefix` as one of `argv`'s words opens, or not. */
static int given(int argc, char **argv, const char *prefix) {
	for (int i = 1; i < argc; ++i) {
		if (!strncmp(argv[i], prefix, strlen(prefix))) return 1;
	}
	return 0;
}

int bssh_fuzznet_main(int argc, char **argv) {
	/* First, before anything is opened: it may execute this program again. */
	fzn_daemon_refuse_huge_pages(argv);
	for (size_t i = 0; i < LINKS_MAX; ++i) node.links[i].io = -1;

	/*
	 * beerssh's numbers, fuzznet's to assign (its chain/service.h): every
	 * host and device of one estate must agree on them, and a pairing
	 * checks the capability they name, so they are given here unless the
	 * command line names others -- fuzznet's own test pair, say.
	 */
	static char service[24];
	static char product[24];
	snprintf(service, sizeof(service), "--fuzznet-service=%u", (unsigned)FZN_SERVICE_NODE);
	snprintf(product, sizeof(product), "--fuzznet-product=%u", (unsigned)FZN_PRODUCT_BEERSSH);
	char *with[argc + 3];
	int n = 0;
	for (int i = 0; i < argc; ++i) with[n++] = argv[i];
	if (!given(argc, argv, "--fuzznet-service=")) with[n++] = service;
	if (!given(argc, argv, "--fuzznet-product=")) with[n++] = product;
	with[n] = NULL;
	return fzn_daemon_main(n, with, &extension);
}
