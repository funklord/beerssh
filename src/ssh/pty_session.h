#ifndef BSSH_PTY_SESSION_H
#define BSSH_PTY_SESSION_H

#include "ssh/ssh_session.h"

class QSocketNotifier;

/*
 * bssh_pty_session -- a local shell on a pseudo-terminal, as a transport
 * (project.md sec 2, sec 3).
 *
 * This reverses a documented non-goal, and it is worth saying where. sec 2
 * listed "local terminal / on-device shell" as out of scope for v1 and sec
 * 3's core principle 1 said the desktop build COULD open a PTY and must
 * not, precisely so that the emulator could never come to depend on one.
 * That decision was reversed deliberately: the desktop now opens a local
 * shell by default. What the principle was protecting is kept -- the
 * emulator, the renderer, the input router and the scrollback still know
 * nothing about a PTY, because this sits behind bssh_ssh_session with
 * everything else.
 *
 * That interface is an odd shape for a local shell, and deliberately not
 * reshaped for it. Seven of its methods are meaningless here -- password
 * authentication, keepalive, compression, an identity file, a jump host,
 * accepting a host key, a key passphrase -- and are no-ops that say so.
 * The alternative is a second interface and a fork in every caller, which
 * would put "is this local?" into the session registry, the pane
 * container, the tab bar and the status line. A no-op is cheaper than a
 * branch in ten places.
 */
class bssh_pty_session : public bssh_ssh_session {
	Q_OBJECT

public:
	explicit bssh_pty_session(QObject *parent = nullptr);
	~bssh_pty_session() override;

	/*
	 * host and user are ignored; port is ignored. They are the SSH
	 * interface's, and a local shell has no use for any of them -- what
	 * runs is set_shell()'s command, or the user's own shell.
	 */
	void connect_to_host(const QString &host, quint16 port, const QString &user) override;

	void open_shell(int cols, int rows) override;
	void resize(int cols, int rows) override;
	void send(const QByteArray &bytes) override;
	void disconnect_from_host() override;

	/*
	 * What to run. Empty means $SHELL, and /bin/sh when even that is
	 * unset -- the same order login(1) uses, and the one that respects a
	 * user who has chosen a shell.
	 */
	void set_shell(const QString &shell) { m_shell = shell; }
	QString shell() const { return m_shell; }

	/* TERM reaches the child's environment rather than a protocol field. */
	void set_term(const QString &term) override { m_term = term; }

	/*
	 * The seven that do not apply. Named rather than left to a default
	 * implementation so that reading this class tells you what a local
	 * shell does not have, which is most of what SSH is.
	 */
	void authenticate_password(const QString &password) override { Q_UNUSED(password); }
	void set_keepalive(int seconds) override { Q_UNUSED(seconds); }
	void set_compression(bool compression) override { Q_UNUSED(compression); }
	void set_identity_file(const QString &path) override { Q_UNUSED(path); }
	void set_proxy_jump(const QString &spec) override { Q_UNUSED(spec); }
	void accept_host_key() override {}
	void supply_key_passphrase(const QString &passphrase) override { Q_UNUSED(passphrase); }

private:
	void on_readable();
	void reap();

	QString m_shell;
	QString m_term;

	int m_master;
	qint64 m_child;
	QSocketNotifier *m_notifier;
};

#endif /* BSSH_PTY_SESSION_H */
