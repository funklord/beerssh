#include "ssh/jump_tunnel.h"

#include <QDataStream>
#include <QIODevice>

#include "ssh/ssh_session_libssh.h"

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

bssh_jump_tunnel::bssh_jump_tunnel(QObject *parent) : QThread(parent) {
}

bssh_jump_tunnel::~bssh_jump_tunnel() {
	close();
}

bssh_jump_tunnel::hop_t bssh_jump_tunnel::parse(const QString &spec,
                                                 const QString &fallback_user) {
	hop_t hop;

	QString rest = spec.trimmed();
	if (rest.isEmpty()) return hop;

	/*
	 * A chain is refused rather than half-honoured. Taking only the first
	 * hop of "a,b" would connect through a machine the user named as a
	 * STEP and treat it as the destination's gateway, which is a
	 * different route than the one they wrote.
	 */
	if (rest.contains(QLatin1Char(','))) return hop;

	const int at = rest.lastIndexOf(QLatin1Char('@'));
	if (at >= 0) {
		hop.user = rest.left(at);
		rest = rest.mid(at + 1);
	} else {
		hop.user = fallback_user;
	}

	/*
	 * Where the port is, and where it only looks like one.
	 *
	 * An IPv6 literal is full of colons, so splitting on the last one is
	 * wrong twice over: "2001:db8::1" would become the host "2001:db8:"
	 * on port 1, and "[2001:db8::1]:2222" would keep its port inside the
	 * hostname. Both dial somewhere that is not a machine, and neither
	 * says the spec was not understood.
	 *
	 * So the bracketed form is taken apart at the bracket, and an
	 * unbracketed address with more than one colon is a bare IPv6
	 * literal, which cannot carry a port at all.
	 */
	if (rest.startsWith(QLatin1Char('['))) {
		const int end = rest.indexOf(QLatin1Char(']'));
		if (end < 0) return hop;

		const QString after = rest.mid(end + 1);
		if (after.startsWith(QLatin1Char(':'))) {
			bool ok = false;
			const uint value = after.mid(1).toUInt(&ok);
			if (!ok || value == 0 || value > 65535) return hop;
			hop.port = quint16(value);
		} else if (!after.isEmpty()) {
			return hop;
		}
		rest = rest.left(end + 1);
	} else if (rest.count(QLatin1Char(':')) <= 1) {
		const int colon = rest.lastIndexOf(QLatin1Char(':'));
		if (colon >= 0) {
			bool ok = false;
			const uint value = rest.mid(colon + 1).toUInt(&ok);
			if (ok && value > 0 && value <= 65535) {
				hop.port = quint16(value);
				rest = rest.left(colon);
			}
		}
	}

	hop.host = rest;
	hop.valid = !hop.host.isEmpty();
	return hop;
}

bool bssh_jump_tunnel::authenticate() {
	/*
	 * Keys and the agent only, deliberately.
	 *
	 * A password prompt for the bastion is not a missing line of code, it
	 * is a second credential flow: two hosts asking for two secrets in
	 * one connection attempt, each able to fail on its own, with a
	 * reconnect afterwards that can supply neither (sec 11 keeps no
	 * password past first use). Saying so plainly beats a prompt that
	 * cannot work on reconnect.
	 */
	const int rc = ssh_userauth_publickey_auto(m_session, nullptr, nullptr);
	if (rc == SSH_AUTH_SUCCESS) return true;

	m_why = QStringLiteral("the jump host would not accept a key or the agent");
	return false;
}

