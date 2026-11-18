#include "ssh/fuzznet_session.h"

#include <QMetaObject>
#include <QMutexLocker>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>

#include "session/fuzznet_link.h"

namespace {

/*
 * A host that has acknowledged nothing through this many timeouts in a row,
 * each twice the last -- about half a minute -- is taken for gone, as an SSH
 * keepalive that goes unanswered is; the session then reconnects as it
 * does from any drop.
 */
const unsigned SILENT_BACKOFFS = 5;

/* How long the thread waits to hear the host before it looks again. */
const unsigned PUMP_MS = 50;

/* fuzznet takes a numeric address and resolves nothing, so it is done here. */
QByteArray ipv4_for(const QByteArray &host) {
	in_addr numeric;
	if (inet_pton(AF_INET, host.constData(), &numeric) == 1) return host;
	addrinfo hints = {};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo *found = nullptr;
	if (getaddrinfo(host.constData(), nullptr, &hints, &found) != 0 || !found) return QByteArray();
	char text[INET_ADDRSTRLEN] = {};
	const auto *address = reinterpret_cast<const sockaddr_in *>(found->ai_addr);
	inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
	freeaddrinfo(found);
	return QByteArray(text);
}

} /* namespace */

bssh_fuzznet_session::bssh_fuzznet_session(const QString &image, QObject *parent)
    : bssh_ssh_session(parent), m_image(image) {}

bssh_fuzznet_session::~bssh_fuzznet_session() {
	stop();
}

void bssh_fuzznet_session::connect_to_host(const QString &host, quint16 port, const QString &user) {
	Q_UNUSED(user);
	stop();
	set_state(CONNECTING);
	{
		QMutexLocker held(&m_lock);
		m_outgoing.clear();
	}
	m_stopping = false;
	m_worker = std::thread(&bssh_fuzznet_session::run, this, m_image.toLatin1(), host.toUtf8(), port);
}

void bssh_fuzznet_session::open_shell(int cols, int rows) {
	Q_UNUSED(cols);
	Q_UNUSED(rows);
	if (!m_raw || m_command.isEmpty()) {
		emit failed(QStringLiteral("over fuzznet a host keeps BeerSSH's sessions only, and this "
		                           "one is not asked to keep its shell"));
		stop();
		set_state(FAILED);
		return;
	}
	/* The command line first, as an exec request would name it, then the frames. */
	send(m_command.join(QLatin1Char(' ')).toUtf8() + '\n');
}

void bssh_fuzznet_session::send(const QByteArray &bytes) {
	QMutexLocker held(&m_lock);
	m_outgoing.append(bytes);
}

void bssh_fuzznet_session::disconnect_from_host() {
	stop();
	set_state(DISCONNECTED);
}

void bssh_fuzznet_session::stop() {
	m_stopping = true;
	if (m_worker.joinable()) m_worker.join();
}

void bssh_fuzznet_session::post_bytes(const QByteArray &bytes) {
	QMetaObject::invokeMethod(this, [this, bytes] { emit bytes_received(bytes); },
	                          Qt::QueuedConnection);
}

void bssh_fuzznet_session::post_connected() {
	QMetaObject::invokeMethod(this, [this] {
		if (state() == CONNECTING) set_state(CONNECTED);
	}, Qt::QueuedConnection);
}

void bssh_fuzznet_session::post_failed(const QString &reason) {
	QMetaObject::invokeMethod(this, [this, reason] {
		if (state() == DISCONNECTED) return;
		emit failed(reason);
		set_state(FAILED);
	}, Qt::QueuedConnection);
}

/*
 * The link's thread: everything it touches of this object is m_outgoing,
 * under the lock, and m_stopping; what it learns goes back queued.
 */
void bssh_fuzznet_session::run(QByteArray image, QByteArray host, quint16 port) {
	if (image.isEmpty()) {
		post_failed(QStringLiteral("this host's fuzznet device is not in the secret store: unlock "
		                           "it, or pair the host again"));
		return;
	}
	const QByteArray address = ipv4_for(host);
	if (address.isEmpty()) {
		post_failed(QStringLiteral("%1 has no IPv4 address to reach over fuzznet")
		                    .arg(QString::fromUtf8(host)));
		return;
	}
	const auto deliver = [](void *context, const uint8_t *bytes, size_t len) {
		static_cast<bssh_fuzznet_session *>(context)->post_bytes(
		        QByteArray(reinterpret_cast<const char *>(bytes), int(len)));
	};
	const char *why = "";
	bssh_fuzznet_link_t *link =
	        bssh_fuzznet_link_open(image.constData(), address.constData(), port, deliver, this, &why);
	/* The image is the device's keys: no copy of it outlives the link's. */
	image.fill('\0');
	if (!link) {
		post_failed(QStringLiteral("fuzznet: %1").arg(QString::fromUtf8(why)));
		return;
	}

	bool up = false;
	while (!m_stopping) {
		{
			QMutexLocker held(&m_lock);
			if (!m_outgoing.isEmpty()) {
				const size_t taken = bssh_fuzznet_link_write(
				        link, reinterpret_cast<const uint8_t *>(m_outgoing.constData()),
				        size_t(m_outgoing.size()));
				m_outgoing.remove(0, int(taken));
			}
		}
		if (bssh_fuzznet_link_pump(link, PUMP_MS) != 0) {
			post_failed(QStringLiteral("%1 did not answer over fuzznet").arg(QString::fromUtf8(host)));
			break;
		}
		if (!up && bssh_fuzznet_link_up(link)) {
			up = true;
			post_connected();
		}
		if (bssh_fuzznet_link_backoffs(link) >= SILENT_BACKOFFS) {
			post_failed(QStringLiteral("%1 stopped answering over fuzznet").arg(QString::fromUtf8(host)));
			break;
		}
	}
	bssh_fuzznet_link_close(link);
}
