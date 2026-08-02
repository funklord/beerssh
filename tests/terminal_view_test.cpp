#include <QSignalSpy>
#include <QtTest>

#include "terminal_view_test.h"

#include "fake_ssh_session.h"
#include "input/input_router.h"
#include "term/emulator.h"
#include "term/font_catalog.h"
#include "ui/terminal_view.h"


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

void terminal_view_test::renders_without_a_screen_attached() {
	bssh_terminal_view view;
	show_and_resize(view, 400, 200);

	/*
	 * Before any session exists there is no screen. Painting must still
	 * work rather than dereferencing its way out of the process.
	 */
	QVERIFY(view.screen() == nullptr);
	const QPixmap frame = view.grab();
	QCOMPARE(frame.size(), view.size());
}

void terminal_view_test::renders_cell_colours_from_the_screen() {
	bssh_terminal_view view;
	show_and_resize(view, 400, 200);

	bssh_screen screen(20, 5);
	bssh_cell_t cell;
	cell.bg.kind = bssh_color_t::INDEXED;
	cell.bg.index = 1; /* red, in the xterm palette */
	screen.set_cell(0, 0, cell);
	screen.set_cursor_visible(false);

	view.attach_screen(&screen);
	const QImage frame = view.grab().toImage();

	/*
	 * Sampling one pixel inside the first cell is enough: the point is
	 * that an indexed colour reached the painter at all, not what the
	 * exact palette entry is. The cursor is hidden so it cannot be the
	 * thing being sampled.
	 */
	const QColor painted = frame.pixelColor(2, 2);
	QVERIFY(painted.red() > 100);
	QVERIFY(painted.green() < 60);
	QVERIFY(painted.blue() < 60);
}

void terminal_view_test::session_bytes_reach_the_screen_through_the_emulator() {
	bssh_terminal_view view;
	show_and_resize(view, 800, 400);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());

	bssh_fake_ssh_session session;
	session.set_script({QByteArrayLiteral("motd line\r\n"), QByteArrayLiteral("user@host:~$ ")});

	connect(&session, &bssh_ssh_session::bytes_received, &emulator, &bssh_emulator::feed);

	session.connect_to_host(QStringLiteral("example.invalid"), 22, QStringLiteral("user"));
	session.authenticate_password(QStringLiteral("hunter2"));
	session.open_shell(view.cols(), view.rows());

	QCOMPARE(session.state(), bssh_ssh_session::CONNECTED);

	/* End to end: fake server bytes, real emulator, the grid the view draws. */
	const QString text = emulator.screen().to_text();
	QCOMPARE(text.section(QLatin1Char('\n'), 0, 0), QStringLiteral("motd line"));
	QCOMPARE(text.section(QLatin1Char('\n'), 1, 1), QStringLiteral("user@host:~$"));
}

void terminal_view_test::ime_commit_text_becomes_input() {
	bssh_terminal_view view;
	show_and_resize(view, 400, 200);

	bssh_emulator emulator(view.cols(), view.rows());
	bssh_input_router router(&emulator);
	view.attach_screen(&emulator.screen());
	view.attach_input_router(&router);

	/*
	 * Android delivers some keys only as IME commit text (sec 6.1), and
	 * desktop dead keys arrive the same way -- "é" typed as a compose
	 * sequence is one commit string, not a key event.
	 */
	QInputMethodEvent event;
	event.setCommitString(QStringLiteral("é"));
	QCoreApplication::sendEvent(&view, &event);

	QCOMPARE(emulator.take_output(), QString::fromUtf8("é").toUtf8());

	/* Pre-edit text is not input: it must produce nothing. */
	QInputMethodEvent preedit(QStringLiteral("ni"), QList<QInputMethodEvent::Attribute>());
	QCoreApplication::sendEvent(&view, &preedit);
	QCOMPARE(emulator.take_output(), QByteArray());
}