int bssh_jump_tunnel::open(const hop_t &hop, const QString &target_host, quint16 target_port,
                            const QString &known_hosts, const QString &identity_file) {
	if (!hop.valid) {
		m_why = QStringLiteral("the jump host could not be understood "
		                        "(one hop only, as [user@]host[:port])");
		return -1;
	}

	m_session = ssh_new();
	if (!m_session) {
		m_why = QStringLiteral("ssh_new() failed for the jump host");
		return -1;
	}

	const QByteArray host_utf8 = hop.host.toUtf8();
	const QByteArray user_utf8 = hop.user.toUtf8();
	const QByteArray known_utf8 = known_hosts.toUtf8();
	int port_value = hop.port;

	/*
	 * The same ceiling the target session gets. This connect is blocking
	 * too -- ssh_set_blocking(0) comes later, after the channel is up --
	 * and it runs before the pump does, so an unreachable bastion held
	 * the thread here with nothing bounding it. bssh_jump_tunnel::close()
	 * waits five seconds for this thread and then deliberately abandons
	 * the session rather than kill it, so an unbounded connect turns that
	 * documented five-second cost into a leaked thread every time.
	 */
	long timeout_s = 10;
	ssh_options_set(m_session, SSH_OPTIONS_TIMEOUT, &timeout_s);

	ssh_options_set(m_session, SSH_OPTIONS_HOST, host_utf8.constData());
	ssh_options_set(m_session, SSH_OPTIONS_PORT, &port_value);
	if (!user_utf8.isEmpty()) ssh_options_set(m_session, SSH_OPTIONS_USER, user_utf8.constData());
	if (!known_utf8.isEmpty()) {
		ssh_options_set(m_session, SSH_OPTIONS_KNOWNHOSTS, known_utf8.constData());
	}
	ssh_options_set(m_session, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, "/etc/ssh/ssh_known_hosts");

	if (!identity_file.isEmpty()) {
		const QByteArray identity_utf8 = identity_file.toUtf8();
		ssh_options_set(m_session, SSH_OPTIONS_ADD_IDENTITY, identity_utf8.constData());
	}

	if (ssh_connect(m_session) != SSH_OK) {
		m_why = QStringLiteral("cannot reach the jump host %1: %2")
		                .arg(hop.host, QString::fromUtf8(ssh_get_error(m_session)));
		close();
		return -1;
	}

	/*
	 * The same socket options the target's connection gets (sec 5.3), on
	 * the socket that actually carries it. Without them a bastion on a
	 * link that stops passing packets is never noticed by the kernel, and
	 * nothing above can notice either: the session simply stops, looking
	 * connected.
	 */
	bssh_configure_tcp_keepalive(ssh_get_fd(m_session));

	/*
	 * The bastion is a host like any other and gets the same scrutiny --
	 * but it is checked, never prompted about. A trust-on-first-use
	 * dialog for a machine the user did not ask to visit, while the
	 * connection they DID ask for waits behind it, is a prompt nobody can
	 * answer sensibly. Telling them to ssh to it once is a better answer
	 * than teaching them to click through key warnings.
	 */
	const enum ssh_known_hosts_e known = ssh_session_is_known_server(m_session);
	if (known != SSH_KNOWN_HOSTS_OK) {
		m_why = known == SSH_KNOWN_HOSTS_CHANGED
		        ? QStringLiteral("the jump host %1 has a DIFFERENT key than the one recorded")
		                  .arg(hop.host)
		        : QStringLiteral("the jump host %1 is not a known host yet -- ssh to it once "
		                          "so its key is recorded, then try again").arg(hop.host);
		close();
		return -1;
	}

	if (!authenticate()) {
		close();
		return -1;
	}

	m_channel = ssh_channel_new(m_session);
	if (!m_channel) {
		m_why = QStringLiteral("no channel to the jump host");
		close();
		return -1;
	}

	/*
	 * direct-tcpip: the bastion opens a plain TCP connection to the
	 * target and splices it to this channel. It never sees the SSH
	 * session that then runs inside it.
	 */
	const QByteArray target_utf8 = target_host.toUtf8();
	if (ssh_channel_open_forward(m_channel, target_utf8.constData(), target_port,
	                              "127.0.0.1", 0) != SSH_OK) {
		m_why = QStringLiteral("the jump host would not open a route to %1:%2 (%3)")
		                .arg(target_host).arg(target_port)
		                .arg(QString::fromUtf8(ssh_get_error(m_session)));
		close();
		return -1;
	}

	int pair[2] = {-1, -1};
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
		m_why = QStringLiteral("cannot create the local socket pair");
		close();
		return -1;
	}
	m_ours = pair[0];
	m_theirs = pair[1];

	/*
	 * Non-blocking from here on, and only from here on: the connect, the
	 * authentication and the forward above all want to block, and each
	 * would otherwise have to grow its own retry loop.
	 *
	 * What it buys is that no libssh call in the pump can sit
	 * indefinitely. A blocked ssh_channel_write on a wedged link -- the
	 * link stops passing packets rather than refusing them -- used to
	 * mean close() timed out and killed the thread mid-call inside
	 * libssh, and then walked the same session's structures to free them.
	 */
	ssh_set_blocking(m_session, 0);

	m_stop = false;
	start();
	return m_theirs;
}

