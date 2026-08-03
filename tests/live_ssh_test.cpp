#include <QFile>
#include <QStandardPaths>
#include <QElapsedTimer>
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
	bool key_unknown = false;
	bool key_changed = false;
};

/*
 * Which half of the run this is. The host key can only be seen to CHANGE
 * across a restart of the server on the same port, so tools/check-ssh.sh
 * runs the binary twice -- once to establish trust, once after swapping
 * the host key -- and the tests that do not belong to a phase skip.
 */
QString phase() {
	return qEnvironmentVariable("BSSH_TEST_SSH_PHASE");
}

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
outcome_t attempt(const QString &identity, bool trust_host_key = true)
{
	outcome_t out;

	bssh_ssh_session *session = bssh_ssh_session::create();
	QEventLoop loop;
	QByteArray received;

	QObject::connect(session, &bssh_ssh_session::bytes_received,
		                [&received](const QByteArray &bytes) { received += bytes; });
	QObject::connect(session, &bssh_ssh_session::host_key_unknown,
		                [&out, session, trust_host_key, &loop](const QString &) {
		out.key_unknown = true;
		/*
		 * Held, not accepted, unless this attempt says otherwise: the whole
		 * point of sec 11's prompt is that nothing continues until somebody
		 * answers it.
		 */
		if (trust_host_key) session->accept_host_key();
		else loop.quit();
	});
	QObject::connect(session, &bssh_ssh_session::host_key_changed,
		                [&out, &loop](const QString &) {
		out.key_changed = true;
		loop.quit();
	});
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

/*
 * The known-hosts store this run is using -- a throwaway one, put in
 * place by tools/check-ssh.sh through XDG_DATA_HOME.
 */
QString known_hosts_file() {
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
		      QStringLiteral("/known_hosts");
}

/*
 * Forgets every trusted host, so a test that needs an UNKNOWN one gets
 * one. Without this these tests would depend on running before the
 * authentication tests above, which accept the key as a side effect --
 * and a test that passes only in a particular order is a test that will
 * fail for a reason nobody can see.
 */
void forget_all_hosts() {
	QFile::remove(known_hosts_file());
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
	if (phase() == QStringLiteral("rekeyed")) {
		QSKIP("the host key has been swapped; authentication is refused by design here");
	}

	const outcome_t out = attempt(test_key());

	QVERIFY2(out.connected, "a key named by IdentityFile must authenticate");
	QVERIFY2(!out.password_asked, "a key that works must not fall back to a password");
	QVERIFY2(!out.passphrase_asked, "an unencrypted key must not ask for a passphrase");
	QVERIFY2(out.shell_replied, "the shell behind the authenticated channel must answer");
}

void live_ssh_test::the_same_key_works_through_a_tilde()
{
	if (!server_available()) return;
	if (phase() == QStringLiteral("rekeyed")) {
		QSKIP("the host key has been swapped; authentication is refused by design here");
	}

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
	if (phase() == QStringLiteral("rekeyed")) {
		QSKIP("the host key has been swapped; authentication is refused by design here");
	}

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
	if (phase() == QStringLiteral("rekeyed")) {
		QSKIP("the host key has been swapped; authentication is refused by design here");
	}

	const outcome_t out = attempt(test_key() + QStringLiteral(".does-not-exist"));

	QVERIFY2(!out.connected, "a key that is not there cannot authenticate");
	/*
	 * Reaching here at all is most of the point: a path libssh cannot
	 * open must end the attempt, not sit in the handshake until the
	 * guard timer gives up.
	 */
}

void live_ssh_test::an_unknown_host_is_held_until_accepted()
{
	if (!server_available()) return;
	if (phase() != QStringLiteral("trust")) {
		QSKIP("this phase runs before the host key is trusted");
	}

	/*
	 * A host nobody has vouched for is announced and the connection is
	 * HELD -- not failed, and emphatically not accepted (sec 11). This
	 * runs against a store that was created seconds ago, so the server
	 * genuinely is unknown rather than merely assumed to be.
	 */
	forget_all_hosts();
	const outcome_t out = attempt(test_key(), false);

	QVERIFY2(out.key_unknown, "an unseen host key must be announced");
	QVERIFY2(!out.connected, "nothing continues until the prompt is answered");
	QVERIFY2(!out.shell_replied, "and no shell is opened behind it");
}

void live_ssh_test::accepting_once_is_remembered()
{
	if (!server_available()) return;
	if (phase() != QStringLiteral("trust")) {
		QSKIP("this phase establishes the trust the next one contradicts");
	}

	forget_all_hosts();

	/* Accepting gets in... */
	const outcome_t first = attempt(test_key(), true);
	QVERIFY(first.key_unknown);
	QVERIFY2(first.connected, "accepting the key must let the connection proceed");

	/* ...and is remembered, so the second time asks nothing. */
	const outcome_t second = attempt(test_key(), false);
	QVERIFY2(!second.key_unknown, "a key accepted once must not be asked about again");
	QVERIFY2(second.connected, "a remembered host connects without a prompt");

	/*
	 * Into BeerSSH's own store, never the user's ~/.ssh/known_hosts
	 * (sec 11): two tools with two different trust prompts writing one
	 * database is how an entry accepted by one becomes an entry the other
	 * never asks about.
	 */
	QVERIFY2(QFile::exists(known_hosts_file()),
	          "the accepted key must land in this app's own store");

	const QString theirs = qEnvironmentVariable("HOME") + QStringLiteral("/.ssh/known_hosts");
	QVERIFY2(!QFile::exists(theirs), "and must not be written to OpenSSH's file");
}

void live_ssh_test::a_changed_host_key_is_refused()
{
	if (!server_available()) return;
	if (phase() != QStringLiteral("rekeyed")) {
		QSKIP("this phase needs the server restarted with a different host key");
	}

	/*
	 * The same host and port, a different key, and a store that already
	 * holds the old one. This is both what a rebuilt server and what an
	 * interception attack look like (sec 11), and the one thing that must
	 * never happen is connecting anyway without saying so.
	 */
	const outcome_t out = attempt(test_key(), false);

	QVERIFY2(out.key_changed, "a host key that changed must be reported as CHANGED");
	QVERIFY2(!out.key_unknown, "a changed key is not an unknown one -- the difference is the warning");
	QVERIFY2(!out.connected, "a changed host key must not connect silently");
	QVERIFY2(!out.shell_replied, "and must not reach a shell");
}

void live_ssh_test::a_black_holed_link_is_noticed()
{
	if (!server_available()) return;
	if (phase() != QStringLiteral("drop")) {
		QSKIP("this phase needs the link black-holed from outside");
	}

	/*
	 * sec 5.3's budget: TCP keepalive after 30s idle, three probes 10s
	 * apart, and TCP_USER_TIMEOUT bounding unacknowledged data at 60s --
	 * "fail in about a minute". Measured against a real black hole it is
	 * 62s; with the socket options removed the same link is still
	 * believed good after 240s, which is what says the options rather
	 * than the defaults are doing the work.
	 *
	 * The window is wide because it is checking that the design holds,
	 * not that a kernel timer is punctual: anything under half a minute
	 * would mean something is failing early, and anything past two and a
	 * half means the socket options stopped being applied.
	 */
	bssh_ssh_session *session = bssh_ssh_session::create();
	QEventLoop loop;
	QElapsedTimer since_ready;
	bool failed = false;

	QObject::connect(session, &bssh_ssh_session::host_key_unknown,
	                  [session](const QString &) { session->accept_host_key(); });
	QObject::connect(session, &bssh_ssh_session::failed,
	                  [&](const QString &) { failed = true; loop.quit(); });
	QObject::connect(session, &bssh_ssh_session::state_changed,
	                  [&](bssh_ssh_session::state_t state) {
		if (state == bssh_ssh_session::CONNECTED) {
			session->open_shell(80, 24);
			/*
			 * The file is how the script knows the session is up and it
			 * is safe to drop the link. Written after the shell rather
			 * than on CONNECTED, so the drop lands on an established
			 * session rather than mid-handshake.
			 */
			QTimer::singleShot(500, [&] {
				QFile ready(qEnvironmentVariable("BSSH_TEST_SSH_READY"));
				if (ready.open(QIODevice::WriteOnly)) ready.close();
				since_ready.start();
			});
			return;
		}
		if (state == bssh_ssh_session::DISCONNECTED && since_ready.isValid()) {
			failed = true;
			loop.quit();
		}
	});

	session->set_identity_file(test_key());
	session->set_keepalive(0);
	session->connect_to_host(QStringLiteral("127.0.0.1"), test_port(),
	                          qEnvironmentVariable("USER"));

	QTimer guard;
	guard.setSingleShot(true);
	QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
	guard.start(200000);
	loop.exec();

	const double seconds = since_ready.isValid() ? since_ready.elapsed() / 1000.0 : -1.0;
	session->disconnect_from_host();
	delete session;

	QVERIFY2(failed,
	          qPrintable(QStringLiteral("a black-holed link must be noticed; still alive after %1s")
	                             .arg(seconds, 0, 'f', 1)));
	QVERIFY2(seconds >= 30.0 && seconds <= 150.0,
	          qPrintable(QStringLiteral("noticed after %1s, outside sec 5.3's budget")
	                             .arg(seconds, 0, 'f', 1)));
}
