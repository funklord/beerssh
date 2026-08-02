#include "ssh/ssh_session_libssh.h"

#include <QDir>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>

#include <libssh/libssh.h>

/*
 * Everything below the facade runs on the worker thread. Nothing here
 * touches a widget, and nothing outside this file includes libssh.h.
 */
namespace {

/*
 * TERM for the PTY request. Hardcoded at M1; it becomes a preset-owned
 * setting at M10 (project.md sec 10.2), which is where a BusyBox router
 * that only understands "vt100" gets its own answer.
 */
const char *const BSSH_TERM = "xterm-256color";

/*
 * How often to look for data libssh has already buffered but the socket
 * notifier will never announce, because the bytes arrived with a packet
 * that also carried something else.
 *
 * A poll is not the end state: it wakes the CPU on an idle session, which
 * is exactly the wrong behaviour on a phone with the screen off (sec 5.1).
 * M5 replaces it with libssh's own event loop integration when the
 * keepalive and reconnection work lands. It is here now because a missed
 * wakeup shows up as a terminal that renders half a prompt and then sits
 * there, which is far harder to diagnose than a timer is to delete.
 */
const int BSSH_POLL_INTERVAL_MS = 25;

QString known_hosts_path() {
	/*
	 * Our own store, deliberately NOT ~/.ssh/known_hosts (sec 11).
	 * Sharing that file with OpenSSH means two tools with two different
	 * trust prompts writing one database -- and an entry silently
	 * accepted by one becomes an entry the other never asks about.
	 */
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	QDir().mkpath(dir);
	return dir + QStringLiteral("/known_hosts");
}

} /* namespace */

class bssh_libssh_worker : public QObject {
	Q_OBJECT

public:
	bssh_libssh_worker() : m_session(nullptr), m_channel(nullptr), m_notifier(nullptr), m_poll(nullptr) {
	}

	~bssh_libssh_worker() override {
		teardown();
	}

public slots:
	void do_connect(const QString &host, quint16 port, const QString &user);
	void do_authenticate_password(const QString &password);
	void do_open_shell(int cols, int rows);
	void do_resize(int cols, int rows);
	void do_send(const QByteArray &bytes);
	void do_disconnect();
	void drain();

signals:
	/*
	 * int, not state_t, so the queued connection across the thread
	 * boundary needs no metatype registration for an enum that only
	 * exists to be displayed. The facade maps it back.
	 */
	void state_changed(int state);
	void bytes_received(const QByteArray &bytes);
	void failed(const QString &reason);
	void exited(int exit_status);
	void password_required();

private:
	void teardown();
	void fail(const QString &reason);
	QString session_error() const;
	bool verify_host_key();

	ssh_session m_session;
	ssh_channel m_channel;
	QSocketNotifier *m_notifier;
	QTimer *m_poll;
};

QString bssh_libssh_worker::session_error() const {
	if (!m_session) return QStringLiteral("no session");
	return QString::fromUtf8(ssh_get_error(m_session));
}

void bssh_libssh_worker::fail(const QString &reason) {
	teardown();
	emit state_changed(bssh_ssh_session::FAILED);
	emit failed(reason);
}

void bssh_libssh_worker::teardown() {
	delete m_notifier;
	m_notifier = nullptr;
	delete m_poll;
	m_poll = nullptr;

	if (m_channel) {
		if (ssh_channel_is_open(m_channel)) ssh_channel_close(m_channel);
		ssh_channel_free(m_channel);
		m_channel = nullptr;
	}
	if (m_session) {
		ssh_disconnect(m_session);
		ssh_free(m_session);
		m_session = nullptr;
	}
}

bool bssh_libssh_worker::verify_host_key() {
	const enum ssh_known_hosts_e known = ssh_session_is_known_server(m_session);
	if (known == SSH_KNOWN_HOSTS_OK) return true;

	/*
	 * Never silently accept (sec 11). At M1 there is no UI to ask
	 * through, so an unknown or changed key is a hard failure that names
	 * the fingerprint -- the user can compare it and add the entry
	 * themselves. M6 turns this into the trust-on-first-use prompt and
	 * the key-changed warning, which is the same decision with a dialog
	 * in front of it, not a different policy.
	 */
	QString fingerprint = QStringLiteral("<unavailable>");
	ssh_key key = nullptr;
	if (ssh_get_server_publickey(m_session, &key) == SSH_OK) {
		unsigned char *hash = nullptr;
		size_t hash_len = 0;
		if (ssh_get_publickey_hash(key, SSH_PUBLICKEY_HASH_SHA256, &hash, &hash_len) == 0) {
			char *text = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hash_len);
			if (text) {
				fingerprint = QString::fromUtf8(text);
				ssh_string_free_char(text);
			}
			ssh_clean_pubkey_hash(&hash);
		}
		ssh_key_free(key);
	}

	if (known == SSH_KNOWN_HOSTS_CHANGED) {
		fail(QStringLiteral("HOST KEY CHANGED -- possible man-in-the-middle. Server key is %1. "
		                     "Not connecting.").arg(fingerprint));
	} else {
		fail(QStringLiteral("Host key is not known: %1. Add it to %2 to continue "
		                     "(the trust prompt arrives at M6).")
		         .arg(fingerprint, known_hosts_path()));
	}
	return false;
}