/* A drag, as three events, at cell coordinates rather than pixels. */
static void drag(bssh_terminal_view &view, int from_col, int from_row,
                  int to_col, int to_row, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
	const QPoint from(from_col * view.cell_width() + 1, from_row * view.cell_height() + 1);
	const QPoint to(to_col * view.cell_width() + 1, to_row * view.cell_height() + 1);

	QMouseEvent press(QEvent::MouseButtonPress, from, view.mapToGlobal(from),
	                   Qt::LeftButton, Qt::LeftButton, modifiers);
	QMouseEvent move(QEvent::MouseMove, to, view.mapToGlobal(to),
	                  Qt::NoButton, Qt::LeftButton, modifiers);
	QMouseEvent release(QEvent::MouseButtonRelease, to, view.mapToGlobal(to),
	                     Qt::LeftButton, Qt::NoButton, modifiers);

	QCoreApplication::sendEvent(&view, &press);
	QCoreApplication::sendEvent(&view, &move);
	QCoreApplication::sendEvent(&view, &release);
}

void terminal_view_test::drag_selects_text() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	emulator.feed(QByteArrayLiteral("hello world"));

	QVERIFY(!view.has_selection());
	drag(view, 0, 0, 4, 0);

	QVERIFY(view.has_selection());
	QCOMPARE(view.selection_text(), QStringLiteral("hello"));

	/*
	 * Trailing blanks are padding: a terminal row is always full width,
	 * and copying the padding is how a pasted command arrives with a tail
	 * of spaces.
	 */
	drag(view, 0, 0, view.cols() - 1, 0);
	QCOMPARE(view.selection_text(), QStringLiteral("hello world"));
}

void terminal_view_test::selection_spans_the_scrollback() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 96);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	for (int i = 0; i < 20; ++i) {
		emulator.feed(QByteArrayLiteral("row") + QByteArray::number(i) + QByteArrayLiteral("\r\n"));
	}
	QVERIFY(emulator.scrollback().count() > 0);

	/* Scroll back, then select a line that only exists in history. */
	view.scroll_by(5);
	drag(view, 0, 0, 3, 0);

	QVERIFY(view.has_selection());
	QVERIFY(view.selection_text().startsWith(QStringLiteral("row")));

	/*
	 * Scrolling must not move the selection: it is anchored to absolute
	 * lines, not to view rows, or the highlight would slide across the
	 * text as the user scrolls to see the rest of it.
	 */
	const QString before = view.selection_text();
	view.scroll_by(-2);
	QCOMPARE(view.selection_text(), before);
}

void terminal_view_test::double_click_selects_a_word() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	emulator.feed(QByteArrayLiteral("cat /etc/hosts here"));

	/* Inside the path: a double-click should take all of it, not "etc". */
	const QPoint point(6 * view.cell_width() + 1, 1);
	QMouseEvent event(QEvent::MouseButtonDblClick, point, view.mapToGlobal(point),
	                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(&view, &event);

	QCOMPARE(view.selection_text(), QStringLiteral("/etc/hosts"));
}

void terminal_view_test::triple_click_selects_the_line() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	emulator.feed(QByteArrayLiteral("cat /etc/hosts here"));

	/*
	 * Qt has no triple-click event: it sends press, double-click, press.
	 * The third press is what this counts, which is why the test sends
	 * exactly that sequence rather than a single synthetic event.
	 */
	const QPoint point(6 * view.cell_width() + 1, 1);
	const auto press = [&](QEvent::Type type) {
		QMouseEvent event(type, point, view.mapToGlobal(point),
		                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
		QCoreApplication::sendEvent(&view, &event);
	};

	press(QEvent::MouseButtonPress);
	press(QEvent::MouseButtonDblClick);
	press(QEvent::MouseButtonPress);

	/* The whole line, with the row's trailing padding trimmed off. */
	QCOMPARE(view.selection_text(), QStringLiteral("cat /etc/hosts here"));
}

