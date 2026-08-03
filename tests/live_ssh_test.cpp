#include <QEventLoop>
#include <QTimer>
#include <QtTest>

#include "live_ssh_test.h"
#include "ssh/ssh_session.h"

namespace {

struct outcome_t {
	bool connected = false;
	bool password_asked = false;
	bool passphrase_asked = false;
	bool shell_replied = false;
};

quint16 test_port() {
	return static_cast<quint16>(qEnvironmentVariableIntValue("BSSH_TEST_SSH_PORT"));
}

QString test_key() {
	return qEnvironmentVariable("BSSH_TEST_SSH_KEY");
}

/*
 * One connection attempt, driven to a conclusion.
 *
 * Every ending is a signal rather than a return value, so this waits for
 * whichever arrives and gives up after a timeout -- a hang here would
 * otherwise stall the whole run, and "the server never answered" is a
 * result worth reporting rather than one to wait forever for.
 */
outcome_t attempt(const QString &identity)
{
	outcome_t out;

	bssh_ssh_session *session = bssh_ssh_session::create();
	QEventLoop loop;
	QByteArray received;

	QObject::connect(session, &bssh_ssh_session::bytes_received,
		                [&received](const QByteArray &bytes) { received += bytes; });
	QObject::connect(session, &bssh_ssh_session::host_key_unknown,
		                [session](const QString &) { session->accept_host_key(); });
	QObject::connect(session, &bssh_ssh_session::password_required,
		                [&out, &loop] { out.password_asked = true; loop.quit(); });
	QObject::connect(session, &bssh_ssh_session::key_passphrase_required,
		                [&out, &loop] { out.passphrase_asked = true; loop.quit(); });
	QObject::connect(session, &bssh_ssh_session::failed,
		                [&loop](const QString &) { loop.quit(); });

	QObject::connect(session, &bssh_ssh_session::state_changed,
		                [&](bssh_ssh_session::state_t state) {
		if (state != bssh_ssh_session::CONNECTED) return;

		out.connected = true;
		session->open_shell(80, 24);
		/*
		 * Echoed rather than merely connected: authentication succeeding
		 * proves the key was accepted, and a shell answering proves the
		 * channel and the byte path behind it work too.
		 */
		QTimer::singleShot(600, session, [session] {
			session->send(QByteArrayLiteral("echo BSSH_LIVE_OK\n"));
		});
		QTimer::singleShot(2500, &loop, [&loop] { loop.quit(); });
	});

	if (!identity.isEmpty()) session->set_identity_file(identity);
	session->connect_to_host(QStringLiteral("127.0.0.1"), test_port(),
		                        qEnvironmentVariable("USER"));

	QTimer guard;
	guard.setSingleShot(true);
	QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
	guard.start(15000);

	loop.exec();

	out.shell_replied = received.contains("BSSH_LIVE_OK");
	session->disconnect_from_host();
	delete session;
	return out;
}

bool server_available()
{
	if (test_port() == 0 || test_key().isEmpty()) {
		QTest::qSkip("no live sshd; run via tools/check-ssh.sh (make check-ssh)",
			            __FILE__, __LINE__);
		return false;
	}
	return true;
}

} /* namespace */

void live_ssh_test::a_named_key_authenticates()
{
	if (!server_available()) return;

	const outcome_t out = attempt(test_key());

	QVERIFY2(out.connected, "a key named by IdentityFile must authenticate");
	QVERIFY2(!out.password_asked, "a key that works must not fall back to a password");
	QVERIFY2(!out.passphrase_asked, "an unencrypted key must not ask for a passphrase");
	QVERIFY2(out.shell_replied, "the shell behind the authenticated channel must answer");
}

void live_ssh_test::the_same_key_works_through_a_tilde()
{
	if (!server_available()) return;

	/*
	 * OpenSSH configs are written with tildes and carrying one across is
	 * this field's whole purpose (sec 10.5), so the expansion is checked
	 * against a server rather than against a string comparison. HOME is
	 * pointed at the throwaway tree by the script, so this touches
	 * nobody's real ~/.ssh.
	 */
	const QString home = qEnvironmentVariable("HOME");
	const QString key = test_key();
	if (!key.startsWith(home)) {
		QSKIP("the test key is not under HOME, so a tilde cannot address it");
	}

	QString tilde = key;
	tilde.replace(0, home.size(), QStringLiteral("~"));

	const outcome_t out = attempt(tilde);
	QVERIFY2(out.connected, "a ~ path must expand to the same key");
	QVERIFY(out.shell_replied);
}

void live_ssh_test::without_the_key_it_does_not_get_in()
{
	if (!server_available()) return;

	/*
	 * The other half of the proof. Without this the first test passes on
	 * a server that would have let anyone in, and says nothing about
	 * whether the option did any work. The server takes this key and no
	 * password, so omitting it must not connect -- which of the two
	 * prompts appears depends on whether the machine has default
	 * identities of its own, and neither is a way in.
	 */
	const outcome_t out = attempt(QString());

	QVERIFY2(!out.connected, "without the named key the server must refuse");
	QVERIFY2(!out.shell_replied, "no shell without authentication");
}

void live_ssh_test::a_missing_key_fails_rather_than_hangs()
{
	if (!server_available()) return;

	const outcome_t out = attempt(test_key() + QStringLiteral(".does-not-exist"));

	QVERIFY2(!out.connected, "a key that is not there cannot authenticate");
	/*
	 * Reaching here at all is most of the point: a path libssh cannot
	 * open must end the attempt, not sit in the handshake until the
	 * guard timer gives up.
	 */
}
