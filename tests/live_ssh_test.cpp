#include <QFile>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QTimer>
#include <QtTest>

#include "live_ssh_test.h"
#include "ssh/ssh_session.h"
#include "model/session.h"
#include "model/conn_manager.h"

namespace {

struct outcome_t {
	bool connected = false;
	bool password_asked = false;
	bool passphrase_asked = false;
	bool shell_replied = false;
	bool key_unknown = false;
	bool key_changed = false;
	bool key_revoked = false;
	QString reason;
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
outcome_t attempt(const QString &identity, bool trust_host_key = true,
	                 const QString &jump = QString())
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
	QObject::connect(session, &bssh_ssh_session::host_key_revoked,
		                [&out, &loop](const QString &) {
		out.key_revoked = true;
		loop.quit();
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
		                [&out, &loop](const QString &reason) {
		out.reason = reason;
		loop.quit();
	});

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
	if (!jump.isEmpty()) session->set_proxy_jump(jump);
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

/*
 * The user's OpenSSH file is consulted read-only during verification
 * (sec 11), so a test that needs a genuinely UNKNOWN host has to put it
 * out of reach -- otherwise it is testing a host something already
 * vouches for. Moving it aside rather than deleting it keeps the file the
 * later test needs.
 */
void hide_openssh_known_hosts(bool hidden) {
	const QString real = qEnvironmentVariable("BSSH_TEST_OPENSSH_KNOWN_HOSTS");
	if (real.isEmpty()) return;

	const QString aside = real + QStringLiteral(".aside");
	if (hidden) {
		QFile::remove(aside);
		QFile::rename(real, aside);
	} else if (QFile::exists(aside)) {
		QFile::remove(real);
		QFile::rename(aside, real);
	}
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
	hide_openssh_known_hosts(true);
	const outcome_t out = attempt(test_key(), false);
	hide_openssh_known_hosts(false);

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
	hide_openssh_known_hosts(true);

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
	/*
	 * Recorded somewhere the next attempt can find it. Which file that is
	 * depends on whether ~/.ssh is reachable -- it is preferred, so the
	 * user's other client sees the same decision, and this app's own
	 * store is the fallback (sec 11). Either satisfies "remembered"; the
	 * WHERE is the next test's business.
	 */
	const QString theirs = qEnvironmentVariable("HOME") + QStringLiteral("/.ssh/known_hosts");
	QVERIFY2(QFile::exists(known_hosts_file()) || QFile::exists(theirs),
	          "an accepted key must be recorded somewhere");

	hide_openssh_known_hosts(false);
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

void live_ssh_test::a_dropped_session_reconnects_when_the_link_returns()
{
	if (!server_available()) return;
	if (phase() != QStringLiteral("reconnect")) {
		QSKIP("this phase needs the link dropped and then restored from outside");
	}

	/*
	 * The whole loop, end to end: a live session, a link whose packets
	 * vanish, dead-peer detection noticing, the backoff scheduling a
	 * retry, and a connection actually coming back when the link does.
	 *
	 * bssh_conn_manager's schedule is a pure function and is unit-tested
	 * as one (sec 12.3). What no unit test can show is that it ENGAGES on
	 * a real drop -- the failure mode being a session that goes quiet and
	 * stays quiet -- or that what it reconnects is usable afterwards.
	 *
	 * Key authentication is what makes this seamless: the password was
	 * consumed at first connect and deliberately not kept (sec 11), so a
	 * password-authenticated session could only reconnect as far as
	 * asking again. This host authenticates with a named key, which is
	 * the case sec 5.3 says makes reconnection silent.
	 */
	bssh_profile_t profile;
	profile.name = QStringLiteral("drop-test");
	profile.host = QStringLiteral("127.0.0.1");
	profile.port = test_port();
	profile.user = qEnvironmentVariable("USER");
	profile.identity_file = test_key();

	bssh_session session(profile);
	bssh_conn_manager manager(&session);
	manager.set_auto_reconnect(true);

	QEventLoop loop;
	QElapsedTimer since_ready;
	bool scheduled = false;
	int reconnects = 0;
	bool back = false;

	QObject::connect(&manager, &bssh_conn_manager::reconnect_scheduled,
	                  [&](int, int) {
		if (scheduled) return;
		scheduled = true;

		/*
		 * Telling the script the drop has been noticed is what lets it
		 * restore the link at the right moment. Restoring it earlier
		 * would prove nothing about detection; leaving it dropped would
		 * only ever prove the backoff can count.
		 */
		QFile noticed(qEnvironmentVariable("BSSH_TEST_SSH_NOTICED"));
		if (noticed.open(QIODevice::WriteOnly)) noticed.close();
	});
	QObject::connect(&manager, &bssh_conn_manager::reconnect_started,
	                  [&](int) { ++reconnects; });

	QObject::connect(&session, &bssh_session::state_changed, [&] {
		if (session.state() != bssh_ssh_session::CONNECTED) return;

		if (!since_ready.isValid()) {
			/* First connection: tell the script it may drop the link. */
			QFile ready(qEnvironmentVariable("BSSH_TEST_SSH_READY"));
			if (ready.open(QIODevice::WriteOnly)) ready.close();
			since_ready.start();
			return;
		}

		back = true;
		loop.quit();
	});

	session.connect_to_host();

	QTimer guard;
	guard.setSingleShot(true);
	QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
	guard.start(220000);
	loop.exec();

	QVERIFY2(since_ready.isValid(), "the session never came up in the first place");
	QVERIFY2(scheduled, "a dropped link must schedule a reconnection, not go quiet");
	QVERIFY2(reconnects > 0, "the scheduled reconnection must actually be attempted");
	QVERIFY2(back, "the session must come back once the link does");
	QCOMPARE(session.state(), bssh_ssh_session::CONNECTED);
}

void live_ssh_test::a_host_openssh_already_trusts_is_not_asked_about()
{
	if (!server_available()) return;
	if (phase() != QStringLiteral("trust")) {
		QSKIP("this phase has a server whose key is the one OpenSSH recorded");
	}

	/*
	 * It is the same person deciding about the same hosts, so a
	 * fingerprint they already checked in OpenSSH should not be put to
	 * them again (sec 11). Two costs to asking: the prompt fatigue that
	 * makes people stop reading prompts, and -- worse -- a CHANGED key
	 * reading as a first contact, because with no previous key on file
	 * there is nothing to notice a change against.
	 *
	 * The script hands us the file OpenSSH itself wrote, hashed by the
	 * system's own HashKnownHosts default, so this also checks libssh
	 * reads that format rather than quietly finding nothing.
	 */
	const QString openssh_file = qEnvironmentVariable("BSSH_TEST_OPENSSH_KNOWN_HOSTS");
	if (openssh_file.isEmpty() || !QFile::exists(openssh_file)) {
		QSKIP("no OpenSSH-written known_hosts to consult");
	}

	hide_openssh_known_hosts(false);
	forget_all_hosts();
	const outcome_t out = attempt(test_key(), false);

	QVERIFY2(!out.key_unknown,
	          "a host OpenSSH already trusts must not be presented as unknown");
	QVERIFY2(out.connected, "and must connect without a prompt");

	/*
	 * Read, never written. Their file is hashed by system default and
	 * libssh writes plaintext, so appending to it would silently undo
	 * the setting that keeps a leaked copy from listing someone's
	 * machines.
	 */
	QFile theirs(openssh_file);
	QVERIFY(theirs.open(QIODevice::ReadOnly));
	const QByteArray after = theirs.readAll();
	theirs.close();

	/* Trimmed: the snapshot travels through the environment, which drops
	 * the trailing newline, and that is not a change to the file. */
	QCOMPARE(after.trimmed(), qgetenv("BSSH_TEST_OPENSSH_SNAPSHOT").trimmed());
	QVERIFY2(!after.contains("\n[127.0.0.1]"),
	          "nothing unhashed may be appended to the user's file");
}

void live_ssh_test::accepting_writes_a_hashed_entry_openssh_can_use()
{
	if (!server_available()) return;
	if (phase() != QStringLiteral("trust")) {
		QSKIP("this phase has the server whose key is being recorded");
	}

	const QString theirs = qEnvironmentVariable("BSSH_TEST_OPENSSH_KNOWN_HOSTS");
	if (theirs.isEmpty()) QSKIP("no OpenSSH known_hosts location for this run");

	/*
	 * Accepting a host records it where the user's other SSH client will
	 * see it (sec 11) -- one store, because the same person is deciding
	 * about the same hosts and a decision only half their tools know
	 * about is one they have to make twice.
	 *
	 * Two things have to be true of what gets written, and neither is
	 * obvious from the code: it must be HASHED, because libssh writes
	 * host names in the clear and the system hashes them on purpose; and
	 * it must be something OpenSSH can actually read back, which is
	 * checked in check-ssh.sh by having ssh(1) connect against it with
	 * StrictHostKeyChecking=yes -- a mode that prompts for nothing and so
	 * can only succeed on a match.
	 */
	/*
	 * The host has to be UNKNOWN for accepting to record anything, and it
	 * is currently in this very file. So the file is replaced with one
	 * unrelated entry: enough to make the host unknown again, and enough
	 * that "appends rather than rewrites" has something to be true about.
	 *
	 * Hiding the file instead -- the trick the first-contact tests use --
	 * cannot work here, because restoring it afterwards would discard the
	 * entry this test exists to inspect. That is exactly what the first
	 * version did.
	 */
	const QByteArray unrelated =
	        QByteArrayLiteral("|1|dGVzdHNhbHRzYWx0c2FsdHNhbA==|dGVzdGhhc2hoYXNoaGFzaGhhcw== "
	                           "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIH0000000000000000000000"
	                           "0000000000000000000\n");
	{
		QFile seed(theirs);
		QVERIFY(seed.open(QIODevice::WriteOnly | QIODevice::Truncate));
		seed.write(unrelated);
		seed.close();
	}
	const QList<QByteArray> existing = QList<QByteArray>() << unrelated.trimmed();

	forget_all_hosts();
	const outcome_t out = attempt(test_key(), true);
	QVERIFY2(out.key_unknown, "the host had to be unknown for this to record anything");

	QFile after(theirs);
	QVERIFY(after.open(QIODevice::ReadOnly));
	const QList<QByteArray> lines = after.readAll().split('\n');
	after.close();

	int added = 0;
	for (const QByteArray &line : lines) {
		if (line.trimmed().isEmpty()) continue;
		if (existing.contains(line)) continue;
		++added;
		QVERIFY2(line.startsWith("|1|"),
		          "an entry written into the user's file must be hashed like the rest");
		QVERIFY2(!line.contains("127.0.0.1"),
		          "a hashed entry must not carry the host name in the clear");
	}
	QCOMPARE(added, 1);

	/*
	 * Append only. Somebody else's file is not ours to rewrite, and the
	 * entries already in it must survive untouched.
	 */
	for (const QByteArray &line : existing) {
		if (line.trimmed().isEmpty()) continue;
		QVERIFY2(lines.contains(line), "an existing entry must not be disturbed");
	}
}

void live_ssh_test::a_revoked_key_is_refused_not_offered()
{
	if (!server_available()) return;
	if (phase() != QStringLiteral("trust")) {
		QSKIP("this phase has the server whose key is being revoked");
	}

	const QString theirs = qEnvironmentVariable("BSSH_TEST_OPENSSH_KNOWN_HOSTS");
	if (theirs.isEmpty()) QSKIP("no known_hosts location for this run");

	/*
	 * libssh reports a revoked key as UNKNOWN, so without the check this
	 * covers the app would put a friendly "trust this new host?" prompt
	 * in front of a key somebody went out of their way to revoke -- and
	 * accepting would write it back as trusted. Reading OpenSSH's file
	 * (sec 11) while disagreeing about what a line MEANS is worse than
	 * not reading it at all.
	 *
	 * The entry is written the way ssh-keygen -R would leave it: the
	 * host's own line, marked. Hashed, because that is what is actually
	 * in these files, and a revocation nobody can match is a revocation
	 * that does nothing.
	 */
	forget_all_hosts();
	hide_openssh_known_hosts(true);
	const outcome_t discover = attempt(test_key(), true);
	hide_openssh_known_hosts(false);
	QVERIFY2(discover.key_unknown, "the host had to be recorded before it can be revoked");

	/* Whatever accepting wrote is now the entry to revoke. */
	QFile written(theirs);
	QVERIFY(written.open(QIODevice::ReadOnly));
	QList<QByteArray> lines = written.readAll().split('\n');
	written.close();

	QByteArray ours;
	for (const QByteArray &line : lines) {
		if (line.startsWith("|1|")) ours = line.trimmed();
	}
	QVERIFY2(!ours.isEmpty(), "the accepted key should have been recorded hashed");

	QFile revoke(theirs);
	QVERIFY(revoke.open(QIODevice::WriteOnly | QIODevice::Truncate));
	revoke.write("@revoked " + ours + "\n");
	revoke.close();

	forget_all_hosts();
	const outcome_t out = attempt(test_key(), true);

	/*
	 * Put the file back before asserting. This test deliberately leaves a
	 * revocation in a file other checks read afterwards, and a test that
	 * changes the world and then fails leaves the next one failing for
	 * its reason rather than its own.
	 */
	QFile restore(theirs);
	if (restore.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		restore.write(ours + "\n");
		restore.close();
	}

	QVERIFY2(out.key_revoked, "a key marked @revoked must be reported as revoked");
	QVERIFY2(!out.key_unknown,
	          "and must NOT be offered as a new host -- accepting would re-trust it");
	QVERIFY2(!out.connected, "a revoked key must not connect");
}

void live_ssh_test::a_host_is_reachable_through_a_jump_host()
{
	if (!server_available()) return;
	if (phase() == QStringLiteral("rekeyed")) {
		QSKIP("the host key has been swapped; authentication is refused by design here");
	}

	/*
	 * The one sshd is both bastion and target: it carries a direct-tcpip
	 * channel to itself, and a SECOND, independent SSH session runs
	 * inside that channel. Two hosts would prove nothing this does not --
	 * the tunnel cannot tell where the far end is -- and needing two
	 * servers would make this check something nobody runs.
	 *
	 * Trust first, deliberately. The bastion is verified and never
	 * prompted about, so its key has to be recorded before it can be used
	 * as one; doing it here rather than relying on an earlier test keeps
	 * this from passing only in a particular order.
	 */
	const outcome_t direct = attempt(test_key());
	QVERIFY2(direct.connected, "the server has to be reachable directly first");

	const QString jump = QStringLiteral("%1@127.0.0.1:%2")
	                             .arg(qEnvironmentVariable("USER"))
	                             .arg(test_port());

	/*
	 * And a bastion that is not there must NOT connect. Without this the
	 * check below passes on a build that ignores the jump host entirely
	 * and simply dials the target -- which is exactly what it looked like
	 * when this was written, since the target here is the same machine.
	 */
	const outcome_t nowhere =
	        attempt(test_key(), true, QStringLiteral("127.0.0.1:1"));
	QVERIFY2(!nowhere.connected,
	          "a jump host that is not listening must stop the connection");

	const outcome_t out = attempt(test_key(), true, jump);

	QVERIFY2(out.connected, qPrintable(QStringLiteral(
	                 "a session must run inside the jump host's channel: %1").arg(out.reason)));
	QVERIFY2(out.shell_replied,
	          "bytes must travel both ways through the tunnel, not just the handshake");
	QVERIFY2(!out.password_asked,
	          "the target's own key authentication must happen inside the tunnel");
	/*
	 * The target's key was already trusted by the direct attempt above,
	 * and it is the SAME key seen through the tunnel -- which is the
	 * point. A jump host that terminated the session would present its
	 * own key here, and this would come back unknown.
	 */
	QVERIFY2(!out.key_unknown,
	          "the key seen through the tunnel must be the target's own");
}

void live_ssh_test::an_unknown_jump_host_is_refused_rather_than_prompted_about()
{
	if (!server_available()) return;
	if (phase() == QStringLiteral("rekeyed")) {
		QSKIP("the host key has been swapped; there is no trusted state to remove");
	}

	/*
	 * A trust-on-first-use prompt for a machine the user did not ask to
	 * visit, while the connection they DID ask for waits behind it, is a
	 * prompt nobody can answer sensibly -- so an unknown bastion is a
	 * refusal with a reason. What this guards against is the easy
	 * alternative: connecting to it anyway.
	 */
	hide_openssh_known_hosts(true);
	forget_all_hosts();

	const QString jump = QStringLiteral("%1@127.0.0.1:%2")
	                             .arg(qEnvironmentVariable("USER"))
	                             .arg(test_port());
	const outcome_t out = attempt(test_key(), true, jump);

	hide_openssh_known_hosts(false);

	QVERIFY2(!out.connected, "an unknown jump host must not be used anyway");
	QVERIFY2(!out.key_unknown,
	          "an unknown jump host must be refused, not prompted about");
	QVERIFY2(out.reason.contains(QStringLiteral("jump host")),
	          qPrintable(QStringLiteral("the reason must name what went wrong: %1")
	                             .arg(out.reason)));
}

namespace {

/*
 * Opens a shell at a given size, runs a command, and returns everything
 * that came back. Separate from attempt() because the question here is
 * not whether the connection works -- that is settled above -- but what
 * the far end believes about the terminal it is drawing into.
 */
QByteArray shell_output(int cols, int rows, const QByteArray &command,
                         int resize_cols = 0, int resize_rows = 0)
{
	bssh_ssh_session *session = bssh_ssh_session::create();
	QEventLoop loop;
	QByteArray received;

	QObject::connect(session, &bssh_ssh_session::bytes_received,
		                [&received](const QByteArray &bytes) { received += bytes; });
	QObject::connect(session, &bssh_ssh_session::host_key_unknown,
		                [session](const QString &) { session->accept_host_key(); });
	QObject::connect(session, &bssh_ssh_session::failed,
		                [&loop](const QString &) { loop.quit(); });

	QObject::connect(session, &bssh_ssh_session::state_changed,
		                [&](bssh_ssh_session::state_t state) {
		if (state != bssh_ssh_session::CONNECTED) return;

		session->open_shell(cols, rows);

		/*
		 * The resize, when one is asked for, goes AFTER the shell is up
		 * and before the question -- which is the order a user produces
		 * by rotating the phone while looking at a prompt.
		 */
		if (resize_cols > 0) {
			QTimer::singleShot(600, session, [session, resize_cols, resize_rows] {
				session->resize(resize_cols, resize_rows);
			});
		}

		QTimer::singleShot(1200, session, [session, command] {
			session->send(command + QByteArrayLiteral("\n"));
		});
		QTimer::singleShot(3000, &loop, [&loop] { loop.quit(); });
	});

	session->set_identity_file(test_key());
	session->connect_to_host(QStringLiteral("127.0.0.1"), test_port(),
		                        qEnvironmentVariable("USER"));

	QTimer guard;
	guard.setSingleShot(true);
	QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
	guard.start(15000);
	loop.exec();

	session->disconnect_from_host();
	delete session;
	return received;
}

} /* namespace */

void live_ssh_test::the_remote_is_told_the_size_it_was_given()
{
	if (!server_available()) return;
	if (phase() == QStringLiteral("rekeyed")) {
		QSKIP("the host key has been swapped; authentication is refused by design here");
	}

	/*
	 * Deliberately not 80x24: those are the defaults a pty falls back to,
	 * so a size that never arrived would look exactly like a size that
	 * did. 97x37 can only be there because this app sent it.
	 */
	const QByteArray out = shell_output(97, 37, QByteArrayLiteral("stty size"));

	QVERIFY2(out.contains("37 97"),
	          qPrintable(QStringLiteral("the remote reported: %1")
	                             .arg(QString::fromUtf8(out.simplified()))));
}

void live_ssh_test::a_resize_reaches_the_remote()
{
	if (!server_available()) return;
	if (phase() == QStringLiteral("rekeyed")) {
		QSKIP("the host key has been swapped; authentication is refused by design here");
	}

	/*
	 * A window-change on a LIVE shell, which is the case that matters:
	 * the initial size rides along with the pty request and could work
	 * while the update that follows every rotation and every pane drag
	 * goes nowhere.
	 */
	const QByteArray out = shell_output(97, 37, QByteArrayLiteral("stty size"), 61, 19);

	QVERIFY2(out.contains("19 61"),
	          qPrintable(QStringLiteral("the remote reported: %1")
	                             .arg(QString::fromUtf8(out.simplified()))));
}
