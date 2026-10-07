#include "sshd/sshd.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

/*
 * libssh declares its server API only under WITH_SERVER. Set here, in the
 * source, rather than by each build system: qmake carried it and fmake,
 * which reads no .pro, built this file without it and failed.
 */
#ifndef WITH_SERVER
#define WITH_SERVER
#endif
#include <libssh/callbacks.h>
#include <libssh/libssh.h>
#include <libssh/server.h>
#include <libssh/sftp.h>

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sshd/root_path.h"
#include "sshd/sftp_handler.h"

namespace {

/* Long enough for a person typing a passphrase into their agent, and no longer. */
const auto LOGIN_DEADLINE = std::chrono::seconds(60);

/* How long a finished connection waits for the client to hang up before doing it. */
const auto HANGUP_GRACE = std::chrono::seconds(5);

/* Keys offered before giving up; an agent with many keys offers each in turn. */
const int AUTH_ATTEMPT_LIMIT = 20;

QString peer_of(ssh_session session) {
	struct sockaddr_storage address;
	socklen_t length = sizeof address;
	if (::getpeername(ssh_get_fd(session), reinterpret_cast<sockaddr *>(&address), &length) != 0)
		return QStringLiteral("?");

	char text[INET6_ADDRSTRLEN] = "";
	const void *raw = address.ss_family == AF_INET6
	        ? static_cast<const void *>(&reinterpret_cast<sockaddr_in6 *>(&address)->sin6_addr)
	        : static_cast<const void *>(&reinterpret_cast<sockaddr_in *>(&address)->sin_addr);
	::inet_ntop(address.ss_family, raw, text, sizeof text);
	return QString::fromLatin1(text);
}

QString fingerprint_of(ssh_key key) {
	unsigned char *hash = nullptr;
	size_t length = 0;
	if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &length) != 0) return QString();
	char *text = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, length);
	ssh_clean_pubkey_hash(&hash);
	const QString result = text ? QString::fromLatin1(text) : QString();
	ssh_string_free_char(text);
	return result;
}

/*
 * Whether `offered` is one of the keys in an authorized_keys file.
 *
 * Read at every attempt rather than once at start, so a key added or
 * removed applies to the next login without restarting the server.
 *
 * A line carrying options -- `from=`, `command=`, `restrict` and the rest
 * -- is skipped, not honoured without them. Each option narrows what the
 * key may do, and this server enforces none of them, so using the key
 * anyway would grant more than the line says.
 */
ssh_key key_from_line(const QByteArray &raw) {
	const QByteArray line = raw.trimmed();
	if (line.isEmpty() || line.startsWith('#')) return nullptr;

	const QList<QByteArray> fields = line.simplified().split(' ');
	if (fields.size() < 2) return nullptr;

	const enum ssh_keytypes_e type = ssh_key_type_from_name(fields.at(0).constData());
	if (type == SSH_KEYTYPE_UNKNOWN) return nullptr;

	ssh_key key = nullptr;
	if (ssh_pki_import_pubkey_base64(fields.at(1).constData(), type, &key) != SSH_OK) return nullptr;
	return key;
}

bool authorized(const QString &path, ssh_key offered) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return false;

	while (!file.atEnd()) {
		ssh_key listed = key_from_line(file.readLine(64 * 1024));
		if (!listed) continue;
		const bool same = ssh_key_cmp(listed, offered, SSH_KEY_CMP_PUBLIC) == 0;
		ssh_key_free(listed);
		if (same) return true;
	}
	return false;
}

/* The host key, generated on first use. Empty fingerprint on failure. */
QString prepare_host_key(const QString &path, QString *error) {
	ssh_key key = nullptr;

	if (QFileInfo::exists(path)) {
		if (ssh_pki_import_privkey_file(QFile::encodeName(path).constData(), nullptr, nullptr,
		                                nullptr, &key) != SSH_OK) {
			*error = QStringLiteral("The host key %1 could not be read.").arg(path);
			return QString();
		}
	} else {
		QDir().mkpath(QFileInfo(path).absolutePath());
		if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK) {
			*error = QStringLiteral("A host key could not be generated.");
			return QString();
		}

		/*
		 * Written with a umask that leaves nobody else a bit, rather than
		 * chmod'ed afterwards: in between, the private key would sit
		 * readable by whatever the umask allowed.
		 */
		const mode_t previous = ::umask(077);
		const int rc = ssh_pki_export_privkey_file(key, nullptr, nullptr, nullptr,
		                                           QFile::encodeName(path).constData());
		::umask(previous);
		if (rc != SSH_OK) {
			ssh_key_free(key);
			*error = QStringLiteral("The host key could not be saved to %1.").arg(path);
			return QString();
		}
	}

	const QString fingerprint = fingerprint_of(key);
	ssh_key_free(key);
	return fingerprint;
}

} /* namespace */

