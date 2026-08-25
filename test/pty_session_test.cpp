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

void pty_session_test::a_command_runs_instead_of_the_shell() {
	/*
	 * sec 8.6's -e, and the half of it that cannot be got from a shell:
	 * the words are EXECED as given, with nothing between them and the
	 * kernel. `-e echo one two` therefore prints "one two" and not
	 * whatever a shell would have made of it.
	 */
	bssh_pty_session session;
	session.set_command({
		QStringLiteral("/bin/echo"), QStringLiteral("bssh-e-works"), QStringLiteral("*"),
	});

	session.connect_to_host(QString(), 0, QString());
	session.open_shell(80, 24);

	QByteArray seen;
	QVERIFY2(wait_for_output(&session, QByteArrayLiteral("bssh-e-works *"), &seen),
	          qPrintable(QStringLiteral("the command did not run unexpanded; saw: %1")
	                             .arg(QString::fromLocal8Bit(seen.left(200)))));

	/*
	 * The star is the assertion, not decoration. A command handed to a
	 * shell would have had it expanded against the working directory --
	 * which is precisely what xterm's -e does not do, and what a caller
	 * passing a filename with a bracket in it is relying on.
	 */
	session.disconnect_from_host();
}

void pty_session_test::a_command_starts_where_it_was_told_to() {
	/* sec 8.6's --workdir. */
	bssh_pty_session session;
	session.set_command(QStringList(QStringLiteral("/bin/pwd")));
	session.set_working_directory(QStringLiteral("/tmp"));

	session.connect_to_host(QString(), 0, QString());
	session.open_shell(80, 24);

	QByteArray seen;
	QVERIFY2(wait_for_output(&session, QByteArrayLiteral("/tmp"), &seen),
	          qPrintable(QStringLiteral("the child did not start in /tmp; saw: %1")
	                             .arg(QString::fromLocal8Bit(seen.left(200)))));

	session.disconnect_from_host();
}

void pty_session_test::a_command_that_cannot_be_run_says_so() {
	/*
	 * A mistyped -e is the ordinary way to get here, and the child has
	 * already replaced this process's view of the world by the time it
	 * finds out. It writes to the pty -- which is the terminal that asked
	 * -- and exits with the shell's own 127, so a pane that opens and
	 * closes is not the only evidence.
	 */
	bssh_pty_session session;
	session.set_command(QStringList(QStringLiteral("/nonexistent/bssh-not-a-command")));

	QSignalSpy exited(&session, &bssh_ssh_session::shell_exited);

	session.connect_to_host(QString(), 0, QString());
	session.open_shell(80, 24);

	QByteArray seen;
	QVERIFY2(wait_for_output(&session, QByteArrayLiteral("cannot run"), &seen),
	          qPrintable(QStringLiteral("a command that will not run said nothing; saw: %1")
	                             .arg(QString::fromLocal8Bit(seen.left(200)))));

	QElapsedTimer timer;
	timer.start();
	while (exited.isEmpty() && timer.elapsed() < 5000) {
		QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
	}

	QVERIFY2(!exited.isEmpty(), "a child that could not exec must still be reaped");
	QCOMPARE(exited.first().at(0).toInt(), 127);
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
