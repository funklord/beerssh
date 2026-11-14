#include "ssh/ssh_session_libssh.h"

#include "ssh/jump_tunnel.h"
#ifndef _WIN32
#include "ssh/pty_session.h"
#endif

#include "platform/platform.h"
#include "model/piece_map.h"

#include <QDataStream>
#include <QDir>
#include <QIODevice>
#include <QFileInfo>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <QFile>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QTimer>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include <libssh/libssh.h>
#include <libssh/sftp.h>

#include <QDirIterator>
#include <fcntl.h>
#include <memory>

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

/*
 * TCP-level dead-peer detection (sec 4, sec 5.3).
 *
 * The application-level keepalive below keeps the NAT mapping alive; it
 * cannot notice a peer that has gone away, because SSH_MSG_IGNORE is
 * never answered and a write into a dead socket succeeds into the kernel
 * buffer. What DOES notice is TCP keepalive, so the socket is configured
 * to probe rather than left at the system default of two hours -- which
 * is a session that stays "connected" for the rest of the afternoon.
 *
 * Chosen to fail in about a minute: idle 30s, then probes 10s apart, three
 * of them. Fast enough that a user reaching for a dead terminal sees it
 * reconnecting rather than typing into nothing, slow enough not to be a
 * radio wakeup every few seconds on a phone (sec 5.1).
 */
const int BSSH_TCP_KEEPALIVE_IDLE_S = 30;
const int BSSH_TCP_KEEPALIVE_INTERVAL_S = 10;
const int BSSH_TCP_KEEPALIVE_COUNT = 3;


/*
 * Whether the user has a private key at all.
 *
 * libssh does not say WHY publickey_auto failed -- no key, wrong key, or a
 * key it could not decrypt all look the same from here. This is the
 * heuristic that distinguishes "ask for the passphrase" from "ask for the
 * password": if a default identity exists and unaided key auth failed, a
 * passphrase is the likely missing piece. Guessing wrong costs one
 * dismissable prompt, and never guessing costs every user with an
 * encrypted key and no agent.
 */
bool default_identity_exists() {
	const QString home = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
	const QStringList names = {QStringLiteral("id_ed25519"), QStringLiteral("id_ecdsa"),
		                          QStringLiteral("id_rsa"), QStringLiteral("id_dsa")};

	for (const QString &name : names) {
		if (QFile::exists(home + QStringLiteral("/.ssh/") + name)) return true;
	}
	return false;
}

QString known_hosts_path() {
	/*
	 * Where this app WRITES trust decisions -- its own file, never the
	 * user's (sec 11).
	 *
	 * Not because their trust is different from ours: it is the same
	 * person deciding about the same hosts. It is because of what writing
	 * would do to their file. Debian and Ubuntu ship HashKnownHosts yes,
	 * so a real ~/.ssh/known_hosts is entirely hashed -- measured on this
	 * machine, 12 entries out of 12 -- and that hashing exists so a
	 * leaked copy does not disclose which machines someone connects to.
	 * libssh writes entries UNHASHED. Appending to their file would
	 * silently undo that setting for exactly the hosts they used this app
	 * for, and they would never see it happen.
	 */
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	QDir().mkpath(dir);
	return dir + QStringLiteral("/known_hosts");
}

/*
 * The user's OpenSSH known_hosts, consulted READ-ONLY.
 *
 * Reading it is the half that was wrong to refuse. It is the same person
 * who verified those fingerprints, and not looking at them costs twice:
 * a host they have already checked gets asked about again, which is
 * exactly the prompt-fatigue that makes people stop reading prompts; and
 * worse, a host key that CHANGED reads as a first contact here rather
 * than as the warning OpenSSH would give, because we had no previous key
 * to compare against. Refusing to look does not make the app safer than
 * OpenSSH -- it makes it quieter about the one thing worth shouting
 * about.
 *
 * libssh's "global" slot is exactly this: consulted during verification,
 * never written to. Its hashed-hostname support is real -- checked
 * against a file OpenSSH itself wrote, which libssh recognised.
 *
 * On Android there is no ~/.ssh and the path simply does not exist, which
 * libssh treats as an empty file. That is why this needs no platform
 * branch.
 */
/*
 * The file this app both reads and writes: the user's, when it can be
 * reached, and its own otherwise.
 *
 * One decision rather than two, because the two must agree. If libssh is
 * pointed at the user's file while the accept path writes to ours, a host
 * is recorded in one place and looked for in the other -- and the second
 * connection asks again about a host that was just accepted.
 */
QString openssh_known_hosts_path() {
	const bssh_platform *platform = bssh_platform::instance();
	const QString dir = platform ? platform->openssh_directory() : QString();
	if (dir.isEmpty()) return QString();
	return dir + QStringLiteral("/known_hosts");
}

} /* namespace */

/*
 * Set the socket options that make a dead connection announce itself.
 *
 * The #ifdefs here are about which POSIX socket options this KERNEL has,
 * not about which platform the app is running on, so they do not belong
 * behind bssh_platform (sec 3): Android and desktop Linux take exactly
 * the same branch, and it is macOS and Windows that differ. Every one of
 * them is best-effort -- a failure means slower detection, never a
 * connection that does not work, so none of them is checked. The value
 * goes as const char *, which Winsock's setsockopt takes and POSIX's
 * const void * accepts.
 */
void bssh_configure_tcp_keepalive(int fd) {
	if (fd < 0) return;

	const int on = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char *>(&on), sizeof(on));

#ifdef TCP_KEEPIDLE
	const int idle = BSSH_TCP_KEEPALIVE_IDLE_S;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, reinterpret_cast<const char *>(&idle), sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
	const int interval = BSSH_TCP_KEEPALIVE_INTERVAL_S;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, reinterpret_cast<const char *>(&interval), sizeof(interval));
#endif
#ifdef TCP_KEEPCNT
	const int count = BSSH_TCP_KEEPALIVE_COUNT;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, reinterpret_cast<const char *>(&count), sizeof(count));
#endif
#ifdef TCP_USER_TIMEOUT
	/*
	 * The one that matters when the user is actually typing: it bounds
	 * how long UNACKNOWLEDGED data may sit before the socket errors.
	 * Without it, keystrokes sent into a dead link are retransmitted for
	 * many minutes while the terminal looks merely slow.
	 */
	const int user_timeout_ms =
	        (BSSH_TCP_KEEPALIVE_IDLE_S +
	         BSSH_TCP_KEEPALIVE_INTERVAL_S * BSSH_TCP_KEEPALIVE_COUNT) * 1000;
	setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, reinterpret_cast<const char *>(&user_timeout_ms), sizeof(user_timeout_ms));
#endif
}