void bssh_jump_tunnel::run() {
	char buffer[16384];

	/*
	 * Both ends are polled, and that is the point: waiting on the bastion
	 * socket as well as the local one means this sleeps until there is
	 * something to do rather than waking up to ask. A short timeout is
	 * kept only as a safety net for data libssh has already buffered out
	 * of the socket, where the descriptor has nothing left to report.
	 *
	 * The old version polled the local end alone on a 20ms timer: fifty
	 * wakeups a second for as long as a session was open, which on a
	 * backgrounded phone holding a session under the foreground service
	 * (sec 5.1) is a battery cost with nothing to show for it.
	 */
	/*
	 * A pause is honoured here, between transfers, and nowhere else: a
	 * transfer stopped part-way would lose what was read and not yet
	 * written, which on a moving session is bytes of a packet.
	 */
	while (!m_stop && !m_pause) {
		struct pollfd fds[2];
		fds[0].fd = m_ours;
		fds[0].events = POLLIN;
		fds[0].revents = 0;
		fds[1].fd = ssh_get_fd(m_session);
		fds[1].events = POLLIN;
		fds[1].revents = 0;

		const int ready = poll(fds, 2, 200);
		if (m_stop) break;
		if (ready < 0 && errno != EINTR) break;

		if (ready > 0 && (fds[0].revents & POLLIN)) {
			const ssize_t n = ::read(m_ours, buffer, sizeof(buffer));
			if (n == 0) break;
			if (n < 0 && errno != EINTR && errno != EAGAIN) break;
			if (n > 0 && !write_to_channel(buffer, int(n))) break;
		}
		if (ready > 0 && (fds[0].revents & (POLLHUP | POLLERR))) break;

		if (!drain_channel(buffer, sizeof(buffer))) break;
		if (ssh_channel_is_eof(m_channel)) break;

		/*
		 * No socket-level death check here, and that is measured rather
		 * than assumed. One was written -- peek at the bastion socket for
		 * end-of-stream, since libssh's non-blocking read returns a
		 * placid 0 for a connection that has been gone for minutes -- and
		 * then removed: with the socket options above in place, BOTH a
		 * killed bastion and a black-holed one are noticed without it.
		 * Code no test can justify is code that gets believed later
		 * without cause.
		 */
	}

	/*
	 * However this ended, the local end goes down.
	 *
	 * Three of the ways out are the BASTION dying -- a write error, a
	 * read error, the channel reaching EOF -- and leaving the socketpair
	 * open after them left the target session holding a descriptor that
	 * was open and permanently silent: never readable, never an error, so
	 * never a dropped connection. The UI said "connected", the terminal
	 * was frozen, and the reconnect machine (sec 5.3) was never told
	 * anything had happened. Shutting down here turns every one of those
	 * into the ordinary drop it already handles.
	 */
	/* Paused, the pair is left exactly as it is: it is about to move. */
	if (m_ours >= 0 && !m_pause) ::shutdown(m_ours, SHUT_RDWR);
}

bool bssh_jump_tunnel::write_to_channel(const char *data, int size) {
	int written = 0;

	while (written < size && !m_stop) {
		const int n = ssh_channel_write(m_channel, data + written, size - written);
		if (n == SSH_ERROR) return false;
		if (n == SSH_AGAIN || n == 0) {
			/*
			 * The channel's window is full: the far end has not read
			 * what it already has. Waiting on the bastion socket rather
			 * than spinning, since the window opens when a packet
			 * arrives from it.
			 */
			struct pollfd fd;
			fd.fd = ssh_get_fd(m_session);
			fd.events = POLLIN;
			fd.revents = 0;
			if (poll(&fd, 1, 200) < 0 && errno != EINTR) return false;
			continue;
		}
		written += n;
	}

	return !m_stop;
}