void bssh_libssh_worker::do_connect(const QString &host, quint16 port, const QString &user) {
	teardown();

	m_session = ssh_new();
	if (!m_session) {
		fail(QStringLiteral("ssh_new() failed"));
		return;
	}

	const QByteArray host_utf8 = host.toUtf8();
	const QByteArray user_utf8 = user.toUtf8();
	const QByteArray known_utf8 = known_hosts_path().toUtf8();
	int port_value = port;

	ssh_options_set(m_session, SSH_OPTIONS_HOST, host_utf8.constData());
	ssh_options_set(m_session, SSH_OPTIONS_PORT, &port_value);
	ssh_options_set(m_session, SSH_OPTIONS_USER, user_utf8.constData());
	ssh_options_set(m_session, SSH_OPTIONS_KNOWNHOSTS, known_utf8.constData());

	emit state_changed(bssh_ssh_session::RESOLVING);
	emit state_changed(bssh_ssh_session::CONNECTING);

	if (ssh_connect(m_session) != SSH_OK) {
		fail(session_error());
		return;
	}

	if (!verify_host_key()) return;

	emit state_changed(bssh_ssh_session::AUTHENTICATING);

	/*
	 * Keys first, and this is the ordering every SSH client uses:
	 * ssh_userauth_publickey_auto tries the agent and then the default
	 * identities (~/.ssh/id_ed25519 and friends). Most people have one,
	 * and asking for a password before trying is asking for a credential
	 * that was never needed.
	 *
	 * It also removes the reason M5 had to keep the password in memory
	 * for the life of the session: a key-authenticated session can
	 * reconnect on its own after a network drop, with nothing held.
	 *
	 * A passphrase-protected key with no agent loaded is NOT handled here
	 * -- passing nullptr means "unencrypted keys and the agent only", and
	 * a passphrase prompt is its own piece of UI. Such a key simply falls
	 * through to the password path rather than silently failing.
	 */
	const int key_rc = ssh_userauth_publickey_auto(m_session, nullptr, nullptr);
	if (key_rc == SSH_AUTH_SUCCESS) {
		emit state_changed(bssh_ssh_session::CONNECTED);
		return;
	}

	emit password_required();
}

void bssh_libssh_worker::do_authenticate_password(const QString &password) {
	if (!m_session) {
		fail(QStringLiteral("not connected"));
		return;
	}

	const QByteArray password_utf8 = password.toUtf8();
	const int rc = ssh_userauth_password(m_session, nullptr, password_utf8.constData());
	if (rc != SSH_AUTH_SUCCESS) {
		fail(QStringLiteral("Authentication failed"));
		return;
	}

	emit state_changed(bssh_ssh_session::CONNECTED);
}

void bssh_libssh_worker::do_open_shell(int cols, int rows) {
	if (!m_session) {
		fail(QStringLiteral("not connected"));
		return;
	}

	m_channel = ssh_channel_new(m_session);
	if (!m_channel) {
		fail(session_error());
		return;
	}
	if (ssh_channel_open_session(m_channel) != SSH_OK) {
		fail(session_error());
		return;
	}
	/*
	 * PTY over the channel -- there is no local pty anywhere in this,
	 * on either platform (sec 3, core principle 1). The remote allocates
	 * it; this side only ever says how big the grid is.
	 */
	if (ssh_channel_request_pty_size(m_channel, BSSH_TERM, cols, rows) != SSH_OK) {
		fail(session_error());
		return;
	}
	if (ssh_channel_request_shell(m_channel) != SSH_OK) {
		fail(session_error());
		return;
	}

	m_notifier = new QSocketNotifier(ssh_get_fd(m_session), QSocketNotifier::Read, this);
	connect(m_notifier, &QSocketNotifier::activated, this, &bssh_libssh_worker::drain);

	m_poll = new QTimer(this);
	m_poll->setInterval(BSSH_POLL_INTERVAL_MS);
	connect(m_poll, &QTimer::timeout, this, &bssh_libssh_worker::drain);
	m_poll->start();

	drain();
}

