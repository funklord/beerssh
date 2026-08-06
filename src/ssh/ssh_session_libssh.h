#ifndef BSSH_SSH_SESSION_LIBSSH_H
#define BSSH_SSH_SESSION_LIBSSH_H

#include <QThread>

#include "ssh/ssh_session.h"

class bssh_libssh_worker;

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

private:
	QThread m_thread;
	bssh_libssh_worker *m_worker;
};

#endif /* BSSH_SSH_SESSION_LIBSSH_H */