class bssh_libssh_worker : public QObject {
	Q_OBJECT

public:
	bssh_libssh_worker() : m_session(nullptr), m_channel(nullptr), m_notifier(nullptr), m_poll(nullptr),
	                       m_keepalive(nullptr), m_keepalive_seconds(0),
	                       m_term(QString::fromLatin1(BSSH_TERM)), m_compression(false) {
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
	void do_accept_host_key();
	void do_key_passphrase(const QString &passphrase);
	void do_set_keepalive(int seconds);
	void do_set_term(const QString &term);
	void do_set_command(const QStringList &command);
	void do_set_raw_channel(bool raw) { m_raw_channel = raw; }
	void do_run_on_the_side(const QString &command);
	void do_find_working_directory();
	void do_upload(const QStringList &paths, const QString &directory);
	void do_cancel_upload();
	void drop_partial_upload();
	void do_list_directory(const QString &path);
	void do_download(const QStringList &paths, const QString &directory,
	                 std::shared_ptr<bssh_piece_map> pieces = nullptr);
	void do_cancel_download();
	void do_pause_upload(bool paused);
	void do_pause_download(bool paused);
	void drop_partial_download();
	void do_change_remote(int change, const QString &path, const QString &to);
	void do_restart_shell(const QStringList &command);
	void do_set_compression(bool compression);
	void do_set_identity_file(const QString &path);
	void do_set_proxy_jump(const QString &spec);
	void apply_identity_file();
	void drain();

	/* Moving the connection to another process (sec 8.4); see the
	 * interface. Run on this thread through blocking calls. */
	bool do_export(QByteArray &state, QList<int> &fds, QString &why);
	void do_release();
	void do_resume();
	bool do_import(const QByteArray &state, const QList<int> &fds, QString &why);

	/* See do_send: what the channel window could not take yet. */
	void flush_pending();

signals:
	/*
	 * int, not state_t, so the queued connection across the thread
	 * boundary needs no metatype registration for an enum that only
	 * exists to be displayed. The facade maps it back.
	 */
	void state_changed(int state);
	void bytes_received(const QByteArray &bytes);
	void side_output_ready(const QByteArray &output);
	void working_directory_found(const bssh_working_directory_t &found);
	void upload_progress(qint64 done, qint64 total);
	void upload_finished(bool ok, const QString &directory, const QStringList &written,
	                     const QString &why);
	void directory_listed(const QString &path, const QList<bssh_remote_entry_t> &entries,
	                      const QString &why);
	void download_progress(qint64 done, qint64 total);
	void download_finished(bool ok, const QString &directory, const QStringList &written,
	                       const QString &why);
	void remote_changed(bool ok, const QString &path, const QString &why);
	void failed(const QString &reason);
	void exited(int exit_status);
	void password_required();
	void password_rejected();
	void key_passphrase_required();
	void host_key_unknown(const QString &fingerprint);
	void host_key_changed(const QString &fingerprint);
	void host_key_revoked(const QString &fingerprint);

private:
	bool m_raw_channel = false;
	void teardown();
	void fail(const QString &reason);
	bool set_option(int option, const void *value, const char *what);
	QString session_error() const;
	void start_polling();
	bool verify_host_key();
	bool server_key_is_revoked() const;
	QStringList known_hosts_files() const;
	QString server_fingerprint() const;
	void authenticate();

	ssh_session m_session;
	ssh_channel m_channel;
	QSocketNotifier *m_notifier;
	QTimer *m_poll;

	/*
	 * Sends SSH_MSG_IGNORE on a timer. Kept alongside the interval so a
	 * preset change mid-session retimes an existing connection rather
	 * than only affecting the next one.
	 */
	QTimer *m_keepalive;
	int m_keepalive_seconds;

	/*
	 * Output the remote's channel window had no room for. Held here
	 * rather than waited on -- see do_send.
	 */
	QByteArray m_pending;

	/* Applied at connect time; see set_term's comment on the interface. */
	QString m_term;

	/* Empty asks for a shell; anything else is an exec request (sec 8.6). */
	QString m_command;

	/*
	 * run_on_the_side()'s channel, read by drain() beside the shell's and
	 * answered once it ends, overflows or runs out of time.
	 */
	ssh_channel m_side_channel = nullptr;

	/* The grid the shell was opened or last resized to, for a restart. */
	int m_cols = 80;
	int m_rows = 24;
	QByteArray m_side_output;
	QElapsedTimer m_side_started;
	void drain_side();
	void finish_side();

	/* find_working_directory()'s probe: its own channel, so it never waits
	 * on the tmux listing, read and answered the same way. */
	ssh_channel m_probe_channel = nullptr;
	QByteArray m_probe_output;
	QElapsedTimer m_probe_started;
	void drain_probe();
	void finish_probe();

	/*
	 * upload(): one copy at a time, a piece per turn of this thread's
	 * event loop, so the shell's channel is drained between pieces and a
	 * large file does not freeze the terminal it was dropped on.
	 */
	struct upload_item {
		QString local;
		QString remote;
		bool directory = false;
	};
	struct upload_job {
		sftp_session sftp = nullptr;
		sftp_file file = nullptr;
		QFile source;
		QList<upload_item> items;
		int at = 0;
		qint64 done = 0;
		qint64 total = 0;
		QString directory;
		QStringList written;
		QElapsedTimer since_progress;
		bool paused = false;
		bool scheduled = false;   /* a step is queued: never two at once */
	};
	std::unique_ptr<upload_job> m_upload;
	void schedule_upload();
	void step_upload();
	void finish_upload(bool ok, const QString &why);
	QString sftp_error() const;
	QString sftp_error_of(sftp_session sftp) const;

	/*
	 * list_directory(): the file panel's listing (sec 6.4), an SFTP
	 * session of its own opened for it and freed after, so nothing is held
	 * open between listings -- an open one would refuse the move of a kept
	 * session to another process (export_transport). A batch of entries
	 * per turn, the shell's channel drained between them.
	 */
	struct list_job {
		sftp_session sftp = nullptr;
		sftp_dir dir = nullptr;
		QString path;
		QList<bssh_remote_entry_t> entries;
	};
	std::unique_ptr<list_job> m_list;
	QList<QString> m_list_queue;
	void step_list();
	void finish_list(const QString &why);

	/*
	 * download(): the upload's mirror. A directory is listed at its own
	 * turn and its contents put after it, so a large tree is never walked
	 * in one go with the shell waiting; the total grows as it is found.
	 */
	struct download_item {
		QString remote;
		QString local;
		bool directory = false;
		qint64 size = 0;
	};
	struct download_job {
		sftp_session sftp = nullptr;
		sftp_file file = nullptr;
		/* The directory being read, and what it held so far. */
		sftp_dir dir = nullptr;
		QList<download_item> inside;
		QFile target;
		QList<download_item> items;
		int at = 0;
		qint64 done = 0;
		qint64 total = 0;
		QString directory;
		QStringList written;
		QElapsedTimer since_progress;
		bool paused = false;
		bool scheduled = false;
		/*
		 * One file filled in the order its reader asks (sec 6.4, *Playing
		 * a file*): the piece wanted first, then on, then back for the
		 * gaps. `position` is where the remote file's offset stands, so a
		 * seek is asked only when the order jumps.
		 */
		std::shared_ptr<bssh_piece_map> pieces;
		qint64 position = 0;
	};
	std::unique_ptr<download_job> m_download;
	void schedule_download();
	void step_download();
	void finish_download(bool ok, const QString &why);
	void step_pieces();

	/*
	 * change_remote()'s removal of a tree: directories expanded at their
	 * own turn, everything removed deepest first, a batch per turn.
	 */
	struct removal_item {
		QString path;
		bool directory = false;
		bool expanded = false;
	};
	struct removal_job {
		sftp_session sftp = nullptr;
		QString path;
		QList<removal_item> stack;
	};
	std::unique_ptr<removal_job> m_removal;
	void step_removal();
	void finish_removal(const QString &why);

	/* Empty means the default identities; see set_identity_file. */
	QString m_identity_file;

	/*
	 * The destination as connected to. Kept because a revocation is filed
	 * under the host's NAME, and by the time the key is checked libssh
	 * only offers the key.
	 */
	QString m_host;
	quint16 m_port = 22;

	/* ProxyJump, and the tunnel it needs while a session is up. */
	QString m_proxy_jump;
	bssh_jump_tunnel *m_jump = nullptr;
	bool m_compression;
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
	/*
	 * The tunnel goes after the session that was using it: the target's
	 * descriptor is one end of its socket pair, and freeing the pump
	 * first would pull the floor out from under ssh_disconnect.
	 */

	delete m_notifier;
	m_notifier = nullptr;
	delete m_poll;
	m_poll = nullptr;
	delete m_keepalive;
	m_keepalive = nullptr;

	/* Nothing to flush into a channel that is going. */
	m_pending.clear();

	if (m_side_channel) {
		ssh_channel_free(m_side_channel);
		m_side_channel = nullptr;
		m_side_output.clear();
	}
	if (m_probe_channel) {
		ssh_channel_free(m_probe_channel);
		m_probe_channel = nullptr;
		m_probe_output.clear();
		emit working_directory_found(bssh_working_directory_t());
	}

	/*
	 * SFTP lives on the session, so it goes first; a copy cut off says so.
	 * Where the connection is still up -- the tab closed, Disconnect -- the
	 * half-written file goes too, as a stopped copy's does; one that has
	 * already dropped has nothing left to remove it over.
	 */
	if (m_upload) {
		if (m_session && ssh_is_connected(m_session)) drop_partial_upload();
		finish_upload(false, QStringLiteral("the connection closed during the copy"));
	}
	if (m_download) {
		drop_partial_download();
		finish_download(false, QStringLiteral("the connection closed during the copy"));
	}
	if (m_removal) finish_removal(QStringLiteral("the connection closed during the removal"));
	m_list_queue.clear();
	if (m_list) finish_list(QStringLiteral("the connection closed"));

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

	if (m_jump) {
		m_jump->close();
		delete m_jump;
		m_jump = nullptr;
	}
}

QString bssh_libssh_worker::server_fingerprint() const {
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

	return fingerprint;
}

/*
 * Whether a known-hosts line marked @revoked names this server's key.
 *
 * libssh does not do this. It reports a revoked key as UNKNOWN, so
 * without this the app would offer a friendly "trust this new host?"
 * prompt for a key somebody went out of their way to revoke -- and
 * accepting would then write it back as trusted. OpenSSH refuses
 * outright, and reading the same file (sec 11) while disagreeing about
 * what a line MEANS is worse than not reading it.
 *
 * The host field may be plain or hashed, so both are compared: hashed
 * entries are the default on Debian and Ubuntu, and a revocation nobody
 * can match is a revocation that does nothing.
 */
/*
 * Every file a revocation could be recorded in, in the order they are
 * consulted. All of them are read: a revocation in any one of them is a
 * revocation, and picking only the first would let a stale trusted entry
 * in one file mask a revocation in another.
 */
QStringList bssh_libssh_worker::known_hosts_files() const {
	QStringList files;
	files << openssh_known_hosts_path();
	files << known_hosts_path();
	files << QStringLiteral("/etc/ssh/ssh_known_hosts");
	files.removeAll(QString());
	return files;
}

static QString primary_known_hosts_path() {
	const QString theirs = openssh_known_hosts_path();
	if (!theirs.isEmpty()) {
		const QFileInfo info(theirs);
		if (QDir().mkpath(info.absolutePath())) {
			/*
			 * Writable, or creatable. Checked rather than assumed: a
			 * sandboxed build may see the path and not be allowed to
			 * open it, and discovering that at accept time would lose
			 * the decision.
			 */
			QFile probe(theirs);
			if (probe.open(QIODevice::Append | QIODevice::WriteOnly)) {
				probe.close();
				return theirs;
			}
		}
	}
	return known_hosts_path();
}

static bool bssh_host_field_matches(const QByteArray &field, const QList<QByteArray> &names) {
	if (!field.startsWith("|1|")) {
		/* Plain, possibly a comma-separated list of names. */
		for (const QByteArray &candidate : field.split(',')) {
			if (names.contains(candidate)) return true;
		}
		return false;
	}

	const QList<QByteArray> parts = field.split('|');
	if (parts.size() < 4) return false;

	const QByteArray salt = QByteArray::fromBase64(parts.at(2));
	const QByteArray want = QByteArray::fromBase64(parts.at(3));
	if (salt.isEmpty() || want.isEmpty()) return false;

	for (const QByteArray &name : names) {
		unsigned char mac[EVP_MAX_MD_SIZE];
		unsigned int mac_len = 0;
		if (!HMAC(EVP_sha1(), salt.constData(), int(salt.size()),
		           reinterpret_cast<const unsigned char *>(name.constData()),
		           size_t(name.size()), mac, &mac_len)) {
			continue;
		}
		if (QByteArray(reinterpret_cast<const char *>(mac), int(mac_len)) == want) return true;
	}
	return false;
}

bool bssh_libssh_worker::server_key_is_revoked() const {
	if (!m_session) return false;

	ssh_key key = nullptr;
	if (ssh_get_server_publickey(m_session, &key) != SSH_OK || !key) return false;

	char *base64 = nullptr;
	const int rc = ssh_pki_export_pubkey_base64(key, &base64);
	const QByteArray type = QByteArray(ssh_key_type_to_char(ssh_key_type(key)));
	ssh_key_free(key);

	if (rc != SSH_OK || !base64) return false;
	const QByteArray blob = QByteArray(base64).trimmed();
	ssh_string_free_char(base64);

	/*
	 * The two spellings OpenSSH uses for a host: bare when the port is
	 * 22, bracketed with the port otherwise. A revocation filed under one
	 * must not be missed because the connection was described with the
	 * other.
	 */
	const QByteArray host = m_host.toUtf8();
	QList<QByteArray> names;
	names << host;
	names << '[' + host + "]:" + QByteArray::number(m_port);
	if (m_port == 22) names << host;

	for (const QString &path : known_hosts_files()) {
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

		while (!file.atEnd()) {
			const QByteArray line = file.readLine().trimmed();
			if (!line.startsWith("@revoked")) continue;

			const QList<QByteArray> fields =
			        line.split(' ');
			QList<QByteArray> kept;
			for (const QByteArray &field : fields) {
				if (!field.isEmpty()) kept << field;
			}
			/* @revoked <host> <keytype> <key> */
			if (kept.size() < 4) continue;
			if (kept.at(2) != type) continue;
			if (kept.at(3) != blob) continue;
			if (!bssh_host_field_matches(kept.at(1), names)) continue;

			file.close();
			return true;
		}
		file.close();
	}
	return false;
}

bssh_host_key_verdict_t bssh_host_key_verdict(int known_hosts_result) {
	/*
	 * Switched as an int rather than as the enum, deliberately. Casting a
	 * value libssh never defined into the enum in order to switch on it
	 * is undefined behaviour, and coping with exactly that value is what
	 * this function is for. The cost is that -Wswitch can no longer warn
	 * about an enumerator nobody handled; the default below is what
	 * covers that, and it refuses rather than trusts.
	 */
	switch (known_hosts_result) {
	case SSH_KNOWN_HOSTS_OK:
		return BSSH_HOST_KEY_TRUSTED;

	/*
	 * Not recorded, which is an ordinary first connection. NOT_FOUND is
	 * the same answer arrived at differently -- there is no known-hosts
	 * file yet -- and libssh says so itself: "the host is thus unknown".
	 */
	case SSH_KNOWN_HOSTS_UNKNOWN:
	case SSH_KNOWN_HOSTS_NOT_FOUND:
		return BSSH_HOST_KEY_FIRST_CONTACT;

	/*
	 * Both of these are libssh's "possible attack", and they were not
	 * treated alike. CHANGED is the famous one. OTHER -- "the server
	 * gave us a key of a type while we had an other type recorded" --
	 * fell through to the first-contact prompt, which offers "Accept and
	 * connect" for something libssh documents as an attack. The warning
	 * dialog's own words fit it exactly: the key is NOT the one recorded.
	 */
	case SSH_KNOWN_HOSTS_CHANGED:
	case SSH_KNOWN_HOSTS_OTHER:
		return BSSH_HOST_KEY_MISMATCH;

	/*
	 * The check did not run -- an unreadable or malformed known-hosts
	 * file, say. This also fell through to the first-contact prompt,
	 * which told the user the host was not recorded and invited them to
	 * trust it. "I could not tell" and "it is not there" are different
	 * answers, and rendering them identically is the vacuous pass at its
	 * most expensive: a trust decision taken on a verification that
	 * never happened.
	 */
	case SSH_KNOWN_HOSTS_ERROR:
		return BSSH_HOST_KEY_UNVERIFIED;
	}

	/*
	 * A value this libssh has and this code does not know. Refused
	 * rather than trusted, because the safe default for an unrecognised
	 * verdict about a host key is not to proceed.
	 */
	return BSSH_HOST_KEY_UNVERIFIED;
}

/*
 * How long any BLOCKING libssh call may take before it gives up.
 *
 * Nothing set this, so the ceiling was libssh's own: about ten seconds
 * for ssh_connect, and for a blocking session established afterwards,
 * SSH_TIMEOUT_DEFAULT resolves to no limit at all. That reaches the GUI
 * thread. The facade's destructor asks the worker to disconnect over a
 * BlockingQueuedConnection, which waits on the worker's EVENT LOOP -- and
 * that loop is not running while the worker sits inside libssh. So
 * closing a tab that is still connecting, or still authenticating,
 * freezes the window for as long as the call takes.
 *
 * That is the symptom the worker thread exists to prevent, in this
 * class's own words: "a UI that freezes for the length of a TCP handshake
 * to an unreachable host is a UI that looks crashed -- which on Android
 * is also a UI the system may kill for not responding."
 *
 * Ten seconds, which is what libssh already applies to a connect. The
 * point is not to shorten that -- it is that every OTHER blocking call
 * now has the same ceiling instead of none, and that the number is this
 * program's to choose rather than the library's to assume. Lower would
 * start refusing slow key exchanges on a loaded server, which is a
 * working connection turned into a failed one.
 */
const long BSSH_SSH_TIMEOUT_S = 10;

bool bssh_libssh_worker::set_option(int option, const void *value, const char *what) {
	if (ssh_options_set(m_session, ssh_options_e(option), value) == SSH_OK) return true;

	/*
	 * Loud, because these options decide WHAT the connection is rather
	 * than how it behaves. The one that made this necessary is
	 * SSH_OPTIONS_FD: it hands libssh the bastion tunnel's descriptor,
	 * and SSH_OPTIONS_HOST is deliberately the TARGET's name so the host
	 * key is checked against the right machine. So a discarded failure
	 * there does not fail to connect -- ssh_connect opens its own socket
	 * to the target and succeeds, the host key verifies, and the user
	 * gets an ordinary-looking session that never went through the jump
	 * host they asked for, with the tunnel still open beside it pumping
	 * nothing.
	 *
	 * The known-hosts paths are the same shape: unset, libssh falls back
	 * to its own defaults, so the app would verify against a different
	 * store than the one it believes it is using.
	 *
	 * Compression and the identity file are deliberately NOT routed
	 * through this. Losing either degrades the session without changing
	 * which machine it reaches or what its key is checked against, and a
	 * connection refused over a compression flag would be the worse
	 * outcome.
	 */
	fail(QStringLiteral("could not set %1: %2")
	             .arg(QString::fromLatin1(what), session_error()));
	return false;
}

bssh_auth_outcome_t bssh_auth_outcome(int auth_result) {
	/* An int rather than the enum, for the reason bssh_host_key_verdict
	 * gives: a value libssh never defined must not be cast into it. */
	switch (auth_result) {
	case SSH_AUTH_SUCCESS:
		return BSSH_AUTH_ACCEPTED;

	case SSH_AUTH_DENIED:
		return BSSH_AUTH_REJECTED;

	/*
	 * The credential WAS accepted and the server wants another method --
	 * `AuthenticationMethods password,keyboard-interactive` and friends.
	 * Reported as "Authentication failed", which is false in the way that
	 * costs the most: the user retypes a password that was right.
	 *
	 * The key paths already fell through to the next prompt on this,
	 * which is the correct move and was an accident of testing only for
	 * SUCCESS. Naming it makes both deliberate.
	 */
	case SSH_AUTH_PARTIAL:
		return BSSH_AUTH_NEEDS_MORE;

	/*
	 * No verdict on the credential. ERROR is a transport or protocol
	 * fault; INFO and AGAIN cannot occur here at all -- INFO belongs to
	 * keyboard-interactive and AGAIN to a non-blocking session, and every
	 * libssh call in this file is blocking (see the header). If one
	 * arrives anyway, the safe reading is that nothing was decided rather
	 * than that the password was wrong.
	 */
	case SSH_AUTH_ERROR:
	case SSH_AUTH_INFO:
	case SSH_AUTH_AGAIN:
		return BSSH_AUTH_ERRORED;
	}

	return BSSH_AUTH_ERRORED;
}

bool bssh_libssh_worker::verify_host_key() {
	/*
	 * Revocation first, and it outranks every other verdict including
	 * OK: a key can be both recorded as trusted and later revoked, and
	 * the revocation is the newer statement. libssh has no opinion here
	 * (see server_key_is_revoked), so this is checked before its verdict
	 * is consulted at all.
	 */
	if (server_key_is_revoked()) {
		emit host_key_revoked(server_fingerprint());
		return false;
	}

	const bssh_host_key_verdict_t verdict =
	        bssh_host_key_verdict(ssh_session_is_known_server(m_session));
	if (verdict == BSSH_HOST_KEY_TRUSTED) return true;

	/*
	 * A check that could not run is a failure, not a question. Everything
	 * below holds the connection open while a person decides; there is
	 * nothing to decide here, because nothing was verified.
	 */
	if (verdict == BSSH_HOST_KEY_UNVERIFIED) {
		fail(QStringLiteral("could not check the host key against known hosts: %1")
		             .arg(session_error()));
		return false;
	}

	/*
	 * Never accepted silently (sec 11), and never decided here: the
	 * connection is HELD -- session open, nothing torn down -- while a
	 * person answers. Failing instead, which is what this did before,
	 * meant the only way to reach a new host was to edit known_hosts by
	 * hand.
	 */
	const QString fingerprint = server_fingerprint();

	if (verdict == BSSH_HOST_KEY_MISMATCH) {
		emit host_key_changed(fingerprint);
	} else {
		emit host_key_unknown(fingerprint);
	}
	return false;
}

void bssh_libssh_worker::do_key_passphrase(const QString &passphrase) {
	if (!m_session) return;

	const QByteArray passphrase_utf8 = passphrase.toUtf8();
	const bssh_auth_outcome_t outcome = bssh_auth_outcome(
	        ssh_userauth_publickey_auto(m_session, nullptr, passphrase_utf8.constData()));

	if (outcome == BSSH_AUTH_ACCEPTED) {
		emit state_changed(bssh_ssh_session::CONNECTED);
		return;
	}

	/* Not a wrong passphrase; nothing was decided. See authenticate(). */
	if (outcome == BSSH_AUTH_ERRORED) {
		fail(session_error());
		return;
	}

	/*
	 * Wrong passphrase, or a key the server does not accept -- libssh
	 * does not distinguish, so the honest next step is the password path
	 * rather than a second guess at the passphrase.
	 */
	emit password_required();
}

/*
 * A host field in OpenSSH's hashed form: |1|<b64 salt>|<b64 HMAC-SHA1>.
 *
 * Written by hand because libssh has no option to hash what it stores --
 * it writes host names in the clear. Hashing is not decoration: Debian
 * and Ubuntu ship HashKnownHosts yes precisely so a leaked or backed-up
 * known_hosts cannot be read as a list of the machines someone connects
 * to, and appending one plaintext line is enough to give that away for
 * the host it names.
 *
 * Verified the only way that means anything: an entry hashed here is
 * accepted by OpenSSH itself with StrictHostKeyChecking=yes, which
 * prompts for nothing and so can only have matched.
 */
static QByteArray bssh_hash_host_field(const QByteArray &host) {
	unsigned char salt[20];
	if (RAND_bytes(salt, sizeof(salt)) != 1) return QByteArray();

	unsigned char mac[EVP_MAX_MD_SIZE];
	unsigned int mac_len = 0;
	if (!HMAC(EVP_sha1(), salt, int(sizeof(salt)),
	           reinterpret_cast<const unsigned char *>(host.constData()),
	           size_t(host.size()), mac, &mac_len)) {
		return QByteArray();
	}

	return QByteArrayLiteral("|1|") +
	        QByteArray(reinterpret_cast<const char *>(salt), int(sizeof(salt))).toBase64() +
	        QByteArrayLiteral("|") +
	        QByteArray(reinterpret_cast<const char *>(mac), int(mac_len)).toBase64();
}

/*
 * Appends one hashed entry to the user's ~/.ssh/known_hosts, or returns
 * false so the caller can fall back to this app's own store.
 *
 * APPEND only. Never rewrite, never remove a line, never reorder: this is
 * somebody else's file and the app's job in it is to add the one fact it
 * was told. OpenSSH adds entries the same way, which is what makes two
 * writers tolerable.
 *
 * Returns false whenever anything is not right -- no home directory, a
 * directory that cannot be made, a file that cannot be opened -- because
 * a trust decision the user made must never be lost just because their
 * ~/.ssh is somewhere this process cannot reach. Android has no ~/.ssh at
 * all, and a sandboxed desktop build may not be allowed near it.
 */
bool bssh_append_line(const QString &path, const QByteArray &line) {
	QFile file(path);
	if (!file.open(QIODevice::ReadWrite)) return false;

	/*
	 * Start on a line of our own. Append mode writes at the end, so a
	 * file whose last byte is not a newline swallows the new entry onto
	 * its last line -- which corrupts the entry already there as well as
	 * this one, in a file OpenSSH also reads.
	 *
	 * Asked of the last byte rather than assumed from who wrote it: this
	 * app always terminates what it appends, and OpenSSH does too, but a
	 * hand-edited file need not -- and a short write here would leave
	 * exactly this condition for the next append to fall into.
	 */
	QByteArray prefix;
	if (file.size() > 0) {
		if (!file.seek(file.size() - 1)) return false;
		const QByteArray last = file.read(1);
		if (last != QByteArrayLiteral("\n")) prefix = QByteArrayLiteral("\n");
	}

	if (!file.seek(file.size())) return false;

	const QByteArray payload = prefix + line + '\n';

	/*
	 * The whole line or nothing reported as success. This checked for
	 * "> 0", so a short write -- a full disk, most obviously -- was a
	 * truncated entry recorded as a trust decision made.
	 */
	if (file.write(payload) != payload.size()) return false;

	/* close() reports what write() cannot: a flush that failed. */
	return file.flush() && (file.close(), file.error() == QFileDevice::NoError);
}

static bool bssh_append_hashed_known_host(ssh_session session) {
	const QString path = primary_known_hosts_path();
	if (path.isEmpty()) return false;

	char *exported = nullptr;
	if (ssh_session_export_known_hosts_entry(session, &exported) != SSH_OK || !exported) {
		return false;
	}

	const QByteArray entry = QByteArray(exported).trimmed();
	ssh_string_free_char(exported);

	/* "host keytype key" -- only the host field is hashed. */
	const int split = entry.indexOf(' ');
	if (split <= 0) return false;

	const QByteArray hashed = bssh_hash_host_field(entry.left(split));
	if (hashed.isEmpty()) return false;

	const QFileInfo info(path);
	if (!QDir().mkpath(info.absolutePath())) return false;
	QFile::setPermissions(info.absolutePath(),
	                       QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

	if (!bssh_append_line(path, hashed + entry.mid(split))) return false;

	QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
	return true;
}

void bssh_libssh_worker::do_accept_host_key() {
	if (!m_session) return;

	/*
	 * Recorded where the user's other SSH client will see it (sec 11),
	 * and only ever in response to someone having said yes.
	 *
	 * One store rather than two: the same person is deciding about the
	 * same hosts, and a decision that only half their tools know about is
	 * a decision they have to make twice -- and a host key that later
	 * CHANGES is only noticed by whichever tool recorded the first one.
	 * Hashed on the way in, because libssh would otherwise write it in
	 * the clear into a file the system hashes on purpose.
	 *
	 * Falls back to this app's own store when their file is out of reach:
	 * Android has no ~/.ssh, and a trust decision must not be lost
	 * because of where it could not be filed.
	 */
	if (!bssh_append_hashed_known_host(m_session) &&
	     ssh_session_update_known_hosts(m_session) != SSH_OK) {
		fail(session_error());
		return;
	}

	authenticate();
}

void bssh_libssh_worker::do_connect(const QString &host, quint16 port, const QString &user) {
	teardown();

	m_session = ssh_new();
	if (!m_session) {
		fail(QStringLiteral("ssh_new() failed"));
		return;
	}

	m_host = host;
	m_port = port;

	const QByteArray host_utf8 = host.toUtf8();
	const QByteArray user_utf8 = user.toUtf8();
	const QByteArray known_utf8 = primary_known_hosts_path().toUtf8();
	int port_value = port;

	/*
	 * Not routed through set_option(), following the split that function
	 * documents: it is loud for options deciding WHAT the connection is,
	 * and quiet for ones deciding how it behaves. A timeout is the
	 * second kind -- losing it does not reach a different machine or
	 * check a different key store.
	 */
	long timeout_s = BSSH_SSH_TIMEOUT_S;
	ssh_options_set(m_session, SSH_OPTIONS_TIMEOUT, &timeout_s);

	if (!set_option(SSH_OPTIONS_HOST, host_utf8.constData(), "the host") ||
	     !set_option(SSH_OPTIONS_PORT, &port_value, "the port") ||
	     !set_option(SSH_OPTIONS_USER, user_utf8.constData(), "the user")) {
		return;
	}
	/*
	 * The user's own file where it is reachable, so one decision serves
	 * both clients (sec 11); this app's store otherwise.
	 */
	if (!set_option(SSH_OPTIONS_KNOWNHOSTS, known_utf8.constData(),
	                 "the known-hosts file")) {
		return;
	}

	/*
	 * The GLOBAL slot stays what libssh means by it:
	 * /etc/ssh/ssh_known_hosts, which is how an administrator hands host
	 * keys to every account on a managed machine. Putting the user's file
	 * here -- as the first version of this did -- quietly displaced that,
	 * so a host the admin had vouched for came back as unknown.
	 */
	if (!set_option(SSH_OPTIONS_GLOBAL_KNOWNHOSTS, "/etc/ssh/ssh_known_hosts",
	                 "the system known-hosts file")) {
		return;
	}

	/*
	 * Before ssh_connect, because compression is negotiated during the
	 * handshake: setting it afterwards sets a value nothing will read.
	 * Worth having as a per-host setting rather than a global one -- it
	 * trades CPU for bandwidth, which is the right trade over a phone's
	 * mobile data and the wrong one on a LAN to a low-powered box.
	 */
	ssh_options_set(m_session, SSH_OPTIONS_COMPRESSION, m_compression ? "yes" : "no");
	apply_identity_file();

	emit state_changed(bssh_ssh_session::RESOLVING);
	emit state_changed(bssh_ssh_session::CONNECTING);

	/*
	 * Through a bastion, when one is named. The tunnel carries the whole
	 * handshake below, so it has to be up and pumping before ssh_connect
	 * is called -- and the session is handed a descriptor rather than a
	 * host, which is why SSH_OPTIONS_HOST above stays the TARGET's name:
	 * that is what the host key is checked against and what a
	 * known_hosts entry is filed under.
	 */
	if (!m_proxy_jump.isEmpty()) {
		m_jump = new bssh_jump_tunnel(this);

		const bssh_jump_tunnel::hop_t hop =
		        bssh_jump_tunnel::parse(m_proxy_jump, user);
		const int fd = m_jump->open(hop, host, port, primary_known_hosts_path(),
		                             m_identity_file);
		if (fd < 0) {
			const QString why = m_jump->why();
			delete m_jump;
			m_jump = nullptr;
			fail(why);
			return;
		}

		/*
		 * The one that must never be discarded. Unset, ssh_connect
		 * below opens its own socket to the target and the jump host is
		 * silently skipped -- see set_option.
		 */
		if (!set_option(SSH_OPTIONS_FD, &fd, "the jump host's connection")) return;
	}

	if (ssh_connect(m_session) != SSH_OK) {
		fail(session_error());
		return;
	}

	if (!verify_host_key()) return;

	authenticate();
}

void bssh_libssh_worker::authenticate() {
	if (!m_session) return;

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
	const bssh_auth_outcome_t key_outcome =
	        bssh_auth_outcome(ssh_userauth_publickey_auto(m_session, nullptr, nullptr));
	if (key_outcome == BSSH_AUTH_ACCEPTED) {
		emit state_changed(bssh_ssh_session::CONNECTED);
		return;
	}

	/*
	 * A transport fault is not a rejected key. Prompting for a credential
	 * after one asks the user to answer for something that never reached
	 * a verdict, and their answer cannot help.
	 */
	if (key_outcome == BSSH_AUTH_ERRORED) {
		fail(session_error());
		return;
	}

	/*
	 * The key WORKED and the server wants a second factor
	 * (`AuthenticationMethods publickey,password`). Straight to the
	 * password prompt: offering to unlock the key would be answering a
	 * question nobody asked, since the key is already accepted.
	 */
	if (key_outcome == BSSH_AUTH_NEEDS_MORE) {
		emit password_required();
		return;
	}

	/*
	 * A key on disk that did not work unaided is most likely encrypted
	 * with no agent loaded. Offer to unlock it before falling back to a
	 * password -- which for a server that only accepts keys is not a
	 * fallback at all, just a prompt that cannot succeed.
	 */
	if (default_identity_exists()) {
		emit key_passphrase_required();
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

	switch (bssh_auth_outcome(rc)) {
	case BSSH_AUTH_ACCEPTED:
		emit state_changed(bssh_ssh_session::CONNECTED);
		return;

	/*
	 * Refused, and the session is still there to try another: libssh
	 * keeps it authenticating until the server gives up on its own
	 * MaxAuthTries, which arrives as a dropped connection.
	 */
	case BSSH_AUTH_REJECTED:
		emit password_rejected();
		return;

	/*
	 * Said plainly, and without blaming the credential: the server took
	 * it. A user told "Authentication failed" here retypes a password
	 * that was right, which is the worst possible instruction.
	 */
	case BSSH_AUTH_NEEDS_MORE:
		fail(QStringLiteral("The server accepted that, and requires another "
		               "authentication method as well -- which BeerSSH "
		               "cannot answer yet."));
		return;

	case BSSH_AUTH_ERRORED:
		fail(session_error());
		return;
	}
}

void bssh_libssh_worker::do_open_shell(int cols, int rows) {
	m_cols = cols;
	m_rows = rows;
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
	if (!m_raw_channel && ssh_channel_request_pty_size(m_channel, m_term.toUtf8().constData(),
	                                                    cols, rows) != SSH_OK) {
		fail(session_error());
		return;
	}
	/*
	 * A shell, or the command sec 8.6's -e named -- and the PTY above is
	 * requested either way, which is what `ssh -t host htop` does and why
	 * this app asks for one at all: htop, vim and less are exactly what
	 * somebody runs this way, and every one of them needs a terminal.
	 *
	 * An exec request carries ONE string, which the far end passes to the
	 * account's shell. So the words are joined here rather than sent as
	 * argv, because there is no argv in the protocol to send -- the same
	 * thing OpenSSH does with the words after a hostname, and the reason
	 * a remote command needs quoting where a local one does not.
	 */
	const int requested = m_command.isEmpty()
	        ? ssh_channel_request_shell(m_channel)
	        : ssh_channel_request_exec(m_channel, m_command.toUtf8().constData());

	if (requested != SSH_OK) {
		fail(session_error());
		return;
	}

	start_polling();
	bssh_configure_tcp_keepalive(ssh_get_fd(m_session));

	/*
	 * Re-applied here because the interval usually arrives before there is
	 * a connection to apply it to: the host's settings are resolved when
	 * the session is created, and this runs once the shell is open.
	 */
	do_set_keepalive(m_keepalive_seconds);

	drain();
}

void bssh_libssh_worker::do_restart_shell(const QStringList &command) {
	if (!m_session || !m_channel) return;

	/*
	 * The old channel closed rather than left: the far end hangs up its
	 * pty, and a tmux or screen client on it detaches, leaving its session
	 * running. Freed here, before anything reads its end-of-file, so the
	 * close is not taken for the shell exiting.
	 */
	m_pending.clear();
	if (ssh_channel_is_open(m_channel)) ssh_channel_close(m_channel);
	ssh_channel_free(m_channel);
	m_channel = nullptr;

	do_set_command(command);
	do_open_shell(m_cols, m_rows);
}

void bssh_libssh_worker::do_run_on_the_side(const QString &command) {
	/* One at a time, and only on a connection that is up. */
	if (!m_session || m_side_channel) {
		emit side_output_ready(QByteArray());
		return;
	}

	m_side_channel = ssh_channel_new(m_session);
	if (!m_side_channel || ssh_channel_open_session(m_side_channel) != SSH_OK ||
	    ssh_channel_request_exec(m_side_channel, command.toUtf8().constData()) != SSH_OK) {
		/* A server that refuses a second channel or an exec is a server
		 * where nothing gets listed -- not a broken connection. */
		if (m_side_channel) ssh_channel_free(m_side_channel);
		m_side_channel = nullptr;
		emit side_output_ready(QByteArray());
		return;
	}
	m_side_output.clear();
	m_side_started.start();

	/*
	 * Asked before the shell opens, which is when a kept session's probe runs
	 * (sec 5.6): nothing else has started the poll that reads the answer and
	 * ends the wait, and the session would sit there with a blank screen.
	 */
	if (!m_poll) start_polling();
	drain_side();
}

void bssh_libssh_worker::do_find_working_directory() {
	if (!m_session) {
		emit working_directory_found(bssh_working_directory_t());
		return;
	}
	/* One in flight answers for everybody who asked. */
	if (m_probe_channel) return;

	m_probe_channel = ssh_channel_new(m_session);
	const QByteArray probe = bssh_working_directory_probe().toUtf8();
	if (!m_probe_channel || ssh_channel_open_session(m_probe_channel) != SSH_OK ||
	    ssh_channel_request_exec(m_probe_channel, probe.constData()) != SSH_OK) {
		if (m_probe_channel) ssh_channel_free(m_probe_channel);
		m_probe_channel = nullptr;
		emit working_directory_found(bssh_working_directory_t());
		return;
	}
	m_probe_output.clear();
	m_probe_started.start();
	drain_probe();
}

void bssh_libssh_worker::drain_probe() {
	if (!m_probe_channel) return;
	char buffer[4096];
	for (;;) {
		const int n = ssh_channel_read_nonblocking(m_probe_channel, buffer, sizeof(buffer), 0);
		if (n == SSH_ERROR) {
			finish_probe();
			return;
		}
		if (n <= 0) break;
		if (m_probe_output.size() < 64 * 1024) m_probe_output.append(buffer, n);
	}
	if (ssh_channel_is_eof(m_probe_channel) || !ssh_channel_is_open(m_probe_channel) ||
	    m_probe_started.elapsed() > 5000)
		finish_probe();
}

void bssh_libssh_worker::finish_probe() {
	const bssh_working_directory_t found = bssh_parse_working_directory(m_probe_output);
	if (ssh_channel_is_open(m_probe_channel)) ssh_channel_close(m_probe_channel);
	ssh_channel_free(m_probe_channel);
	m_probe_channel = nullptr;
	m_probe_output.clear();
	emit working_directory_found(found);
}

QString bssh_libssh_worker::sftp_error() const {
	return sftp_error_of(m_upload ? m_upload->sftp : nullptr);
}

QString bssh_libssh_worker::sftp_error_of(sftp_session sftp) const {
	if (!sftp) return session_error();
	switch (sftp_get_error(sftp)) {
	case SSH_FX_PERMISSION_DENIED: return QStringLiteral("permission denied");
	case SSH_FX_NO_SUCH_FILE:      return QStringLiteral("no such directory");
	case SSH_FX_FILE_ALREADY_EXISTS: return QStringLiteral("a file of that name appeared");
	default:                       return session_error();
	}
}

void bssh_libssh_worker::do_upload(const QStringList &paths, const QString &directory) {
	if (!m_session) {
		emit upload_finished(false, directory, QStringList(), QStringLiteral("not connected"));
		return;
	}
	if (m_upload) {
		emit upload_finished(false, directory, QStringList(),
		                     QStringLiteral("another copy is still running"));
		return;
	}

	m_upload = std::make_unique<upload_job>();
	upload_job &job = *m_upload;
	job.directory = directory;
	job.sftp = sftp_new(m_session);
	if (!job.sftp || sftp_init(job.sftp) != SSH_OK) {
		/* A server without the SFTP subsystem: said in its own words. */
		finish_upload(false, QStringLiteral("the server offers no SFTP"));
		return;
	}
	if (job.directory.isEmpty()) {
		char *home = sftp_canonicalize_path(job.sftp, ".");
		if (home) {
			job.directory = QString::fromUtf8(home);
			ssh_string_free_char(home);
		}
	}
	const QString base = job.directory.endsWith(QLatin1Char('/')) ? job.directory
	                                                                : job.directory + QLatin1Char('/');

	/* Each top-level name made free first; what is inside a new directory
	 * cannot collide, since the directory is new. */
	for (const QString &path : paths) {
		const QFileInfo info(path);
		if (!info.exists()) continue;
		const QString name = bssh_free_name(info.fileName(), [&job, &base](const QString &candidate) {
			sftp_attributes there = sftp_lstat(job.sftp, (base + candidate).toUtf8().constData());
			if (!there) return false;
			sftp_attributes_free(there);
			return true;
		});
		if (name.isEmpty()) continue;
		const QString remote = base + name;
		job.written << name;
		if (!info.isDir()) {
			job.items.append({ info.absoluteFilePath(), remote, false });
			job.total += info.size();
			continue;
		}
		job.items.append({ info.absoluteFilePath(), remote, true });
		QDirIterator inside(info.absoluteFilePath(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
		                    QDirIterator::Subdirectories);
		while (inside.hasNext()) {
			const QFileInfo each(inside.next());
			if (each.isSymLink() && each.isDir()) continue;
			const QString relative = QDir(info.absoluteFilePath()).relativeFilePath(each.absoluteFilePath());
			job.items.append({ each.absoluteFilePath(), remote + QLatin1Char('/') + relative, each.isDir() });
			if (!each.isDir()) job.total += each.size();
		}
	}
	job.since_progress.start();
	emit upload_progress(0, job.total);
	schedule_upload();
}

void bssh_libssh_worker::schedule_upload() {
	if (!m_upload || m_upload->scheduled) return;
	m_upload->scheduled = true;
	QTimer::singleShot(0, this, &bssh_libssh_worker::step_upload);
}

void bssh_libssh_worker::do_pause_upload(bool paused) {
	if (!m_upload) return;
	m_upload->paused = paused;
	if (!paused) schedule_upload();
}

void bssh_libssh_worker::step_upload() {
	if (!m_upload) return;
	upload_job &job = *m_upload;
	job.scheduled = false;
	if (job.paused) return;   /* held between pieces; resuming schedules again */

	if (!job.file) {
		if (job.at >= job.items.size()) {
			finish_upload(true, QString());
			return;
		}
		const upload_item &item = job.items.at(job.at);
		const QByteArray remote = item.remote.toUtf8();
		if (item.directory) {
			if (sftp_mkdir(job.sftp, remote.constData(), 0755) != SSH_OK) {
				finish_upload(false, QStringLiteral("could not make %1: %2").arg(item.remote, sftp_error()));
				return;
			}
			++job.at;
			schedule_upload();
			return;
		}
		job.source.setFileName(item.local);
		if (!job.source.open(QIODevice::ReadOnly)) {
			finish_upload(false, QStringLiteral("could not read %1").arg(item.local));
			return;
		}
		/* Its permissions, so a script dropped stays a script; O_EXCL, so
		 * the free name found a moment ago is not overwritten either. */
		const mode_t perms = (QFileInfo(item.local).isExecutable() ? 0755 : 0644);
		job.file = sftp_open(job.sftp, remote.constData(), O_WRONLY | O_CREAT | O_EXCL, perms);
		if (!job.file) {
			job.source.close();
			finish_upload(false, QStringLiteral("could not write %1: %2").arg(item.remote, sftp_error()));
			return;
		}
	}

	char buffer[32 * 1024];
	const qint64 n = job.source.read(buffer, sizeof(buffer));
	if (n < 0) {
		finish_upload(false, QStringLiteral("could not read %1").arg(job.source.fileName()));
		return;
	}
	if (n > 0) {
		if (sftp_write(job.file, buffer, size_t(n)) != ssize_t(n)) {
			finish_upload(false, QStringLiteral("writing %1 failed: %2")
			                             .arg(job.items.at(job.at).remote, sftp_error()));
			return;
		}
		job.done += n;
	}
	if (n == 0 || job.source.atEnd()) {
		sftp_close(job.file);
		job.file = nullptr;
		job.source.close();
		++job.at;
	}
	if (job.since_progress.elapsed() >= 100) {
		job.since_progress.restart();
		emit upload_progress(job.done, job.total);
	}
	schedule_upload();
}

void bssh_libssh_worker::do_cancel_upload() {
	if (!m_upload) return;
	drop_partial_upload();
	finish_upload(false, QString::fromLatin1(BSSH_COPY_STOPPED));
}

/* The file half-written goes, as the local copy's does. */
void bssh_libssh_worker::drop_partial_upload() {
	upload_job &job = *m_upload;
	if (job.file && job.at < job.items.size()) {
		sftp_close(job.file);
		job.file = nullptr;
		sftp_unlink(job.sftp, job.items.at(job.at).remote.toUtf8().constData());
	}
}

void bssh_libssh_worker::finish_upload(bool ok, const QString &why) {
	if (!m_upload) return;
	std::unique_ptr<upload_job> job = std::move(m_upload);
	if (job->file) sftp_close(job->file);
	if (job->source.isOpen()) job->source.close();
	if (job->sftp) sftp_free(job->sftp);
	if (ok) emit upload_progress(job->total, job->total);
	emit upload_finished(ok, job->directory, job->written, why);
}

/* An SFTP session opened for one operation; null, said why, when none. */
static sftp_session open_sftp(ssh_session session) {
	sftp_session sftp = sftp_new(session);
	if (sftp && sftp_init(sftp) != SSH_OK) {
		sftp_free(sftp);
		sftp = nullptr;
	}
	return sftp;
}

static bssh_remote_entry_t entry_from(sftp_attributes attributes) {
	bssh_remote_entry_t entry;
	entry.name = QString::fromUtf8(attributes->name);
	switch (attributes->type) {
	case SSH_FILEXFER_TYPE_REGULAR:   entry.kind = bssh_remote_entry_t::FILE; break;
	case SSH_FILEXFER_TYPE_DIRECTORY: entry.kind = bssh_remote_entry_t::DIRECTORY; break;
	case SSH_FILEXFER_TYPE_SYMLINK:   entry.kind = bssh_remote_entry_t::LINK; break;
	default:                          entry.kind = bssh_remote_entry_t::OTHER; break;
	}
	entry.size = qint64(attributes->size);
	entry.permissions = attributes->permissions & 07777;
	if (attributes->flags & SSH_FILEXFER_ATTR_ACMODTIME)
		entry.modified = QDateTime::fromSecsSinceEpoch(qint64(attributes->mtime));
	return entry;
}

static QString joined(const QString &directory, const QString &name) {
	return directory.endsWith(QLatin1Char('/')) ? directory + name
	                                             : directory + QLatin1Char('/') + name;
}

void bssh_libssh_worker::do_list_directory(const QString &path) {
	/* One listing at a time; the next waits its turn rather than racing. */
	m_list_queue.append(path);
	if (m_list) return;
	QTimer::singleShot(0, this, &bssh_libssh_worker::step_list);
}

void bssh_libssh_worker::step_list() {
	if (!m_list) {
		if (m_list_queue.isEmpty()) return;
		const QString path = m_list_queue.takeFirst();
		if (!m_session) {
			emit directory_listed(path, QList<bssh_remote_entry_t>(), QStringLiteral("not connected"));
			QTimer::singleShot(0, this, &bssh_libssh_worker::step_list);
			return;
		}
		m_list = std::make_unique<list_job>();
		m_list->path = path;
		m_list->sftp = open_sftp(m_session);
		if (!m_list->sftp) {
			finish_list(QStringLiteral("the server offers no SFTP"));
			return;
		}
		char *canonical = sftp_canonicalize_path(m_list->sftp,
		                                         (path.isEmpty() ? QStringLiteral(".") : path).toUtf8().constData());
		if (!canonical) {
			finish_list(sftp_error_of(m_list->sftp));
			return;
		}
		m_list->path = QString::fromUtf8(canonical);
		ssh_string_free_char(canonical);
		m_list->dir = sftp_opendir(m_list->sftp, m_list->path.toUtf8().constData());
		if (!m_list->dir) {
			finish_list(sftp_error_of(m_list->sftp));
			return;
		}
	}

	list_job &job = *m_list;
	for (int n = 0; n < 64; ++n) {
		sftp_attributes attributes = sftp_readdir(job.sftp, job.dir);
		if (!attributes) {
			finish_list(sftp_dir_eof(job.dir) ? QString() : sftp_error_of(job.sftp));
			return;
		}
		bssh_remote_entry_t entry = entry_from(attributes);
		sftp_attributes_free(attributes);
		if (entry.name == QStringLiteral(".") || entry.name == QStringLiteral("..")) continue;
		if (entry.kind == bssh_remote_entry_t::LINK) {
			sftp_attributes target = sftp_stat(job.sftp, joined(job.path, entry.name).toUtf8().constData());
			if (target) {
				entry.leads_to_directory = target->type == SSH_FILEXFER_TYPE_DIRECTORY;
				sftp_attributes_free(target);
			}
		}
		job.entries.append(entry);
	}
	QTimer::singleShot(0, this, &bssh_libssh_worker::step_list);
}

void bssh_libssh_worker::finish_list(const QString &why) {
	if (!m_list) return;
	std::unique_ptr<list_job> job = std::move(m_list);
	if (job->dir) sftp_closedir(job->dir);
	if (job->sftp) sftp_free(job->sftp);
	emit directory_listed(job->path, why.isEmpty() ? job->entries : QList<bssh_remote_entry_t>(), why);
	if (!m_list_queue.isEmpty()) QTimer::singleShot(0, this, &bssh_libssh_worker::step_list);
}

void bssh_libssh_worker::do_download(const QStringList &paths, const QString &directory,
                                     std::shared_ptr<bssh_piece_map> pieces) {
	if (!m_session) {
		emit download_finished(false, directory, QStringList(), QStringLiteral("not connected"));
		return;
	}
	if (m_download) {
		emit download_finished(false, directory, QStringList(),
		                       QStringLiteral("another copy is still running"));
		return;
	}
	m_download = std::make_unique<download_job>();
	download_job &job = *m_download;
	job.directory = directory;
	job.sftp = open_sftp(m_session);
	if (!job.sftp) {
		finish_download(false, QStringLiteral("the server offers no SFTP"));
		return;
	}

	/* Each top-level name made free here; inside a new directory nothing
	 * can collide, since the directory is new. */
	const QDir into(directory);
	for (const QString &path : paths) {
		sftp_attributes attributes = sftp_stat(job.sftp, path.toUtf8().constData());
		if (!attributes) {
			finish_download(false, QStringLiteral("could not read %1: %2").arg(path, sftp_error_of(job.sftp)));
			return;
		}
		const bool is_directory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
		const qint64 size = qint64(attributes->size);
		sftp_attributes_free(attributes);

		QString base = path;
		while (base.size() > 1 && base.endsWith(QLatin1Char('/'))) base.chop(1);
		const QString name = bssh_free_name(base.mid(base.lastIndexOf(QLatin1Char('/')) + 1),
		                                    [&into](const QString &candidate) {
			return QFileInfo::exists(into.filePath(candidate)) || QFileInfo(into.filePath(candidate)).isSymLink();
		});
		if (name.isEmpty()) continue;
		job.written << name;
		job.items.append({ base, into.filePath(name), is_directory, size });
		if (!is_directory) job.total += size;
	}
	/*
	 * Out of order only for the one file a map was made for, under the
	 * name it expects; anything else -- a folder, a name taken after all
	 * -- goes in order, and the map is told so.
	 */
	if (pieces) {
		const bool one_file = job.items.size() == 1 && !job.items.first().directory
		                      && job.items.first().local == pieces->staging()
		                      && job.items.first().size == pieces->size();
		if (one_file) job.pieces = pieces;
		else pieces->set_in_order();
	}
	job.since_progress.start();
	emit download_progress(0, job.total);
	schedule_download();
}

void bssh_libssh_worker::schedule_download() {
	if (!m_download || m_download->scheduled) return;
	m_download->scheduled = true;
	QTimer::singleShot(0, this, &bssh_libssh_worker::step_download);
}

void bssh_libssh_worker::do_pause_download(bool paused) {
	if (!m_download) return;
	m_download->paused = paused;
	if (!paused) schedule_download();
}

void bssh_libssh_worker::step_download() {
	if (!m_download) return;
	download_job &job = *m_download;
	job.scheduled = false;
	if (job.paused) return;

	if (!job.file) {
		if (job.at >= job.items.size()) {
			finish_download(true, QString());
			return;
		}
		const download_item item = job.items.at(job.at);
		if (item.directory) {
			/*
			 * Made here, then read 64 entries a turn as a listing is, its
			 * contents put after it once it is all read. A link to a
			 * directory is not followed, as the upload does not follow
			 * one: a loop would copy for ever.
			 */
			if (!job.dir) {
				if (!QDir().mkdir(item.local)) {
					finish_download(false, QStringLiteral("could not make %1").arg(item.local));
					return;
				}
				job.dir = sftp_opendir(job.sftp, item.remote.toUtf8().constData());
				if (!job.dir) {
					finish_download(false, QStringLiteral("could not read %1: %2")
					                               .arg(item.remote, sftp_error_of(job.sftp)));
					return;
				}
				job.inside.clear();
			}
			for (int n = 0; n < 64; ++n) {
				sftp_attributes attributes = sftp_readdir(job.sftp, job.dir);
				if (!attributes) {
					const bool complete = sftp_dir_eof(job.dir);
					sftp_closedir(job.dir);
					job.dir = nullptr;
					if (!complete) {
						finish_download(false, QStringLiteral("could not read %1: %2")
						                               .arg(item.remote, sftp_error_of(job.sftp)));
						return;
					}
					for (int i = 0; i < job.inside.size(); ++i) job.items.insert(job.at + 1 + i, job.inside.at(i));
					job.inside.clear();
					++job.at;
					break;
				}
				const QString name = QString::fromUtf8(attributes->name);
				const quint8 type = attributes->type;
				const qint64 size = qint64(attributes->size);
				sftp_attributes_free(attributes);
				if (name == QStringLiteral(".") || name == QStringLiteral("..")) continue;
				if (type == SSH_FILEXFER_TYPE_SYMLINK) {
					sftp_attributes target = sftp_stat(job.sftp, joined(item.remote, name).toUtf8().constData());
					if (!target) continue;
					const bool to_directory = target->type == SSH_FILEXFER_TYPE_DIRECTORY;
					const qint64 target_size = qint64(target->size);
					sftp_attributes_free(target);
					if (to_directory) continue;
					job.inside.append({ joined(item.remote, name), joined(item.local, name), false });
					job.total += target_size;
					continue;
				}
				if (type != SSH_FILEXFER_TYPE_REGULAR && type != SSH_FILEXFER_TYPE_DIRECTORY) continue;
				const bool is_directory = type == SSH_FILEXFER_TYPE_DIRECTORY;
				job.inside.append({ joined(item.remote, name), joined(item.local, name), is_directory });
				if (!is_directory) job.total += size;
			}
			schedule_download();
			return;
		}
		job.file = sftp_open(job.sftp, item.remote.toUtf8().constData(), O_RDONLY, 0);
		if (!job.file) {
			finish_download(false, QStringLiteral("could not read %1: %2")
			                               .arg(item.remote, sftp_error_of(job.sftp)));
			return;
		}
		/* NewOnly, as the upload's O_EXCL: the free name found a moment ago
		 * is not written over if something took it since. */
		job.target.setFileName(item.local);
		if (!job.target.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
			sftp_close(job.file);
			job.file = nullptr;
			finish_download(false, QStringLiteral("could not write %1").arg(item.local));
			return;
		}
		/* Filled out of order: its full length from the start, the holes
		 * not yet fetched reading as zeros nobody is told are there. */
		if (job.pieces && !job.target.resize(item.size)) {
			finish_download(false, QStringLiteral("could not write %1").arg(item.local));
			return;
		}
		job.position = 0;
	}

	if (job.pieces) {
		step_pieces();
		return;
	}

	char buffer[32 * 1024];
	const ssize_t n = sftp_read(job.file, buffer, sizeof(buffer));
	if (n < 0) {
		finish_download(false, QStringLiteral("reading %1 failed: %2")
		                               .arg(job.items.at(job.at).remote, sftp_error_of(job.sftp)));
		return;
	}
	if (n > 0) {
		if (job.target.write(buffer, n) != n) {
			finish_download(false, QStringLiteral("could not write %1").arg(job.target.fileName()));
			return;
		}
		job.done += n;
	} else {
		sftp_close(job.file);
		job.file = nullptr;
		job.target.close();
		++job.at;
	}
	if (job.since_progress.elapsed() >= 100) {
		job.since_progress.restart();
		emit download_progress(job.done, job.total);
	}
	schedule_download();
}

/*
 * One piece of a file filled out of order: the one its reader wants, or
 * the next missing after it. Read whole -- several sftp_reads where the
 * server answers short -- written at its place and flushed before it is
 * marked, so a reader told it is there finds it on disk.
 */
void bssh_libssh_worker::step_pieces() {
	download_job &job = *m_download;
	bssh_piece_map &map = *job.pieces;
	const qint64 index = map.next_missing();
	if (index < 0) {
		sftp_close(job.file);
		job.file = nullptr;
		job.target.close();
		++job.at;
		emit download_progress(job.done, job.total);
		schedule_download();
		return;
	}
	const qint64 offset = index * map.piece();
	const qint64 length = qMin(map.piece(), map.size() - offset);
	if (offset != job.position) {
		if (sftp_seek64(job.file, quint64(offset)) < 0) {
			finish_download(false, QStringLiteral("could not seek in %1: %2")
			                               .arg(job.items.at(job.at).remote, sftp_error_of(job.sftp)));
			return;
		}
		job.position = offset;
	}
	QByteArray buffer(int(length), Qt::Uninitialized);
	qint64 got = 0;
	while (got < length) {
		const ssize_t n = sftp_read(job.file, buffer.data() + got, size_t(qMin<qint64>(32 * 1024, length - got)));
		if (n < 0) {
			finish_download(false, QStringLiteral("reading %1 failed: %2")
			                               .arg(job.items.at(job.at).remote, sftp_error_of(job.sftp)));
			return;
		}
		if (n == 0) break;
		got += n;
	}
	job.position += got;
	if (got != length) {
		finish_download(false, QStringLiteral("%1 changed size while it was copied")
		                               .arg(job.items.at(job.at).remote));
		return;
	}
	if (!job.target.seek(offset) || job.target.write(buffer) != length || !job.target.flush()) {
		finish_download(false, QStringLiteral("could not write %1").arg(job.target.fileName()));
		return;
	}
	map.mark(index);
	job.done += length;
	if (job.since_progress.elapsed() >= 100) {
		job.since_progress.restart();
		emit download_progress(job.done, job.total);
	}
	schedule_download();
}

void bssh_libssh_worker::do_cancel_download() {
	if (!m_download) return;
	drop_partial_download();
	finish_download(false, QString::fromLatin1(BSSH_COPY_STOPPED));
}

/* The file half-written goes. It is local, so it goes whether or not the
 * connection is still there. */
void bssh_libssh_worker::drop_partial_download() {
	download_job &job = *m_download;
	if (job.dir) {
		sftp_closedir(job.dir);
		job.dir = nullptr;
	}
	if (job.file) {
		sftp_close(job.file);
		job.file = nullptr;
	}
	if (job.target.isOpen()) {
		job.target.close();
		job.target.remove();
	}
}

void bssh_libssh_worker::finish_download(bool ok, const QString &why) {
	if (!m_download) return;
	/* A file still open here is one not finished: it goes. */
	drop_partial_download();
	std::unique_ptr<download_job> job = std::move(m_download);
	if (job->sftp) sftp_free(job->sftp);
	if (ok) emit download_progress(job->total, job->total);
	emit download_finished(ok, job->directory, job->written, why);
}

void bssh_libssh_worker::do_change_remote(int change, const QString &path, const QString &to) {
	if (!m_session) {
		emit remote_changed(false, path, QStringLiteral("not connected"));
		return;
	}
	if (m_removal) {
		emit remote_changed(false, path, QStringLiteral("a removal is still running"));
		return;
	}
	sftp_session sftp = open_sftp(m_session);
	if (!sftp) {
		emit remote_changed(false, path, QStringLiteral("the server offers no SFTP"));
		return;
	}
	const QByteArray at = path.toUtf8();
	QString why;
	switch (change) {
	case bssh_ssh_session::RENAME: {
		/*
		 * Never over what is there: the rename libssh uses where the
		 * server has it, OpenSSH's posix-rename, replaces a target, so
		 * the name is looked for first. A name taken between the look
		 * and the rename is the one case left open.
		 */
		sftp_attributes there = sftp_lstat(sftp, to.toUtf8().constData());
		if (there) {
			sftp_attributes_free(there);
			why = QStringLiteral("%1 is already there").arg(to);
		} else if (sftp_rename(sftp, at.constData(), to.toUtf8().constData()) != SSH_OK) {
			why = sftp_error_of(sftp);
		}
		break;
	}
	case bssh_ssh_session::MAKE_DIRECTORY:
		if (sftp_mkdir(sftp, at.constData(), 0755) != SSH_OK) why = sftp_error_of(sftp);
		break;
	case bssh_ssh_session::REMOVE: {
		sftp_attributes attributes = sftp_lstat(sftp, at.constData());
		if (!attributes) {
			why = sftp_error_of(sftp);
			break;
		}
		const bool directory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
		sftp_attributes_free(attributes);
		m_removal = std::make_unique<removal_job>();
		m_removal->sftp = sftp;
		m_removal->path = path;
		m_removal->stack.append({ path, directory, false });
		QTimer::singleShot(0, this, &bssh_libssh_worker::step_removal);
		return;
	}
	default:
		why = QStringLiteral("not a change this connection knows");
	}
	sftp_free(sftp);
	emit remote_changed(why.isEmpty(), path, why);
}

void bssh_libssh_worker::step_removal() {
	if (!m_removal) return;
	removal_job &job = *m_removal;
	for (int n = 0; n < 32; ++n) {
		if (job.stack.isEmpty()) {
			finish_removal(QString());
			return;
		}
		removal_item &top = job.stack.last();
		const QByteArray at = top.path.toUtf8();
		if (top.directory && !top.expanded) {
			/* Its contents go on the stack above it, so they go first. A
			 * link inside is removed as a link, never followed. */
			top.expanded = true;
			const QString parent = top.path;
			sftp_dir dir = sftp_opendir(job.sftp, at.constData());
			if (!dir) {
				finish_removal(QStringLiteral("could not read %1: %2").arg(parent, sftp_error_of(job.sftp)));
				return;
			}
			while (sftp_attributes attributes = sftp_readdir(job.sftp, dir)) {
				const QString name = QString::fromUtf8(attributes->name);
				const bool directory = attributes->type == SSH_FILEXFER_TYPE_DIRECTORY;
				sftp_attributes_free(attributes);
				if (name == QStringLiteral(".") || name == QStringLiteral("..")) continue;
				job.stack.append({ joined(parent, name), directory, false });
			}
			const bool complete = sftp_dir_eof(dir);
			sftp_closedir(dir);
			if (!complete) {
				finish_removal(QStringLiteral("could not read %1: %2").arg(parent, sftp_error_of(job.sftp)));
				return;
			}
			continue;
		}
		const int done = top.directory ? sftp_rmdir(job.sftp, at.constData())
		                               : sftp_unlink(job.sftp, at.constData());
		if (done != SSH_OK) {
			finish_removal(QStringLiteral("could not remove %1: %2").arg(top.path, sftp_error_of(job.sftp)));
			return;
		}
		job.stack.removeLast();
	}
	QTimer::singleShot(0, this, &bssh_libssh_worker::step_removal);
}

void bssh_libssh_worker::finish_removal(const QString &why) {
	if (!m_removal) return;
	std::unique_ptr<removal_job> job = std::move(m_removal);
	if (job->sftp) sftp_free(job->sftp);
	emit remote_changed(why.isEmpty(), job->path, why);
}

void bssh_libssh_worker::drain_side() {
	if (!m_side_channel) return;

	const int cap = 64 * 1024;
	char buffer[4096];
	for (;;) {
		const int n = ssh_channel_read_nonblocking(m_side_channel, buffer, sizeof(buffer), 0);
		if (n == SSH_ERROR) {
			finish_side();
			return;
		}
		if (n <= 0) break;
		m_side_output.append(buffer, qMin(n, cap - int(m_side_output.size())));
		if (m_side_output.size() >= cap) {
			finish_side();
			return;
		}
	}
	if (ssh_channel_is_eof(m_side_channel) || !ssh_channel_is_open(m_side_channel) ||
	    m_side_started.elapsed() > 5000)
		finish_side();
}

void bssh_libssh_worker::finish_side() {
	const QByteArray output = m_side_output;
	if (ssh_channel_is_open(m_side_channel)) ssh_channel_close(m_side_channel);
	ssh_channel_free(m_side_channel);
	m_side_channel = nullptr;
	m_side_output.clear();
	emit side_output_ready(output);
}

void bssh_libssh_worker::do_set_command(const QStringList &command) {
	/*
	 * Joined with spaces, which is what the protocol can carry and what
	 * ssh(1) itself does with the words after a hostname. Nothing is
	 * quoted on the way: the remote shell will split this string, and a
	 * caller who needs an argument with a space in it has to say so the
	 * same way they would to ssh.
	 */
	m_command = command.join(QLatin1Char(' '));
}

void bssh_libssh_worker::do_set_term(const QString &term) {
	/*
	 * Empty means the compiled-in default rather than an empty TERM: a
	 * remote told its terminal is "" behaves worse than one told nothing
	 * useful, and a preset with the field cleared means "no opinion".
	 */
	m_term = term.isEmpty() ? QString::fromLatin1(BSSH_TERM) : term;
}

/*
 * Hands the key path to libssh, expanding a leading ~ first.
 *
 * OpenSSH configs are written with tildes and this field's whole purpose
 * is to carry one across from an imported config (sec 10.5). libssh
 * stores the string as given -- ssh_options_get hands back "~/.ssh/id"
 * unchanged -- and expands it only when it goes to load the key, so
 * expanding here is what makes the path this app acts on the same path it
 * could check and show. A key that silently does not load looks exactly
 * like a server refusing the key.
 */
void bssh_libssh_worker::apply_identity_file() {
	if (!m_session || m_identity_file.isEmpty()) return;

	QString path = m_identity_file;
	if (path == QStringLiteral("~") || path.startsWith(QStringLiteral("~/"))) {
		path.replace(0, 1, QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
	}

	/*
	 * ADD_IDENTITY, not a replacement, and checked against libssh rather
	 * than assumed: it PREPENDS, so the named key is offered first and the
	 * defaults still follow behind it. That is what OpenSSH's IdentityFile
	 * means without IdentitiesOnly, and a client that quietly stopped
	 * trying the agent because a config named one key would break the
	 * common case of having both.
	 */
	const QByteArray path_utf8 = path.toUtf8();
	ssh_options_set(m_session, SSH_OPTIONS_ADD_IDENTITY, path_utf8.constData());
}

void bssh_libssh_worker::do_set_identity_file(const QString &path) {
	m_identity_file = path;

	/* Applied now if there is a session, at connect time otherwise. */
	apply_identity_file();
}

void bssh_libssh_worker::do_set_proxy_jump(const QString &spec) {
	/*
	 * Applied at connect time, like compression: the route to a host is
	 * decided when the connection is made and cannot be changed under a
	 * live one.
	 */
	m_proxy_jump = spec;
}

void bssh_libssh_worker::do_set_compression(bool compression) {
	m_compression = compression;

	/*
	 * Applied to the session as soon as there is one; otherwise it waits
	 * for do_connect, which sets it before ssh_connect. Compression is
	 * negotiated in the handshake and cannot be turned on afterwards.
	 */
	if (!m_session) return;
	ssh_options_set(m_session, SSH_OPTIONS_COMPRESSION,
	                 m_compression ? "yes" : "no");
}

void bssh_libssh_worker::do_set_keepalive(int seconds) {
	m_keepalive_seconds = seconds > 0 ? seconds : 0;

	delete m_keepalive;
	m_keepalive = nullptr;

	if (m_keepalive_seconds == 0 || !m_session) return;

	m_keepalive = new QTimer(this);
	m_keepalive->setInterval(m_keepalive_seconds * 1000);

	/*
	 * SSH_MSG_IGNORE, which is the one thing libssh 0.11 exposes that puts
	 * bytes on the wire without meaning anything (sec 4 -- it has no
	 * client keepalive API, only a constant for requests arriving the
	 * other way). Its job is NAT: a phone's carrier drops an idle mapping
	 * in minutes, and the session it silently kills looks to the user like
	 * a terminal that stopped responding for no reason.
	 *
	 * It is deliberately not a liveness check. Nothing answers it, so a
	 * successful send proves only that the local kernel accepted the
	 * bytes; noticing a peer that has gone is TCP keepalive's job, above.
	 * An error, though, is real and worth reporting -- it means libssh has
	 * already given up on the socket.
	 */
	connect(m_keepalive, &QTimer::timeout, this, [this] {
		if (!m_session) return;
		if (ssh_send_ignore(m_session, "") == SSH_ERROR) {
			fail(session_error());
		}
	});

	m_keepalive->start();
}

void bssh_libssh_worker::drain() {
	/* The side channel first: it is short, and the shell's loop below can
	 * end the session. And before the shell's check, as there may not be one
	 * yet: a kept session asks on the side before opening it. */
	drain_side();
	drain_probe();
	if (!m_channel) return;

	/*
	 * Anything the window had no room for last time. This runs from the
	 * poll and from the read notifier, and a window opens because a
	 * packet arrived -- which is exactly what brings us here.
	 *
	 * Re-checked afterwards because flush_pending() can fail() and take
	 * the channel down underneath this.
	 */
	flush_pending();
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

			/* In a binary stream it would read as frames, and be none. */
			if (is_stderr && m_raw_channel) continue;
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
		/*
		 * ssh_channel_get_exit_state arrived in libssh 0.11 and this has
		 * to build against 0.10, which is what Ubuntu 24.04 -- and so
		 * the CI runner -- ships. The older call returns only the code,
		 * which is all this uses: the signal and core-dump flags are
		 * read to be freed, not to be reported.
		 */
		int exit_code = 0;
#if defined(LIBSSH_VERSION_INT) && LIBSSH_VERSION_INT >= SSH_VERSION_INT(0, 11, 0)
		uint32_t code = 0;
		char *exit_signal = nullptr;
		int core_dumped = 0;
		ssh_channel_get_exit_state(m_channel, &code, &exit_signal, &core_dumped);
		if (exit_signal) ssh_string_free_char(exit_signal);
		exit_code = int(code);
#else
		exit_code = ssh_channel_get_exit_status(m_channel);
		if (exit_code < 0) exit_code = 0;
#endif

		emit exited(exit_code);

		teardown();
		emit state_changed(bssh_ssh_session::DISCONNECTED);
	}
}

void bssh_libssh_worker::do_resize(int cols, int rows) {
	m_cols = cols;
	m_rows = rows;
	if (!m_channel || m_raw_channel) return;

	/*
	 * A window-change the remote never hears leaves it drawing to the
	 * old geometry, which is not a cosmetic fault: a full-screen program
	 * wraps and clears against a size the terminal no longer has, and
	 * the screen is garbage until something redraws it.
	 *
	 * SSH_ERROR here is the transport failing rather than the server
	 * declining -- a window-change carries want_reply false and is not
	 * answered -- so this is the same condition the read pump treats as
	 * fatal, and it is treated the same way.
	 */
	if (ssh_channel_change_pty_size(m_channel, cols, rows) != SSH_OK) {
		fail(session_error());
	}
}

void bssh_libssh_worker::do_send(const QByteArray &bytes) {
	if (!m_channel || bytes.isEmpty()) return;

	/*
	 * These are the user's keystrokes and pastes, and the result was
	 * thrown away -- so a write that failed took the input with it and
	 * said nothing, leaving a terminal that looks alive and swallows
	 * everything typed into it.
	 *
	 * QUEUED, and never asked for more than the remote will take right
	 * now. This session is BLOCKING and is never set otherwise, and the
	 * comment that used to stand here read that as a guarantee of
	 * progress: "libssh writes everything or returns SSH_ERROR and the
	 * loop turns once". That is true of a PARTIAL write and says nothing
	 * about the wait. When the remote program stops reading its stdin the
	 * channel window closes, and a blocking ssh_channel_write does not
	 * return at all.
	 *
	 * What that costs is the whole worker: this thread stops returning to
	 * its event loop, so drain() never runs again and output stops, and
	 * do_disconnect never runs either -- so Disconnect does nothing and
	 * the facade's destructor, which waits on this loop, waits for ever.
	 * TCP_USER_TIMEOUT does not rescue it, because the peer is alive and
	 * acknowledging; it simply is not reading.
	 *
	 * ssh_channel_window_size() is what the remote will accept without a
	 * wait, so writing at most that much cannot block. What does not fit
	 * waits for the window to open, which arrives as a packet and so
	 * arrives through drain() -- the poll this worker already runs.
	 *
	 * This is the third implementation of one rule and was the only
	 * careless one. bssh_jump_tunnel::write_to_channel polls with a
	 * 200ms cap and rechecks its stop flag; bssh_pty_session::send queues
	 * and returns, and its comment describes this very scenario: "a paste
	 * bigger than the pty buffer is enough to reach it, and only needs
	 * the child not to be reading".
	 */
	m_pending.append(bytes);
	flush_pending();
}

/*
 * Hands the channel as much as its window allows, and leaves the rest
 * queued. Never waits: see do_send.
 */
void bssh_libssh_worker::flush_pending() {
	while (!m_pending.isEmpty() && m_channel) {
		const uint32_t window = ssh_channel_window_size(m_channel);

		/*
		 * Nothing to be done this turn. Not an error and not a reason to
		 * drop anything -- the remote is simply not reading yet, and
		 * drain() comes back when a packet says it is.
		 */
		if (window == 0) return;

		const int take = int(qMin<qsizetype>(m_pending.size(), qsizetype(window)));
		const int n = ssh_channel_write(m_channel, m_pending.constData(), uint32_t(take));

		if (n == SSH_ERROR) {
			m_pending.clear();
			fail(session_error());
			return;
		}
		if (n <= 0) {
			const qsizetype left = m_pending.size();
			m_pending.clear();
			fail(QStringLiteral("the connection accepted none of %1 byte(s) sent")
			             .arg(left));
			return;
		}

		m_pending.remove(0, n);
	}
}

void bssh_libssh_worker::do_disconnect() {
	teardown();
	emit state_changed(bssh_ssh_session::DISCONNECTED);
}

/* The read notifier and the poll that drain the channel. */
void bssh_libssh_worker::start_polling() {
	delete m_notifier;
	delete m_poll;
	m_notifier = new QSocketNotifier(ssh_get_fd(m_session), QSocketNotifier::Read, this);
	connect(m_notifier, &QSocketNotifier::activated, this, &bssh_libssh_worker::drain);

	m_poll = new QTimer(this);
	m_poll->setInterval(BSSH_POLL_INTERVAL_MS);
	connect(m_poll, &QTimer::timeout, this, &bssh_libssh_worker::drain);
	m_poll->start();
}

/* ---- moving the connection to another process (sec 8.4) ----------- */

namespace {

const char BSSH_SSH_HANDOFF_MAGIC[] = "bssh-ssh-transport";
/* 2: a session through a jump host carries its tunnel too. */
const quint32 BSSH_SSH_HANDOFF_FORMAT = 2;

/* How often, and how far apart, an export is retried while libssh is
 * between a packet's first block and its last, or inside a rekey: the
 * server is already sending what completes either, so a second or two
 * is generous. Past it the move is called off and the session goes on. */
const int BSSH_EXPORT_TRIES = 100;
const int BSSH_EXPORT_POLL_MS = 20;

}  // namespace

bool bssh_libssh_worker::do_export(QByteArray &state, QList<int> &fds, QString &why) {
	if (!m_session || !m_channel) {
		why = QStringLiteral("the connection is not open");
		return false;
	}
	/* The export carries one channel, the shell's. A second one open would
	 * be left behind half-read, so it waits for the listing to end. */
	if (m_side_channel) {
		why = QStringLiteral("the connection is still listing its tmux and screen sessions");
		return false;
	}
	/*
	 * Nor while a dropped file is being looked for a place or copied (sec
	 * 6.4): the copy's SFTP channel would stay here, and its next piece
	 * would be written into a connection another process now owns --
	 * corrupting the stream under its shell.
	 */
	if (m_probe_channel) {
		why = QStringLiteral("the connection is still finding the shell's directory");
		return false;
	}
	if (m_upload || m_download) {
		why = QStringLiteral("a copy is still running on the connection; stop it or let it finish");
		return false;
	}
	if (m_list) {
		why = QStringLiteral("the connection is still listing a directory");
		return false;
	}
	if (m_removal) {
		why = QStringLiteral("the connection is still removing files");
		return false;
	}
	/* Stopped: nothing reads the channel from here until the release or
	 * the resume. What libssh reads while the export waits stays in its
	 * channel buffer and travels with the state. */
	delete m_notifier;
	m_notifier = nullptr;
	delete m_poll;
	m_poll = nullptr;
	if (m_keepalive) m_keepalive->stop();

	ssh_string exported = nullptr;
	int rc = SSH_AGAIN;
	for (int tries = 0; tries < BSSH_EXPORT_TRIES; ++tries) {
		rc = ssh_session_export_transport(m_session, &exported);
		if (rc != SSH_AGAIN) break;
		/* Lets libssh read the rest of a packet or a key exchange without
		 * taking anything off the channel. */
		ssh_channel_poll_timeout(m_channel, BSSH_EXPORT_POLL_MS, 0);
	}
	if (rc != SSH_OK) {
		why = rc == SSH_AGAIN
		        ? QStringLiteral("the connection was busy for too long to stop: %1").arg(session_error())
		        : session_error();
		do_resume();
		return false;
	}

	/*
	 * Through a jump host, the tunnel goes too -- stopped only now, after
	 * the target session: while that one was finishing a packet, the pump
	 * was what brought the rest of it. Paused between transfers, the pair
	 * keeps whatever is in flight, and it travels with the descriptors.
	 */
	QByteArray tunnel;
	QList<int> tunnel_fds;
	if (m_jump) {
		if (!m_jump->pause() || !m_jump->export_state(tunnel, tunnel_fds, why)) {
			if (why.isEmpty()) why = QStringLiteral("the jump host's tunnel would not stop");
			ssh_string_burn(exported);
			ssh_string_free(exported);
			do_resume();
			return false;
		}
	}

	QDataStream out(&state, QIODevice::WriteOnly);
	out.setVersion(QDataStream::Qt_6_0);
	out << QByteArray(BSSH_SSH_HANDOFF_MAGIC) << BSSH_SSH_HANDOFF_FORMAT
	    << QByteArray(static_cast<const char *>(ssh_string_data(exported)),
	                  int(ssh_string_len(exported)))
	    << m_term << m_command << m_identity_file << m_host << m_port
	    << qint32(m_keepalive_seconds) << m_compression << m_pending
	    << bool(m_jump) << m_proxy_jump << tunnel;
	ssh_string_burn(exported);
	ssh_string_free(exported);

	fds = QList<int>{ ssh_get_fd(m_session) } + tunnel_fds;
	return true;
}

/* The receiver has it: let go, sending nothing. */
void bssh_libssh_worker::do_release() {
	delete m_notifier;
	m_notifier = nullptr;
	delete m_poll;
	m_poll = nullptr;
	delete m_keepalive;
	m_keepalive = nullptr;
	m_pending.clear();
	if (m_session) ssh_session_detach_transport(m_session);
	if (m_channel) {
		ssh_channel_free(m_channel);
		m_channel = nullptr;
	}
	if (m_session) {
		ssh_free(m_session);
		m_session = nullptr;
	}
	/* After the target session, whose end of the pair it was. */
	if (m_jump) {
		m_jump->release();
		delete m_jump;
		m_jump = nullptr;
	}
}

/* The move did not happen: carry on exactly where it stopped. */
void bssh_libssh_worker::do_resume() {
	if (!m_session || !m_channel) return;
	if (m_jump) m_jump->resume();
	start_polling();
	if (m_keepalive) m_keepalive->start();
	drain();
}

bool bssh_libssh_worker::do_import(const QByteArray &state, const QList<int> &fds,
                                   QString &why) {
	if (m_session || fds.isEmpty()) {
		why = QStringLiteral("the connection arrived without its socket");
		return false;
	}

	QDataStream in(state);
	in.setVersion(QDataStream::Qt_6_0);
	QByteArray magic, transport, pending;
	quint32 format = 0;
	QString term, command, identity, host;
	quint16 port = 22;
	qint32 keepalive = 0;
	bool compression = false, through_jump = false;
	QString proxy_jump;
	QByteArray tunnel;
	in >> magic >> format >> transport >> term >> command >> identity >> host >> port
	   >> keepalive >> compression >> pending >> through_jump >> proxy_jump >> tunnel;
	if (in.status() != QDataStream::Ok || !in.atEnd() || magic != BSSH_SSH_HANDOFF_MAGIC ||
	    format != BSSH_SSH_HANDOFF_FORMAT) {
		why = QStringLiteral("the connection's state is not one this BeerSSH reads");
		return false;
	}
	if (fds.size() != (through_jump ? 3 : 1)) {
		why = QStringLiteral("the connection arrived without all of its sockets");
		return false;
	}

	/*
	 * Duplicates, through a jump host: two sessions and a pump take them
	 * one at a time, and a failure part-way must leave the caller holding
	 * every descriptor it gave -- its contract -- rather than some of them
	 * already closed by a half-built tunnel. On success the originals are
	 * closed, being the session's now.
	 */
	bssh_jump_tunnel *jump = nullptr;
	int target_fd = fds.first();
	if (through_jump) {
		const int jump_fd = ::dup(fds.at(1));
		const int ours_fd = ::dup(fds.at(2));
		target_fd = ::dup(fds.at(0));
		jump = new bssh_jump_tunnel;
		if (jump_fd < 0 || ours_fd < 0 || target_fd < 0 ||
		    !jump->adopt(tunnel, jump_fd, ours_fd, target_fd, why)) {
			for (int fd : { jump_fd, ours_fd, target_fd })
				if (fd >= 0) ::close(fd);
			if (why.isEmpty()) why = QStringLiteral("the jump host's tunnel could not be taken");
			delete jump;
			return false;
		}
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
	const int rc = ssh_session_import_transport(session, target_fd, blob);
	ssh_string_burn(blob);
	ssh_string_free(blob);
	ssh_channel channel = rc == SSH_OK ? ssh_session_get_channel(session, 0) : nullptr;
	if (!channel) {
		why = rc == SSH_OK ? QStringLiteral("the connection arrived without its channel")
		                   : QString::fromUtf8(ssh_get_error(session));
		/* On a failed import the descriptor is still the caller's; on a
		 * channel missing from a good one it is the session's, and freeing
		 * a detached session closes it without a word to the server. */
		if (rc == SSH_OK) ssh_session_detach_transport(session);
		ssh_free(session);
		if (jump) {
			/* Its own duplicates, and the target's, which the failed
			 * import left open. */
			jump->release();
			delete jump;
			if (rc != SSH_OK) ::close(target_fd);
		}
		return false;
	}
	if (jump) {
		for (int fd : fds) ::close(fd);
		m_jump = jump;
		m_proxy_jump = proxy_jump;
		/* Only now: see bssh_jump_tunnel::adopt(). */
		m_jump->resume();
	}

	m_session = session;
	m_channel = channel;
	m_term = term;
	m_command = command;
	m_identity_file = identity;
	m_host = host;
	m_port = port;
	m_compression = compression;
	m_pending = pending;

	start_polling();
	bssh_configure_tcp_keepalive(ssh_get_fd(m_session));
	do_set_keepalive(keepalive);
	/* No state_changed: it would arrive queued, after the session has
	 * wired a handler that opens a shell on CONNECTED. The facade sets the
	 * state itself before returning. The drain is queued for the same
	 * reason -- what it reads must reach a session that is listening. */
	QMetaObject::invokeMethod(this, &bssh_libssh_worker::drain, Qt::QueuedConnection);
	return true;
}

/* ---- facade, GUI thread ------------------------------------------- */

bssh_ssh_session_libssh::bssh_ssh_session_libssh(QObject *parent)
    : bssh_ssh_session(parent),
      m_worker(new bssh_libssh_worker) {
	m_worker->moveToThread(&m_thread);

	connect(m_worker, &bssh_libssh_worker::bytes_received,
	         this, &bssh_ssh_session::bytes_received);
	connect(m_worker, &bssh_libssh_worker::side_output_ready,
	         this, &bssh_ssh_session::side_output_ready);
	qRegisterMetaType<bssh_working_directory_t>();
	connect(m_worker, &bssh_libssh_worker::working_directory_found,
	         this, &bssh_ssh_session::working_directory_found);
	connect(m_worker, &bssh_libssh_worker::upload_progress,
	         this, &bssh_ssh_session::upload_progress);
	connect(m_worker, &bssh_libssh_worker::upload_finished,
	         this, &bssh_ssh_session::upload_finished);
	qRegisterMetaType<bssh_remote_entry_t>();
	qRegisterMetaType<QList<bssh_remote_entry_t>>();
	connect(m_worker, &bssh_libssh_worker::directory_listed,
	         this, &bssh_ssh_session::directory_listed);
	connect(m_worker, &bssh_libssh_worker::download_progress,
	         this, &bssh_ssh_session::download_progress);
	connect(m_worker, &bssh_libssh_worker::download_finished,
	         this, &bssh_ssh_session::download_finished);
	connect(m_worker, &bssh_libssh_worker::remote_changed,
	         this, &bssh_ssh_session::remote_changed);
	connect(m_worker, &bssh_libssh_worker::failed,
	         this, &bssh_ssh_session::failed);
	connect(m_worker, &bssh_libssh_worker::exited,
	         this, &bssh_ssh_session::shell_exited);
	connect(m_worker, &bssh_libssh_worker::password_required,
	         this, &bssh_ssh_session::password_required);
	connect(m_worker, &bssh_libssh_worker::password_rejected,
	         this, &bssh_ssh_session::password_rejected);
	connect(m_worker, &bssh_libssh_worker::key_passphrase_required,
	         this, &bssh_ssh_session::key_passphrase_required);
	connect(m_worker, &bssh_libssh_worker::host_key_unknown,
	         this, &bssh_ssh_session::host_key_unknown);
	connect(m_worker, &bssh_libssh_worker::host_key_revoked,
	         this, &bssh_ssh_session::host_key_revoked, Qt::QueuedConnection);
	connect(m_worker, &bssh_libssh_worker::host_key_changed,
	         this, &bssh_ssh_session::host_key_changed);
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

void bssh_ssh_session_libssh::set_keepalive(int seconds) {
	QMetaObject::invokeMethod(m_worker, [this, seconds] {
		m_worker->do_set_keepalive(seconds);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::set_term(const QString &term) {
	QMetaObject::invokeMethod(m_worker, [this, term] {
		m_worker->do_set_term(term);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::restart_shell(const QStringList &command) {
	QMetaObject::invokeMethod(m_worker, [this, command] {
		m_worker->do_restart_shell(command);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::find_working_directory() {
	QMetaObject::invokeMethod(m_worker, [this] { m_worker->do_find_working_directory(); },
	                          Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::upload(const QStringList &paths, const QString &directory) {
	QMetaObject::invokeMethod(m_worker, [this, paths, directory] {
		m_worker->do_upload(paths, directory);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::cancel_upload() {
	QMetaObject::invokeMethod(m_worker, [this] { m_worker->do_cancel_upload(); }, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::list_directory(const QString &path) {
	QMetaObject::invokeMethod(m_worker, [this, path] { m_worker->do_list_directory(path); },
	                          Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::download(const QStringList &paths, const QString &directory) {
	QMetaObject::invokeMethod(m_worker, [this, paths, directory] {
		m_worker->do_download(paths, directory);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::download_pieces(const QString &path, const QString &directory,
                                              std::shared_ptr<bssh_piece_map> pieces) {
	QMetaObject::invokeMethod(m_worker, [this, path, directory, pieces] {
		m_worker->do_download({ path }, directory, pieces);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::change_remote(change_t change, const QString &path, const QString &to) {
	QMetaObject::invokeMethod(m_worker, [this, change, path, to] {
		m_worker->do_change_remote(int(change), path, to);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::pause_upload(bool paused) {
	QMetaObject::invokeMethod(m_worker, [this, paused] { m_worker->do_pause_upload(paused); }, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::pause_download(bool paused) {
	QMetaObject::invokeMethod(m_worker, [this, paused] { m_worker->do_pause_download(paused); }, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::cancel_download() {
	QMetaObject::invokeMethod(m_worker, [this] { m_worker->do_cancel_download(); }, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::run_on_the_side(const QString &command) {
	QMetaObject::invokeMethod(m_worker, [this, command] {
		m_worker->do_run_on_the_side(command);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::set_raw_channel(bool raw) {
	QMetaObject::invokeMethod(m_worker, [this, raw] {
		m_worker->do_set_raw_channel(raw);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::set_command(const QStringList &command) {
	QMetaObject::invokeMethod(m_worker, [this, command] {
		m_worker->do_set_command(command);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::set_compression(bool compression) {
	QMetaObject::invokeMethod(m_worker, [this, compression] {
		m_worker->do_set_compression(compression);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::set_identity_file(const QString &path) {
	QMetaObject::invokeMethod(m_worker, [this, path] {
		m_worker->do_set_identity_file(path);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::set_proxy_jump(const QString &spec) {
	QMetaObject::invokeMethod(m_worker, [this, spec] {
		m_worker->do_set_proxy_jump(spec);
	}, Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::disconnect_from_host() {
	QMetaObject::invokeMethod(m_worker, "do_disconnect", Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::accept_host_key() {
	QMetaObject::invokeMethod(m_worker, "do_accept_host_key", Qt::QueuedConnection);
}

void bssh_ssh_session_libssh::supply_key_passphrase(const QString &passphrase) {
	QMetaObject::invokeMethod(m_worker, [this, passphrase] {
		m_worker->do_key_passphrase(passphrase);
	}, Qt::QueuedConnection);
}

QString bssh_ssh_session_libssh::kind() const {
	return QStringLiteral("ssh");
}

/*
 * Blocking calls into the worker's thread: the caller has stopped its side
 * of the session for the length of a move, and wants the answer now.
 */
bool bssh_ssh_session_libssh::export_transport(QByteArray &state, QList<int> &fds, QString *why) {
	bool ok = false;
	QString reason;
	QMetaObject::invokeMethod(m_worker, [this, &state, &fds, &reason, &ok] {
		ok = m_worker->do_export(state, fds, reason);
	}, Qt::BlockingQueuedConnection);
	if (!ok && why) *why = reason;
	return ok;
}

void bssh_ssh_session_libssh::release_transport() {
	QMetaObject::invokeMethod(m_worker, [this] { m_worker->do_release(); },
	                          Qt::BlockingQueuedConnection);
}

void bssh_ssh_session_libssh::resume_transport() {
	QMetaObject::invokeMethod(m_worker, [this] { m_worker->do_resume(); },
	                          Qt::BlockingQueuedConnection);
}

bool bssh_ssh_session_libssh::import_transport(const QByteArray &state, const QList<int> &fds,
                                               QString *why) {
	bool ok = false;
	QString reason;
	QMetaObject::invokeMethod(m_worker, [this, &state, &fds, &reason, &ok] {
		ok = m_worker->do_import(state, fds, reason);
	}, Qt::BlockingQueuedConnection);
	if (!ok && why) *why = reason;
	if (ok) set_state(CONNECTED);
	return ok;
}

bssh_ssh_session *bssh_ssh_session::create_for_import(const QString &kind, QObject *parent) {
	if (kind == QStringLiteral("ssh")) return new bssh_ssh_session_libssh(parent);
#ifndef _WIN32
	if (kind == QStringLiteral("local")) return new bssh_pty_session(parent);
#endif
	return nullptr;
}

bssh_ssh_session *bssh_ssh_session::create(QObject *parent) {
	if (bssh_ssh_session *injected = create_from_factory(parent)) return injected;

	return new bssh_ssh_session_libssh(parent);
}

#include "ssh_session_libssh.moc"