void bssh_libssh_worker::drain() {
	if (!m_channel) return;

	char buffer[4096];

	/*
	 * Both streams, and stderr is not optional: a remote program writing
	 * diagnostics there would otherwise vanish, which reads as the
	 * terminal losing output rather than as a missing feature. The
	 * emulator merges them into one screen, exactly as a real terminal
	 * does.
	 */
	for (int is_stderr = 0; is_stderr <= 1; ++is_stderr) {
		for (;;) {
			const int n = ssh_channel_read_nonblocking(m_channel, buffer, sizeof(buffer), is_stderr);
			if (n == SSH_ERROR) {
				fail(session_error());
				return;
			}
			if (n <= 0) break;
			emit bytes_received(QByteArray(buffer, n));
		}
	}

	if (ssh_channel_is_eof(m_channel)) {
		/*
		 * The remote shell exited. Reported as its own thing rather than
		 * as a plain disconnect: this function knew it was "a clean end,
		 * not a failure" and kept that to itself, so the reconnection
		 * machine saw only a dead session and reopened the shell the
		 * user had just exited -- endlessly.
		 */
		uint32_t exit_code = 0;
		char *exit_signal = nullptr;
		int core_dumped = 0;
		ssh_channel_get_exit_state(m_channel, &exit_code, &exit_signal, &core_dumped);
		if (exit_signal) ssh_string_free_char(exit_signal);

		emit exited(int(exit_code));

		teardown();
		emit state_changed(bssh_ssh_session::DISCONNECTED);
	}
}

void bssh_libssh_worker::do_resize(int cols, int rows) {
	if (!m_channel) return;
	ssh_channel_change_pty_size(m_channel, cols, rows);
}

void bssh_libssh_worker::do_send(const QByteArray &bytes) {
	if (!m_channel || bytes.isEmpty()) return;
	ssh_channel_write(m_channel, bytes.constData(), bytes.size());
}

void bssh_libssh_worker::do_disconnect() {
	teardown();
	emit state_changed(bssh_ssh_session::DISCONNECTED);
}

/* ---- facade, GUI thread ------------------------------------------- */

bssh_ssh_session_libssh::bssh_ssh_session_libssh(QObject *parent)
    : bssh_ssh_session(parent),
      m_worker(new bssh_libssh_worker) {
	m_worker->moveToThread(&m_thread);

	connect(m_worker, &bssh_libssh_worker::bytes_received,
	         this, &bssh_ssh_session::bytes_received);
	connect(m_worker, &bssh_libssh_worker::failed,
	         this, &bssh_ssh_session::failed);
	connect(m_worker, &bssh_libssh_worker::exited,
	         this, &bssh_ssh_session::shell_exited);
	connect(m_worker, &bssh_libssh_worker::password_required,
	         this, &bssh_ssh_session::password_required);
	connect(m_worker, &bssh_libssh_worker::state_changed, this, [this](int state) {
		set_state(static_cast<state_t>(state));
	});
	connect(&m_thread, &QThread::finished, m_worker, &QObject::deleteLater);

	m_thread.start();
}

bssh_ssh_session_libssh::~bssh_ssh_session_libssh() {
	/*
	 * Blocking-queued, not queued: the worker must finish tearing the
	 * libssh session down before the thread's event loop stops, or the
	 * socket and the session leak for the life of the process.
	 */
	QMetaObject::invokeMethod(m_worker, "do_disconnect", Qt::BlockingQueuedConnection);
	m_thread.quit();
	m_thread.wait();
}

void bssh_ssh_session_libssh::connect_to_host(const QString &host, quint16 port, const QString &user) {
	QMetaObject::invokeMethod(m_worker, [this, host, port, user] {
		m_worker->do_connect(host, port, user);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::authenticate_password(const QString &password) {
	QMetaObject::invokeMethod(m_worker, [this, password] {
		m_worker->do_authenticate_password(password);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::open_shell(int cols, int rows) {
	QMetaObject::invokeMethod(m_worker, [this, cols, rows] {
		m_worker->do_open_shell(cols, rows);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::resize(int cols, int rows) {
	QMetaObject::invokeMethod(m_worker, [this, cols, rows] {
		m_worker->do_resize(cols, rows);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::send(const QByteArray &bytes) {
	QMetaObject::invokeMethod(m_worker, [this, bytes] {
		m_worker->do_send(bytes);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::disconnect_from_host() {
	QMetaObject::invokeMethod(m_worker, "do_disconnect", Qt::QueuedConnection);
}

bssh_ssh_session *bssh_ssh_session::create(QObject *parent) {
	return new bssh_ssh_session_libssh(parent);
}

#include "ssh_session_libssh.moc"