bool bssh_jump_tunnel::drain_channel(char *buffer, int size) {
	while (!m_stop) {
		const int n = ssh_channel_read_nonblocking(m_channel, buffer, size, 0);
		if (n == SSH_ERROR) return false;
		if (n <= 0) return true;

		int written = 0;
		while (written < n) {
			const ssize_t w = ::write(m_ours, buffer + written, size_t(n - written));
			if (w < 0 && (errno == EINTR || errno == EAGAIN)) continue;
			if (w <= 0) return false;
			written += int(w);
		}
	}

	return true;
}

void bssh_jump_tunnel::close() {
	m_stop = true;

	/*
	 * Our end first: the pump is waiting on it, and shutting it down is
	 * what wakes the thread.
	 */
	if (m_ours >= 0) ::shutdown(m_ours, SHUT_RDWR);

	if (isRunning() && !wait(5000)) {
		/*
		 * It should not get here: every libssh call the pump makes is
		 * non-blocking and its longest wait is a 200ms poll. If it
		 * somehow does, the thread keeps the session and the channel and
		 * this walks away from both.
		 *
		 * Killing it instead -- which is what this did -- cancels it at
		 * whatever point it has reached inside libssh and then frees the
		 * structures it was halfway through mutating. Leaking a thread
		 * and a session is a bounded cost; freeing memory another thread
		 * is standing in is a crash that will be blamed on something
		 * else entirely.
		 */
		m_channel = nullptr;
		m_session = nullptr;
		m_ours = -1;
		m_theirs = -1;
		return;
	}

	if (m_channel) {
		ssh_channel_close(m_channel);
		ssh_channel_free(m_channel);
		m_channel = nullptr;
	}
	if (m_session) {
		ssh_disconnect(m_session);
		ssh_free(m_session);
		m_session = nullptr;
	}

	if (m_ours >= 0) {
		::close(m_ours);
		m_ours = -1;
	}

	/*
	 * And the other end too, AFTER the target session that was using it
	 * has been freed -- which teardown() guarantees by freeing it before
	 * it gets here.
	 *
	 * The comment that used to sit here said libssh owned this
	 * descriptor and closed it with ssh_free. It does not: measured
	 * against libssh 0.11.2, a socket handed over with SSH_OPTIONS_FD is
	 * still open after a full handshake, ssh_disconnect and ssh_free.
	 * Believing otherwise leaked one descriptor per connection attempt,
	 * and a host behind a bastion on a flaky link retries for as long as
	 * it takes -- until socketpair() itself starts failing.
	 */
	if (m_theirs >= 0) {
		::close(m_theirs);
		m_theirs = -1;
	}
}

/* ---- moving the tunnel with its session (sec 8.4) --------------------- */

namespace {

const char BSSH_JUMP_HANDOFF_MAGIC[] = "bssh-jump-tunnel";
const quint32 BSSH_JUMP_HANDOFF_FORMAT = 1;
const int BSSH_JUMP_EXPORT_TRIES = 100;
const int BSSH_JUMP_EXPORT_POLL_MS = 20;

}  // namespace

bool bssh_jump_tunnel::pause() {
	if (!m_session || !m_channel) return false;
	m_pause = true;
	/* The pump's poll waits at most 200ms, and a transfer in progress
	 * finishes first: bounded, and generously. */
	if (isRunning() && !wait(10000)) {
		m_pause = false;
		return false;
	}
	return true;
}

void bssh_jump_tunnel::resume() {
	if (!m_session || !m_channel || isRunning()) return;
	m_pause = false;
	m_stop = false;
	start();
}