struct bssh_sshd::connection_t {
	bssh_sshd *owner = nullptr;
	ssh_session session = nullptr;
	std::thread thread;
	std::atomic<bool> done{false};

	/* Login state, written by libssh's callbacks on the connection's own thread. */
	QString peer;
	QString user;
	bool authenticated = false;
	int attempts = 0;
	ssh_channel channel = nullptr;
	bool sftp_requested = false;
	struct ssh_channel_callbacks_struct channel_callbacks;

	/* Freed only once the thread has been joined; see the end of serve(). */
	~connection_t() {
		if (session) ssh_free(session);
	}
};

namespace {

int on_auth_pubkey(ssh_session, const char *user, struct ssh_key_struct *key,
                   char signature_state, void *userdata);
ssh_channel on_channel_open(ssh_session session, void *userdata);
int on_subsystem(ssh_session, ssh_channel channel, const char *subsystem, void *userdata);

} /* namespace */

bssh_sshd::bssh_sshd(QObject *parent) : QObject(parent) {
}

int bssh_sshd::usable_keys(const QByteArray &text) {
	int count = 0;
	for (const QByteArray &line : text.split('\n')) {
		ssh_key key = key_from_line(line);
		if (!key) continue;
		ssh_key_free(key);
		++count;
	}
	return count;
}

bssh_sshd::~bssh_sshd() {
	stop();
}

bool bssh_sshd::start(const bssh_sshd_config_t &config, QString *error) {
	QString ignored;
	if (!error) error = &ignored;
	if (running()) {
		*error = QStringLiteral("Already running.");
		return false;
	}

	if (!bssh_root_path(config.root).valid()) {
		*error = QStringLiteral("The folder to serve, %1, does not exist.")
		         .arg(QFile::decodeName(config.root));
		return false;
	}

	m_fingerprint = prepare_host_key(config.host_key_path, error);
	if (m_fingerprint.isEmpty()) return false;

	ssh_bind bind = ssh_bind_new();
	const QByteArray key_path = QFile::encodeName(config.host_key_path);
	const int port = config.port;
	if (ssh_bind_options_set(bind, SSH_BIND_OPTIONS_HOSTKEY, key_path.constData()) != SSH_OK
	    || ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDADDR, config.bind_address.constData()) != SSH_OK
	    || ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT, &port) != SSH_OK
	    || ssh_bind_listen(bind) != SSH_OK) {
		*error = QStringLiteral("Could not listen on port %1: %2")
		         .arg(port).arg(QString::fromLocal8Bit(ssh_get_error(bind)));
		ssh_bind_free(bind);
		return false;
	}

	struct sockaddr_storage address;
	socklen_t length = sizeof address;
	m_port = port;
	if (::getsockname(ssh_bind_get_fd(bind), reinterpret_cast<sockaddr *>(&address), &length) == 0) {
		m_port = address.ss_family == AF_INET6
		       ? ntohs(reinterpret_cast<sockaddr_in6 *>(&address)->sin6_port)
		       : ntohs(reinterpret_cast<sockaddr_in *>(&address)->sin_port);
	}

	if (::pipe2(m_wake, O_CLOEXEC) != 0) {
		*error = QStringLiteral("Could not create the stop pipe.");
		ssh_bind_free(bind);
		return false;
	}

	m_config = config;
	m_bind = bind;
	m_stopping = false;
	m_accepting = std::thread([this] { accept_loop(); });
	return true;
}

