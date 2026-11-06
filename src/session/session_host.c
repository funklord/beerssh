#define _GNU_SOURCE

#include "session/session_host.h"

#include "session/wire.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHUNK 16384u

typedef struct host {
	bssh_output_log_t log;
	int pty;              /* -1 once the program has gone */
	int listener;
	int client;           /* -1 while detached */
	pid_t child;
	int status;

	int hello_due;        /* a new link has not had its first bytes */
	int attached;         /* the client has said where to start */
	uint64_t sent;        /* next offset to send */

	uint8_t in[BSSH_WIRE_FRAME_MAX];
	uint32_t in_used;

	uint8_t out[BSSH_WIRE_FRAME_MAX];
	size_t out_used;
	size_t out_done;

	/* Input the program has not taken yet. Frames wait behind it. */
	uint8_t to_program[BSSH_WIRE_BODY_MAX];
	size_t to_program_used;
	size_t to_program_done;

	int told_exit;
} host_t;

static int64_t now_ms(void) {
	struct timespec at;

	clock_gettime(CLOCK_MONOTONIC, &at);
	return (int64_t)at.tv_sec * 1000 + at.tv_nsec / 1000000;
}

static void set_nonblocking(int fd) {
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
}

static int listen_on(const char *path) {
	struct sockaddr_un address;

	if (strlen(path) >= sizeof(address.sun_path)) return -1;
	const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;

	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strcpy(address.sun_path, path);

	/* Owner only from the moment it exists: the umask covers the bind. */
	const mode_t mask = umask(0077);
	const int bound = bind(fd, (struct sockaddr *)&address, sizeof(address));
	umask(mask);

	if (bound != 0 || listen(fd, 2) != 0) {
		close(fd);
		return -1;
	}
	set_nonblocking(fd);
	return fd;
}

static void drop_client(host_t *host) {
	if (host->client >= 0) close(host->client);
	host->client = -1;
	host->hello_due = 0;
	host->attached = 0;
	host->in_used = 0;
	host->out_used = 0;
	host->out_done = 0;
}

static void accept_client(host_t *host) {
	const int fd = accept4(host->listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
	if (fd < 0) return;

	/* The socket's mode says the same; this holds if the mode ever does not. */
	struct ucred peer;
	socklen_t len = sizeof(peer);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &len) != 0 || peer.uid != geteuid()) {
		close(fd);
		return;
	}

	drop_client(host);
	host->client = fd;
	host->hello_due = 1;
}

static void set_size(host_t *host, uint16_t columns, uint16_t rows) {
	if (host->pty < 0 || !columns || !rows) return;

	struct winsize size;
	memset(&size, 0, sizeof(size));
	size.ws_col = columns;
	size.ws_row = rows;
	ioctl(host->pty, TIOCSWINSZ, &size);
}

static void flush_to_program(host_t *host) {
	while (host->to_program_done < host->to_program_used) {
		if (host->pty < 0) {
			host->to_program_done = host->to_program_used;
			break;
		}
		const ssize_t wrote = write(host->pty, host->to_program + host->to_program_done,
		                            host->to_program_used - host->to_program_done);
		if (wrote <= 0) return;
		host->to_program_done += (size_t)wrote;
	}
	host->to_program_used = 0;
	host->to_program_done = 0;
}

