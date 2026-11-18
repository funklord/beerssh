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

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include "posix_compat.h"
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sshd/helper_channel.h"
#include "sshd/root_path.h"
#include "sshd/sftp_handler.h"

namespace {

/* Long enough for a person typing a passphrase into their agent, and no longer. */
const auto LOGIN_DEADLINE = std::chrono::seconds(60);

/* How long a finished connection waits for the client to hang up before doing it. */
const auto HANGUP_GRACE = std::chrono::seconds(5);

/* Keys offered before giving up; an agent with many keys offers each in turn. */
const int AUTH_ATTEMPT_LIMIT = 20;

/* Session channels a connection may hold where the helper is served. */
const size_t MAX_CHANNELS = 8;

/* How much of either direction waits in this process before the other side is made to. */
const int HELPER_BUFFER = 256 * 1024;

/*
 * A logged-in connection with no channel at all, between a client's probe
 * and its attach, is held this long and no longer: it holds a slot.
 */
const auto IDLE_LIMIT = std::chrono::seconds(60);

/*
 * Passwords, unlike keys, can be guessed: three tries a connection, a
 * second's pause after each wrong one, and every one of them in the log.
 * That bounds a connection and nothing else, so bssh_login_throttle counts
 * them across connections.
 */
const int PASSWORD_ATTEMPT_LIMIT = 3;
const auto PASSWORD_FAILURE_PAUSE = std::chrono::seconds(1);

/*
 * PBKDF2-SHA256's iterations: what a guess costs whoever holds the file. Paid
 * once per login attempt here, which the limits above already keep rare.
 */
const int PASSWORD_ITERATIONS = 200000;
const char PASSWORD_SCHEME[] = "pbkdf2-sha256";

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

/*
 * One session channel of a connection that may hold several: where the
 * helper is served, a client opens one to ask its version, one to attach,
 * and one for SFTP beside them (its file panel), all on one login.
 */
struct served_channel_t {
	bssh_sshd::connection_t *connection = nullptr;
	ssh_channel channel = nullptr;
	struct ssh_channel_callbacks_struct callbacks;

	enum kind_t { OPEN, SFTP, HELPER } kind = OPEN;
	bool finished = false;

	sftp_session sftp = nullptr;
	std::unique_ptr<bssh_sftp_handler> handler;

	bssh_helper_process_t helper;
	QList<QByteArray> arguments;
	QByteArray to_helper;      /* from the client, not yet taken by the helper */
	QByteArray from_helper;    /* its stdout, not yet sent */
	QByteArray errors_out;     /* its stderr, not yet sent */
	bool input_shut = false;   /* the client's EOF passed on */
	bool output_ended = false;
	bool errors_ended = false;
	bool watching = false;     /* its stdout is in the event loop */
	bool watching_errors = false;
	int status = -1;

	~served_channel_t() {
		/* Only where the connection ended before its loop did: the loop reaps its own. */
		if (helper.pid >= 0) {
			::kill(helper.pid, SIGKILL);
			bssh_helper_reap(&helper, true);
		}
		if (sftp) sftp_server_free(sftp);
		bssh_helper_close(&helper);
		if (channel) ssh_channel_free(channel);
	}
};

struct bssh_sshd::connection_t {
	bssh_sshd *owner = nullptr;
	ssh_session session = nullptr;
	std::thread thread;
	std::atomic<bool> done{false};

	/* Login state, written by libssh's callbacks on the connection's own thread. */
	QString peer;
	QString user;
	std::atomic<bool> authenticated{false};   /* read by accept_loop too */
	int attempts = 0;
	int password_attempts = 0;
	ssh_channel channel = nullptr;
	bool sftp_requested = false;
	struct ssh_channel_callbacks_struct channel_callbacks;

	/* Where the helper is served: every channel, and whether any asked for something. */
	std::list<std::unique_ptr<served_channel_t>> channels;
	bool requested = false;