bool bssh_jump_tunnel::export_state(QByteArray &state, QList<int> &fds, QString &why) {
	if (!m_session || !m_channel || isRunning()) {
		why = QStringLiteral("the jump host's tunnel is not paused");
		return false;
	}
	ssh_string exported = nullptr;
	int rc = SSH_AGAIN;
	for (int tries = 0; tries < BSSH_JUMP_EXPORT_TRIES; ++tries) {
		rc = ssh_session_export_transport(m_session, &exported);
		if (rc != SSH_AGAIN) break;
		/* Lets libssh read what completes a packet or a rekey, into the
		 * channel's buffer, which travels. */
		ssh_channel_poll_timeout(m_channel, BSSH_JUMP_EXPORT_POLL_MS, 0);
	}
	if (rc != SSH_OK) {
		why = QStringLiteral("the jump host's connection could not be stopped: %1")
		              .arg(QString::fromUtf8(ssh_get_error(m_session)));
		return false;
	}
	state.clear();
	QDataStream out(&state, QIODevice::WriteOnly);
	out.setVersion(QDataStream::Qt_6_0);
	out << QByteArray(BSSH_JUMP_HANDOFF_MAGIC) << BSSH_JUMP_HANDOFF_FORMAT
	    << QByteArray(static_cast<const char *>(ssh_string_data(exported)),
	                  int(ssh_string_len(exported)));
	ssh_string_burn(exported);
	ssh_string_free(exported);
	fds = { ssh_get_fd(m_session), m_ours };
	return true;
}

void bssh_jump_tunnel::release() {
	m_stop = true;
	if (isRunning()) wait(10000);
	if (m_session) ssh_session_detach_transport(m_session);
	if (m_channel) {
		ssh_channel_free(m_channel);
		m_channel = nullptr;
	}
	if (m_session) {
		ssh_free(m_session);
		m_session = nullptr;
	}
	if (m_ours >= 0) ::close(m_ours);
	m_ours = -1;
	/* The target session's end, and its to close. */
	m_theirs = -1;
}

bool bssh_jump_tunnel::adopt(const QByteArray &state, int jump_fd, int ours_fd, int theirs_fd,
                             QString &why) {
	if (m_session) {
		why = QStringLiteral("this tunnel is already open");
		return false;
	}
	QDataStream in(state);
	in.setVersion(QDataStream::Qt_6_0);
	QByteArray magic, transport;
	quint32 format = 0;
	in >> magic >> format >> transport;
	if (in.status() != QDataStream::Ok || !in.atEnd() || magic != BSSH_JUMP_HANDOFF_MAGIC ||
	    format != BSSH_JUMP_HANDOFF_FORMAT) {
		why = QStringLiteral("the jump host's tunnel is not one this BeerSSH reads");
		return false;
	}
	ssh_session session = ssh_new();
	ssh_string blob = ssh_string_new(size_t(transport.size()));
	if (!session || !blob ||
	    ssh_string_fill(blob, transport.constData(), size_t(transport.size())) != 0) {
		if (blob) ssh_string_free(blob);
		if (session) ssh_free(session);
		why = QStringLiteral("out of memory");
		return false;
	}
	const int rc = ssh_session_import_transport(session, jump_fd, blob);
	ssh_string_burn(blob);
	ssh_string_free(blob);
	ssh_channel channel = rc == SSH_OK ? ssh_session_get_channel(session, 0) : nullptr;
	if (!channel) {
		why = rc == SSH_OK ? QStringLiteral("the jump host's tunnel arrived without its channel")
		                   : QString::fromUtf8(ssh_get_error(session));
		if (rc == SSH_OK) ssh_session_detach_transport(session);
		ssh_free(session);
		return false;
	}
	ssh_set_blocking(session, 0);
	m_session = session;
	m_channel = channel;
	m_ours = ours_fd;
	m_theirs = theirs_fd;
	/*
	 * Paused, not started: the caller starts it (resume()) once the
	 * target session has been imported too. A pump running while that
	 * could still fail would read the jump host's packets with these
	 * keys, and the giver -- resuming the same connection -- would then
	 * be a packet behind.
	 */
	m_stop = false;
	m_pause = true;
	return true;
}
