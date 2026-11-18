#ifndef BSSH_HELPER_CHANNEL_H
#define BSSH_HELPER_CHANNEL_H

#include <QByteArray>
#include <QList>

#include <sys/types.h>

/*
 * The session helper beersshd carries (project.md sec 5.6, holder
 * 2026-11-18): the two commands a client sends it, and the process that
 * answers them.
 */

/*
 * The helper's arguments for an exec request, or empty for anything else.
 * Only "beerssh-session version" and "beerssh-session attach NAME" are
 * taken, word for word as the client joins them, so an exec request can
 * never name another program or reach a shell's parser: the words go to
 * execv, not to sh -c.
 */
QList<QByteArray> bssh_helper_arguments(const QByteArray &command);

struct bssh_helper_process_t {
	pid_t pid = -1;
	/* The helper's stdin and stdout, one socket; shut for writing at EOF. */
	int io = -1;
	/* Its stderr, read only. */
	int errors = -1;
};

/*
 * Starts `program` with `arguments` (the first is argv[0]) and every other
 * descriptor of this process closed, so neither a listening socket nor a
 * client's connection is held by a session that outlives beersshd. False,
 * with nothing left open, when it could not start.
 */
bool bssh_helper_start(const QByteArray &program, const QList<QByteArray> &arguments,
                       bssh_helper_process_t *out);

/*
 * The exit status once it has ended, reaped, or -1 while it runs. Ended by
 * a signal is 128 plus the signal, as a shell reports it.
 */
int bssh_helper_reap(bssh_helper_process_t *process, bool wait);

/* Closes both descriptors; the process, if still running, is left to end. */
void bssh_helper_close(bssh_helper_process_t *process);

#endif