void terminal_view_test::mouse_goes_to_the_remote_when_it_asks_for_it() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	emulator.feed(QByteArrayLiteral("text"));
	QVERIFY(!emulator.mouse_reporting());

	/* The remote turns on mouse tracking, SGR encoding. */
	emulator.feed(QByteArrayLiteral("\x1b[?1000h\x1b[?1006h"));
	QVERIFY(emulator.mouse_reporting());
	emulator.take_output();

	drag(view, 2, 1, 2, 1);

	/* The click became a report, and did NOT become a selection. */
	QVERIFY(!emulator.take_output().isEmpty());
	QVERIFY(!view.has_selection());
}

void terminal_view_test::shift_overrides_mouse_reporting_to_select() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	emulator.feed(QByteArrayLiteral("selectable"));
	emulator.feed(QByteArrayLiteral("\x1b[?1000h"));
	emulator.take_output();

	/*
	 * Shift is the conventional escape hatch: without it there is no way
	 * to copy text out of a full-screen program that grabbed the mouse.
	 */
	drag(view, 0, 0, 5, 0, Qt::ShiftModifier);

	QVERIFY(view.has_selection());
	QCOMPARE(view.selection_text(), QStringLiteral("select"));
	QCOMPARE(emulator.take_output(), QByteArray());
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

void terminal_view_test::a_hosts_resolved_font_reaches_its_view() {
	/*
	 * The wiring sec 10.1 describes, end to end: a host's resolved
	 * settings decide what its terminal is drawn in. bssh_font_catalog
	 * existed for a whole commit before anything called it, which is the
	 * failure this test is here to catch -- a font picker that changes a
	 * stored value and nothing else looks like it works.
	 */
	bssh_terminal_view view;

	QJsonObject settings;
	settings.insert(QStringLiteral("font_family"), QStringLiteral("hack"));
	settings.insert(QStringLiteral("font_size"), 14);
	bssh_apply_appearance(&view, settings);

	QCOMPARE(view.font_id(), QStringLiteral("hack"));

	/* A display name, as an imported profile might carry it (sec 9.4). */
	settings.insert(QStringLiteral("font_family"), QStringLiteral("DejaVu Sans Mono"));
	bssh_apply_appearance(&view, settings);
	QCOMPARE(view.font_id(), QStringLiteral("dejavu-sans-mono"));
}

void terminal_view_test::an_unset_font_means_the_default_not_the_fallback_chain() {
	/*
	 * Every shipped preset leaves the font unset (sec 10.4), so this is
	 * the path almost every host takes. An empty string must mean "the
	 * platform's default", not "no font" -- the latter drops into sec
	 * 9.3's chain and renders every out-of-the-box host in DejaVu.
	 */
	bssh_terminal_view view;

	QJsonObject settings;
	settings.insert(QStringLiteral("font_family"), QString());
	settings.insert(QStringLiteral("font_size"), 0);
	bssh_apply_appearance(&view, settings);

	const bssh_font_catalog catalog;
	QCOMPARE(view.font_id(), catalog.default_id(false));
	QVERIFY(catalog.by_id(view.font_id()) != nullptr);
}

void terminal_view_test::changing_a_hosts_font_changes_its_grid() {
	/*
	 * A font change is a geometry change (sec 9.5): different cell size,
	 * different column count, and the remote has to be told or it keeps
	 * laying out for the old width. Asserting on the cell metrics rather
	 * than on a signal, because the metrics are what the emulator and the
	 * window-change message are computed from.
	 */
	bssh_terminal_view view;
	view.resize(800, 400);

	QJsonObject settings;
	settings.insert(QStringLiteral("font_family"), QStringLiteral("dejavu-sans-mono"));
	settings.insert(QStringLiteral("font_size"), 10);
	bssh_apply_appearance(&view, settings);

	const int narrow_cell = view.cell_width();
	const int narrow_cols = view.cols();

	settings.insert(QStringLiteral("font_size"), 20);
	bssh_apply_appearance(&view, settings);

	QVERIFY2(view.cell_width() > narrow_cell, "a bigger font must mean a wider cell");
	QVERIFY2(view.cols() < narrow_cols, "a wider cell must mean fewer columns");
}
