#include "ssh/ssh_session_libssh.h"

#include "ssh/jump_tunnel.h"

#include "platform/platform.h"

#include <QDir>
#include <QFileInfo>
#include <openssl/rand.h>
#include <openssl/hmac.h>
#include <QFile>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTimer>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

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
 * connection that does not work, so none of them is checked.
 */
void bssh_configure_tcp_keepalive(int fd) {
	if (fd < 0) return;

	const int on = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));

#ifdef TCP_KEEPIDLE
	const int idle = BSSH_TCP_KEEPALIVE_IDLE_S;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
	const int interval = BSSH_TCP_KEEPALIVE_INTERVAL_S;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
#endif
#ifdef TCP_KEEPCNT
	const int count = BSSH_TCP_KEEPALIVE_COUNT;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
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
	setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &user_timeout_ms, sizeof(user_timeout_ms));
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
	void do_set_compression(bool compression);
	void do_set_identity_file(const QString &path);
	void do_set_proxy_jump(const QString &spec);
	void apply_identity_file();
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
	void key_passphrase_required();
	void host_key_unknown(const QString &fingerprint);
	void host_key_changed(const QString &fingerprint);
	void host_key_revoked(const QString &fingerprint);

private:
	void teardown();
	void fail(const QString &reason);
	QString session_error() const;
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

	/* Applied at connect time; see set_term's comment on the interface. */
	QString m_term;

	/* Empty asks for a shell; anything else is an exec request (sec 8.6). */
	QString m_command;

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

	const enum ssh_known_hosts_e known = ssh_session_is_known_server(m_session);
	if (known == SSH_KNOWN_HOSTS_OK) return true;

	/*
	 * Never accepted silently (sec 11), and never decided here: the
	 * connection is HELD -- session open, nothing torn down -- while a
	 * person answers. Failing instead, which is what this did before,
	 * meant the only way to reach a new host was to edit known_hosts by
	 * hand.
	 */
	const QString fingerprint = server_fingerprint();

	if (known == SSH_KNOWN_HOSTS_CHANGED) {
		emit host_key_changed(fingerprint);
	} else {
		emit host_key_unknown(fingerprint);
	}
	return false;
}

void bssh_libssh_worker::do_key_passphrase(const QString &passphrase) {
	if (!m_session) return;

	const QByteArray passphrase_utf8 = passphrase.toUtf8();
	const int rc = ssh_userauth_publickey_auto(m_session, nullptr, passphrase_utf8.constData());

	if (rc == SSH_AUTH_SUCCESS) {
		emit state_changed(bssh_ssh_session::CONNECTED);
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

	QFile file(path);
	if (!file.open(QIODevice::Append | QIODevice::WriteOnly | QIODevice::Text)) return false;

	const bool ok = file.write(hashed + entry.mid(split) + '\n') > 0;
	file.close();

	if (ok) {
		QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
	}
	return ok;
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

	ssh_options_set(m_session, SSH_OPTIONS_HOST, host_utf8.constData());
	ssh_options_set(m_session, SSH_OPTIONS_PORT, &port_value);
	ssh_options_set(m_session, SSH_OPTIONS_USER, user_utf8.constData());
	/*
	 * The user's own file where it is reachable, so one decision serves
	 * both clients (sec 11); this app's store otherwise.
	 */
	ssh_options_set(m_session, SSH_OPTIONS_KNOWNHOSTS, known_utf8.constData());

	/*
	 * The GLOBAL slot stays what libssh means by it:
	 * /etc/ssh/ssh_known_hosts, which is how an administrator hands host
	 * keys to every account on a managed machine. Putting the user's file
	 * here -- as the first version of this did -- quietly displaced that,
	 * so a host the admin had vouched for came back as unknown.
	 */
	ssh_options_set(m_session, SSH_OPTIONS_GLOBAL_KNOWNHOSTS, "/etc/ssh/ssh_known_hosts");

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

		ssh_options_set(m_session, SSH_OPTIONS_FD, &fd);
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
	const int key_rc = ssh_userauth_publickey_auto(m_session, nullptr, nullptr);
	if (key_rc == SSH_AUTH_SUCCESS) {
		emit state_changed(bssh_ssh_session::CONNECTED);
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
	if (ssh_channel_request_pty_size(m_channel, m_term.toUtf8().constData(),
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

	m_notifier = new QSocketNotifier(ssh_get_fd(m_session), QSocketNotifier::Read, this);
	connect(m_notifier, &QSocketNotifier::activated, this, &bssh_libssh_worker::drain);

	m_poll = new QTimer(this);
	m_poll->setInterval(BSSH_POLL_INTERVAL_MS);
	connect(m_poll, &QTimer::timeout, this, &bssh_libssh_worker::drain);
	m_poll->start();

	bssh_configure_tcp_keepalive(ssh_get_fd(m_session));

	/*
	 * Re-applied here because the interval usually arrives before there is
	 * a connection to apply it to: the host's settings are resolved when
	 * the session is created, and this runs once the shell is open.
	 */
	do_set_keepalive(m_keepalive_seconds);

	drain();
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

bssh_ssh_session *bssh_ssh_session::create(QObject *parent) {
	if (bssh_ssh_session *injected = create_from_factory(parent)) return injected;

	return new bssh_ssh_session_libssh(parent);
}

#include "ssh_session_libssh.moc"
