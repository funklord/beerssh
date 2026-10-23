#include "model/handoff_wire.h"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <QElapsedTimer>

namespace {

void set_why(QString *why, const QString &text) {
	if (why) *why = text;
}

/* Waits for the socket to be ready for `events`, within what is left of
 * the deadline. */
bool wait_for(int socket, short events, const QElapsedTimer &clock, int timeout_ms) {
	for (;;) {
		const qint64 left = timeout_ms - clock.elapsed();
		if (left <= 0) return false;
		pollfd entry = { socket, events, 0 };
		const int ready = ::poll(&entry, 1, int(left));
		if (ready > 0) return (entry.revents & (events | POLLHUP | POLLERR)) != 0;
		if (ready < 0 && errno != EINTR) return false;
		if (ready == 0) return false;
	}
}

bool write_all(int socket, const char *data, qint64 size, const QElapsedTimer &clock,
               int timeout_ms) {
	while (size > 0) {
		if (!wait_for(socket, POLLOUT, clock, timeout_ms)) return false;
		const ssize_t wrote = ::send(socket, data, size_t(size), MSG_NOSIGNAL | MSG_DONTWAIT);
		if (wrote < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
			return false;
		}
		data += wrote;
		size -= wrote;
	}
	return true;
}

bool read_all(int socket, char *data, qint64 size, const QElapsedTimer &clock, int timeout_ms) {
	while (size > 0) {
		if (!wait_for(socket, POLLIN, clock, timeout_ms)) return false;
		const ssize_t got = ::recv(socket, data, size_t(size), MSG_DONTWAIT);
		if (got == 0) return false;
		if (got < 0) {
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
			return false;
		}
		data += got;
		size -= got;
	}
	return true;
}

}  // namespace

bool bssh_handoff_wire::send(int socket, const QByteArray &bytes, const QList<int> &fds,
                             int timeout_ms, QString *why) {
	if (fds.size() > BSSH_HANDOFF_MAX_FDS || bytes.size() > BSSH_HANDOFF_MAX_BYTES) {
		set_why(why, QStringLiteral("the message is larger than a handoff may carry"));
		return false;
	}
	QElapsedTimer clock;
	clock.start();

	/*
	 * The length, eight bytes, carries the descriptors: SCM_RIGHTS needs at
	 * least one byte of data to ride on, and the receiver reads the length
	 * first, so the two arrive together however the stream is split.
	 */
	quint64 length = quint64(bytes.size());
	iovec iov = { &length, sizeof(length) };
	msghdr message = {};
	message.msg_iov = &iov;
	message.msg_iovlen = 1;

	char control[CMSG_SPACE(sizeof(int) * BSSH_HANDOFF_MAX_FDS)];
	if (!fds.isEmpty()) {
		std::memset(control, 0, sizeof(control));
		message.msg_control = control;
		message.msg_controllen = CMSG_SPACE(sizeof(int) * size_t(fds.size()));
		cmsghdr *header = CMSG_FIRSTHDR(&message);
		header->cmsg_level = SOL_SOCKET;
		header->cmsg_type = SCM_RIGHTS;
		header->cmsg_len = CMSG_LEN(sizeof(int) * size_t(fds.size()));
		std::memcpy(CMSG_DATA(header), fds.constData(), sizeof(int) * size_t(fds.size()));
	}

	for (;;) {
		if (!wait_for(socket, POLLOUT, clock, timeout_ms)) {
			set_why(why, QStringLiteral("the other process did not take the message"));
			return false;
		}
		const ssize_t wrote = ::sendmsg(socket, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
		if (wrote == ssize_t(sizeof(length))) break;
		if (wrote < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
		set_why(why, QStringLiteral("sending to the other process failed: %1")
		                     .arg(QString::fromLocal8Bit(std::strerror(errno))));
		return false;
	}

	if (!write_all(socket, bytes.constData(), bytes.size(), clock, timeout_ms)) {
		set_why(why, QStringLiteral("the other process stopped reading"));
		return false;
	}
	return true;
}

bool bssh_handoff_wire::receive(int socket, bssh_handoff_message_t &out, int timeout_ms,
                                QString *why) {
	out.bytes.clear();
	out.fds.clear();
	QElapsedTimer clock;
	clock.start();

	quint64 length = 0;
	iovec iov = { &length, sizeof(length) };
	char control[CMSG_SPACE(sizeof(int) * BSSH_HANDOFF_MAX_FDS)];
	msghdr message = {};
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);

	ssize_t got = -1;
	for (;;) {
		if (!wait_for(socket, POLLIN, clock, timeout_ms)) {
			set_why(why, QStringLiteral("nothing arrived from the other process"));
			return false;
		}
		got = ::recvmsg(socket, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT);
		if (got >= 0) break;
		if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
		set_why(why, QStringLiteral("reading from the other process failed"));
		return false;
	}

	/* The descriptors first, so that whatever happens next they are the
	 * caller's to close rather than leaked. */
	for (cmsghdr *header = CMSG_FIRSTHDR(&message); header;
	     header = CMSG_NXTHDR(&message, header)) {
		if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS) continue;
		const size_t count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		for (size_t i = 0; i < count; ++i) {
			int fd = -1;
			std::memcpy(&fd, CMSG_DATA(header) + i * sizeof(int), sizeof(int));
			out.fds.append(fd);
		}
	}
	const bool truncated = (message.msg_flags & MSG_CTRUNC) != 0;

	auto fail = [&out, why](const QString &text) {
		for (int fd : out.fds) ::close(fd);
		out.fds.clear();
		set_why(why, text);
		return false;
	};
	if (truncated) return fail(QStringLiteral("the other process sent too many descriptors"));
	if (got == 0) return fail(QStringLiteral("the other process closed the connection"));

	/* The rest of the length, if the stream split it. */
	if (got < ssize_t(sizeof(length)) &&
	    !read_all(socket, reinterpret_cast<char *>(&length) + got, qint64(sizeof(length)) - got,
	              clock, timeout_ms))
		return fail(QStringLiteral("the other process stopped part-way through a message"));
	if (length > quint64(BSSH_HANDOFF_MAX_BYTES))
		return fail(QStringLiteral("the other process sent a message too large to take"));

	out.bytes.resize(qsizetype(length));
	if (!read_all(socket, out.bytes.data(), qint64(length), clock, timeout_ms))
		return fail(QStringLiteral("the other process stopped part-way through a message"));
	return true;
}

bool bssh_handoff_wire::peer_is_same_user(int socket) {
	ucred peer = {};
	socklen_t size = sizeof(peer);
	if (::getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0) return false;
	return peer.uid == ::getuid();
}