	/* Freed only once the thread has been joined; see the end of serve(). */
	~connection_t() {
		channels.clear();   /* serve() has emptied it; their channels belong to the session */
		if (session) ssh_free(session);
	}
};

namespace {

int on_auth_pubkey(ssh_session, const char *user, struct ssh_key_struct *key,
                   char signature_state, void *userdata);
int on_auth_password(ssh_session, const char *user, const char *password, void *userdata);
ssh_channel on_channel_open(ssh_session session, void *userdata);
int on_subsystem(ssh_session, ssh_channel channel, const char *subsystem, void *userdata);
int on_served_subsystem(ssh_session, ssh_channel, const char *subsystem, void *userdata);
int on_served_exec(ssh_session, ssh_channel, const char *command, void *userdata);

} /* namespace */

bssh_sshd::bssh_sshd(QObject *parent) : QObject(parent) {
}

namespace {

/* PBKDF2-SHA256 of `password` over `salt`, 32 bytes; empty on failure. */
QByteArray derive(const QByteArray &password, const QByteArray &salt, int iterations) {
	QByteArray out(32, 0);
	if (PKCS5_PBKDF2_HMAC(password.constData(), int(password.size()),
	                      reinterpret_cast<const unsigned char *>(salt.constData()), int(salt.size()),
	                      iterations, EVP_sha256(), int(out.size()),
	                      reinterpret_cast<unsigned char *>(out.data())) != 1)
		return QByteArray();
	return out;
}

struct stored_password_t {
	int iterations = 0;
	QByteArray salt;
	QByteArray hash;
};

/* "pbkdf2-sha256:<iterations>:<salt hex>:<hash hex>", or nothing usable. */
bool read_password(const QString &path, stored_password_t *out) {
	if (path.isEmpty()) return false;
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return false;
	const QList<QByteArray> parts = file.readAll().trimmed().split(':');
	if (parts.size() != 4 || parts.at(0) != PASSWORD_SCHEME) return false;
	bool ok = false;
	out->iterations = parts.at(1).toInt(&ok);
	out->salt = QByteArray::fromHex(parts.at(2));
	out->hash = QByteArray::fromHex(parts.at(3));
	return ok && out->iterations > 0 && out->salt.size() >= 16 && out->hash.size() == 32;
}

} /* namespace */

bool bssh_sshd::set_password(const QString &path, const QString &password, QString *error) {
	if (path.isEmpty()) {
		if (error) *error = QStringLiteral("nowhere to keep a password");
		return false;
	}
	if (password.isEmpty()) {
		if (QFile::exists(path) && !QFile::remove(path)) {
			if (error) *error = QStringLiteral("could not remove %1").arg(path);
			return false;
		}
		return true;
	}

	QByteArray salt(16, 0);
	if (RAND_bytes(reinterpret_cast<unsigned char *>(salt.data()), int(salt.size())) != 1) {
		if (error) *error = QStringLiteral("no random bytes for the salt");
		return false;
	}
	const QByteArray hash = derive(password.toUtf8(), salt, PASSWORD_ITERATIONS);
	if (hash.isEmpty()) {
		if (error) *error = QStringLiteral("the password could not be hashed");
		return false;
	}
	const QByteArray line = QByteArray(PASSWORD_SCHEME) + ':' + QByteArray::number(PASSWORD_ITERATIONS)
	        + ':' + salt.toHex() + ':' + hash.toHex() + '\n';

	/*
	 * Created owner-only rather than chmod'ed afterwards, as the host key
	 * is, so the hash is never readable by anybody else in between.
	 */
	const QString temporary = path + QStringLiteral(".new");
	const mode_t was = umask(077);
	QFile file(temporary);
	const bool written = file.open(QIODevice::WriteOnly | QIODevice::Truncate)
	                     && file.write(line) == line.size() && file.flush();
	file.close();
	umask(was);
	if (!written || std::rename(QFile::encodeName(temporary).constData(),
	                            QFile::encodeName(path).constData()) != 0) {
		QFile::remove(temporary);
		if (error) *error = QStringLiteral("could not write %1").arg(path);
		return false;
	}
	return true;
}

bool bssh_sshd::has_password(const QString &path) {
	stored_password_t stored;
	return read_password(path, &stored);
}

bool bssh_sshd::password_matches(const QString &path, const QByteArray &password) {
	stored_password_t stored;
	if (!read_password(path, &stored) || password.isEmpty()) return false;
	const QByteArray hash = derive(password, stored.salt, stored.iterations);
	return hash.size() == stored.hash.size()
	       && CRYPTO_memcmp(hash.constData(), stored.hash.constData(), size_t(hash.size())) == 0;
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
	std::lock_guard<std::mutex> lifecycle(m_lifecycle);
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

	if (bssh_pipe_cloexec(m_wake) != 0) {
		*error = QStringLiteral("Could not create the stop pipe.");
		ssh_bind_free(bind);
		return false;
	}

	m_config = config;
	m_bind = bind;
	m_stopping = false;
	m_listening = true;
	m_accepting = std::thread([this] { accept_loop(); });
	m_running = true;
	return true;
}

void bssh_sshd::stop() {
	std::lock_guard<std::mutex> lifecycle(m_lifecycle);
	if (!running()) return;
	m_running = false;

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

/*
 * Listens again on the address and port the server started with. The
 * port is the one in use rather than the configured one, which may have
 * been 0 for "any". The keys are already loaded, so this is a socket, a
 * bind and a listen -- cheap enough to try once a second.
 */
static bool listen_again(ssh_bind bind, int port) {
	ssh_bind_set_fd(bind, SSH_INVALID_SOCKET);
	return ssh_bind_options_set(bind, SSH_BIND_OPTIONS_BINDPORT, &port) == SSH_OK
	       && ssh_bind_listen(bind) == SSH_OK;
}

void bssh_sshd::accept_loop() {
	ssh_bind bind = static_cast<ssh_bind>(m_bind);
	const QString where = QStringLiteral("%1:%2")
	                      .arg(QString::fromLatin1(m_config.bind_address)).arg(m_port);

	while (!m_stopping) {
		/*
		 * A negative fd is one poll() skips: while the listener is gone,
		 * only the stop pipe and the one-second timeout wake this loop.
		 */
		const bool listening = m_listening.load();
		struct pollfd fds[2] = {
			{listening ? ssh_bind_get_fd(bind) : -1, POLLIN, 0},
			{m_wake[0], POLLIN, 0},
		};
		if (::poll(fds, 2, 1000) < 0 && errno != EINTR) break;
		reap(false);
		if (m_stopping || fds[1].revents) break;

		if (!listening) {
			if (!listen_again(bind, m_port)) continue;
			m_listening = true;
			emit logged(QStringLiteral("Listening again on %1").arg(where));
			emit listening_changed(true);
			continue;
		}

		/*
		 * The listener died under us. Android destroys every socket bound
		 * to an address the moment the network that owned it goes, so a
		 * Wi-Fi drop does this while nothing on Qt's thread is running to
		 * notice (project.md sec 19.6). Seen on an API 36 emulator: left
		 * in the poll set, the dead socket reported an error at once on
		 * every pass and this loop spun a core at ~90% for as long as
		 * Wi-Fi was off, and when it came back nothing listened -- while
		 * the screen and the notification still said "Serving". So it is
		 * closed and taken out of the set, and the address is tried again
		 * once a second until it is back.
		 */
		if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			::close(ssh_bind_get_fd(bind));
			ssh_bind_set_fd(bind, SSH_INVALID_SOCKET);
			m_listening = false;
			emit logged(QStringLiteral("No longer listening on %1: the network is gone. "
			                           "Listening again when it is back.").arg(where));
			emit listening_changed(false);
			continue;
		}
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

		/*
		 * One address may not take the last slot before it has logged
		 * in, or anybody on the network shuts the owner out for as long
		 * as a login may take. Those who have logged in count for
		 * nothing here: a backup tool opening four at once ends up with
		 * four, the last as soon as an earlier one is in.
		 */
		const QString peer = peer_of(session);
		int waiting = 0;
		{
			std::lock_guard<std::mutex> lock(m_mutex);
			for (const std::unique_ptr<connection_t> &other : m_connections) {
				if (!other->done && !other->authenticated && other->peer == peer) ++waiting;
			}
		}
		if (waiting >= m_config.max_connections - 1) {
			emit logged(QStringLiteral("%1: refused, too many logins waiting").arg(peer));
			ssh_disconnect(session);
			ssh_free(session);
			continue;
		}

		auto connection = std::make_unique<connection_t>();
		connection->owner = this;
		connection->session = session;
		connection->peer = peer;
		connection_t *raw = connection.get();

		emit connections_changed(++m_live);
		std::lock_guard<std::mutex> lock(m_mutex);
		m_connections.push_back(std::move(connection));
		raw->thread = std::thread([this, raw] { serve(raw); });
	}
}

void bssh_sshd::serve(connection_t *connection) {
	ssh_session session = connection->session;
	/*
	 * A password is offered only while one is set: with none, the client is
	 * not even asked, which is what keys-only meant before there was one.
	 */
	const bool passwords = has_password(m_config.password_path);
	ssh_set_auth_methods(session, SSH_AUTH_METHOD_PUBLICKEY
	                              | (passwords ? SSH_AUTH_METHOD_PASSWORD : 0));

	struct ssh_server_callbacks_struct server_callbacks;
	std::memset(&server_callbacks, 0, sizeof server_callbacks);
	server_callbacks.userdata = connection;
	server_callbacks.auth_pubkey_function = on_auth_pubkey;
	if (passwords) server_callbacks.auth_password_function = on_auth_password;
	server_callbacks.channel_open_request_session_function = on_channel_open;
	ssh_callbacks_init(&server_callbacks);
	ssh_set_server_callbacks(session, &server_callbacks);

	std::memset(&connection->channel_callbacks, 0, sizeof connection->channel_callbacks);
	connection->channel_callbacks.userdata = connection;
	connection->channel_callbacks.channel_subsystem_request_function = on_subsystem;
	ssh_callbacks_init(&connection->channel_callbacks);

	/*
	 * A limit on the exchange, which is before LOGIN_DEADLINE below and
	 * had none: a peer that connected and said nothing held its slot for
	 * ever, and four such sockets were every slot there is. Taken off
	 * again after, or it would end an SFTP session sitting idle.
	 */
	long seconds = m_config.exchange_seconds;
	ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &seconds);
	const bool exchanged = ssh_handle_key_exchange(session) == SSH_OK;
	seconds = 0;
	ssh_options_set(session, SSH_OPTIONS_TIMEOUT, &seconds);