static int handle_frame(host_t *host, const bssh_wire_frame_t *frame) {
	uint64_t offset = 0;
	uint16_t columns = 0;
	uint16_t rows = 0;
	uint8_t mode = 0;

	switch (frame->kind) {
	case BSSH_WIRE_ATTACH:
		if (bssh_wire_read_attach(frame, &offset, &columns, &rows)) return -1;
		host->attached = 1;
		host->sent = offset < host->log.end ? offset : host->log.end;
		bssh_output_log_ack(&host->log, host->sent);
		set_size(host, columns, rows);
		return 0;
	case BSSH_WIRE_INPUT:
		memcpy(host->to_program, frame->body, frame->body_len);
		host->to_program_used = frame->body_len;
		host->to_program_done = 0;
		flush_to_program(host);
		return 0;
	case BSSH_WIRE_ACK:
		if (bssh_wire_read_offset(frame, &offset)) return -1;
		bssh_output_log_ack(&host->log, offset);
		return 0;
	case BSSH_WIRE_RESIZE:
		if (bssh_wire_read_resize(frame, &columns, &rows)) return -1;
		set_size(host, columns, rows);
		return 0;
	case BSSH_WIRE_MODE:
		if (bssh_wire_read_mode(frame, &mode)) return -1;
		bssh_output_log_set_faithful(&host->log, mode != BSSH_WIRE_MODE_LIVE);
		return 0;
	default:
		/* A newer client's. The length is why this can be skipped. */
		return 0;
	}
}

static void read_client(host_t *host) {
	/* Full, behind input the program has not taken: 0 here would read as EOF. */
	if (host->in_used == sizeof(host->in)) return;

	const ssize_t got = read(host->client, host->in + host->in_used, sizeof(host->in) - host->in_used);

	if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR)) {
		drop_client(host);
		return;
	}
	if (got > 0) host->in_used += (uint32_t)got;
}

static void take_frames(host_t *host) {
	while (host->client >= 0 && !host->to_program_used) {
		bssh_wire_frame_t frame;
		uint32_t used = 0;
		const int taken = bssh_wire_take(host->in, host->in_used, &frame, &used);

		if (taken == 0) return;
		if (taken < 0 || handle_frame(host, &frame) != 0) {
			drop_client(host);
			return;
		}
		memmove(host->in, host->in + used, host->in_used - used);
		host->in_used -= used;
	}
}

/*
 * The peer has gone. What it sent first is still in the socket, and input that
 * reached this far is applied: somebody typed it. Reading ends with EOF, or
 * with a read that brought nothing.
 */
static void drain_and_drop(host_t *host) {
	while (host->client >= 0) {
		const uint32_t before = host->in_used;

		read_client(host);
		if (host->client >= 0 && host->in_used == before) break;
		take_frames(host);
	}
	drop_client(host);
}

static void fill_out(host_t *host) {
	if (host->out_used) return;
	if (host->hello_due) {
		host->out_used = bssh_wire_put_hello(host->out, sizeof(host->out));
		host->hello_due = 0;
		return;
	}
	if (!host->attached) return;

	uint8_t chunk[CHUNK];
	size_t len = 0;
	const bssh_output_log_answer_t answer =
	        bssh_output_log_read(&host->log, host->sent, chunk, sizeof(chunk), &len);

	if (answer == BSSH_OUTPUT_LOG_GONE) {
		host->out_used = bssh_wire_put_offset(host->out, sizeof(host->out), BSSH_WIRE_LOST,
		                                      host->log.start);
		host->sent = host->log.start;
	} else if (answer == BSSH_OUTPUT_LOG_OK) {
		host->out_used = bssh_wire_put_output(host->out, sizeof(host->out), host->sent, chunk, len);
		host->sent += len;
	} else if (host->pty < 0 && !host->told_exit) {
		host->out_used = bssh_wire_put_exited(host->out, sizeof(host->out), host->status);
		host->told_exit = 1;
	}
}

static void write_client(host_t *host) {
	while (host->client >= 0) {
		fill_out(host);
		if (host->out_done == host->out_used) return;

		const ssize_t wrote = send(host->client, host->out + host->out_done,
		                           host->out_used - host->out_done, MSG_NOSIGNAL);
		if (wrote < 0) {
			if (errno == EAGAIN || errno == EINTR) return;
			drain_and_drop(host);
			return;
		}
		host->out_done += (size_t)wrote;
		if (host->out_done == host->out_used) {
			host->out_used = 0;
			host->out_done = 0;
		}
	}
}

static size_t log_room(const host_t *host) {
	if (!host->log.faithful) return CHUNK;
	const size_t room = host->log.capacity - (size_t)(host->log.end - host->log.acked);
	return room < CHUNK ? room : CHUNK;
}