void bssh_sshd::stop() {
	if (!running()) return;

	m_stopping = true;
	const char byte = 0;
	(void)!::write(m_wake[1], &byte, 1);
	m_accepting.join();

	/*
	 * Each connection is blocked reading its socket. Shutting the socket
	 * down from here is what returns that read; the thread then frees its
	 * own session, which nothing else touches.
	 */
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (const std::unique_ptr<connection_t> &connection : m_connections) {
			if (!connection->done) ::shutdown(ssh_get_fd(connection->session), SHUT_RDWR);
		}
	}
	reap(true);

	ssh_bind_free(static_cast<ssh_bind>(m_bind));
	m_bind = nullptr;
	::close(m_wake[0]);
	::close(m_wake[1]);
	m_wake[0] = m_wake[1] = -1;
}

void bssh_sshd::reap(bool all) {
	std::list<std::unique_ptr<connection_t>> finished;
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		for (auto it = m_connections.begin(); it != m_connections.end();) {
			if (all || (*it)->done) {
				finished.push_back(std::move(*it));
				it = m_connections.erase(it);
			} else {
				++it;
			}
		}
	}
	for (const std::unique_ptr<connection_t> &connection : finished) connection->thread.join();
}

void bssh_sshd::accept_loop() {
	ssh_bind bind = static_cast<ssh_bind>(m_bind);

	while (!m_stopping) {
		struct pollfd fds[2] = {
			{ssh_bind_get_fd(bind), POLLIN, 0},
			{m_wake[0], POLLIN, 0},
		};
		if (::poll(fds, 2, 1000) < 0 && errno != EINTR) break;
		reap(false);
		if (m_stopping || fds[1].revents) break;
		if (!(fds[0].revents & POLLIN)) continue;

		ssh_session session = ssh_new();
		if (ssh_bind_accept(bind, session) != SSH_OK) {
			ssh_free(session);
			continue;
		}

		if (m_live.load() >= m_config.max_connections) {
			emit logged(QStringLiteral("%1: refused, %2 connections already open")
			           .arg(peer_of(session)).arg(m_config.max_connections));
			ssh_disconnect(session);
			ssh_free(session);
			continue;
		}

		auto connection = std::make_unique<connection_t>();
		connection->owner = this;
		connection->session = session;
		connection->peer = peer_of(session);
		connection_t *raw = connection.get();

		emit connections_changed(++m_live);
		std::lock_guard<std::mutex> lock(m_mutex);
		m_connections.push_back(std::move(connection));
		raw->thread = std::thread([this, raw] { serve(raw); });
	}
}