	if (exchanged) {
		/*
		 * Login and the request for SFTP, under a deadline: a connection
		 * that never authenticates must not hold one of the few slots for
		 * as long as it likes.
		 */
		ssh_event loop = ssh_event_new();
		ssh_event_add_session(loop, session);
		const auto deadline = std::chrono::steady_clock::now() + LOGIN_DEADLINE;

		while (!connection->sftp_requested && !connection->requested && !m_stopping) {
			if (ssh_event_dopoll(loop, 100) == SSH_ERROR) break;
			if (std::chrono::steady_clock::now() > deadline) break;
			if (connection->attempts > AUTH_ATTEMPT_LIMIT) break;
			if (connection->password_attempts >= PASSWORD_ATTEMPT_LIMIT
			    && !connection->authenticated) break;
			if (ssh_get_status(session) & (SSH_CLOSED | SSH_CLOSED_ERROR)) break;
		}

		ssh_event_remove_session(loop, session);
		ssh_event_free(loop);

		if (connection->requested && connection->authenticated && !m_stopping) {
			serve_channels(connection);
		} else if (connection->sftp_requested && connection->authenticated && !m_stopping) {
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
				bssh_sftp_handler handler(sftp, root, [this](const QByteArray &real) {
					emit file_changed(QFile::decodeName(real));
				});
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
	/*
	 * Before the disconnect, which frees every channel the session still
	 * has: one opened and never asked for anything is still in this list.
	 */
	connection->channels.clear();
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

/* A helper's stdout or stderr, read into its buffer; the loop sends it on. */
int helper_readable(socket_t fd, int, void *userdata) {
	auto *served = static_cast<served_channel_t *>(userdata);
	const bool errors = fd == served->helper.errors;
	QByteArray &into = errors ? served->errors_out : served->from_helper;
	bool &ended = errors ? served->errors_ended : served->output_ended;

	char chunk[16384];
	while (!ended && into.size() < HELPER_BUFFER) {
		const ssize_t got = ::read(fd, chunk, sizeof chunk);
		if (got > 0) {
			into.append(chunk, int(got));
		} else if (got < 0 && errno == EINTR) {
			continue;
		} else if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			break;
		} else {
			ended = true;
		}
	}
	return 0;
}

/*
 * In the event loop exactly while there is room to read into: a full
 * buffer left in would wake the loop for ever with nothing it could do.
 */
void watch(served_channel_t *served, ssh_event loop, bool stopping) {
	const bool output = !stopping && !served->output_ended && served->from_helper.size() < HELPER_BUFFER;
	if (output != served->watching) {
		if (output) ssh_event_add_fd(loop, served->helper.io, POLLIN, helper_readable, served);
		else ssh_event_remove_fd(loop, served->helper.io);
		served->watching = output;
	}
	const bool errors = !stopping && !served->errors_ended && served->errors_out.size() < HELPER_BUFFER;
	if (errors != served->watching_errors) {
		if (errors) ssh_event_add_fd(loop, served->helper.errors, POLLIN, helper_readable, served);
		else ssh_event_remove_fd(loop, served->helper.errors);
		served->watching_errors = errors;
	}
}

/* As much of `bytes` as the client's window takes now; the rest waits. */
void send_some(ssh_channel channel, QByteArray *bytes, bool errors) {
	if (bytes->isEmpty()) return;
	if (!ssh_channel_is_open(channel)) {
		bytes->clear();
		return;
	}
	const int room = int(qMin<qint64>(ssh_channel_window_size(channel), bytes->size()));
	if (room <= 0) return;
	const int wrote = errors ? ssh_channel_write_stderr(channel, bytes->constData(), uint32_t(room))
	                         : ssh_channel_write(channel, bytes->constData(), uint32_t(room));
	if (wrote > 0) bytes->remove(0, wrote);
	else if (wrote == SSH_ERROR) bytes->clear();
}

/*
 * One turn of a helper's channel: the client's bytes to the helper, the
 * helper's to the client, and the end passed on either way. True while
 * something waits that a moment will move.
 */
bool pump_helper(served_channel_t *served) {
	ssh_channel channel = served->channel;

	if (!served->input_shut) {
		const int available = ssh_channel_poll(channel, 0);
		const int room = HELPER_BUFFER - int(served->to_helper.size());
		if (available > 0 && room > 0) {
			QByteArray chunk(qMin(available, room), Qt::Uninitialized);
			const int got = ssh_channel_read_nonblocking(channel, chunk.data(), uint32_t(chunk.size()), 0);
			if (got > 0) served->to_helper.append(chunk.constData(), got);
		}
		while (!served->to_helper.isEmpty()) {
			const ssize_t sent = ::send(served->helper.io, served->to_helper.constData(),
			                            size_t(served->to_helper.size()), BSSH_MSG_NOSIGNAL);
			if (sent > 0) {
				served->to_helper.remove(0, int(sent));
			} else if (sent < 0 && errno == EINTR) {
				continue;
			} else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				break;
			} else {
				served->to_helper.clear();   /* it has gone, and nothing will read this */
			}
		}
		/* EOF, an error, or a channel the client closed outright: the helper hears it as EOF. */
		const bool client_done = ssh_channel_poll(channel, 0) < 0 || !ssh_channel_is_open(channel);
		if (client_done && served->to_helper.isEmpty()) {
			::shutdown(served->helper.io, ssh_channel_is_open(channel) ? SHUT_WR : SHUT_RDWR);
			served->input_shut = true;
		}
	}

	send_some(channel, &served->from_helper, false);
	send_some(channel, &served->errors_out, true);

	if (served->output_ended && served->errors_ended && served->from_helper.isEmpty()
	    && served->errors_out.isEmpty()) {
		if (served->status < 0) served->status = bssh_helper_reap(&served->helper, false);
		if (served->status >= 0) {
			/* Ended as sshd ends a command: its status, EOF, close. */
			if (ssh_channel_is_open(channel)) {
				ssh_channel_request_send_exit_status(channel, served->status);
				ssh_channel_send_eof(channel);
				ssh_channel_close(channel);
			}
			served->finished = true;
			return false;
		}
		return true;
	}
	return !served->to_helper.isEmpty() || !served->from_helper.isEmpty() || !served->errors_out.isEmpty();
}

/* What arrived for an SFTP channel, answered without waiting for more. */
void pump_sftp(served_channel_t *served, ssh_session session, const bssh_root_path &root,
               const bssh_sftp_handler::changed_t &changed) {
	ssh_channel channel = served->channel;
	bool ended = !ssh_channel_is_open(channel);

	if (!ended && !served->sftp) {
		served->sftp = sftp_server_new(session, channel);
		/* As in serve(): the one public way to answer INIT and drive the loop here. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
		const bool ready = served->sftp && sftp_server_init(served->sftp) == 0;
#pragma GCC diagnostic pop
		if (ready) served->handler = std::make_unique<bssh_sftp_handler>(served->sftp, root, changed);
		else ended = true;
	}
	while (!ended && ssh_channel_poll(channel, 0) > 0) {
		sftp_client_message message = sftp_get_client_message(served->sftp);
		if (!message) {
			ended = true;
			break;
		}
		const bool answered = served->handler->handle(message);
		sftp_client_message_free(message);
		if (!answered) ended = true;
	}
	if (!ended && ssh_channel_poll(channel, 0) < 0) ended = true;
	if (!ended) return;

	if (ssh_channel_is_open(channel)) {
		ssh_channel_request_send_exit_status(channel, 0);
		ssh_channel_send_eof(channel);
		ssh_channel_close(channel);
	}
	served->finished = true;
}

} /* namespace */

/*
 * A connection where the helper is served: any number of channels up to
 * MAX_CHANNELS, each a helper or SFTP, driven from one loop so that one
 * cannot stall another. Ends when the client hangs up, when the server
 * stops, or when the connection has sat with no channel for IDLE_LIMIT.
 */
void bssh_sshd::serve_channels(connection_t *connection) {
	ssh_session session = connection->session;
	const bssh_root_path root(m_config.root);
	ssh_event loop = ssh_event_new();
	ssh_event_add_session(loop, session);
	auto idle_since = std::chrono::steady_clock::now();
	const bssh_sftp_handler::changed_t changed = [this](const QByteArray &real) {
		emit file_changed(QFile::decodeName(real));
	};

	while (!m_stopping) {
		bool busy = false;
		for (const std::unique_ptr<served_channel_t> &served : connection->channels) {
			if (served->kind == served_channel_t::HELPER) {
				busy |= pump_helper(served.get());
				watch(served.get(), loop, served->finished);
			} else if (served->kind == served_channel_t::SFTP) {
				pump_sftp(served.get(), session, root, changed);
			} else if (!ssh_channel_is_open(served->channel)) {
				served->finished = true;   /* opened, asked for nothing, closed */
			}
		}
		connection->channels.remove_if(
		        [](const std::unique_ptr<served_channel_t> &served) { return served->finished; });

		const auto now = std::chrono::steady_clock::now();
		if (!connection->channels.empty()) idle_since = now;
		else if (now - idle_since > IDLE_LIMIT) break;

		if (ssh_get_status(session) & (SSH_CLOSED | SSH_CLOSED_ERROR)) break;
		if (ssh_event_dopoll(loop, busy ? 20 : 200) == SSH_ERROR) break;
	}

	/*
	 * Every helper still running is told its client has gone, which ends
	 * an attach the way a dropped connection does -- the session itself
	 * goes on, that being the point -- and is waited for, briefly.
	 */
	for (const std::unique_ptr<served_channel_t> &served : connection->channels) {
		if (served->kind != served_channel_t::HELPER) continue;
		watch(served.get(), loop, true);
		if (served->helper.io >= 0) ::shutdown(served->helper.io, SHUT_RDWR);
	}
	for (const std::unique_ptr<served_channel_t> &served : connection->channels) {
		if (served->kind != served_channel_t::HELPER) continue;
		for (int waited = 0; served->helper.pid >= 0 && waited < 2000; waited += 20) {
			if (bssh_helper_reap(&served->helper, false) >= 0) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
		}
		if (served->helper.pid >= 0) {
			::kill(served->helper.pid, SIGKILL);
			bssh_helper_reap(&served->helper, true);
		}
		if (ssh_channel_is_open(served->channel)) {
			ssh_channel_send_eof(served->channel);
			ssh_channel_close(served->channel);
		}
	}
	ssh_event_remove_session(loop, session);
	ssh_event_free(loop);
	connection->channels.clear();
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

int on_auth_password(ssh_session, const char *user, const char *password, void *userdata) {
	auto *connection = static_cast<bssh_sshd::connection_t *>(userdata);
	if (connection->password_attempts >= PASSWORD_ATTEMPT_LIMIT) return SSH_AUTH_DENIED;
	++connection->password_attempts;

	/* Not even hashed: a guess during the wait learns nothing, right or wrong. */
	bssh_login_throttle &throttle = connection->owner->throttle();
	const auto now = bssh_login_throttle::clock::now();
	if (!throttle.allowed(connection->peer, now)) {
		emit connection->owner->logged(
		        QStringLiteral("%1: too many wrong passwords, not tried").arg(connection->peer));
		std::this_thread::sleep_for(PASSWORD_FAILURE_PAUSE);
		return SSH_AUTH_DENIED;
	}

	/*
	 * Re-read now, like the keys: a password changed or removed on the
	 * phone applies to the next attempt, not the next start.
	 */
	if (password && bssh_sshd::password_matches(connection->owner->config().password_path,
	                                             QByteArray(password))) {
		connection->authenticated = true;
		connection->user = QString::fromUtf8(user);
		throttle.succeeded(connection->peer);
		return SSH_AUTH_SUCCESS;
	}

	throttle.failed(connection->peer, now);
	emit connection->owner->logged(QStringLiteral("%1: wrong password").arg(connection->peer));
	std::this_thread::sleep_for(PASSWORD_FAILURE_PAUSE);
	return SSH_AUTH_DENIED;
}

ssh_channel on_channel_open(ssh_session session, void *userdata) {
	auto *connection = static_cast<bssh_sshd::connection_t *>(userdata);

	if (!connection->authenticated) return nullptr;

	/*
	 * Where the helper is served, several: its version, the attach and
	 * SFTP beside them. Bounded all the same, as the connections are.
	 */
	if (!connection->owner->config().session_helper.isEmpty()) {
		if (connection->channels.size() >= MAX_CHANNELS) return nullptr;
		auto served = std::make_unique<served_channel_t>();
		served->connection = connection;
		served->channel = ssh_channel_new(session);
		if (!served->channel) return nullptr;
		std::memset(&served->callbacks, 0, sizeof served->callbacks);
		served->callbacks.userdata = served.get();
		served->callbacks.channel_subsystem_request_function = on_served_subsystem;
		served->callbacks.channel_exec_request_function = on_served_exec;
		ssh_callbacks_init(&served->callbacks);
		ssh_set_channel_callbacks(served->channel, &served->callbacks);
		connection->channels.push_back(std::move(served));
		return connection->channels.back()->channel;
	}

	/* One session channel per connection: SFTP needs no more. */
	if (connection->channel) return nullptr;

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

int on_served_subsystem(ssh_session, ssh_channel, const char *subsystem, void *userdata) {
	auto *served = static_cast<served_channel_t *>(userdata);
	if (served->kind != served_channel_t::OPEN || std::strcmp(subsystem, "sftp") != 0) return SSH_ERROR;
	served->kind = served_channel_t::SFTP;
	served->connection->requested = true;
	return SSH_OK;
}

/*
 * The helper's two commands, and nothing else: anything more is what the
 * server without a helper refuses as well. Started here, so that a client
 * told OK finds it running.
 */
int on_served_exec(ssh_session, ssh_channel, const char *command, void *userdata) {
	auto *served = static_cast<served_channel_t *>(userdata);
	bssh_sshd::connection_t *connection = served->connection;
	const QByteArray program = connection->owner->config().session_helper;
	if (served->kind != served_channel_t::OPEN || program.isEmpty()) return SSH_ERROR;

	QList<QByteArray> arguments = bssh_helper_arguments(QByteArray(command));
	if (arguments.isEmpty()) {
		emit connection->owner->logged(QStringLiteral("%1: refused to run %2")
		                               .arg(connection->peer, QString::fromUtf8(command).left(80)));
		return SSH_ERROR;
	}
	if (!bssh_helper_start(program, arguments, &served->helper)) {
		emit connection->owner->logged(QStringLiteral("%1: the session helper would not start")
		                               .arg(connection->peer));
		return SSH_ERROR;
	}
	served->arguments = arguments;
	served->kind = served_channel_t::HELPER;
	connection->requested = true;
	if (arguments.at(1) == "attach") {
		emit connection->owner->logged(QStringLiteral("%1: %2 attached to session %3")
		                               .arg(connection->peer, connection->user,
		                                    QString::fromUtf8(arguments.at(2))));
	}
	return SSH_OK;
}

} /* namespace */
