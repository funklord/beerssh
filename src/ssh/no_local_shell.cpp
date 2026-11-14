#include "ssh/ssh_session.h"

#include <QTimer>

/*
 * Windows (project.md sec 1, best-effort): the local shell is a forkpty()
 * child (ssh/pty_session.cpp), and Windows has neither forkpty nor a
 * POSIX shell to run. Its terminal is ConPTY, which is a port of its own
 * and not yet written. Until it is, a local shell fails to connect with a
 * reason, shown as any other failure is, rather than being a tab that
 * silently never prints -- absent, not faked. SSH hosts work as anywhere.
 */
namespace {

class bssh_no_local_shell : public bssh_ssh_session {
public:
	explicit bssh_no_local_shell(QObject *parent) : bssh_ssh_session(parent) {}

	void connect_to_host(const QString &host, quint16 port, const QString &user) override {
		Q_UNUSED(host);
		Q_UNUSED(port);
		Q_UNUSED(user);
		/* After the caller has connected to the signal, as a real
		 * transport's failure always is. */
		QTimer::singleShot(0, this, [this] {
			emit failed(QStringLiteral("A local shell is not available on Windows yet; "
			                           "connect to a host over SSH instead"));
		});
	}
	void authenticate_password(const QString &password) override { Q_UNUSED(password); }
	void open_shell(int cols, int rows) override { Q_UNUSED(cols); Q_UNUSED(rows); }
	void set_command(const QStringList &command) override { Q_UNUSED(command); }
	void resize(int cols, int rows) override { Q_UNUSED(cols); Q_UNUSED(rows); }
	void send(const QByteArray &bytes) override { Q_UNUSED(bytes); }
	void disconnect_from_host() override {}
	void set_keepalive(int seconds) override { Q_UNUSED(seconds); }
	void set_term(const QString &term) override { Q_UNUSED(term); }
	void set_compression(bool compression) override { Q_UNUSED(compression); }
	void set_identity_file(const QString &path) override { Q_UNUSED(path); }
	void set_proxy_jump(const QString &spec) override { Q_UNUSED(spec); }
	void accept_host_key() override {}
	void supply_key_passphrase(const QString &passphrase) override { Q_UNUSED(passphrase); }
	QString kind() const override { return QStringLiteral("local"); }
};

} /* namespace */

bssh_ssh_session *bssh_ssh_session::create_local(const QString &shell,
                                                  const QString &working_directory,
                                                  QObject *parent) {
	Q_UNUSED(shell);
	Q_UNUSED(working_directory);
	if (bssh_ssh_session *injected = create_from_factory(parent)) return injected;
	return new bssh_no_local_shell(parent);
}
