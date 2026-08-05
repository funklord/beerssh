#include <QSignalSpy>
#include <QtTest>

#include "model/profile.h"
#include "ssh/pty_session.h"
#include "pty_session_test.h"

namespace {

/*
 * Waits for the shell to say something containing `wanted`.
 *
 * Output arrives in whatever chunks the kernel hands over, so a test that
 * asserted on one signal would be asserting on a buffer boundary. The
 * bytes are accumulated and the whole is searched, which is also what the
 * emulator does with them.
 */
bool wait_for_output(bssh_pty_session *session, const QByteArray &wanted, QByteArray *seen) {
	QSignalSpy output(session, &bssh_ssh_session::bytes_received);

	QElapsedTimer timer;
	timer.start();
	while (timer.elapsed() < 5000) {
		QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
		while (!output.isEmpty()) seen->append(output.takeFirst().at(0).toByteArray());
		if (seen->contains(wanted)) return true;
	}
	return false;
}

} /* namespace */

void pty_session_test::a_shell_starts_and_echoes_what_it_is_given() {
	bssh_pty_session session;
	session.set_shell(QStringLiteral("/bin/sh"));

	QSignalSpy states(&session, &bssh_ssh_session::state_changed);
	session.connect_to_host(QString(), 0, QString());

	/*
	 * CONNECTED without resolving, authenticating or asking anything --
	 * there is nothing to connect to. The state still moves, because the
	 * status line, the tab dot and the reconnection machine all read it
	 * and would otherwise show a working shell as disconnected.
	 */
	QCOMPARE(session.state(), bssh_ssh_session::CONNECTED);
	QVERIFY(!states.isEmpty());

	session.open_shell(80, 24);

	/* A command, and its output coming back through the same interface
	 * the SSH transport uses. */
	session.send(QByteArrayLiteral("echo bssh-pty-works\n"));

	QByteArray seen;
	QVERIFY2(wait_for_output(&session, QByteArrayLiteral("bssh-pty-works"), &seen),
	          qPrintable(QStringLiteral("the shell never echoed; saw: %1")
	                             .arg(QString::fromLocal8Bit(seen.left(200)))));

	session.disconnect_from_host();
}

void pty_session_test::exiting_the_shell_is_not_a_dropped_connection() {
	/*
	 * sec 5.3's distinction, and the reason the pty reports EIO as an
	 * exit rather than a failure: a reconnection machine that cannot tell
	 * them apart reopens a shell somebody deliberately typed `exit` into,
	 * forever.
	 */
	bssh_pty_session session;
	session.set_shell(QStringLiteral("/bin/sh"));

	QSignalSpy exited(&session, &bssh_ssh_session::shell_exited);
	QSignalSpy failures(&session, &bssh_ssh_session::failed);

	session.connect_to_host(QString(), 0, QString());
	session.open_shell(80, 24);
	session.send(QByteArrayLiteral("exit 3\n"));

	QElapsedTimer timer;
	timer.start();
	while (exited.isEmpty() && timer.elapsed() < 5000) {
		QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
	}

	QVERIFY2(!exited.isEmpty(), "the shell exiting must be reported as an exit");
	QCOMPARE(exited.first().at(0).toInt(), 3);

	/* And NOT as a failure, which is what would trigger a reconnect. */
	QVERIFY2(failures.isEmpty(), "a deliberate exit must not look like a dropped link");
	QCOMPARE(session.state(), bssh_ssh_session::DISCONNECTED);
}

void pty_session_test::a_resize_reaches_the_shell() {
	/*
	 * The local equivalent of sec 7's window-change: TIOCSWINSZ, which
	 * the kernel turns into SIGWINCH for the foreground process group.
	 * Asked of the shell itself rather than of the ioctl's return value,
	 * because what matters is that the program on the far side sees it.
	 */
	bssh_pty_session session;
	session.set_shell(QStringLiteral("/bin/sh"));

	session.connect_to_host(QString(), 0, QString());
	session.open_shell(80, 24);
	session.resize(132, 40);

	session.send(QByteArrayLiteral("stty size\n"));

	QByteArray seen;
	QVERIFY2(wait_for_output(&session, QByteArrayLiteral("40 132"), &seen),
	          qPrintable(QStringLiteral("the shell did not see 40x132; saw: %1")
	                             .arg(QString::fromLocal8Bit(seen.left(200)))));

	session.disconnect_from_host();
}

void pty_session_test::a_local_profile_is_valid_without_a_host() {
	/*
	 * is_valid() gates whether anything will open a profile at all, and
	 * it asked for a host. A local shell has none, so without this the
	 * desktop's default session could be constructed and never opened.
	 */
	bssh_profile_t local;
	local.local = true;
	local.name = QStringLiteral("Local shell");

	QVERIFY2(local.is_valid(), "a local profile has no host and must still be valid");

	/* And an SSH profile with no host is still invalid, which is the
	 * check this one must not have weakened. */
	bssh_profile_t remote;
	QVERIFY2(!remote.is_valid(), "an SSH profile still needs a host");
}
