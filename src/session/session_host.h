#ifndef BSSH_SESSION_HOST_H
#define BSSH_SESSION_HOST_H

/*
 * The server side of a session that outlives its connection (project.md sec
 * 5.6): owns the pty, keeps reading it whether or not anyone is attached, and
 * lets a client come back and continue from any offset still held. This is
 * what makes a dropped connection cost nothing: the program never learns it
 * happened.
 *
 * One client at a time, on a Unix socket only this user may reach. A second
 * attach replaces the first, because the usual reason for one is a client
 * that has reconnected before its old socket was noticed dead. Whatever
 * carries the bytes further (an SSH exec channel, fuzznet) joins them to this
 * socket, so nothing here knows the transport.
 *
 * WHAT MAKES IT STOP. The program ending, with its output delivered to a
 * client or `linger_ms` passing with nobody there to take it; or a signal.
 * While the program runs it stays, attached or not, which is the point.
 */

#include "session/output_log.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bssh_session_host_options {
	const char *socket_path;
	char *const *argv;         /* what to run; argv[0] is looked up in PATH */
	uint8_t *log_bytes;        /* the raw log's memory, as bssh_output_log_init */
	size_t log_capacity;
	bssh_output_log_mark_t *log_marks;
	size_t log_marks_capacity;
	int linger_ms;
} bssh_session_host_options_t;

/* Runs until one of the above. Returns the program's wait status, or -1 when
 * it could not start. */
int bssh_session_host_run(const bssh_session_host_options_t *options);

#ifdef __cplusplus
}
#endif

#endif
