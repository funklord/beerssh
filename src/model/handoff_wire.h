#ifndef BSSH_HANDOFF_WIRE_H
#define BSSH_HANDOFF_WIRE_H

#include <QByteArray>
#include <QList>
#include <QString>

/*
 * The wire a live session crosses between two BeerSSH processes (project.md
 * sec 8.4): one message at a time over a unix stream socket, a length and
 * the bytes, with any descriptors riding on it by SCM_RIGHTS -- the SSH
 * socket, a pty master, a jump tunnel's ends.
 *
 * Plain POSIX rather than QLocalSocket, which cannot carry a descriptor.
 * Blocking, with every wait bounded by a deadline, because a handoff is a
 * short exchange between two processes of the same user and the caller
 * has stopped its session for the length of it.
 *
 * Desktop only: on Android an app is one process, and an update or a
 * process death kills every process of the package, so nothing would be
 * left to hand a session to.
 */

/* A message: its bytes, and the descriptors that came with it. */
struct bssh_handoff_message_t {
	QByteArray bytes;
	QList<int> fds;
};

namespace bssh_handoff_wire {

/*
 * Sends one message. The descriptors are duplicated into the receiver by
 * the kernel; the caller still owns its own and closes them as it likes.
 * False on any failure, with why saying what.
 */
bool send(int socket, const QByteArray &bytes, const QList<int> &fds, int timeout_ms,
          QString *why = nullptr);

/*
 * Receives one message, waiting at most timeout_ms in all. Descriptors
 * arrive close-on-exec and are the caller's to close. A message larger
 * than BSSH_HANDOFF_MAX_BYTES is refused rather than allocated.
 */
bool receive(int socket, bssh_handoff_message_t &message, int timeout_ms,
             QString *why = nullptr);

/*
 * Whether the process at the other end of a connected unix socket is this
 * user's. A session's keys travel in these messages, so a peer of any
 * other uid is refused before anything is sent.
 */
bool peer_is_same_user(int socket);

}  // namespace bssh_handoff_wire

/*
 * Large enough for a session with a long scrollback full of pictures; a
 * damaged length must not become an allocation.
 */
const qint64 BSSH_HANDOFF_MAX_BYTES = qint64(512) * 1024 * 1024;

/* At most this many descriptors in one message. */
const int BSSH_HANDOFF_MAX_FDS = 8;

#endif /* BSSH_HANDOFF_WIRE_H */
