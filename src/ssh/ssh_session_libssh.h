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
	void disconnect_from_host() override;

private:
	QThread m_thread;
	bssh_libssh_worker *m_worker;
};

#endif /* BSSH_SSH_SESSION_LIBSSH_H */