void bssh_sshd::serve(connection_t *connection) {
	ssh_session session = connection->session;
	ssh_set_auth_methods(session, SSH_AUTH_METHOD_PUBLICKEY);

	struct ssh_server_callbacks_struct server_callbacks;
	std::memset(&server_callbacks, 0, sizeof server_callbacks);
	server_callbacks.userdata = connection;
	server_callbacks.auth_pubkey_function = on_auth_pubkey;
	server_callbacks.channel_open_request_session_function = on_channel_open;
	ssh_callbacks_init(&server_callbacks);
	ssh_set_server_callbacks(session, &server_callbacks);

	std::memset(&connection->channel_callbacks, 0, sizeof connection->channel_callbacks);
	connection->channel_callbacks.userdata = connection;
	connection->channel_callbacks.channel_subsystem_request_function = on_subsystem;
	ssh_callbacks_init(&connection->channel_callbacks);

	if (ssh_handle_key_exchange(session) == SSH_OK) {
		/*
		 * Login and the request for SFTP, under a deadline: a connection
		 * that never authenticates must not hold one of the few slots for
		 * as long as it likes.
		 */
		ssh_event loop = ssh_event_new();
		ssh_event_add_session(loop, session);
		const auto deadline = std::chrono::steady_clock::now() + LOGIN_DEADLINE;

		while (!connection->sftp_requested && !m_stopping) {
			if (ssh_event_dopoll(loop, 100) == SSH_ERROR) break;
			if (std::chrono::steady_clock::now() > deadline) break;
			if (connection->attempts > AUTH_ATTEMPT_LIMIT) break;
			if (ssh_get_status(session) & (SSH_CLOSED | SSH_CLOSED_ERROR)) break;
		}

		ssh_event_remove_session(loop, session);
		ssh_event_free(loop);

		if (connection->sftp_requested && connection->authenticated && !m_stopping) {
			emit logged(QStringLiteral("%1: %2 started SFTP").arg(connection->peer, connection->user));

			sftp_session sftp = sftp_server_new(session, connection->channel);
			const bssh_root_path root(m_config.root);

			/*
			 * sftp_server_init is deprecated in favour of libssh's callback
			 * server -- whose handlers are the unconfined defaults this
			 * server exists not to use. It is the one public way to answer
			 * INIT before driving the message loop ourselves.
			 */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
			const bool ready = sftp && sftp_server_init(sftp) == 0;
#pragma GCC diagnostic pop

			if (ready) {
				bssh_sftp_handler handler(sftp, root);
				while (!m_stopping) {
					sftp_client_message message = sftp_get_client_message(sftp);
					if (!message) break;
					const bool answered = handler.handle(message);
					sftp_client_message_free(message);
					if (!answered) break;
				}
			}
			if (sftp) sftp_server_free(sftp);
		} else if (!connection->authenticated) {
			emit logged(QStringLiteral("%1: no authorized key offered").arg(connection->peer));
		}
	}

	/*
	 * Ended the way sshd ends a subsystem: an exit status, EOF, close --
	 * and then the CLIENT hangs up. Two halves, measured separately:
	 * without the exit status scp exits 1 after delivering the file
	 * intact, reading the silence as a failed remote side; without the
	 * wait, every OpenSSH client prints "closed by remote host", 3 runs
	 * in 3, for a session that ended normally.
	 */
	if (connection->channel) {
		if (connection->sftp_requested && !m_stopping) {
			ssh_channel_request_send_exit_status(connection->channel, 0);
		}
		ssh_channel_send_eof(connection->channel);
		ssh_channel_close(connection->channel);

		ssh_event loop = ssh_event_new();
		ssh_event_add_session(loop, session);
		const auto deadline = std::chrono::steady_clock::now() + HANGUP_GRACE;
		while (!m_stopping && std::chrono::steady_clock::now() < deadline) {
			if (ssh_event_dopoll(loop, 100) == SSH_ERROR) break;
			if (ssh_get_status(session) & (SSH_CLOSED | SSH_CLOSED_ERROR)) break;
		}
		ssh_event_remove_session(loop, session);
		ssh_event_free(loop);

		ssh_channel_free(connection->channel);
		connection->channel = nullptr;
	}
	ssh_disconnect(session);

	emit logged(QStringLiteral("%1: disconnected").arg(connection->peer));
	emit connections_changed(--m_live);
	connection->done = true;

	/*
	 * Not freed here: stop() may be reading this session's fd under the
	 * lock to shut it down. The session goes with the connection when it
	 * is reaped, after this thread has been joined.
	 */
}

namespace {

int on_auth_pubkey(ssh_session, const char *user, struct ssh_key_struct *key,
                   char signature_state, void *userdata) {
	auto *connection = static_cast<bssh_sshd::connection_t *>(userdata);
	++connection->attempts;

	if (signature_state != SSH_PUBLICKEY_STATE_NONE && signature_state != SSH_PUBLICKEY_STATE_VALID)
		return SSH_AUTH_DENIED;
	if (!authorized(connection->owner->config().authorized_keys_path, key)) return SSH_AUTH_DENIED;

	/*
	 * NONE is the client asking whether this key WOULD do, before it
	 * signs anything; VALID is the signature, checked by libssh, and only
	 * that logs in.
	 */
	if (signature_state == SSH_PUBLICKEY_STATE_VALID) {
		connection->authenticated = true;
		connection->user = QString::fromUtf8(user);
	}
	return SSH_AUTH_SUCCESS;
}

ssh_channel on_channel_open(ssh_session session, void *userdata) {
	auto *connection = static_cast<bssh_sshd::connection_t *>(userdata);

	/* One session channel per connection: SFTP needs no more. */
	if (!connection->authenticated || connection->channel) return nullptr;

	connection->channel = ssh_channel_new(session);
	ssh_set_channel_callbacks(connection->channel, &connection->channel_callbacks);
	return connection->channel;
}

int on_subsystem(ssh_session, ssh_channel channel, const char *subsystem, void *userdata) {
	auto *connection = static_cast<bssh_sshd::connection_t *>(userdata);
	if (channel != connection->channel || std::strcmp(subsystem, "sftp") != 0) return SSH_ERROR;

	connection->sftp_requested = true;
	return SSH_OK;
}

} /* namespace */
