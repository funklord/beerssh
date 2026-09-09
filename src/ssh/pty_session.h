#ifndef BSSH_PTY_SESSION_H
#define BSSH_PTY_SESSION_H

#include <QStringList>

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

	/*
	 * A command to run INSTEAD of the shell (sec 8.6's -e), argv-style
	 * and already split.
	 *
	 * Run directly, with no shell between: `-e ls *.c` is xterm's and
	 * konsole's spelling and neither hands it to sh, so there is nothing
	 * to quote against and a filename with a space in it survives being
	 * an argument. The caller has already done the splitting -- the
	 * command line handed it to us pre-split, which is the only place
	 * word boundaries are known for certain.
	 *
	 * On the interface rather than only here, because SSH answers the
	 * same request by asking the far end to run it.
	 */
	void set_command(const QStringList &command) override { m_command = command; }
	QStringList command() const { return m_command; }

	/*
	 * Where the child starts (sec 8.6's --workdir); empty inherits this
	 * process's directory, which is what a terminal launched from a shell
	 * has always done.
	 *
	 * Local only, and deliberately not on the interface: SSH has no way
	 * to ask a server to start somewhere, so an option that quietly did
	 * nothing over SSH would be worse than one that refuses. The command
	 * line refuses it.
	 */
	void set_working_directory(const QString &directory) { m_working_directory = directory; }

	/*
	 * A terminfo database to hand the child, or empty to say nothing
	 * (project.md sec 12.3p). Set from bssh_platform by create_local();
	 * a test sets it directly, which is why it is here rather than
	 * looked up inside open_shell().
	 */
	void set_terminfo_directory(const QString &directory) {
		m_terminfo_directory = directory;
	}
	QString terminfo_directory() const { return m_terminfo_directory; }
	QString working_directory() const { return m_working_directory; }

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
	void on_writable();
	void flush_pending();
	void reap();

	QString m_shell;
	QStringList m_command;
	QString m_working_directory;
	QString m_terminfo_directory;
	QString m_term;

	int m_master;
	qint64 m_child;
	QSocketNotifier *m_notifier;

	/*
	 * What send() could not hand to the kernel yet, and the notifier that
	 * says when there is room for more.
	 *
	 * The master is O_NONBLOCK, so a full pty answers EAGAIN. send() used
	 * to sit in a usleep loop until it drained -- ON THE UI THREAD, which
	 * this file forbids a few hundred lines up for exactly this reason:
	 * "Never an unbounded waitpid. This runs on the UI thread ... would
	 * otherwise hang the window."
	 *
	 * Measured: with the slave in RAW mode and nothing reading it -- what
	 * any full-screen program does to its pty -- 15,360 bytes of a 1 MB
	 * paste reach the buffer and the loop is still spinning 3,500ms
	 * later. In canonical mode it completes, because the line discipline
	 * discards on overflow; so the case that hangs is the one a TUI is
	 * always in.
	 */
	QByteArray m_pending;
	QSocketNotifier *m_write_notifier;
};

#endif /* BSSH_PTY_SESSION_H */
