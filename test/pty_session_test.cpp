#include <QSignalSpy>
#include <QtTest>
#include <QTemporaryDir>

#include <QStandardPaths>
#include <QTemporaryDir>

#include "model/multiplexer.h"
#include "model/profile.h"
#include "model/session.h"
#include "term/emulator.h"
#include "platform/platform.h"
#include "ssh/pty_session.h"
#include "input/input_router.h"

#include <QKeyEvent>
#include <fcntl.h>
#include <unistd.h>
#include <QRegularExpression>
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

void pty_session_test::a_big_send_does_not_block_the_ui_thread() {
	/*
	 * send() used to loop on EAGAIN with a usleep and no bound, on the UI
	 * thread -- the thing collect() a few hundred lines up is carefully
	 * written NOT to do, and for the reason stated there: "This runs on
	 * the UI thread ... would otherwise hang the window."
	 *
	 * The master is O_NONBLOCK and only a READ notifier existed, so
	 * nothing ever said when there was room to write and spinning was the
	 * only recourse. Measured with a standalone probe: with the slave in
	 * RAW mode and nothing draining it, 15,360 bytes of a 1 MB write land
	 * and the loop is still going 3,500ms later. In CANONICAL mode it
	 * completes, because the line discipline discards on overflow -- so
	 * the configuration that hangs is the one every full-screen program
	 * puts its pty into, and the one that hides it is a bare shell.
	 *
	 * Two things are asserted, because either alone is satisfiable by a
	 * broken implementation: send() RETURNS, and everything it was given
	 * still arrives. Dropping the overflow would pass the first; the old
	 * spin would eventually pass the second.
	 */
	bssh_pty_session session;
	session.set_command(QStringList(QStringLiteral("/bin/cat")));

	session.connect_to_host(QString(), 0, QString());
	session.open_shell(80, 24);

	/*
	 * Comfortably past any pty buffer, in lines short enough that the
	 * canonical line discipline is not the thing under test.
	 */
	QByteArray payload;
	for (int i = 0; i < 4000; ++i) {
		payload.append(QByteArrayLiteral("filler-line-of-some-length\n"));
	}
	payload.append(QByteArrayLiteral("bssh-tail-marker\n"));

	QElapsedTimer timer;
	timer.start();
	session.send(payload);
	const qint64 elapsed = timer.elapsed();

	/*
	 * Returns rather than waiting for the child. Generous, because this
	 * is asserting "does not sit in a retry loop" and not a latency
	 * budget -- the old code was still spinning after seconds.
	 */
	QVERIFY2(elapsed < 1000,
	          qPrintable(QStringLiteral("send() took %1ms, which is a wait, not a hand-off")
	                             .arg(elapsed)));

	/*
	 * And the TAIL comes back, which it can only do if the remainder that
	 * did not fit was flushed later from the write notifier.
	 */
	QByteArray seen;
	QVERIFY2(wait_for_output(&session, QByteArrayLiteral("bssh-tail-marker"), &seen),
	          qPrintable(QStringLiteral("the queued remainder never reached the child; "
	                                     "saw %1 bytes").arg(seen.size())));

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

void pty_session_test::the_factory_asks_the_platform_for_the_terminfo_directory() {
	/*
	 * sec 12.3q recorded this half as untestable: `create_local()` asks
	 * the platform for the terminfo directory, and on a desktop the
	 * answer is empty BY DESIGN -- so a factory that asks correctly and
	 * one that never asks produce the same session. "Closing that needs
	 * a platform test double, which this tree does not have."
	 *
	 * It needed one ANSWER forced rather than a whole double, which is
	 * the shape force_touch_primary() has had since sec 6.2 wanted the
	 * idiom decided in a test. That is this file's own lesson about an
	 * untestable condition: the differences are usually smaller than the
	 * label.
	 */
	const QString forced = QStringLiteral("/nonexistent/terminfo-for-a-test");
	bssh_platform::force_terminfo_directory(forced);

	bssh_ssh_session *made = bssh_ssh_session::create_local(QString(), QString());
	QVERIFY(made);

	bssh_pty_session *pty = qobject_cast<bssh_pty_session *>(made);
	QVERIFY2(pty, "create_local did not build a pty session");

	QCOMPARE(pty->terminfo_directory(), forced);

	/*
	 * And the EMPTY answer, which is the desktop's real one and means
	 * "say nothing, let the library look where it always looks". It is a
	 * different state from "nothing is forcing this", which is why the
	 * hook takes a QString and tests isNull -- and a test that only ever
	 * forced a path would not have exercised the distinction the
	 * sentinel exists for.
	 */
	bssh_platform::force_terminfo_directory(QString(QLatin1String("")));

	bssh_ssh_session *quiet = bssh_ssh_session::create_local(QString(), QString());
	QVERIFY(quiet);

	bssh_pty_session *quiet_pty = qobject_cast<bssh_pty_session *>(quiet);
	QVERIFY(quiet_pty);
	QVERIFY2(quiet_pty->terminfo_directory().isEmpty(),
	          "an empty platform answer arrived as something else");

	bssh_platform::force_terminfo_directory(QString());

	delete made;
	delete quiet;
}

void pty_session_test::a_shipped_terminfo_reaches_the_shell() {
	if (QStandardPaths::findExecutable(QStringLiteral("sh")).isEmpty()) {
		QSKIP("no sh, and the child's environment is the point");
	}

	/*
	 * sec 12.3p: Android has no terminfo database, so a local shell
	 * there is handed a TERM naming an entry nothing on the device can
	 * look up. The platform may ship one; this is the wiring that hands
	 * it to the child.
	 *
	 * Asked of the CHILD's environment rather than of the setter,
	 * because the setter agreeing with itself proves nothing about
	 * whether anything reaches an exec'd process -- and the whole
	 * mechanism is one setenv between fork and exec, which is exactly
	 * the code a unit test cannot see.
	 */
	QTemporaryDir shipped;
	QVERIFY(shipped.isValid());

	struct { QString directory; bool expected; } cases[] = {
		{ shipped.path(), true },
		{ QString(),      false },   /* what a desktop's platform returns */
	};

	for (const auto &one : cases) {
		bssh_pty_session session;
		session.set_shell(QStringLiteral("sh"));
		session.set_terminfo_directory(one.directory);

		session.connect_to_host(QString(), 0, QString());
		QCOMPARE(session.state(), bssh_ssh_session::CONNECTED);

		QByteArray seen;
		connect(&session, &bssh_ssh_session::bytes_received,
		         &session, [&seen](const QByteArray &bytes) { seen.append(bytes); });

		session.open_shell(80, 24);

		/*
		 * A marker split so the echoed command cannot satisfy the wait
		 * -- the shell prints what it was given before it runs it, and
		 * a test that matched that would pass without the child ever
		 * having been exec'd.
		 */
		session.send(QByteArrayLiteral(
		        "printf 'TI''=[%s]\\n' \"$TERMINFO\"\n"));

		QTRY_VERIFY_WITH_TIMEOUT(seen.contains(QByteArrayLiteral("TI=[")), 15000);
		QTRY_VERIFY_WITH_TIMEOUT(seen.contains(QByteArrayLiteral("]")), 15000);

		const int open_at = seen.lastIndexOf(QByteArrayLiteral("TI=["));
		const int close_at = seen.indexOf(']', open_at);
		QVERIFY(close_at > open_at);
		const QByteArray value = seen.mid(open_at + 4, close_at - open_at - 4);

		if (one.expected) {
			QCOMPARE(value, one.directory.toLocal8Bit());
		} else {
			QVERIFY2(value.isEmpty(),
			          qPrintable(QStringLiteral("TERMINFO was set to \"%1\" when the "
			                                     "platform ships nothing")
			                             .arg(QString::fromLocal8Bit(value))));
		}
	}
}

void pty_session_test::backspace_crosses_a_wrapped_line() {
	if (QStandardPaths::findExecutable(QStringLiteral("bash")).isEmpty()) {
		QSKIP("bash is not installed, and the line editor is the point");
	}

	/*
	 * Narrow on purpose: 40 columns makes a 60-character line wrap once
	 * whatever the prompt costs, and the assertion below counts what is
	 * left rather than where it sits, so the prompt's length cannot
	 * decide the verdict.
	 */
	const int cols = 40, rows = 10;

	bssh_pty_session session;
	session.set_shell(QStringLiteral("bash"));

	/*
	 * TERM IS SET HERE, and leaving it to the environment is what made
	 * this fail in the container while passing on a desktop.
	 *
	 * readline's redraw across a wrap is built from terminfo: with TERM
	 * unset it has no cursor movement to emit and does not attempt one.
	 * The runner's own TERM is `screen` on the machine this was written
	 * on and UNSET in the CI image, so the test was silently asking two
	 * different questions -- and the one it asked in the container had a
	 * shell that could not have passed.
	 *
	 * xterm rather than the richer values: it is the terminfo every
	 * machine has, and this test is about the line editor rather than
	 * about any capability beyond cursor addressing.
	 *
	 * Measured in the CI image itself rather than inferred. The same
	 * bash 5.2.21, with TERM unset, emits the CANONICAL pattern -- 104
	 * backspaces, `\b \b`, zero cursor-up -- where this machine's emits
	 * `\b ESC[K` and two cursor-ups. Readline is running in both; with
	 * no terminfo it has no cursor movement to emit and no clear-to-end
	 * of line, so it cannot cross the wrap and shows a window of the
	 * line rather than wrapping it. Same shell, same version, and the
	 * environment decides.
	 *
	 * SEEN TO FAIL with this line removed and TERM cleared: the run dies
	 * on the typed count, which is the container's exact failure. That
	 * control was got wrong once -- rebuilt with plain `make`, which
	 * does not build the test binary, so it ran a stale one with this
	 * line still in it and "proved" the opposite.
	 */
	session.set_term(QStringLiteral("xterm"));

	session.connect_to_host(QString(), 0, QString());
	QCOMPARE(session.state(), bssh_ssh_session::CONNECTED);

	/*
	 * Everything the shell says goes through the emulator, which is what
	 * bssh_session does -- the question is whether OUR screen follows
	 * bash's redraw, so a test reading the raw bytes would answer a
	 * different question.
	 */
	bssh_emulator emulator(cols, rows);
	connect(&session, &bssh_ssh_session::bytes_received,
	         &emulator, [&emulator](const QByteArray &bytes) { emulator.feed(bytes); });

	session.open_shell(cols, rows);

	/*
	 * WAIT FOR THE LINE EDITOR, not for a number of milliseconds.
	 *
	 * Both halves of this were got wrong once. The first version slept a
	 * fixed 1200ms and passed here and failed in the CI container at
	 * "only 25 of 60 characters arrived" -- a slower machine turned into
	 * a false verdict about the code. Replacing the sleeps with condition
	 * waits then dropped the wait for the PROMPT, so the characters were
	 * typed before bash reached readline, the kernel's canonical mode
	 * echoed them instead, and the test reproduced the very failure it
	 * exists to rule out. Both mistakes look like flakiness and neither
	 * is: one is a machine speed, the other is a missing precondition.
	 *
	 * So the shell announces itself with a PROMPT, and the marker is
	 * split by quoting -- `PS1="RE""ADY> "` types as RE""ADY> and draws
	 * as READY>, so the string waited for can only be the prompt bash
	 * printed, never the command line echoing back as it is typed. That
	 * distinction cost a run: waiting for `echo bssh-up` matched the
	 * ECHO, which happens before the command has run, so the characters
	 * still went to a busy shell. A marker that a terminal shows twice
	 * is not a marker.
	 *
	 * It also cannot pass vacuously: a bash that came up non-interactive
	 * prints no prompt at all, so the wait expires and this fails rather
	 * than measuring the line discipline and calling it a line editor.
	 * The marker carries no 'z', which is what the count below uses.
	 */
	session.send(QByteArrayLiteral("PS1=\"RE\"\"ADY> \"\n"));
	QTRY_VERIFY_WITH_TIMEOUT(
	        emulator.screen().to_text().contains(QStringLiteral("READY>")), 15000);

	/*
	 * Counted as a DELTA from what is already on screen, because a prompt
	 * carries a hostname and a path and this test cannot know whether
	 * either contains the character it is counting.
	 */
	const int before = emulator.screen().to_text().count(QLatin1Char('z'));

	session.send(QByteArray(60, 'z'));
	QTRY_VERIFY_WITH_TIMEOUT(
	        emulator.screen().to_text().count(QLatin1Char('z')) >= before + 60, 15000);

	/* 55 of the 60, so the erase has to cross the wrap rather than stop at it. */
	session.send(QByteArray(55, '\x7f'));
	QTRY_VERIFY_WITH_TIMEOUT(
	        emulator.screen().to_text().count(QLatin1Char('z')) <= before + 5, 15000);

	const int left = emulator.screen().to_text().count(QLatin1Char('z')) - before;
	QVERIFY2(left <= 5,
	          qPrintable(QStringLiteral("%1 characters still on screen after deleting 55 of "
	                                     "60 -- the erase stopped at the wrap")
	                             .arg(left)));

	session.disconnect_from_host();
}

void pty_session_test::kitty_itself_decodes_what_the_router_sends() {
	/*
	 * A second witness for the kitty keyboard protocol (sec 7.1, tier 2).
	 * The router's tests compare against sequences written from the
	 * specification; a misreading of the specification would be in both
	 * halves and pass. So the keys go through the real router and emulator
	 * into kitty's own `kitten show-key`, on a pty, and what kitty's
	 * decoder says it received is the assertion. Where kitty is not
	 * installed there is no second witness to ask, and it says so.
	 */
	const QString kitten = QStandardPaths::findExecutable(QStringLiteral("kitten"));
	if (kitten.isEmpty()) QSKIP("kitty's kitten is not installed here, so it cannot be asked");

	bssh_emulator emulator(120, 40);
	bssh_input_router router(&emulator);
	bssh_pty_session session;
	session.set_command({ kitten, QStringLiteral("show-key"), QStringLiteral("-m"),
	                      QStringLiteral("kitty") });

	QByteArray seen;
	QObject::connect(&session, &bssh_ssh_session::bytes_received, &emulator,
	                 [&](const QByteArray &bytes) {
		seen.append(bytes);
		emulator.feed(bytes);
	});
	QObject::connect(&emulator, &bssh_emulator::output_ready, &session,
	                 [&] { session.send(emulator.take_output()); });

	session.connect_to_host(QString(), 0, QString());
	session.open_shell(120, 40);

	/* show-key pushes all five flags before it reads a key. */
	QElapsedTimer timer;
	timer.start();
	while (emulator.kitty_keyboard_flags() != 31 && timer.elapsed() < 10000)
		QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
	QCOMPARE(emulator.kitty_keyboard_flags(), 31);

	/* Its report, formatting stripped, as a person would read it. */
	auto decoded = [&](const QString &wanted) {
		QElapsedTimer wait;
		wait.start();
		while (wait.elapsed() < 5000) {
			QString text = QString::fromUtf8(seen);
			text.remove(QRegularExpression(QStringLiteral("\x1b\\[[0-9;?]*[A-Za-z]")));
			text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
			if (text.contains(wanted)) return true;
			QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
		}
		return false;
	};
	auto key = [&](QEvent::Type type, int code, Qt::KeyboardModifiers modifiers, const QString &text) {
		QKeyEvent event(type, code, modifiers, text);
		if (type == QEvent::KeyPress) router.handle_key(&event);
		else router.handle_key_release(&event);
	};

	key(QEvent::KeyPress, Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A"));
	QVERIFY2(decoded(QStringLiteral("shift+a PRESS A")), seen.right(400).constData());
	key(QEvent::KeyRelease, Qt::Key_A, Qt::ShiftModifier, QStringLiteral("A"));
	QVERIFY2(decoded(QStringLiteral("shift+a RELEASE")), seen.right(400).constData());

	key(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier, QString());
	key(QEvent::KeyRelease, Qt::Key_Up, Qt::NoModifier, QString());
	QVERIFY2(decoded(QStringLiteral("UP RELEASE")), seen.right(400).constData());

	key(QEvent::KeyPress, Qt::Key_A, Qt::MetaModifier, QStringLiteral("a"));
	QVERIFY2(decoded(QStringLiteral("super+a PRESS")), seen.right(400).constData());

	key(QEvent::KeyPress, Qt::Key_Enter, Qt::KeypadModifier, QString());
	QVERIFY2(decoded(QStringLiteral("KP_ENTER PRESS")), seen.right(400).constData());

	key(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, QStringLiteral("\r"));
	QVERIFY2(decoded(QStringLiteral("ENTER PRESS")), seen.right(400).constData());

	session.disconnect_from_host();
}

/*
 * A live session moved to another session object (project.md sec 8.4),
 * the way it moves to another process: export, the descriptor duplicated
 * as SCM_RIGHTS would, import, release. The shell keeps running -- the
 * moved session runs a command in it and sees the answer, and the screen
 * from before the move came along.
 */
namespace {

bssh_profile_t local_shell_profile() {
	bssh_profile_t profile = bssh_profile_t::local_shell();
	profile.shell = QStringLiteral("/bin/sh");
	return profile;
}

bool screen_shows(bssh_session *session, const QString &wanted, int timeout_ms = 5000) {
	QElapsedTimer timer;
	timer.start();
	while (timer.elapsed() < timeout_ms) {
		if (session->emulator()->screen().to_text().contains(wanted)) return true;
		QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 50);
	}
	return session->emulator()->screen().to_text().contains(wanted);
}

}  // namespace

void pty_session_test::a_live_local_session_moves_to_another_session() {
	bssh_session *from = new bssh_session(local_shell_profile());
	from->resize(60, 12);
	from->connect_to_host();
	QTRY_COMPARE(from->state(), bssh_ssh_session::CONNECTED);
	from->input_router()->send_text(QStringLiteral("echo before-move\n"));
	QVERIFY(screen_shows(from, QStringLiteral("before-move")));
	QVERIFY(from->transport());
	QCOMPARE(from->transport()->kind(), QStringLiteral("local"));

	QByteArray state;
	QList<int> fds;
	QString why;
	QVERIFY2(from->export_handoff(state, fds, &why), qPrintable(why));
	QCOMPARE(fds.size(), 1);
	QVERIFY(from->is_moving());

	/* What SCM_RIGHTS would have done: a descriptor of the receiver's own. */
	QList<int> received = { ::dup(fds.first()) };
	QVERIFY(received.first() >= 0);

	bssh_session *to = bssh_session::import_handoff(state, received, nullptr, &why);
	QVERIFY2(to, qPrintable(why));
	QCOMPARE(to->state(), bssh_ssh_session::CONNECTED);

	/* The giving side lets go without a hangup -- the shell answering
	 * below, after this session is gone, is the proof it got none. */
	from->release_handoff();
	delete from;

	/* The terminal came with it, and the shell answers the new owner. */
	QVERIFY2(to->emulator()->screen().to_text().contains(QStringLiteral("before-move")),
	          "the screen from before the move did not come along");
	to->input_router()->send_text(QStringLiteral("echo after-$((6*7))\n"));
	QVERIFY2(screen_shows(to, QStringLiteral("after-42")),
	          qPrintable(to->emulator()->screen().to_text()));

	to->disconnect_from_host();
	delete to;
}

void pty_session_test::a_refused_move_leaves_the_session_running() {
	bssh_session *from = new bssh_session(local_shell_profile());
	from->resize(60, 12);
	from->connect_to_host();
	QTRY_COMPARE(from->state(), bssh_ssh_session::CONNECTED);

	QByteArray state;
	QList<int> fds;
	QString why;
	QVERIFY2(from->export_handoff(state, fds, &why), qPrintable(why));

	/* A receiver that cannot read it: refused, and the descriptor still
	 * the caller's to close. */
	QList<int> received = { ::dup(fds.first()) };
	state.chop(state.size() / 2);
	QVERIFY(!bssh_session::import_handoff(state, received, nullptr, &why));
	QVERIFY(!why.isEmpty());
	QVERIFY(::fcntl(received.first(), F_GETFD) >= 0);
	::close(received.first());

	/* The move called off: the session carries on as before. */
	from->resume_handoff();
	QVERIFY(!from->is_moving());
	from->input_router()->send_text(QStringLiteral("echo still-$((2*3))\n"));
	QVERIFY2(screen_shows(from, QStringLiteral("still-6")),
	          qPrintable(from->emulator()->screen().to_text()));

	from->disconnect_from_host();
	delete from;
}

void pty_session_test::a_local_shell_lists_tmux_and_screen_beside_itself() {
	/*
	 * sec 10.2a, locally: the listing runs as a process through /bin/sh,
	 * the same command line an SSH exec request carries. tmux and screen
	 * here are stand-ins on PATH printing what the real ones print, so
	 * the test does not depend on what is installed or running.
	 */
	QTemporaryDir bin;
	QVERIFY(bin.isValid());
	const auto stand_in = [&bin](const QString &name, const QByteArray &body) {
		QFile file(bin.filePath(name));
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write("#!/bin/sh\n" + body);
		file.close();
		file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
	};
	stand_in(QStringLiteral("tmux"), "[ \"$1\" = list-sessions ] && printf 'dev\\t2\\t0\\n'\n");
	stand_in(QStringLiteral("screen"), "printf 'There is a screen on:\\n\\t4242.notes\\t(Detached)\\n'\n");

	const QByteArray path = qgetenv("PATH");
	qputenv("PATH", bin.path().toUtf8() + ':' + path);

	bssh_pty_session session;
	QSignalSpy listed(&session, &bssh_ssh_session::side_output_ready);
	session.run_on_the_side(bssh_mux_listing_command());
	QVERIFY(listed.wait(10000));
	qputenv("PATH", path);

	const QList<bssh_mux_session_t> found =
	        bssh_mux_parse(listed.first().first().toByteArray());
	QCOMPARE(found.size(), 2);
	QCOMPARE(found.at(0).name, QStringLiteral("dev"));
	QCOMPARE(found.at(1).name, QStringLiteral("4242.notes"));
}
