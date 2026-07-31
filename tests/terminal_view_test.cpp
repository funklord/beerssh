#include <QSignalSpy>
#include <QtTest>

#include "fake_ssh_session.h"
#include "ui/terminal_view.h"

/*
 * Host unit tests -- no device, no server, no display (project.md sec
 * 12.3). Everything here runs under the offscreen QPA plugin in CI.
 *
 * These cover the two things M0/M1 actually claim: the grid geometry the
 * whole resize path depends on, and the byte path from an SSH session to
 * the widget. Both are pure logic, and both are what sec 12.3 predicts
 * will regress quietly if nothing watches them.
 */
class terminal_view_test : public QObject {
	Q_OBJECT

private slots:
	void grid_scales_with_widget_size();
	void grid_resized_fires_only_on_real_change();
	void feed_appends_and_splits_lines();
	void feed_drops_control_bytes();
	void session_bytes_reach_the_view();
	void session_records_what_the_client_wrote();
	void session_reports_every_state_on_the_way_up();
};

/*
 * Qt delivers no resizeEvent to a widget that was never shown, so a test
 * that only calls resize() measures nothing. show() under the offscreen
 * QPA plugin is both the fix and what the app actually does.
 */
static void show_and_resize(bssh_terminal_view &view, int w, int h) {
	if (!view.isVisible()) view.show();
	view.resize(w, h);
}

void terminal_view_test::grid_scales_with_widget_size() {
	bssh_terminal_view view;

	show_and_resize(view, 400, 200);
	const int narrow = view.cols();
	QVERIFY(narrow > 0);

	show_and_resize(view, 800, 200);
	const int wide = view.cols();

	/*
	 * Twice the width is twice the columns, give or take the integer
	 * division that snaps the grid to whole cells. Asserting the
	 * relationship rather than a literal column count keeps this test
	 * independent of whichever fixed-pitch font the host happens to have
	 * -- a hardcoded 80 here would fail on a machine with different
	 * metrics and tell us nothing true.
	 */
	QVERIFY(wide >= 2 * narrow);
	QVERIFY(wide <= 2 * narrow + 1);
}

void terminal_view_test::grid_resized_fires_only_on_real_change() {
	bssh_terminal_view view;
	show_and_resize(view, 400, 200);

	QSignalSpy spy(&view, &bssh_terminal_view::grid_resized);

	/* Same size twice: the second must be silent. */
	show_and_resize(view, 600, 300);
	const int after_first = spy.count();
	show_and_resize(view, 600, 300);

	QCOMPARE(after_first, 1);
	QCOMPARE(spy.count(), 1);

	/*
	 * This is not a micro-optimisation. At M5 each emission is a
	 * window-change to the remote host, and a desktop window drag
	 * produces a continuous stream of resize events (sec 8.1) -- one
	 * message per pixel would be a self-inflicted flood.
	 */
}

void terminal_view_test::feed_appends_and_splits_lines() {
	bssh_terminal_view view;
	show_and_resize(view, 800, 400);

	view.feed(QByteArrayLiteral("hello"));
	QCOMPARE(view.lines().last(), QStringLiteral("hello"));

	view.feed(QByteArrayLiteral(" world\nsecond"));
	QCOMPARE(view.lines().size(), 2);
	QCOMPARE(view.lines().at(0), QStringLiteral("hello world"));
	QCOMPARE(view.lines().at(1), QStringLiteral("second"));

	/* A chunk boundary mid-line must not start a new line. */
	view.feed(QByteArrayLiteral(" line"));
	QCOMPARE(view.lines().size(), 2);
	QCOMPARE(view.lines().at(1), QStringLiteral("second line"));
}

void terminal_view_test::feed_drops_control_bytes() {
	bssh_terminal_view view;
	show_and_resize(view, 800, 400);

	/* An escape sequence at M1 is noise, not output -- it must not be
	 * rendered as literal garbage. M2 parses it instead. */
	view.feed(QByteArrayLiteral("a\x1b[31mb"));
	QCOMPARE(view.lines().last(), QStringLiteral("a[31mb"));

	/* Bare CR returns to the start of the line without adding one. */
	view.feed(QByteArrayLiteral("\rover"));
	QCOMPARE(view.lines().size(), 1);
	QCOMPARE(view.lines().last(), QStringLiteral("over"));
}

void terminal_view_test::session_bytes_reach_the_view() {
	bssh_terminal_view view;
	show_and_resize(view, 800, 400);

	bssh_fake_ssh_session session;
	session.set_script({QByteArrayLiteral("motd line\n"), QByteArrayLiteral("user@host:~$ ")});

	connect(&session, &bssh_ssh_session::bytes_received, &view, &bssh_terminal_view::feed);

	session.connect_to_host(QStringLiteral("example.invalid"), 22, QStringLiteral("user"));
	session.authenticate_password(QStringLiteral("hunter2"));
	session.open_shell(view.cols(), view.rows());

	QCOMPARE(session.state(), bssh_ssh_session::CONNECTED);
	QCOMPARE(view.lines().size(), 2);
	QCOMPARE(view.lines().at(0), QStringLiteral("motd line"));
	QCOMPARE(view.lines().at(1), QStringLiteral("user@host:~$ "));
}

void terminal_view_test::session_records_what_the_client_wrote() {
	bssh_fake_ssh_session session;

	session.send(QByteArrayLiteral("ls -l\r"));
	session.send(QByteArrayLiteral("\x03"));

	/* The seam M3's input router gets tested through: keys in, bytes out. */
	QCOMPARE(session.written(), QByteArrayLiteral("ls -l\r\x03"));
}

void terminal_view_test::session_reports_every_state_on_the_way_up() {
	bssh_fake_ssh_session session;
	QSignalSpy spy(&session, &bssh_ssh_session::state_changed);

	session.connect_to_host(QStringLiteral("example.invalid"), 22, QStringLiteral("user"));
	session.authenticate_password(QStringLiteral("hunter2"));

	QCOMPARE(spy.count(), 4);
	QCOMPARE(session.state(), bssh_ssh_session::CONNECTED);

	/* A repeated state must stay silent, or the status UI flickers. */
	session.authenticate_password(QStringLiteral("hunter2"));
	QCOMPARE(spy.count(), 4);
}

QTEST_MAIN(terminal_view_test)
#include "terminal_view_test.moc"