static void program_gone(host_t *host) {
	close(host->pty);
	host->pty = -1;
	waitpid(host->child, &host->status, 0);
}

static void read_program(host_t *host) {
	uint8_t chunk[CHUNK];
	const size_t room = log_room(host);

	if (!room) return;
	const ssize_t got = read(host->pty, chunk, room);

	/* EIO is how a pty master says the last slave closed. */
	if (got == 0 || (got < 0 && errno != EAGAIN && errno != EINTR)) {
		program_gone(host);
		return;
	}
	if (got > 0) bssh_output_log_append(&host->log, chunk, (size_t)got, (uint64_t)now_ms());
}

int bssh_session_host_run(const bssh_session_host_options_t *options) {
	/* Static would do for one host a process; the heap keeps the stack small. */
	host_t *host = calloc(1, sizeof(*host));
	if (!host) return -1;

	host->client = -1;
	if (bssh_output_log_init(&host->log, options->log_bytes, options->log_capacity,
	                         options->log_marks, options->log_marks_capacity) != 0) {
		free(host);
		return -1;
	}

	host->listener = listen_on(options->socket_path);
	if (host->listener < 0) {
		free(host);
		return -1;
	}

	host->child = forkpty(&host->pty, NULL, NULL, NULL);
	if (host->child < 0) {
		close(host->listener);
		unlink(options->socket_path);
		free(host);
		return -1;
	}
	if (host->child == 0) {
		setenv("TERM", "xterm-256color", 1);
		execvp(options->argv[0], options->argv);
		_exit(127);
	}
	set_nonblocking(host->pty);

	int64_t linger_until = 0;
	for (;;) {
		if (host->pty < 0 && !linger_until) linger_until = now_ms() + options->linger_ms;
		if (host->told_exit && !host->out_used) break;
		if (linger_until && now_ms() >= linger_until) break;

		struct pollfd fds[3];
		nfds_t count = 0;

		fds[count].fd = host->listener;
		fds[count++].events = POLLIN;

		int client_at = -1;
		if (host->client >= 0) {
			client_at = (int)count;
			fds[count].fd = host->client;
			fds[count].events = 0;
			if (host->in_used < sizeof(host->in)) fds[count].events |= POLLIN;
			if (host->out_used || host->hello_due || (host->attached && host->sent < host->log.end)
			    || (host->attached && host->pty < 0))
				fds[count].events |= POLLOUT;
			++count;
		}

		int pty_at = -1;
		if (host->pty >= 0) {
			pty_at = (int)count;
			fds[count].fd = host->pty;
			fds[count].events = 0;
			if (log_room(host)) fds[count].events |= POLLIN;
			if (host->to_program_used) fds[count].events |= POLLOUT;

			/*
			 * Paused: a faithful log with no room. Left in the set, a program
			 * that has gone would wake this loop for ever with POLLHUP nobody
			 * may act on until the client makes room.
			 */
			if (!fds[count].events) fds[count].fd = -1;
			++count;
		}

		const int timeout = linger_until ? (int)(linger_until - now_ms()) : -1;
		if (poll(fds, count, timeout < 0 && linger_until ? 0 : timeout) < 0 && errno != EINTR) break;

		if (fds[0].revents & POLLIN) accept_client(host);

		/* accept_client may have swapped the client this index described. */
		if (client_at >= 0 && fds[client_at].fd == host->client
		    && (fds[client_at].revents & (POLLIN | POLLHUP | POLLERR)))
			read_client(host);

		if (pty_at >= 0 && host->pty >= 0) {
			if (fds[pty_at].revents & POLLOUT) flush_to_program(host);
			if (fds[pty_at].revents & (POLLIN | POLLHUP | POLLERR)) read_program(host);
		}

		take_frames(host);
		write_client(host);
	}

	drop_client(host);
	close(host->listener);
	unlink(options->socket_path);

	if (host->pty >= 0) program_gone(host);
	const int status = host->status;
	free(host);
	return status;
}
