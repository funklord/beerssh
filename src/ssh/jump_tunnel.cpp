#include "ssh/jump_tunnel.h"

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

	const int colon = rest.lastIndexOf(QLatin1Char(':'));
	if (colon >= 0 && !rest.contains(QLatin1Char(']'))) {
		bool ok = false;
		const uint value = rest.mid(colon + 1).toUInt(&ok);
		if (ok && value > 0 && value <= 65535) {
			hop.port = quint16(value);
			rest = rest.left(colon);
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

	m_stop = false;
	start();
	return m_theirs;
}

void bssh_jump_tunnel::run() {
	char buffer[16384];

	/*
	 * Both directions, neither allowed to starve the other, and a short
	 * poll timeout rather than a blocking read: the channel side has no
	 * descriptor to select on, so it is checked by asking rather than by
	 * waiting.
	 */
	while (!m_stop) {
		struct pollfd fd;
		fd.fd = m_ours;
		fd.events = POLLIN;
		fd.revents = 0;

		const int ready = poll(&fd, 1, 20);
		if (m_stop) break;

		if (ready > 0 && (fd.revents & POLLIN)) {
			const ssize_t n = ::read(m_ours, buffer, sizeof(buffer));
			if (n <= 0) break;
			if (ssh_channel_write(m_channel, buffer, int(n)) == SSH_ERROR) break;
		}
		if (ready > 0 && (fd.revents & (POLLHUP | POLLERR))) break;

		while (!m_stop) {
			const int n = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), 0);
			if (n == SSH_ERROR) return;
			if (n <= 0) break;

			ssize_t written = 0;
			while (written < n) {
				const ssize_t w = ::write(m_ours, buffer + written, size_t(n - written));
				if (w <= 0) return;
				written += w;
			}
		}

		if (ssh_channel_is_eof(m_channel)) break;
	}
}

void bssh_jump_tunnel::close() {
	m_stop = true;

	/*
	 * Our end first: the pump is sitting in poll() on it, and closing it
	 * is what wakes the thread. Freeing the session out from under a
	 * running pump would be a use-after-free with a plausible-looking
	 * stack.
	 */
	if (m_ours >= 0) {
		::shutdown(m_ours, SHUT_RDWR);
	}

	if (isRunning()) {
		if (!wait(2000)) {
			terminate();
			wait(500);
		}
	}

	if (m_ours >= 0) {
		::close(m_ours);
		m_ours = -1;
	}
	/*
	 * NOT the other end. It was handed to the target session, which owns
	 * it from that moment and closes it with ssh_free -- closing it here
	 * as well is a double close, and on a busy process that is somebody
	 * else's descriptor.
	 */
	m_theirs = -1;

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
}
