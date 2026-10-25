#ifndef BSSH_SSH_SESSION_LIBSSH_H
#define BSSH_SSH_SESSION_LIBSSH_H

#include <QThread>

#include "ssh/ssh_session.h"

class bssh_libssh_worker;

/*
 * What the known-hosts check said, as a decision this app can act on
 * (project.md sec 11).
 *
 * A pure mapping, kept apart from the connecting so the RULE can be
 * tested -- the same reason bssh_tab_status::state_for exists apart from
 * the tab bar. Nothing else in this file is reachable from a test without
 * a real server, which is precisely why the most consequential rule in it
 * had none.
 *
 * Takes an int rather than enum ssh_known_hosts_e, because of the promise
 * at the top of this header: no libssh type appears in it, so a libssh2
 * backend could replace the .cpp without touching anything that includes
 * this. The caller passes libssh's verdict; the meaning of the values is
 * the .cpp's business.
 */
enum bssh_host_key_verdict_t {
	BSSH_HOST_KEY_TRUSTED,        /* recorded, and unchanged */
	BSSH_HOST_KEY_FIRST_CONTACT,  /* not recorded at all; ask */
	BSSH_HOST_KEY_MISMATCH,       /* recorded differently; warn */
	BSSH_HOST_KEY_UNVERIFIED      /* the check did not run; refuse */
};

bssh_host_key_verdict_t bssh_host_key_verdict(int known_hosts_result);

/*
 * What libssh's authentication result actually said (project.md sec 11).
 *
 * Same reasoning as bssh_host_key_verdict above, and the same int rather
 * than enum ssh_auth_e for the same header promise. libssh answers six
 * ways and only one of them means "the credential was wrong"; collapsing
 * the rest into that is how a correct password came to be reported as a
 * failure.
 */
enum bssh_auth_outcome_t {
	BSSH_AUTH_ACCEPTED,    /* through; nothing further is wanted */
	BSSH_AUTH_REJECTED,    /* the credential was wrong */
	BSSH_AUTH_NEEDS_MORE,  /* accepted, and the server wants another factor */
	BSSH_AUTH_ERRORED      /* no verdict on the credential at all */
};

bssh_auth_outcome_t bssh_auth_outcome(int auth_result);

/*
 * Append one line to a line-oriented file, starting it on a line of its
 * own (project.md sec 11).
 *
 * Written for known_hosts, which this app shares with OpenSSH. Appending
 * blindly joins the new entry onto the last existing one whenever that
 * file does not end in a newline -- corrupting the previous host's entry
 * as well as failing to record this one. A file edited by hand is the
 * ordinary way to get there, and a short write is the other: it leaves no
 * trailing newline, so one partial append arranges the corruption of the
 * next.
 *
 * No libssh type, so it stays inside this header's promise, and takes a
 * path rather than a session so the rule can be tested without a server.
 */
bool bssh_append_line(const QString &path, const QByteArray &line);

/*
 * The libssh implementation of bssh_ssh_session (project.md sec 4).
 *
 * This header and its .cpp are the ONLY places in the tree allowed to know
 * which SSH library is in use -- no libssh type appears even here, in the
 * header, so that a libssh2 backend could replace this file without
 * touching anything that includes it.
 *
 * Threading, per sec 5.2: every libssh call happens on a worker thread,
 * and this class is the facade the GUI thread talks to. The calls are
 * blocking ones (connect, auth) and a UI that freezes for the length of a
 * TCP handshake to an unreachable host is a UI that looks crashed --
 * which on Android is also a UI the system may kill for not responding.
 */
/*
 * sec 5.3's socket options, applied to a connected TCP socket.
 *
 * Shared with bssh_jump_tunnel rather than private to this file: a
 * session reached through a bastion has TWO sockets, and the one the app
 * never sees is the one carrying everything. Left with the defaults, a
 * link that goes silent -- a phone losing signal rather than a server
 * refusing -- would be noticed on the target's socket and not on the
 * bastion's, and the tunnel would hold the session up on a link that no
 * longer exists.
 */
void bssh_configure_tcp_keepalive(int fd);

class bssh_ssh_session_libssh : public bssh_ssh_session {
	Q_OBJECT

public:
	explicit bssh_ssh_session_libssh(QObject *parent = nullptr);
	~bssh_ssh_session_libssh() override;

	void connect_to_host(const QString &host, quint16 port, const QString &user) override;
	void authenticate_password(const QString &password) override;
	void open_shell(int cols, int rows) override;
	void resize(int cols, int rows) override;
	void send(const QByteArray &bytes) override;
	void set_keepalive(int seconds) override;
	void set_term(const QString &term) override;
	void set_command(const QStringList &command) override;
	void set_compression(bool compression) override;
	void set_identity_file(const QString &path) override;
	void set_proxy_jump(const QString &spec) override;
	void disconnect_from_host() override;
	void accept_host_key() override;
	void supply_key_passphrase(const QString &passphrase) override;

	QString kind() const override;
	void run_on_the_side(const QString &command) override;
	bool export_transport(QByteArray &state, QList<int> &fds, QString *why) override;
	void release_transport() override;
	void resume_transport() override;
	bool import_transport(const QByteArray &state, const QList<int> &fds, QString *why) override;

private:
	QThread m_thread;
	bssh_libssh_worker *m_worker;
};

#endif /* BSSH_SSH_SESSION_LIBSSH_H */
