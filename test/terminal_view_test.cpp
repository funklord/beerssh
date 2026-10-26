#include <QHBoxLayout>
#include <QScrollBar>
#include <QUrl>
#include <QMimeData>
#include <QDropEvent>
#include <QSignalSpy>
#include <QtTest>
#include <QEnterEvent>
#include <QScreen>
#include <QWindow>

#include <QAccessible>
#include <QGesture>
#include <QGestureEvent>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QTemporaryDir>
#include "ui/touch_metrics.h"
#include "model/profile_store.h"
#include "ui/session_dialog.h"
#include "model/app_settings.h"
#include <QHash>
#include <QSet>
#include "term/colour_scheme.h"
#include "ui/theme.h"
#include <QContextMenuEvent>
#include <QMenu>
#include "ui/main_window.h"
#include "model/session.h"
#include "terminal_view_test.h"

#include "fake_ssh_session.h"
#include <QClipboard>
#include "input/input_router.h"
#include "term/emulator.h"
#include "platform/platform.h"
#include "ui/multiplexer_drawer.h"
#include "ui/interface_scale.h"
#include "term/font_catalog.h"
#include "ui/terminal_view.h"
#include "model/session_registry.h"
#include "ui/pane_container.h"
#include "ui/find_bar.h"
#include "model/preset.h"
#include <QWheelEvent>


/*
 * Qt delivers no resizeEvent to a widget that was never shown, so a test
 * that only calls resize() measures nothing. show() under the offscreen
 * QPA plugin is both the fix and what the app actually does.
 */
static void show_and_resize(bssh_terminal_view &view, int w, int h) {
	if (!view.isVisible()) view.show();
	view.resize(w, h);
}

/*
 * A wheel notch is 120 eighths of a degree, which is what every mouse on
 * the desktop sends and what the view divides by.
 */
static void wheel(bssh_terminal_view &view, int notches,
                   Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
	const QPointF at(2 * view.cell_width() + 1, view.cell_height() + 1);

	QWheelEvent event(at, view.mapToGlobal(at.toPoint()), QPoint(0, 0),
	                   QPoint(0, notches * 120), Qt::NoButton, modifiers,
	                   Qt::NoScrollPhase, false);
	QCoreApplication::sendEvent(&view, &event);
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
	 * desktop dead keys arrive the same way -- U+00E9 typed as a compose
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

/*
 * Forces the touch idiom for the length of a scope and puts it back, even
 * when a QVERIFY returns early -- the reset written at the bottom of a
 * function is the one line that does not run on the day it matters, and a
 * leaked force makes every later test measure a phone.
 */
struct forced_touch_t {
	explicit forced_touch_t(int touch) { bssh_platform::force_touch_primary(touch); }
	~forced_touch_t() { bssh_platform::force_touch_primary(-1); }
};

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

void terminal_view_test::shrinking_the_scrollback_brings_the_view_back_with_it() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	emulator.set_scrollback_lines(200);
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	for (int i = 0; i < view.rows() + 60; ++i) {
		emulator.feed(QByteArrayLiteral("a line\r\n"));
	}
	QVERIFY(emulator.scrollback().count() > 40);

	view.set_scroll_offset(40);
	QCOMPARE(view.scroll_offset(), 40);

	/*
	 * A preset lowering scrollback_lines, which is a live path: editing
	 * the host's preset reaches set_settings and then the emulator. It
	 * trims the ring with NO damage at all -- so nothing repainted, and
	 * the only announcement is scrollback_changed, which had no listener
	 * anywhere in the tree. The bar went on advertising history that had
	 * been dropped and the offset pointed past the end of what was left.
	 */
	emulator.set_scrollback_lines(5);
	QCOMPARE(emulator.scrollback().count(), 5);

	/*
	 * Asserted against the ring rather than against 5: what matters is
	 * that the view cannot be further back than there is history, which
	 * stays true whatever the cap becomes.
	 */
	QVERIFY2(view.scroll_offset() <= emulator.scrollback().count(),
	          qPrintable(QStringLiteral("offset %1 into %2 lines")
	                      .arg(view.scroll_offset())
	                      .arg(emulator.scrollback().count())));
}

void terminal_view_test::a_selection_stays_on_its_text_when_the_scrollback_evicts() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	emulator.set_scrollback_lines(8);
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	/*
	 * Fill the ring to its cap first, so that everything after this point
	 * evicts. FINDME goes in AFTER that and is then scrolled off, which
	 * puts it among the most recent lines -- far enough from the front to
	 * survive the three evictions below, which is what makes the
	 * assertion about renumbering rather than about deletion.
	 */
	for (int i = 0; i < view.rows() + 12; ++i) {
		emulator.feed(QByteArrayLiteral("filler\r\n"));
	}
	emulator.feed(QByteArrayLiteral("FINDME\r\n"));
	for (int i = 0; i < view.rows(); ++i) {
		emulator.feed(QByteArrayLiteral("filler\r\n"));
	}
	QCOMPARE(emulator.scrollback().count(), 8);

	/* Scroll back until it is the top row, and select it. */
	bool found = false;
	for (int back = 1; back <= 8 && !found; ++back) {
		view.clear_selection();
		view.set_scroll_offset(back);
		drag(view, 0, 0, 5, 0);
		found = view.selection_text() == QStringLiteral("FINDME");
	}
	QVERIFY2(found, qPrintable(view.selection_text()));

	/*
	 * Now output arrives while the ring is already full, so every push
	 * evicts. The ring renumbered every line by one per push and the
	 * anchor did not move, so the highlight walked down the content and
	 * Copy handed back text the user never selected. It was invisible
	 * because the painting agreed with the anchor -- what had moved was
	 * the highlight itself, which is why this asserts on the TEXT.
	 */
	const qint64 base_before = emulator.scrollback().base();
	for (int i = 0; i < 3; ++i) emulator.feed(QByteArrayLiteral("more\r\n"));
	QCOMPARE(emulator.scrollback().base(), base_before + 3);

	QCOMPARE(view.selection_text(), QStringLiteral("FINDME"));
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

void terminal_view_test::a_touch_body_drag_scrolls_in_scroll_mode() {
	forced_touch_t touch(1);

	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	/* History to scroll into, and more than the drag below asks for. */
	for (int i = 0; i < view.rows() + 30; ++i) {
		emulator.feed(QByteArrayLiteral("scrollback line\r\n"));
	}
	QVERIFY(emulator.scrollback().count() > 8);
	QCOMPARE(view.scroll_offset(), 0);

	/*
	 * Sec 8.7's default: a body drag scrolls this terminal's scrollback.
	 * Dragging DOWN reveals the lines above -- further back -- so the
	 * offset rises. This is the route that did not exist on a phone.
	 */
	view.set_input_mode(BSSH_INPUT_SCROLL);
	drag(view, 2, 1, 2, 9);

	QVERIFY2(view.scroll_offset() > 0,
	          "a touch body-drag in scroll mode left the scrollback where it was");
	QVERIFY(!view.has_selection());
}

void terminal_view_test::a_touch_body_drag_reorders_in_move_mode() {
	forced_touch_t touch(1);

	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	/*
	 * Move mode (sec 8.7): a body drag reorders this pane in the column.
	 * The view does not reorder anything itself -- that rebuilds the panes
	 * and would destroy this view mid-gesture -- it only asks, on release,
	 * with the direction the finger travelled. Up the screen is toward the
	 * top of the column (negative), down toward the bottom (positive).
	 */
	view.set_input_mode(BSSH_INPUT_MOVE);
	QSignalSpy spy(&view, &bssh_terminal_view::pane_move_requested);

	drag(view, 2, 9, 2, 1);
	QCOMPARE(spy.count(), 1);
	QCOMPARE(spy.takeFirst().at(0).toInt(), -1);

	drag(view, 2, 1, 2, 9);
	QCOMPARE(spy.count(), 1);
	QCOMPARE(spy.takeFirst().at(0).toInt(), 1);

	/*
	 * A tap that never moved is not a reorder -- it asks for the keyboard,
	 * as the scroll path does, and must not shuffle the column by accident.
	 */
	drag(view, 2, 5, 2, 5);
	QCOMPARE(spy.count(), 0);

	/* And the scrollback stayed put: Move is not Scroll. */
	QCOMPARE(view.scroll_offset(), 0);
}

void terminal_view_test::a_touch_body_drag_pans_the_column_in_pan_mode() {
	forced_touch_t touch(1);

	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	for (int i = 0; i < view.rows() + 30; ++i) {
		emulator.feed(QByteArrayLiteral("scrollback line\r\n"));
	}
	QCOMPARE(view.scroll_offset(), 0);

	/*
	 * Pan mode (sec 8.7): a body drag scrolls the COLUMN, not this
	 * terminal's scrollback. The view owns no scroll area, so it reports
	 * the step since the last move and leaves its own scrollback alone.
	 * The container turns those steps into a viewport scroll.
	 */
	view.set_input_mode(BSSH_INPUT_COLUMN_SCROLL);
	QSignalSpy spy(&view, &bssh_terminal_view::column_scroll_requested);

	drag(view, 2, 1, 2, 9);
	QVERIFY2(spy.count() > 0, "a Pan drag reported no scroll");

	/* The steps sum to the downward distance the finger travelled. */
	int total = 0;
	while (!spy.isEmpty()) total += spy.takeFirst().at(0).toInt();
	QVERIFY2(total > 0, "a downward Pan drag did not ask to scroll down");

	/* Pan is not Scroll: this terminal's own scrollback did not move. */
	QCOMPARE(view.scroll_offset(), 0);

	/* A tap reports nothing and asks for the keyboard instead. */
	drag(view, 2, 5, 2, 5);
	QCOMPARE(spy.count(), 0);
}

void terminal_view_test::find_locates_matches_navigates_and_scrolls() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	/*
	 * One match deep in the history, one nearer, one on the live screen --
	 * so navigation has both a scroll-to-find and an already-visible case.
	 */
	emulator.feed(QByteArrayLiteral("an early needle\r\n"));
	for (int i = 0; i < view.rows() + 20; ++i)
		emulator.feed(QByteArrayLiteral("filler line\r\n"));
	emulator.feed(QByteArrayLiteral("a late NEEDLE\r\n"));
	emulator.feed(QByteArrayLiteral("needle on the last row"));

	/* Case-insensitive: all three are found, in top-to-bottom order. */
	QCOMPARE(view.find_text(QStringLiteral("needle")), 3);
	QCOMPARE(view.find_match_count(), 3);

	/* The current match starts at the last one, nearest the live screen. */
	QCOMPARE(view.current_find_index(), 3);
	QCOMPARE(view.scroll_offset(), 0);

	/* Stepping forward from the last wraps to the first, and the first is
	 * deep in the history, so the view scrolls back to bring it in. */
	view.find_step(1);
	QCOMPARE(view.current_find_index(), 1);
	QVERIFY2(view.scroll_offset() > 0, "the earliest match was not scrolled into view");

	/* And back the other way returns to the last, on the live screen. */
	view.find_step(-1);
	QCOMPARE(view.current_find_index(), 3);

	/* A query that is not there finds nothing and clears the current. */
	QCOMPARE(view.find_text(QStringLiteral("haystack")), 0);
	QCOMPARE(view.current_find_index(), 0);

	view.clear_find();
	QCOMPARE(view.find_match_count(), 0);
	QVERIFY(view.find_query().isEmpty());
}

void terminal_view_test::find_matches_across_a_wrapped_line() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	const int cols = view.cols();
	QVERIFY(cols > 10);

	/*
	 * One logical line that wraps, with the word straddling the wrap:
	 * (cols - 3) filler characters, then "needle", so "nee" ends the first
	 * row and "dle" begins the continuation row. A per-row search would
	 * miss it; the logical-line search joins the rows and finds the one
	 * match, spanning both.
	 */
	emulator.feed(QByteArray(cols - 3, 'x'));
	emulator.feed(QByteArrayLiteral("needle"));

	QCOMPARE(view.find_text(QStringLiteral("needle")), 1);
	QCOMPARE(view.current_find_index(), 1);
}

void terminal_view_test::select_mode_is_the_touch_shift_override() {
	forced_touch_t touch(1);

	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	emulator.feed(QByteArrayLiteral("selectable"));

	/*
	 * The remote grabbed the mouse. On a desktop Shift is the escape that
	 * lets a user select text out of it; a phone has no Shift, so select
	 * mode is that escape (sec 8.7). Without it the drag would be a report
	 * and nothing could be copied out of htop.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[?1000h"));
	QVERIFY(emulator.mouse_reporting());
	emulator.take_output();

	view.set_input_mode(BSSH_INPUT_SELECT);
	drag(view, 0, 0, 5, 0);

	QVERIFY(view.has_selection());
	QCOMPARE(view.selection_text(), QStringLiteral("select"));
	QCOMPARE(emulator.take_output(), QByteArray());
}

void terminal_view_test::click_mode_lets_the_remote_have_the_touch() {
	forced_touch_t touch(1);

	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	emulator.feed(QByteArrayLiteral("text"));
	emulator.feed(QByteArrayLiteral("\x1b[?1000h\x1b[?1006h"));
	QVERIFY(emulator.mouse_reporting());
	emulator.take_output();

	/*
	 * Click mode is the one that forwards, so a full-screen program that
	 * asked for the mouse can be driven from a phone -- the report goes
	 * out and nothing is selected or scrolled locally.
	 */
	view.set_input_mode(BSSH_INPUT_CLICK);
	drag(view, 2, 1, 2, 1);

	QVERIFY(!emulator.take_output().isEmpty());
	QVERIFY(!view.has_selection());
	QCOMPARE(view.scroll_offset(), 0);
}

void terminal_view_test::the_interface_font_scales_and_no_ops_at_one() {
	QFont base;
	base.setPointSizeF(10.0);

	/*
	 * A real scale grows the chrome font; 1.0 and a broken reading leave
	 * it exactly as it was, so an absent or nonsensical system value is a
	 * no-op rather than a collapsed interface (sec 9.5).
	 */
	QCOMPARE(bssh_interface_font(base, 1.5).pointSizeF(), 15.0);
	QCOMPARE(bssh_interface_font(base, 1.0).pointSizeF(), 10.0);
	QCOMPARE(bssh_interface_font(base, 0.0).pointSizeF(), 10.0);
	QCOMPARE(bssh_interface_font(base, -2.0).pointSizeF(), 10.0);
}

void terminal_view_test::the_terminal_grid_ignores_the_application_font() {
	/*
	 * The property the whole font-scale feature rests on (sec 9.5): the
	 * grid builds its own font with an explicit point size, so scaling the
	 * application font moves the chrome around it and never the column
	 * count. Measured rather than trusted, because if the grid ever DID
	 * read the application font, chrome-scaling would silently reflow the
	 * remote's layout.
	 */
	const QFont saved = QApplication::font();

	bssh_terminal_view before;
	before.show();
	before.resize(400, 200);
	const int cell = before.cell_width();
	QVERIFY(cell > 1);

	QFont huge = saved;
	huge.setPointSize(saved.pointSize() > 0 ? saved.pointSize() * 3 : 36);
	QApplication::setFont(huge);

	bssh_terminal_view after;
	after.show();
	after.resize(400, 200);
	const int cell_under_huge_font = after.cell_width();

	QApplication::setFont(saved);

	QCOMPARE(cell_under_huge_font, cell);
}

void terminal_view_test::a_view_builds_its_font_with_the_ligature_default() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	/*
	 * The wiring, not just make_font: a view reads the device default when
	 * it builds its font (sec 9.2), so the setting reaches the grid rather
	 * than sitting in the store. Dropping the default from the view's
	 * make_font call leaves this the one test that goes red.
	 */
	const bool saved = bssh_terminal_view::default_font_ligatures();

	bssh_terminal_view::set_default_font_ligatures(false);
	bssh_terminal_view off;
	QVERIFY2(off.display_font().isFeatureSet(QFont::Tag("liga")),
	          "off pins the feature to 0 so the grid never draws a ligature");
	QCOMPARE(off.display_font().featureValue(QFont::Tag("liga")), quint32(0));

	bssh_terminal_view::set_default_font_ligatures(true);
	bssh_terminal_view on;
	QVERIFY2(!on.display_font().isFeatureSet(QFont::Tag("liga")),
	          "on leaves the feature to the font's own defaults");

	bssh_terminal_view::set_default_font_ligatures(saved);
#else
	QSKIP("per-font OpenType feature control needs Qt 6.7");
#endif
}

void terminal_view_test::the_wheel_goes_to_the_remote_when_it_asked_for_the_mouse() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	/* History to scroll into, so a view that ignored the remote would move. */
	for (int i = 0; i < view.rows() + 10; ++i) {
		emulator.feed(QByteArrayLiteral("shell line\r\n"));
	}
	QVERIFY(emulator.scrollback().count() > 3);

	emulator.feed(QByteArrayLiteral("\x1b[?1000h\x1b[?1006h"));
	QVERIFY(emulator.mouse_reporting());
	emulator.take_output();

	wheel(view, 1);

	/*
	 * SGR encodes the wheel as buttons 4 and 5 with 0x40 added, so 64 is
	 * up and 65 is down. The report is read rather than merely counted:
	 * a wheel sent as button 1 would also be "not empty" and would land
	 * as a click wherever the pointer happened to be.
	 */
	const QByteArray up = emulator.take_output();
	QVERIFY2(up.startsWith(QByteArrayLiteral("\x1b[<64;")), up.toHex().constData());
	QVERIFY(up.endsWith('M'));

	wheel(view, -1);
	QVERIFY(emulator.take_output().startsWith(QByteArrayLiteral("\x1b[<65;")));

	/* And the view did not scroll: the wheel was the remote's, not ours. */
	QCOMPARE(view.scroll_offset(), 0);

	/*
	 * Shift takes it back, exactly as it does for a click -- otherwise a
	 * program that grabs the mouse also takes away the scrollback.
	 */
	wheel(view, 1, Qt::ShiftModifier);
	QCOMPARE(emulator.take_output(), QByteArray());
	QVERIFY(view.scroll_offset() > 0);
}

void terminal_view_test::the_alternate_screen_has_no_history_to_scroll_into() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	for (int i = 0; i < view.rows() + 10; ++i) {
		emulator.feed(QByteArrayLiteral("shell line\r\n"));
	}
	QVERIFY(emulator.scrollback().count() > 3);

	/* On the primary screen that history is what the wheel is for. */
	wheel(view, 1);
	QVERIFY(view.scroll_offset() > 0);

	/*
	 * A full-screen program starts while the view is scrolled back --
	 * DECSET 1049, which is what vim, less and htop send. The offset has
	 * to go with it, or the shell stays on screen over the program.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[?1049h"));
	QVERIFY(emulator.alternate_screen());
	QCOMPARE(view.scroll_offset(), 0);

	/* And it cannot be scrolled back into while the program is up. */
	wheel(view, 1);
	QCOMPARE(view.scroll_offset(), 0);

	/* Kept, not dropped: leaving the program gives the history back. */
	emulator.feed(QByteArrayLiteral("\x1b[?1049l"));
	QVERIFY(!emulator.alternate_screen());
	wheel(view, 1);
	QVERIFY(view.scroll_offset() > 0);
}

/* One press and one release of a given button, at a cell. */
static void click(bssh_terminal_view &view, Qt::MouseButton button, int col, int row) {
	const QPoint at(col * view.cell_width() + 1, row * view.cell_height() + 1);

	QMouseEvent press(QEvent::MouseButtonPress, at, view.mapToGlobal(at),
	                   button, button, Qt::NoModifier);
	QMouseEvent release(QEvent::MouseButtonRelease, at, view.mapToGlobal(at),
	                     button, Qt::NoButton, Qt::NoModifier);

	QCoreApplication::sendEvent(&view, &press);
	QCoreApplication::sendEvent(&view, &release);
}

void terminal_view_test::a_release_names_the_button_the_press_reported() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	emulator.feed(QByteArrayLiteral("\x1b[?1000h\x1b[?1006h"));
	QVERIFY(emulator.mouse_reporting());
	emulator.take_output();

	/*
	 * SGR spells the buttons 0, 1, 2 for left, middle, right, with `M`
	 * for a press and `m` for a release. The release used to be hardcoded
	 * to button 1 whatever was pressed -- and libvterm keeps a button
	 * MASK, so that cleared the wrong bit and left the right-button bit
	 * set for ever.
	 */
	click(view, Qt::RightButton, 3, 1);

	const QByteArray first = emulator.take_output();
	QVERIFY2(first.contains(QByteArrayLiteral("\x1b[<2;")), first.toHex().constData());
	QVERIFY2(first.endsWith('m'), first.toHex().constData());

	/*
	 * And again. This is the assertion that matters: with the bit
	 * stranded, libvterm's "no change" early return swallowed every
	 * later right-click, so the button worked exactly once per session
	 * and the first click alone would have passed.
	 */
	click(view, Qt::RightButton, 4, 1);

	const QByteArray second = emulator.take_output();
	QVERIFY2(second.contains(QByteArrayLiteral("\x1b[<2;")), second.toHex().constData());
	QVERIFY2(second.endsWith('m'), second.toHex().constData());
}

void terminal_view_test::a_click_while_scrolled_back_reports_the_screen_row() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	for (int i = 0; i < view.rows() + 10; ++i) {
		emulator.feed(QByteArrayLiteral("line\r\n"));
	}
	QVERIFY(emulator.scrollback().count() > 3);

	emulator.feed(QByteArrayLiteral("\x1b[?1000h\x1b[?1006h"));
	emulator.take_output();

	/* Row 2 of the view, live, with nothing scrolled back yet. */
	click(view, Qt::LeftButton, 0, 2);
	const QByteArray live = emulator.take_output();

	/*
	 * Scroll back, then click the SAME view row. libvterm's reports are
	 * in live-screen rows, so the answer must be two rows further UP the
	 * screen -- and it used to be the view row either way, because the
	 * mapping went through line_for_row(), which carries the offset.
	 */
	view.set_scroll_offset(2);
	QCOMPARE(view.scroll_offset(), 2);

	click(view, Qt::LeftButton, 0, 2);
	const QByteArray scrolled = emulator.take_output();

	QVERIFY(!live.isEmpty());
	QVERIFY(!scrolled.isEmpty());
	QVERIFY2(live != scrolled, live.toHex().constData());
}

void terminal_view_test::a_sticky_shift_overrides_mouse_reporting_on_touch() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	bssh_input_router router(&emulator);
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);
	view.attach_input_router(&router);

	emulator.feed(QByteArrayLiteral("selectable"));
	emulator.feed(QByteArrayLiteral("\x1b[?1000h\x1b[?1006h"));
	QVERIFY(emulator.mouse_reporting());
	emulator.take_output();

	/*
	 * Without it the tap is the remote's, which is correct and is what a
	 * desktop click does too.
	 */
	drag(view, 0, 0, 5, 0);
	QVERIFY(!view.has_selection());
	QVERIFY(!emulator.take_output().isEmpty());

	/*
	 * Shift is the conventional override, and a phone has no Shift to
	 * hold -- so on touch the override did not exist at all and a
	 * mouse-reporting program took the selection, the clipboard and the
	 * menu with it, with no way back. The key row's sticky modifier is
	 * the Shift a touch device has (sec 6.2).
	 */
	router.set_sticky_modifiers(bssh_emulator::MOD_SHIFT);
	drag(view, 0, 0, 5, 0);

	QVERIFY2(view.has_selection(), "a sticky Shift must reach the pointer, not only keys");
	QCOMPARE(view.selection_text(), QStringLiteral("select"));
	QCOMPARE(emulator.take_output(), QByteArray());

	/*
	 * And consumed, exactly as a key consumes it: one shot means one.
	 * Leaving it set would make every following tap select as well.
	 */
	QCOMPARE(router.sticky_modifiers(), quint8(bssh_emulator::MOD_NONE));

	view.clear_selection();
	drag(view, 0, 0, 5, 0);
	QVERIFY2(!view.has_selection(), "the next tap is the remote's again");
}

void terminal_view_test::a_long_press_reaches_the_menu_even_when_the_remote_has_the_mouse() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_emulator(&emulator);
	view.set_long_press_menu(true);

	emulator.feed(QByteArrayLiteral("\x1b[?1000h\x1b[?1006h"));
	QVERIFY(emulator.mouse_reporting());

	QSignalSpy menu(&view, &bssh_terminal_view::context_menu_requested);

	/*
	 * A real hold, not an emitted signal: what was removed is a guard
	 * inside the long-press handler, so a test that emits the signal
	 * itself would pass with the guard still in place.
	 *
	 * The gesture is the whole argument. A long press is not carried by
	 * any terminal protocol, so it cannot be the remote's to take --
	 * unlike a right-click, which is something a program that grabbed
	 * the mouse asked for and can act on. Suppressing it left a phone
	 * inside such a program with no menu, no clipboard, and no way to
	 * reach either (sec 6.4).
	 */
	const QPoint at(40, 40);
	QMouseEvent press(QEvent::MouseButtonPress, at, view.mapToGlobal(at),
	                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(&view, &press);

	QTest::qWait(700);

	QMouseEvent release(QEvent::MouseButtonRelease, at, view.mapToGlobal(at),
	                     Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QCoreApplication::sendEvent(&view, &release);

	QCOMPARE(menu.count(), 1);
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
	QCOMPARE(view.font_id(), catalog.default_id());
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

void terminal_view_test::the_cell_lands_on_whole_device_pixels() {
	/*
	 * sec 9.5 says to snap the grid to integer pixel metrics and that
	 * fractional scaling is where it matters most. It was snapping to
	 * integer LOGICAL pixels, which is not the same thing: at 150% a
	 * 9-pixel cell is 13.5 device pixels, so every second column boundary
	 * falls on a half pixel, adjacent glyphs are antialiased at different
	 * sub-pixel offsets, and box-drawing characters stop meeting. A TUI's
	 * borders come out seamed at every other column.
	 *
	 * Found by rendering the same box-drawing line at 100% and 150% and
	 * looking at them: solid at 100%, visibly broken at 150%.
	 */
	for (int cell = 6; cell <= 24; ++cell) {
		const struct { qreal ratio; int period; } cases[] = {
			{ 1.0, 1 }, { 1.25, 4 }, { 1.5, 2 }, { 2.0, 1 }, { 3.0, 1 }
		};

		for (const auto &c : cases) {
			const int snapped = bssh_snap_to_device_pixels(cell, c.ratio);

			QVERIFY2(snapped >= 1, "a cell must never round away to nothing");
			QCOMPARE(snapped % c.period, 0);

			/* Whole device pixels is the property that matters. */
			const qreal device = snapped * c.ratio;
			QVERIFY2(qAbs(device - qRound(device)) < 0.001,
			         qPrintable(QStringLiteral("cell %1 at %2x lands on %3 device px")
			                            .arg(cell).arg(c.ratio).arg(device)));

			/* And it stays close to what the font asked for. */
			QVERIFY2(qAbs(snapped - cell) <= c.period,
			         "snapping must not move the cell more than one period");
		}
	}

	/*
	 * An unusual ratio is left alone: 110% would need a period of ten,
	 * and rounding a nine-pixel cell to ten costs more in glyph fidelity
	 * than the seam costs in appearance.
	 */
	QCOMPARE(bssh_snap_to_device_pixels(9, 1.1), 9);
}

/*
 * The x range of a horizontal run of pixels that are not the terminal's
 * background, sampled down the middle of the first row. Returns false when
 * the row is empty, so a test cannot pass on having measured nothing.
 */
static bool ink_span(const QImage &frame, int y, int width, const QColor &background,
                      int &first, int &last) {
	first = -1;
	last = -1;

	for (int x = 0; x < width; ++x) {
		if (frame.pixelColor(x, y) == background) continue;
		if (first < 0) first = x;
		last = x;
	}

	return first >= 0;
}

/*
 * A block cursor does not COVER the character it sits on (sec 8.3).
 *
 * The block is filled with the cursor colour and the glyph is then
 * punched back out of it in the background colour, so the character
 * stays readable under the mark. Drop the punch-out and the cursor is a
 * solid rectangle: the letter under it is simply gone, which on a shell
 * prompt means the character you are about to overtype is invisible.
 *
 * Asserted as a RELATIONSHIP between two renders rather than against a
 * colour or a rectangle. A block over a glyph and a block over an empty
 * cell must not look the same -- and if the punch-out is removed they
 * are identical, because both are then the same filled rectangle. That
 * needs no knowledge of the grid inset, the palette, or which pixels the
 * letter happens to occupy, all of which a pixel-counting version would
 * have to hard-code and none of which this is about.
 *
 * Found by sabotage: replacing the punched-out glyph with an empty
 * QString left the suite green, so the cursor could have covered every
 * character it marked with nothing to say so.
 */
void terminal_view_test::the_block_cursor_does_not_cover_its_character() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	/*
	 * Both conditions the block path needs, stated rather than assumed:
	 * an unfocused view draws an outline instead, and a bar or underline
	 * cursor cannot cover anything, so either would make this pass while
	 * measuring nothing.
	 */
	view.set_cursor_style(QStringLiteral("block"));
	QVERIFY(QTest::qWaitForWindowExposed(&view));
	view.activateWindow();
	view.setFocus();
	QTRY_VERIFY2(view.hasFocus(), "an unfocused view draws an outline, not a block");

	/* A letter with plenty of ink, with the cursor put back on top. */
	emulator.feed(QByteArrayLiteral("\x1b[2J\x1b[H""W\x1b[H"));
	const QImage over_glyph = view.grab().toImage();

	/* The same block, over an empty cell. */
	emulator.feed(QByteArrayLiteral("\x1b[2J\x1b[H"));
	const QImage over_blank = view.grab().toImage();

	QCOMPARE(over_glyph.size(), over_blank.size());
	QVERIFY2(over_glyph != over_blank,
	          "the block over a letter looks exactly like the block over an "
	          "empty cell, so the cursor is covering the character it marks");
}

void terminal_view_test::the_cursor_lands_on_the_cell_it_marks() {
	bssh_terminal_view view;
	view.set_scrollbar_position(bssh_terminal_view::SCROLLBAR_RIGHT);
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	/*
	 * One cell with a background of its own, at the top left, and the
	 * cursor put back on it. The cell is what says where the grid starts;
	 * the cursor has to agree with it, and nothing else in this test
	 * needs to know what that number is.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[41m \x1b[0m\x1b[H"));

	const int y = view.cell_height() / 2;

	/*
	 * The terminal's own background, read off a blank cell at the far end
	 * of the same row rather than taken from the palette. The palette's
	 * Window colour is not what the terminal fills with, and using it made
	 * every pixel count as ink -- both spans came back as the whole widget
	 * and the comparison below passed while measuring nothing.
	 */
	const QColor background = view.grab().toImage().pixelColor(view.width() / 2, y);

	/*
	 * Only the first few columns are looked at, and the bar is on the
	 * right, so a grab() -- which renders child widgets too -- cannot
	 * offer the scrollbar as though it were terminal content. It did:
	 * with the bar on the LEFT the whole measured span was the bar.
	 */
	const int scan = 4 * view.cell_width();

	/* The cell alone: the cursor is off. */
	emulator.feed(QByteArrayLiteral("\x1b[?25l"));
	int cell_first = 0;
	int cell_last = 0;
	QVERIFY(ink_span(view.grab().toImage(), y, scan, background, cell_first, cell_last));

	/* And with the cursor on it, over the same cell. */
	emulator.feed(QByteArrayLiteral("\x1b[?25h"));
	int cursor_first = 0;
	int cursor_last = 0;
	QVERIFY(ink_span(view.grab().toImage(), y, scan, background, cursor_first, cursor_last));

	/*
	 * Every cell is painted through a translate() that steps past the
	 * grid inset and, with a left-hand bar, past the bar as well. The
	 * cursor was painted after resetTransform() in raw widget
	 * coordinates, so it landed one pixel left and up of the cell it was
	 * marking -- and a whole scrollbar's width away when the bar is on
	 * the left. One pixel is what this measures, because it is the case
	 * that can be measured without the bar itself being in the frame.
	 */
	/*
	 * Both spans must be about one cell wide. Without this the test
	 * passes when the whole row reads as ink -- which is exactly what it
	 * did on the first attempt, with the background taken from the
	 * palette rather than from the screen.
	 */
	QVERIFY(cell_last - cell_first < 2 * view.cell_width());
	QVERIFY(cursor_last - cursor_first < 2 * view.cell_width());

	QCOMPARE(cursor_first, cell_first);
	QCOMPARE(cursor_last, cell_last);

	/*
	 * And the block punches the character out of itself rather than
	 * covering it -- the cell being looked at is the one the user is
	 * looking at. A space cannot show that, so this asks with a glyph:
	 * inside the block there must be pixels back at the terminal's
	 * background, which is what the punch-out draws in.
	 *
	 * What this does NOT pin is which FAMILY the punch-out picks. That is
	 * the fallback chain's answer per glyph (sec 9.3) and it is a font
	 * question rather than a pixel one.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[H\x1b[41mM\x1b[0m\x1b[H"));

	const QImage glyph = view.grab().toImage();
	QSet<QRgb> inside;
	for (int row = 1; row < view.cell_height() - 1; ++row) {
		for (int x = cursor_first; x <= cursor_last; ++x) {
			inside.insert(glyph.pixel(x, row));
		}
	}

	/*
	 * More than one colour inside the block. Asked that way rather than
	 * "some pixel is exactly the background", which is what this tried
	 * first and which fails on antialiasing: a punched stroke blends with
	 * the block and need never reach the pure colour it is drawn in.
	 */
	QVERIFY2(inside.size() > 1, "the block covered the character instead of punching it out");
}

void terminal_view_test::a_long_run_of_text_stays_on_the_grid() {
	/*
	 * The renderer draws stretches of same-looking cells in one call
	 * rather than one call per cell (sec 9.5's glyph runs) -- 94 ms per
	 * full-screen repaint became 24. The risk it introduces is drift:
	 * inside a single drawText Qt advances by the FONT's metrics, which
	 * are fractional, while a cell is a whole number of device pixels, so
	 * a long run creeps away from its columns. cell_font pins each
	 * advance with absolute letter spacing; this checks the pin holds.
	 *
	 * A narrow glyph is the one that shows it. 'i' has far less ink than
	 * its cell, so if the run were advancing by the glyph's own width
	 * instead of the cell's, the last character would land nowhere near
	 * the last column.
	 */
	bssh_terminal_view view;
	view.resize(600, 60);

	QJsonObject settings;
	settings.insert(QStringLiteral("font_family"), QStringLiteral("jetbrains-mono"));
	settings.insert(QStringLiteral("font_size"), 12);
	settings.insert(QStringLiteral("cursor_blink"), false);
	bssh_apply_appearance(&view, settings);

	const int cols = view.cols();
	QVERIFY(cols > 30);

	bssh_screen screen(cols, view.rows());
	for (int col = 0; col < cols; ++col) {
		bssh_cell_t cell;
		cell.ch = U'i';
		screen.set_cell(0, col, cell);
	}
	view.attach_screen(&screen);

	const QImage rendered = view.grab().toImage();

	/*
	 * The background sampled from a row nothing was drawn in. Taking it
	 * from near the origin instead was wrong: that pixel is inside the
	 * first character.
	 */
	const QColor background = rendered.pixelColor(rendered.width() - 1,
	                                               rendered.height() - 1);

	/* Ink somewhere in the last column's own slice of the row. */
	const int cell_width = view.cell_width();
	const int last_left = (cols - 1) * cell_width;

	bool ink_in_last_cell = false;
	for (int x = last_left; x < last_left + cell_width && x < rendered.width(); ++x) {
		for (int y = 0; y < view.cell_height(); ++y) {
			if (rendered.pixelColor(x, y) != background) ink_in_last_cell = true;
		}
	}

	QVERIFY2(ink_in_last_cell,
	         "the last character of a full-width run must be drawn in the last column");

	/*
	 * And nothing spills past the grid into the right-hand margin.
	 *
	 * Near the background rather than exactly it. Antialiasing bleeds a
	 * little differently between Qt versions -- measured at 3/255 on
	 * Qt 6.4 against 0 on 6.8 -- and an exact comparison turns that into
	 * a failing build on the older one while testing nothing extra. A
	 * glyph actually drawn here would be the foreground colour, which is
	 * ~180/255 away, so the tolerance is nowhere near what this is
	 * looking for.
	 */
	const int tolerance = 12;
	for (int x = cols * cell_width; x < rendered.width(); ++x) {
		for (int y = 0; y < view.cell_height(); ++y) {
			const QColor pixel = rendered.pixelColor(x, y);
			const int drift = qMax(qAbs(pixel.red() - background.red()),
			                        qMax(qAbs(pixel.green() - background.green()),
			                              qAbs(pixel.blue() - background.blue())));
			QVERIFY2(drift <= tolerance,
			          qPrintable(QStringLiteral("ink at x=%1 y=%2: %3 against %4")
			                             .arg(x).arg(y).arg(pixel.name(), background.name())));
		}
	}
}

void terminal_view_test::the_terminal_reads_itself_to_assistive_technology() {
	/*
	 * The terminal painted characters and told assistive technology
	 * nothing: role Client, no name, no value, no text interface. Every
	 * other control can be fixed by naming it; this one is different,
	 * because what needs reading is not a label but the contents -- which
	 * are the whole application (sec 14, M11).
	 */
	bssh_emulator emulator(40, 4);
	emulator.feed(QByteArrayLiteral("login: nabbe\r\nlast login was Tuesday\r\n$ "));

	bssh_terminal_view view;
	view.attach_screen(&emulator.screen());
	view.resize(600, 200);

	QAccessibleInterface *a = QAccessible::queryAccessibleInterface(&view);
	QVERIFY2(a, "the terminal must have an accessible interface at all");
	QCOMPARE(a->role(), QAccessible::Terminal);
	QCOMPARE(a->text(QAccessible::Name), QStringLiteral("Terminal"));

	QAccessibleTextInterface *text = a->textInterface();
	QVERIFY2(text, "without a text interface a reader has nothing to navigate");

	/* What is on screen is what it reports. */
	const QString value = a->text(QAccessible::Value);
	QVERIFY(value.contains(QStringLiteral("login: nabbe")));
	QVERIFY(value.contains(QStringLiteral("last login was Tuesday")));

	/*
	 * Line navigation is the gesture a reader actually uses, and it works
	 * on '\n' -- which is why rows are joined rather than padded to the
	 * grid width.
	 */
	int start = -1;
	int end = -1;
	/* Qt's line boundary includes the terminator, so the newline is part
	 * of the line rather than a gap between two of them. */
	QCOMPARE(text->textAtOffset(0, QAccessible::LineBoundary, &start, &end),
	          QStringLiteral("login: nabbe\n"));
	QCOMPARE(start, 0);

	QCOMPARE(text->textAtOffset(end, QAccessible::LineBoundary, &start, &end),
	          QStringLiteral("last login was Tuesday\n"));

	/* The cursor lands where the terminal's is, not at zero. */
	QVERIFY2(text->cursorPosition() > 0, "the cursor must be reported where it is");
	QCOMPARE(text->text(0, 5), QStringLiteral("login"));
	QCOMPARE(text->characterCount(), value.size());

	/*
	 * Read-only, and it says so: a reader that believes it can type here
	 * offers an editing gesture that silently does nothing, because keys
	 * reach the remote through the input router instead.
	 */
	QVERIFY(a->state().readOnly);

	const int before = text->cursorPosition();
	text->setCursorPosition(0);
	QCOMPARE(text->cursorPosition(), before);
}

void terminal_view_test::every_control_announces_a_name() {
	/*
	 * A control with no accessible name is announced by its role alone --
	 * "edit box", with nothing to say WHICH. The desktop rows are a label
	 * and a field side by side in a box layout, which does not make the
	 * label a buddy the way QFormLayout::addRow does, so four of them were
	 * anonymous; the touch list's two controls are single glyphs and were
	 * read out as "vertical ellipsis" and "plus".
	 */
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));

	bssh_session_dialog dialog(&store);
	dialog.resize(760, 520);
	dialog.show();

	int anonymous = 0;
	for (QWidget *w : dialog.findChildren<QWidget *>()) {
		const bool interesting = qobject_cast<QLineEdit *>(w) || qobject_cast<QSpinBox *>(w);
		if (!interesting || !w->isVisible()) continue;

		/* A spin box owns an internal line edit; the container is what a
		 * reader lands on, not its guts. */
		bool internal = false;
		for (QWidget *up = w->parentWidget(); up; up = up->parentWidget()) {
			if (qobject_cast<QSpinBox *>(up)) internal = true;
		}
		if (internal) continue;

		QAccessibleInterface *a = QAccessible::queryAccessibleInterface(w);
		if (!a) continue;
		if (a->text(QAccessible::Name).trimmed().isEmpty()) ++anonymous;
	}

	QCOMPARE(anonymous, 0);
}

void terminal_view_test::the_contrast_floor_can_be_turned_off() {
	/*
	 * The floor is a property of the READER, not of the palette (sec 9.3):
	 * schemes stay faithful to what their authors published, and the lift
	 * happens while drawing. So it has to be switchable, and a view has to
	 * change what it draws when it is switched.
	 */
	bssh_terminal_view view;
	QVERIFY2(view.raise_contrast(), "on by default -- several shipped schemes need it");

	view.set_raise_contrast(false);
	QVERIFY(!view.raise_contrast());
	view.set_raise_contrast(true);
	QVERIFY(view.raise_contrast());

	/* The setting itself round-trips, and defaults to on. */
	QTemporaryDir dir;
	{
		bssh_app_settings settings(dir.path());
		QVERIFY2(settings.raise_contrast(), "the default must be on, not merely absent");
		settings.set_raise_contrast(false);
	}
	QVERIFY(!bssh_app_settings(dir.path()).raise_contrast());
}

void terminal_view_test::dimming_takes_the_text_and_the_blocks_but_never_the_background() {
	/*
	 * sec 9.3's night knob. Three things have to be true at once, and the
	 * first version of it only managed two.
	 *
	 * What went wrong is worth keeping: the dim was applied to the glyph
	 * colour only, so an INVERSE cell -- a solid rectangle drawn in what
	 * was the foreground, and the brightest object on the screen -- came
	 * out at full strength while everything around it dimmed. The setting
	 * missed the one thing most likely to have prompted it. Measured
	 * rather than noticed: text went 255 to 127 and the block stayed 255.
	 *
	 * So this asserts on painted pixels, which is the only place the
	 * distinction between "a colour we computed" and "a colour that
	 * reached the screen" is visible.
	 */
	bssh_emulator emulator(24, 2);
	emulator.feed(QByteArrayLiteral("\x1b[7mINVERSE\x1b[0m WHITE"));

	bssh_terminal_view view;
	view.attach_screen(&emulator.screen());
	view.set_colour_scheme(QStringLiteral("default"));
	view.resize(24 * 16, 2 * 32);

	const auto brightest = [](const QImage &image, int x0, int x1, int y0, int y1) {
		int best = -1;
		QRgb found = 0;
		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				const QRgb p = image.pixel(x, y);
				const int sum = qRed(p) + qGreen(p) + qBlue(p);
				if (sum > best) {
					best = sum;
					found = p;
				}
			}
		}
		return found;
	};

	const int run = qMin(view.width(), 7 * view.cell_width());
	const int row = qMin(view.height(), view.cell_height());
	QVERIFY(run > 0 && row > 0);

	view.set_text_brightness(100);
	const QImage full = view.grab().toImage();

	view.set_text_brightness(50);
	const QImage half = view.grab().toImage();

	/* The inverse block: white at full strength, halved when dimmed. */
	const QRgb block_full = brightest(full, 0, run, 0, row);
	const QRgb block_half = brightest(half, 0, run, 0, row);
	QCOMPARE(qRed(block_full), 255);
	QVERIFY2(qRed(block_half) < 200,
	          "an inverse block is the brightest thing on screen and must dim with it");

	/* Ordinary text on the same row, past the inverse run. */
	const QRgb text_full = brightest(full, run, view.width(), 0, row);
	const QRgb text_half = brightest(half, run, view.width(), 0, row);
	QVERIFY(qRed(text_full) > qRed(text_half));

	/*
	 * And the background is untouched at every setting, which is what
	 * makes this a night mode rather than a washed-out one: pure black
	 * stays pure black, and only the light on it comes down.
	 */
	const int deep_y = view.height() - 2;
	QCOMPARE(qRed(full.pixel(view.width() - 2, deep_y)), 0);
	QCOMPARE(qRed(half.pixel(view.width() - 2, deep_y)), 0);

	/* 100 is genuinely untouched, not merely close. */
	view.set_text_brightness(100);
	QCOMPARE(view.grab().toImage(), full);
}

void terminal_view_test::the_floor_lifts_glyphs_and_leaves_backgrounds_faithful() {
	/*
	 * Order matters, and the first version had it wrong. The floor
	 * (sec 9.3) must be applied AFTER inverse and selection have swapped
	 * foreground and background, to whatever ends up drawing the glyph.
	 *
	 * Applied before, a lifted colour lands behind an inverse cell: the
	 * block is repainted a colour the scheme does not contain, for no
	 * readability gain, because what needs to be readable on that block
	 * is the text -- which is the other colour entirely. A background is
	 * not text, and only text is lifted. Contrast being symmetric, the
	 * bug was invisible to any ratio check, which is why this asserts on
	 * the painted block instead.
	 */
	bssh_emulator emulator(24, 2);
	emulator.feed(QByteArrayLiteral("\x1b[7;34mINVERSE BLUE\x1b[0m"));

	bssh_terminal_view view;
	view.attach_screen(&emulator.screen());
	view.set_raise_contrast(true);
	view.resize(24 * 16, 2 * 32);

	const QImage painted = view.grab().toImage();

	const bssh_colour_scheme_t *scheme =
	        bssh_colour_scheme_catalog::instance().by_id(QStringLiteral("default"));
	QVERIFY(scheme);
	const QRgb published = scheme->ansi[4];
	QVERIFY2(bssh_contrast_ratio(published, scheme->background) < BSSH_CONTRAST_FLOOR,
	          "this test needs a colour that the floor would lift");

	/*
	 * The commonest colour across the inverse run is its background, and
	 * it must be exactly what the scheme publishes.
	 */
	QHash<QRgb, int> seen;
	/* The run's real rectangle, from the view's own cell metrics -- a
	 * guessed pixel region samples whatever the font happened to make. */
	const int run_width = qMin(painted.width(), 12 * view.cell_width());
	const int run_height = qMin(painted.height(), view.cell_height());
	QVERIFY(run_width > 0 && run_height > 0);

	for (int y = 0; y < run_height; ++y) {
		for (int x = 0; x < run_width; ++x) seen[painted.pixel(x, y)]++;
	}

	QRgb commonest = 0;
	int best = 0;
	for (auto it = seen.constBegin(); it != seen.constEnd(); ++it) {
		if (it.value() > best) { best = it.value(); commonest = it.key(); }
	}

	QCOMPARE(commonest, published);
	QVERIFY2(commonest != bssh_raise_contrast(published, scheme->background),
	          "an inverse cell's background must keep the palette's own colour");
}

namespace {

/*
 * installUpdateHandler takes a plain function pointer, so what it records
 * has to live at file scope. Types only -- the assertion is about which
 * events were announced, not their payloads.
 */
QList<QAccessible::Event> g_announced;

void record_accessible_event(QAccessibleEvent *event) {
	if (event) g_announced.append(event->type());
}

} /* namespace */

void terminal_view_test::a_selection_change_is_announced_not_only_answerable() {
	/*
	 * sec 14's M11 exposes the selection through the text interface, and
	 * selectionCount()/selection() answer correctly whenever they are
	 * asked. Nothing ever ANNOUNCED a change, so an assistive client
	 * learned of one only by re-querying -- and the whole program emitted
	 * exactly one accessibility event, the text update for damage.
	 *
	 * For a terminal that is the wrong half to leave out: the selection is
	 * what the user is about to copy, and being told what it now covers is
	 * the feedback.
	 *
	 * THIS TEST SKIPS ON THE PLATFORM THE SUITE ACTUALLY RUNS ON, and the
	 * skip is the finding rather than an inconvenience. Both emitters are
	 * guarded on QAccessible::isActive(), which is false with no client
	 * attached -- deliberately, since flattening the screen for an event
	 * nobody receives would put that cost on every drag. Under the
	 * offscreen platform QAccessible::setActive(true) does not take at
	 * all, measured here rather than assumed, so no announcement this
	 * program makes can be observed. The interface is testable because it
	 * is queried directly and bypasses the guard; the NOTIFICATIONS are
	 * not, and never have been.
	 *
	 * Kept rather than deleted so the gap is visible and so this starts
	 * asserting the moment it runs somewhere accessibility can be turned
	 * on.
	 */
	bssh_emulator emulator(30, 3);
	emulator.feed(QByteArrayLiteral("hello\r\nworld\r\n"));

	bssh_terminal_view view;
	show_and_resize(view, 600, 120);
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());

	const bool was_active = QAccessible::isActive();
	QAccessible::setActive(true);
	if (!QAccessible::isActive()) {
		QAccessible::setActive(was_active);
		QSKIP("accessibility cannot be activated here, so no announcement is observable");
	}

	QAccessible::UpdateHandler previous =
	        QAccessible::installUpdateHandler(record_accessible_event);

	g_announced.clear();
	view.select_all();
	QVERIFY2(g_announced.contains(QAccessible::TextSelectionChanged),
	          "selecting has to be announced, not merely answerable");

	/*
	 * And clearing it, which is the half a reader needs most: a selection
	 * that silently went away leaves somebody believing they still have
	 * something to copy.
	 */
	g_announced.clear();
	view.clear_selection();
	QVERIFY2(g_announced.contains(QAccessible::TextSelectionChanged),
	          "clearing the selection has to be announced too");

	/*
	 * And the CARET, which had the same defect one function above the one
	 * that fixed it here. cursorPosition() answered correctly whenever it
	 * was asked and nothing ever said it had moved.
	 *
	 * For a terminal the caret is the worse half to leave out: it is
	 * where your own typing appears, and following it is how a reader
	 * follows you typing. libvterm's movecursor callback routes through
	 * notify_damaged() like everything else, so a move reached a client
	 * as "the text changed" and never as "the caret moved".
	 */
	g_announced.clear();
	emulator.feed(QByteArrayLiteral("abc"));
	view.on_screen_damaged();
	QVERIFY2(g_announced.contains(QAccessible::TextCaretMoved),
	          "typing moves the caret, and that has to be announced");

	/*
	 * And NOT announced when it did not move. Output that leaves the
	 * cursor where it was is the common case -- a scrolling log ends on
	 * the same line -- and a caret event per damage would be a reader
	 * saying it moved when it did not.
	 */
	g_announced.clear();
	view.on_screen_damaged();
	QVERIFY2(!g_announced.contains(QAccessible::TextCaretMoved),
	          "damage that does not move the caret must not announce one");

	QAccessible::installUpdateHandler(previous);
	QAccessible::setActive(was_active);
}

void terminal_view_test::trimming_the_history_reaches_the_reader_too() {
	/*
	 * sec 10.2's scrollback_lines is a per-host setting, so the history a
	 * reader is holding can be cut from under it while it reads.
	 *
	 * The flattened text is cached against the view's content revision,
	 * and that revision is bumped by on_screen_damaged() -- which a trim
	 * does not cause. set_scrollback_lines() removes lines and damages
	 * NOTHING, which is the same property that once left the scrollbar
	 * advertising history that had been dropped (sec 9.5). That was fixed
	 * for the scrollbar by wiring scrollback_changed; the accessibility
	 * cache is the same signal's second consumer and was never wired.
	 *
	 * So a reader kept describing lines that no longer existed, with every
	 * offset past the trim wrong -- and characterRect asks the view for
	 * geometry using history_base(), which the trim DID move, so the
	 * rectangles pointed at the wrong rows as well.
	 *
	 * Who is in that condition matters more than how narrow it looks: it
	 * is people who need the screen described to them, reading a terminal
	 * whose history a host setting just shortened.
	 */
	bssh_emulator emulator(30, 3);
	for (int i = 1; i <= 40; ++i) {
		emulator.feed(QByteArrayLiteral("line ") + QByteArray::number(i) +
		               QByteArrayLiteral("\r\n"));
	}
	QVERIFY2(emulator.scrollback().count() > 10, "the test needs history to trim");

	bssh_terminal_view view;
	show_and_resize(view, 600, 120);
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	QAccessibleInterface *a = QAccessible::queryAccessibleInterface(&view);
	QVERIFY(a);
	QAccessibleTextInterface *text = a->textInterface();
	QVERIFY(text);

	/* Read once, so the flattened text is cached at the old size. */
	const QString before = a->text(QAccessible::Value);
	QVERIFY2(before.contains(QStringLiteral("line 2")),
	          "the early history has to be there before it can be dropped");
	const int count_before = text->characterCount();
	QCOMPARE(count_before, before.size());

	emulator.set_scrollback_lines(3);
	QCoreApplication::processEvents();

	QVERIFY2(emulator.scrollback().count() <= 3, "the trim itself has to have happened");

	const QString after = a->text(QAccessible::Value);

	/*
	 * The control, so this cannot pass against a reader that went blank:
	 * what is still held must still be readable.
	 */
	QVERIFY2(after.contains(QStringLiteral("line 40")),
	          "the live screen must survive a trim");

	QVERIFY2(!after.contains(QStringLiteral("line 2")),
	          "a reader must not go on describing history that was dropped");
	QCOMPARE(text->characterCount(), after.size());
	QVERIFY2(text->characterCount() < count_before,
	          "the reader's text has to shrink when the history does");
}

void terminal_view_test::a_reader_can_reach_the_scrollback_and_the_selection() {
	/*
	 * History is most of what a terminal has to say. Exposing only the
	 * visible screen (sec 14, M11) meant a reader's view stopped at
	 * whatever happened to be on it -- a build log a sighted user can
	 * scroll back through was simply gone.
	 */
	bssh_emulator emulator(30, 3);
	for (int i = 1; i <= 8; ++i) {
		emulator.feed(QByteArrayLiteral("line ") + QByteArray::number(i) +
		               QByteArrayLiteral("\r\n"));
	}
	QVERIFY2(emulator.scrollback().count() >= 5, "the test needs lines to have scrolled off");

	bssh_terminal_view view;
	show_and_resize(view, 600, 120);
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());

	QAccessibleInterface *a = QAccessible::queryAccessibleInterface(&view);
	QVERIFY(a);
	QAccessibleTextInterface *text = a->textInterface();
	QVERIFY(text);

	const QString all = a->text(QAccessible::Value);
	QVERIFY2(all.contains(QStringLiteral("line 1")),
	          "a line that scrolled into history must still be readable");
	QVERIFY(all.contains(QStringLiteral("line 8")));

	/* History comes first, so the reader meets it in the order it happened. */
	QVERIFY(all.indexOf(QStringLiteral("line 1")) < all.indexOf(QStringLiteral("line 8")));
	QCOMPARE(text->characterCount(), all.size());

	/*
	 * The cursor is on the SCREEN, which now starts partway through the
	 * text rather than at zero -- getting that wrong puts the reader's
	 * caret somewhere in the history.
	 */
	QVERIFY2(text->cursorPosition() > all.indexOf(QStringLiteral("line 8")),
	          "the caret must land on the live screen, not in the scrollback");

	/*
	 * And the selection, which could not be reported at all while only
	 * the screen was exposed: one starting in the scrollback had no
	 * offset to start at.
	 */
	QCOMPARE(text->selectionCount(), 0);

	/* Scrolled back, so the drag lands on a line that only exists in
	 * history -- the case that had no offset to report before. */
	view.scroll_by(3);
	drag(view, 0, 0, 4, 0);
	QVERIFY(view.has_selection());

	QCOMPARE(text->selectionCount(), 1);

	int start = -1;
	int end = -1;
	text->selection(0, &start, &end);
	QVERIFY(start >= 0);
	QVERIFY2(end > start, "a selection spanning the buffer must span offsets too");
	QVERIFY(end <= text->characterCount());
}

void terminal_view_test::a_readers_rectangles_follow_the_grid_and_the_scroll() {
	bssh_terminal_view view;
	view.set_scrollbar_position(bssh_terminal_view::SCROLLBAR_LEFT);
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	for (int i = 0; i < view.rows() + 6; ++i) {
		emulator.feed(QByteArrayLiteral("a line of text\r\n"));
	}
	QVERIFY(emulator.scrollback().count() > 3);

	QAccessibleInterface *a = QAccessible::queryAccessibleInterface(&view);
	QVERIFY(a);
	QAccessibleTextInterface *text = a->textInterface();
	QVERIFY(text);

	/*
	 * A character on the FIRST screen row: it is on screen now and stays
	 * on screen when the view scrolls back a line, which is what lets the
	 * two assertions below be about the mapping rather than about a
	 * character that fell off the bottom.
	 */
	const QStringList rows = a->text(QAccessible::Value).split(QLatin1Char('\n'));
	QVERIFY(rows.size() > view.rows());

	int probe = 0;
	for (int i = 0; i < rows.size() - view.rows(); ++i) probe += rows.at(i).size() + 1;

	/*
	 * And five columns IN, not at the row start. Column 0 is the one
	 * place a horizontal error is invisible: cell_at clamps a negative
	 * column to zero, so a rectangle shifted left by a whole scrollbar
	 * still maps back to the character it named. The sabotage runner said
	 * so -- "a rectangle forgets the scrollbar's side" was reported MISSED
	 * against the first version of this test, which probed column 0.
	 */
	probe += 5;
	QVERIFY(probe > 0);

	const QRect rect = text->characterRect(probe);
	QVERIFY2(!rect.isNull(), "the first screen row must have a rectangle");

	/*
	 * Round trip: the middle of a character's rectangle must map back to
	 * that character. This is what a magnifier does, and it is the only
	 * form that cannot pass by agreeing with a second copy of the
	 * arithmetic -- which is precisely what the old code was.
	 *
	 * The scrollbar is on the LEFT deliberately: the old code used the
	 * bare grid inset rather than the origin, so every rectangle sat a
	 * scrollbar's width -- two columns -- left of the glyph it named, and
	 * a right-hand bar would have hidden that behind a single pixel.
	 */
	QCOMPARE(text->offsetAtPoint(rect.center()), probe);

	/*
	 * And it follows the view. Scrolled back one line, the same character
	 * is drawn one row further down; the old code ignored the offset, so
	 * every rectangle stayed where it was.
	 */
	view.set_scroll_offset(1);
	QCOMPARE(view.scroll_offset(), 1);

	const QRect moved = text->characterRect(probe);
	QVERIFY2(!moved.isNull(), "one line back, the row is still on screen");
	QCOMPARE(moved, rect.translated(0, view.cell_height()));
	QCOMPARE(text->offsetAtPoint(moved.center()), probe);
}

void terminal_view_test::a_reader_counts_characters_where_the_grid_counts_cells() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	/*
	 * Two double-width glyphs and then an ASCII marker. The far half of a
	 * wide glyph is not a character, so it is absent from the text a
	 * reader is given -- which means the marker is character 2 and cell
	 * column 4, and every place that used one number for the other put a
	 * magnifier two cells left of the glyph it was naming.
	 */
	emulator.feed(QString::fromUtf8("\xe6\x96\x87\xe6\x9b\xb8X").toUtf8());

	QAccessibleInterface *a = QAccessible::queryAccessibleInterface(&view);
	QVERIFY(a);
	QAccessibleTextInterface *text = a->textInterface();
	QVERIFY(text);

	const QString value = a->text(QAccessible::Value);
	const int marker = value.indexOf(QLatin1Char('X'));
	QVERIFY2(marker >= 0, qPrintable(value.left(20)));

	const QRect rect = text->characterRect(marker);
	QVERIFY(!rect.isNull());

	/*
	 * Column 4, not column 2. Measured against the view's own cell width
	 * rather than a literal, so the assertion holds whatever font the
	 * host happens to have.
	 */
	const QRect origin = text->characterRect(0);
	QVERIFY(!origin.isNull());
	QCOMPARE(rect.left() - origin.left(), 4 * view.cell_width());

	/* And back again, which is what a touch-exploration gesture does. */
	QCOMPARE(text->offsetAtPoint(rect.center()), marker);
}

void terminal_view_test::copying_a_wrapped_line_gives_back_one_line() {
	/*
	 * A command too long for the window is ONE line that ran past the
	 * right edge, not two. Copying it joined every row with a newline, so
	 * pasting it into a shell ran the first half as a command and then
	 * the rest as another -- which is the same hazard as the trailing
	 * padding this function already trims, several orders of magnitude
	 * worse.
	 *
	 * Measured before the fix on this fixture: one newline in the middle
	 * of a single echo.
	 *
	 * libvterm tracks it per row and offers it two ways: sb_pushline4
	 * carries the bit for a line leaving the screen, and
	 * vterm_state_get_lineinfo answers for the rows still on it. The
	 * pushline4 callback was declared in the table and set to nullptr, so
	 * the bit was never asked for.
	 */
	bssh_terminal_view view;
	show_and_resize(view, 640, 96);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	const QByteArray tail(view.cols() + 12, 'a');
	const QByteArray typed = QByteArrayLiteral("echo ") + tail;
	emulator.feed(typed);

	view.select_all();
	const QString text = view.selection_text().trimmed();

	QCOMPARE(text.count(QLatin1Char('\n')), 0);
	QCOMPARE(text, QString::fromLatin1(typed));
}

void terminal_view_test::a_real_newline_still_separates_two_lines() {
	/*
	 * The other direction, and the reason the fix cannot simply drop
	 * newlines: two lines the user actually entered must stay two. A
	 * short line followed by another is not a wrap, and libvterm says so.
	 */
	bssh_terminal_view view;
	show_and_resize(view, 640, 96);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	emulator.feed(QByteArrayLiteral("first\r\nsecond"));

	view.select_all();
	const QString text = view.selection_text().trimmed();

	QCOMPARE(text, QStringLiteral("first\nsecond"));
}

void terminal_view_test::a_wrapped_line_stays_one_line_after_it_scrolls_away() {
	/*
	 * The OTHER half, and the one the screen test cannot reach. A row
	 * still on the live screen is answered by libvterm's own line info;
	 * once it has scrolled off, libvterm no longer holds it and the flag
	 * has to have been STORED on the way past -- which is what
	 * sb_pushline4 carries and what the older sb_pushline cannot.
	 *
	 * Written because the first test covered only the screen: the parse
	 * being right is what hides the delivery being wrong, and here the
	 * two halves are answered by two different mechanisms.
	 */
	bssh_terminal_view view;
	show_and_resize(view, 640, 96);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	const QByteArray tail(view.cols() + 12, 'a');
	const QByteArray typed = QByteArrayLiteral("echo ") + tail;
	emulator.feed(typed + QByteArrayLiteral("\r\n"));

	/* Push it well past the top of the screen. */
	for (int i = 0; i < view.rows() + 5; ++i) {
		emulator.feed(QByteArrayLiteral("filler\r\n"));
	}
	QVERIFY(emulator.scrollback().count() > 0);

	view.select_all();
	const QString text = view.selection_text();

	/*
	 * The command comes back whole, from the scrollback, with nothing
	 * inserted where the window happened to end.
	 */
	QVERIFY2(text.contains(QString::fromLatin1(typed)),
	          qPrintable(text.left(200)));
}

void terminal_view_test::select_all_still_reaches_the_screen_once_the_ring_evicts() {
	/*
	 * select_all() named its lines 0 .. count+rows-1, with no
	 * history_base(). Every other producer and consumer of a line name in
	 * this file carries the base -- line_for_row, cell_for_line,
	 * line_is_continuation -- because a name without it stops meaning
	 * anything the moment the ring evicts.
	 *
	 * So once more lines have scrolled off than the ring holds, Select
	 * All selects a window of names that begins below the base: the
	 * evicted end resolves to blanks and the live screen falls outside
	 * the selection entirely. The painting agrees with the anchors, so it
	 * looks right and only the clipboard disagrees -- which is word for
	 * word how the original base() defect behaved.
	 *
	 * The test beside this one feeds twenty lines into a five-thousand
	 * line ring, so its base is zero and it cannot see any of this.
	 */
	bssh_terminal_view view;
	show_and_resize(view, 640, 96);

	bssh_emulator emulator(view.cols(), view.rows());
	emulator.set_scrollback_lines(8);
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	for (int i = 0; i < 60; ++i) {
		emulator.feed(QByteArrayLiteral("line") + QByteArray::number(i)
		               + QByteArrayLiteral("\r\n"));
	}
	emulator.feed(QByteArrayLiteral("ON-SCREEN-MARKER"));

	QVERIFY2(emulator.scrollback().base() > 0,
	          "the ring must have evicted for this to mean anything");

	view.select_all();
	QVERIFY(view.has_selection());

	const QString text = view.selection_text();

	/* What the user is looking at has to be in what they just selected. */
	QVERIFY2(text.contains(QStringLiteral("ON-SCREEN-MARKER")),
	          qPrintable(QStringLiteral("select all missed the live screen; got %1 chars: %2")
	                             .arg(text.size()).arg(text.left(120))));
}

void terminal_view_test::select_all_reaches_the_scrollback_and_the_screen() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 96);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	for (int i = 0; i < 20; ++i) {
		emulator.feed(QByteArrayLiteral("row") + QByteArray::number(i)
		               + QByteArrayLiteral("\r\n"));
	}
	QVERIFY(emulator.scrollback().count() > 0);

	view.select_all();
	QVERIFY(view.has_selection());

	/*
	 * Both halves, which is the whole point of the one coordinate space:
	 * the oldest line is in the scrollback and the newest is on screen,
	 * and a "select all" that stopped at the fold would quietly copy the
	 * visible part only.
	 */
	const QString text = view.selection_text();
	QVERIFY2(text.contains(QStringLiteral("row0")), qPrintable(text.left(60)));
	QVERIFY2(text.contains(QStringLiteral("row19")), qPrintable(text.right(60)));
}

void terminal_view_test::middle_click_pastes_the_primary_selection() {
	bssh_terminal_view view;
	show_and_resize(view, 320, 96);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_emulator(&emulator);

	QSignalSpy asked(&view, &bssh_terminal_view::primary_paste_requested);

	QTest::mouseClick(&view, Qt::MiddleButton, Qt::NoModifier, QPoint(20, 20));

	/*
	 * The view asks; the window reads the PRIMARY selection and pastes
	 * it. Asserted at the ask, because the buffer itself is the
	 * platform's and a headless test has no reliable one -- what was
	 * broken was that the middle button did nothing whatsoever.
	 */
	QCOMPARE(asked.count(), 1);
}

void terminal_view_test::the_remote_keeps_the_right_button_when_it_asked_for_the_mouse() {
	bssh_terminal_view view;
	show_and_resize(view, 320, 96);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_emulator(&emulator);

	QSignalSpy menus(&view, &bssh_terminal_view::context_menu_requested);

	/* Mouse reporting on: the right button is the program's. */
	emulator.feed(QByteArrayLiteral("\x1b[?1000h"));
	QVERIFY(emulator.mouse_reporting());

	QContextMenuEvent claimed(QContextMenuEvent::Mouse, QPoint(20, 20),
	                           view.mapToGlobal(QPoint(20, 20)));
	QApplication::sendEvent(&view, &claimed);
	QCOMPARE(menus.count(), 0);

	/*
	 * Shift overrides it, the same convention that lets a user select
	 * text out of a full-screen program (sec 6.4).
	 */
	QContextMenuEvent overridden(QContextMenuEvent::Mouse, QPoint(20, 20),
	                              view.mapToGlobal(QPoint(20, 20)), Qt::ShiftModifier);
	QApplication::sendEvent(&view, &overridden);
	QCOMPARE(menus.count(), 1);
}

void terminal_view_test::the_menu_layout_follows_the_device_until_told_otherwise() {
	/*
	 * Automatic means the device decides, which is the default and
	 * differs between a desktop and a phone. Anything else is the user
	 * overruling it, and then the device does not get a say -- a
	 * convertible in tablet mode is exactly why the override exists.
	 */
	QCOMPARE(bssh_main_window::context_menu_layout(
	                 bssh_app_settings::CONTEXT_MENU_AUTO, false),
	          bssh_app_settings::CONTEXT_MENU_DESKTOP);
	QCOMPARE(bssh_main_window::context_menu_layout(
	                 bssh_app_settings::CONTEXT_MENU_AUTO, true),
	          bssh_app_settings::CONTEXT_MENU_TOUCH);

	QCOMPARE(bssh_main_window::context_menu_layout(
	                 bssh_app_settings::CONTEXT_MENU_TOUCH, false),
	          bssh_app_settings::CONTEXT_MENU_TOUCH);
	QCOMPARE(bssh_main_window::context_menu_layout(
	                 bssh_app_settings::CONTEXT_MENU_DESKTOP, true),
	          bssh_app_settings::CONTEXT_MENU_DESKTOP);
}

namespace {

/*
 * The bindings are process-wide, so they must go back however this test
 * leaves -- an assertion failing partway would otherwise hand every later
 * test a rebound Copy.
 */
struct bindings_guard {
	~bindings_guard() { bssh_input_router::set_bindings(QHash<QString, QString>()); }
};

} /* namespace */

void terminal_view_test::the_menu_shows_the_chord_the_router_would_actually_steal() {
	/*
	 * The context menu wrote its Copy and Paste chords out as literals --
	 * "Ctrl+Shift+C" and "Ctrl+Shift+V" -- while every other menu in this
	 * window asks bssh_input_router::binding_for(). So a user who rebound
	 * copy got a menu still advertising the chord they had just replaced,
	 * and the app told them, in writing, to press a key that no longer
	 * did anything.
	 *
	 * It is the failure the router's own table exists to prevent, in its
	 * words: "the list a user is shown and the list the router acts on
	 * cannot be two lists". A literal is a second list that agrees right
	 * up until somebody changes the first, which is why the menu test
	 * next door passed throughout -- it reads the item TEXT, and the text
	 * was never wrong.
	 */
	bindings_guard guard;

	/*
	 * The window is built FIRST: its constructor loads the bindings out
	 * of the app settings and calls set_bindings itself, so anything set
	 * before it is discarded. The menu is what this is about, and it is
	 * built fresh on every right-click -- which is exactly when
	 * binding_for is consulted.
	 */
	bssh_main_window window;

	QHash<QString, QString> bindings;
	bindings.insert(QStringLiteral("copy"), QStringLiteral("Ctrl+Alt+K"));
	bssh_input_router::set_bindings(bindings);

	QMenu *menu = window.build_terminal_menu(&window);
	QVERIFY(menu);

	QAction *copy = menu->findChild<QAction *>(QStringLiteral("terminal_copy"));
	QVERIFY2(copy, "the context menu has no Copy action to check");

	QCOMPARE(copy->shortcut(), QKeySequence(QStringLiteral("Ctrl+Alt+K")));

	/* And specifically not the chord it used to hardcode. */
	QVERIFY2(copy->shortcut() != QKeySequence(QStringLiteral("Ctrl+Shift+C")),
	          "the menu is still advertising the replaced chord");

	/*
	 * Paste was NOT rebound, so it must still show its default -- which
	 * is what keeps this a check on the lookup rather than on the two
	 * actions having been blanked.
	 */
	QAction *paste = menu->findChild<QAction *>(QStringLiteral("terminal_paste"));
	QVERIFY2(paste, "the context menu has no Paste action to check");
	QCOMPARE(paste->shortcut(), QKeySequence(QStringLiteral("Ctrl+Shift+V")));
}

void terminal_view_test::both_menus_agree_on_when_paste_is_offered() {
	/*
	 * This window offers Paste from two menus, and they disagreed: the
	 * menu bar enabled it whenever a session existed, while the
	 * right-click menu also required something on the clipboard. The same
	 * verb, at the same instant, greyed in one place and offered in the
	 * other.
	 *
	 * The bar could not have asked the better question. It re-derives on
	 * session and binding changes, and Copy additionally on
	 * selection_changed -- but nothing watched the CLIPBOARD, so an
	 * answer depending on it would have been whatever was true when the
	 * last session appeared. The missing subscription is the bug; the
	 * weaker rule is what it left behind.
	 *
	 * Asserted as AGREEMENT between the two menus, which is the property
	 * that was broken, and then that the agreed answer actually tracks
	 * the clipboard -- so a window that simply disabled Paste everywhere
	 * would not pass.
	 */
	bssh_main_window window;

	/*
	 * WITH a session, which is the only state where the two rules can
	 * differ: with none, "is there a session" and "is there a session and
	 * something to paste" are both false, and the menus agree for the
	 * wrong reason. The first version of this test had no session and
	 * passed against the bug -- sabotage is what caught it.
	 */
	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);

	bssh_profile_t profile;
	profile.name = QStringLiteral("pastes");
	profile.host = QStringLiteral("pastes.example");
	window.adopt_session(registry->create(profile));

	const auto context_paste_enabled = [&window]() {
		QMenu *menu = window.build_terminal_menu(&window);
		if (!menu) return false;
		const QAction *paste =
		        menu->findChild<QAction *>(QStringLiteral("terminal_paste"));
		const bool enabled = paste && paste->isEnabled();
		delete menu;
		return enabled;
	};

	QAction *bar_paste = window.findChild<QAction *>(QStringLiteral("menu_paste"));
	QVERIFY2(bar_paste, "the menu bar has no Paste action to compare against");

	QGuiApplication::clipboard()->clear();
	QCOMPARE(context_paste_enabled(), bar_paste->isEnabled());

	QGuiApplication::clipboard()->setText(QStringLiteral("something to paste"));
	QCOMPARE(context_paste_enabled(), bar_paste->isEnabled());

	/*
	 * And the agreed answer actually TRACKS the clipboard, so a window
	 * that disabled Paste everywhere would not pass either.
	 */
	QGuiApplication::clipboard()->setText(QStringLiteral("more"));
	QVERIFY2(bar_paste->isEnabled(), "paste stayed off with a session and a full clipboard");

	QGuiApplication::clipboard()->clear();
	QVERIFY2(!bar_paste->isEnabled(), "paste was offered with an empty clipboard");
	QCOMPARE(context_paste_enabled(), bar_paste->isEnabled());
}

void terminal_view_test::the_terminal_menu_offers_the_three_verbs_in_order() {
	bssh_main_window window;

	QMenu *menu = window.build_terminal_menu(&window);
	QVERIFY(menu);

	const QList<QAction *> actions = menu->actions();
	QCOMPARE(actions.size(), 13);

	/*
	 * The order is the Windows Interface Guidelines' and every desktop
	 * terminal since: the edit verbs first in their canonical order, then
	 * a separator, then what is specific to the object. Asserted because
	 * it is the part a later edit would casually reorder, and because a
	 * menu whose items move is one nobody can use without reading.
	 */
	QCOMPARE(actions.at(0)->text(), QStringLiteral("&Copy"));
	QCOMPARE(actions.at(1)->text(), QStringLiteral("&Paste"));
	QVERIFY(actions.at(2)->isSeparator());
	QCOMPARE(actions.at(3)->text(), QStringLiteral("Select &All"));

	/*
	 * Find beside Select All, because it acts on the text in front of
	 * you like the verbs above it -- and because the chord was its only
	 * route until now, which on a phone meant no route at all.
	 */
	QCOMPARE(actions.at(4)->text(), QStringLiteral("&Find..."));
	QCOMPARE(actions.at(5)->text(), QStringLiteral("&Terminal"));
	QVERIFY2(actions.at(5)->menu(), "Terminal must be a submenu, not a verb");

	/*
	 * Then the two GROUPS, and they are groups for a measured reason
	 * rather than a tidy one: flat, this menu wanted 776px of a 640dp
	 * cover screen, because touch gives every row sec 6.2's 48dp.
	 * the_touch_terminal_menu_fits_a_phone is the assertion that says
	 * so, and this one pins what the grouping did to the shape.
	 */
	QVERIFY(actions.at(6)->isSeparator());
	QCOMPARE(actions.at(7)->text(), QStringLiteral("Text &Size"));
	QVERIFY2(actions.at(7)->menu(), "Text Size must be a submenu, not a verb");

	QVERIFY(actions.at(8)->isSeparator());
	QCOMPARE(actions.at(9)->text(), QStringLiteral("&Pane"));
	QVERIFY2(actions.at(9)->menu(), "Pane must be a submenu, not a verb");

	QCOMPARE(actions.at(10)->text(), QStringLiteral("&Session"));
	QVERIFY2(actions.at(10)->menu(), "Session must be a submenu, not a verb");

	/*
	 * And the way to the configuration, behind its own separator (sec 2):
	 * with the desktop opening straight into a local shell, this menu is
	 * a pointer's only route to the host list and the settings. It is a
	 * different kind of thing again, and separated for the same reason
	 * Select All is separated from Copy.
	 */
	QVERIFY(actions.at(11)->isSeparator());
	QCOMPARE(actions.at(12)->text(), QStringLiteral("&Hosts and Settings"));

	/*
	 * The groups' own contents, in their own order. Grouping moved these
	 * rows; it must not have lost one, and a test that only counted the
	 * top level would not notice if it had.
	 */
	const QList<QAction *> sizes = actions.at(7)->menu()->actions();
	QCOMPARE(sizes.size(), 3);
	/* Plain words under "Text Size" (the holder, 2026-10-22): "Zoom In"
	 * under a menu already called Text Size said it twice and less clearly. */
	QCOMPARE(sizes.at(0)->text(), QStringLiteral("&Bigger"));
	QCOMPARE(sizes.at(1)->text(), QStringLiteral("&Smaller"));
	QCOMPARE(sizes.at(2)->text(), QStringLiteral("&Normal"));

	/*
	 * Five since 2026-09-22: Detach sits beside Split, where the menu bar
	 * has always kept it, because they are the two halves of one question
	 * about where a session lives. It was the last verb sec 8.3 had put
	 * on a desktop-only surface and sec 8.7 had left there.
	 */
	const QList<QAction *> panes = actions.at(9)->menu()->actions();
	QCOMPARE(panes.size(), 5);
	QCOMPARE(panes.at(0)->text(), QStringLiteral("Split &Beside Current"));
	QCOMPARE(panes.at(1)->text(), QStringLiteral("Show as &Tab"));
	QCOMPARE(panes.at(2)->text(), QStringLiteral("&Detach to a Window"));
	QCOMPARE(panes.at(3)->text(), QStringLiteral("Move Pane &Left"));
	QCOMPARE(panes.at(4)->text(), QStringLiteral("Move Pane &Right"));

	/*
	 * Access keys on every verb, which is what makes the menu usable from
	 * the keyboard and is half of what those guidelines are about. Walked
	 * into the groups as well, since a row that moved into one did not
	 * stop being a verb.
	 */
	/*
	 * The session group, whose last two verbs were unreachable on touch
	 * until sec 8.7's column left them without a tab bar to live on.
	 * Close sits behind a separator because it is the one here that
	 * cannot be undone.
	 */
	/*
	 * Five here rather than six: this window has no session, so Connect
	 * is absent by the same rule the tab menu follows -- a verb offered
	 * only where it applies. a_session_can_be_closed_without_a_tab_bar
	 * is where its presence is asserted, with a session that is not live.
	 */
	/*
	 * Putting the terminal back in order: the history, and the modes a
	 * dead program left behind. Rows in a group rather than at the top
	 * level, because a fourteenth top-level row does not fit a 640dp
	 * cover screen. The third says what the connection has carried and
	 * whether a mode is changing the keys -- what tells a user whether
	 * Reset above is the cure. The fourth brings back the tmux and screen
	 * drawer (sec 10.2a), here disabled: there is no session to list for.
	 */
	const QList<QAction *> state = actions.at(5)->menu()->actions();
	QCOMPARE(state.size(), 4);
	QCOMPARE(state.at(0)->text(), QStringLiteral("Clear Scroll&back"));
	QCOMPARE(state.at(1)->text(), QStringLiteral("&Reset Terminal"));
	QCOMPARE(state.at(2)->text(), QStringLiteral("Connection &Details"));
	QCOMPARE(state.at(3)->text(), QStringLiteral("tmux and screen &Sessions"));
	QVERIFY(!state.at(3)->isEnabled());

	const QList<QAction *> sessions = actions.at(10)->menu()->actions();
	QCOMPARE(sessions.size(), 5);
	QCOMPARE(sessions.at(0)->text(), QStringLiteral("New &Tab"));
	QCOMPARE(sessions.at(1)->text(), QStringLiteral("New &Window"));
	QCOMPARE(sessions.at(2)->text(), QStringLiteral("&Duplicate"));
	QVERIFY(sessions.at(3)->isSeparator());
	QCOMPARE(sessions.at(4)->text(), QStringLiteral("&Close"));

	QList<QAction *> every = actions;
	every += sizes;
	every += panes;
	every += sessions;
	every += state;
	for (const QAction *action : every) {
		if (action->isSeparator()) continue;
		QVERIFY2(action->text().contains(QLatin1Char('&')),
		          qPrintable(action->text() + QStringLiteral(" has no access key")));
	}

	/*
	 * Copy with nothing selected is DISABLED, not absent. Items that come
	 * and go change the menu's shape, and a hand that has learned where
	 * Paste is should find it there every time.
	 */
	QVERIFY2(!actions.at(0)->isEnabled(), "Copy must be offered and disabled, not hidden");

	/*
	 * And the shape is unchanged by that: the count is asserted again
	 * AFTER the enabled-state check, so an item that "went missing when
	 * unavailable" would be caught here even if the order above still
	 * lined up.
	 */
	QCOMPARE(menu->actions().size(), 13);
}

void terminal_view_test::the_touch_layout_grows_the_rows_and_drops_the_chords() {
	/*
	 * The metrics are the only thing touch changes, and they are shared:
	 * this is driven through the same entry point the tab menu uses, so a
	 * menu added later cannot quietly keep pointer-height rows.
	 *
	 * Its own test rather than one on build_terminal_menu(), because the
	 * layout that function picks comes from the device and the stored
	 * setting, and neither belongs in an assertion about row heights.
	 */
	QMenu menu;
	QAction *verb = menu.addAction(QStringLiteral("&Close"));
	verb->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
	menu.addSeparator();

	bssh_main_window::apply_menu_layout(&menu, bssh_app_settings::CONTEXT_MENU_DESKTOP);
	QVERIFY2(!verb->shortcut().isEmpty(), "a pointer keeps its chord");
	QVERIFY2(menu.styleSheet().isEmpty(), "a pointer keeps its own metrics");

	/*
	 * A submenu's rows are a menu of their own: Qt applies neither the
	 * stylesheet nor the shortcut sweep to them, so the moment this menu
	 * grouped anything the group's contents kept pointer-height rows
	 * with their chords still on. Built here rather than asserted on
	 * build_terminal_menu's own groups, for the reason the comment above
	 * gives -- this is a test about the metrics, not about which verbs
	 * exist.
	 */
	QMenu *group = menu.addMenu(QStringLiteral("&Group"));
	QAction *inner = group->addAction(QStringLiteral("&Inner"));
	inner->setShortcut(QKeySequence(QStringLiteral("Ctrl+I")));

	bssh_main_window::apply_menu_layout(&menu, bssh_app_settings::CONTEXT_MENU_TOUCH);

	/*
	 * The chord goes: it names a key nobody holding a phone has, and the
	 * text is what makes a touch row wide as well as short.
	 */
	QVERIFY2(verb->shortcut().isEmpty(), "the touch layout must drop the chord");

	QVERIFY2(inner->shortcut().isEmpty(),
	          "a row inside a group kept its chord: the layout did not recurse");
	/* Measured, not read out of the stylesheet: the floor is content plus
	 * padding, split since 2026-10-22 so a row is 48dp and not 64. */
	group->sizeHint();
	const int row = group->actionGeometry(inner).height();
	QVERIFY2(row >= 48,
	          qPrintable(QStringLiteral("a group's own rows must reach the 48dp floor, "
	                                     "a row is %1: ").arg(row) + group->styleSheet()));

	/*
	 * And the row reaches sec 6.2's floor. Asserted on the number, not
	 * merely on the stylesheet being non-empty: 48 is the whole point.
	 */
	menu.sizeHint();
	const int verb_row = menu.actionGeometry(verb).height();
	QVERIFY2(verb_row >= 48,
	          qPrintable(QStringLiteral("touch rows must reach the 48dp floor, a row is %1: ")
	                             .arg(verb_row) + menu.styleSheet()));

	/*
	 * And not much past it. Rows were 64dp -- the floor as min-height with
	 * padding on top -- which the holder saw as tiny text in a lot of
	 * empty space, in a menu likely to run off a phone (2026-10-22). The
	 * ceiling leaves room for the larger text and keeps the old height out.
	 */
	QVERIFY2(verb_row <= 56,
	          qPrintable(QStringLiteral("a touch row is %1dp, padding rather than text")
	                             .arg(verb_row)));
}

void terminal_view_test::the_scrollbar_takes_its_width_from_the_grid() {
	bssh_terminal_view view;
	view.resize(800, 400);
	view.show();
	QVERIFY(QTest::qWaitForWindowExposed(&view));

	const int bare = view.cols();
	QVERIFY(bare > 0);

	/*
	 * A scrollbar is not drawn over the terminal: it takes its width from
	 * the grid, and the remote has to be told, or the far end lays out
	 * for a column that is no longer on screen (sec 7).
	 */
	QSignalSpy resized(&view, &bssh_terminal_view::grid_resized);
	view.set_scrollbar_position(bssh_terminal_view::SCROLLBAR_RIGHT);

	QVERIFY2(view.cols() < bare, "a scrollbar must cost the grid its width");
	QVERIFY2(!resized.isEmpty(), "the remote must be told the width changed");

	/* And the same on the left, where the grid also has to MOVE. */
	const int with_bar = view.cols();
	view.set_scrollbar_position(bssh_terminal_view::SCROLLBAR_LEFT);
	QCOMPARE(view.cols(), with_bar);

	/*
	 * The point-to-cell mapping has to have moved with it. Column zero on
	 * the left-hand layout starts after the bar, so a click at the very
	 * left edge is inside the BAR and must not read as a cell in the
	 * middle of the line.
	 */
	QSize needed = view.size_for_grid(20, 5);
	QVERIFY2(needed.width() > 20 * view.cell_width(),
	          "size_for_grid must account for the bar it will have to draw");

	view.set_scrollbar_position(bssh_terminal_view::SCROLLBAR_NONE);
	QCOMPARE(view.cols(), bare);
}

void terminal_view_test::the_scrollbar_follows_the_view_and_drives_it() {
	bssh_emulator emulator(20, 4);

	bssh_terminal_view view;
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	/*
	 * The same connection bssh_pane_container makes for every pane: the
	 * bar learns how much history there is from the damage signal, which
	 * is what tells it the scrollback grew. Attaching the objects without
	 * this is a view that never hears about output.
	 */
	connect(&emulator, &bssh_emulator::damaged, &view,
	         &bssh_terminal_view::on_screen_damaged);

	view.resize(400, 200);
	view.set_scrollbar_position(bssh_terminal_view::SCROLLBAR_RIGHT);
	view.show();
	QVERIFY(QTest::qWaitForWindowExposed(&view));

	/* Enough output to push lines into the scrollback. */
	for (int i = 0; i < 40; ++i) {
		emulator.feed(QStringLiteral("line %1\r\n").arg(i).toUtf8());
	}
	QCoreApplication::processEvents();

	QScrollBar *bar = view.findChild<QScrollBar *>();
	QVERIFY2(bar, "a view told to show a scrollbar must have one");
	QVERIFY2(bar->maximum() > 0, "the bar must know how much history there is");

	/*
	 * The BOTTOM is the live screen, which is where every terminal since
	 * xterm puts it and the opposite of how the offset is counted. A bar
	 * that works and moves the wrong way is worse than no bar.
	 */
	QCOMPARE(bar->value(), bar->maximum());

	view.scroll_by(5);
	QCOMPARE(bar->value(), bar->maximum() - 5);

	/* And dragging it scrolls the view, which is the other direction. */
	bar->setValue(bar->maximum() - 12);
	QCOMPARE(view.scroll_offset(), 12);

	bar->setValue(bar->maximum());
	QCOMPARE(view.scroll_offset(), 0);
}

void terminal_view_test::cleanup() {
	/*
	 * Found the hard way: adding one test that builds a main window
	 * broke three geometry tests further down the file, because the
	 * window applied the settings' scrollbar position to the static
	 * default and left it there. The failures read as grid-arithmetic
	 * bugs and were nothing of the sort.
	 */
	bssh_terminal_view::set_default_scrollbar_position(bssh_terminal_view::SCROLLBAR_NONE);
}

void terminal_view_test::a_dropped_file_arrives_as_a_path_a_shell_can_read() {
	/*
	 * The plumbing is the easy half. What makes this worth testing is the
	 * QUOTING: a path with a space pasted raw is two arguments, and a
	 * path with a quote in it is a syntax error -- or worse, since the
	 * text lands on a command line that is about to be run.
	 */
	QMimeData plain;
	plain.setUrls({ QUrl::fromLocalFile(QStringLiteral("/etc/hosts")) });
	QCOMPARE(bssh_terminal_view::text_for_drop(&plain), QStringLiteral("/etc/hosts"));

	/* Quoted only where it has to be: an ordinary path stays readable. */
	QMimeData spaced;
	spaced.setUrls({ QUrl::fromLocalFile(QStringLiteral("/home/n/my notes.txt")) });
	QCOMPARE(bssh_terminal_view::text_for_drop(&spaced),
	          QStringLiteral("'/home/n/my notes.txt'"));

	/*
	 * The one character single quoting cannot contain. Closing the quote,
	 * emitting an escaped one and reopening is the standard answer, and
	 * getting it wrong ends the quoting early -- which hands the rest of
	 * the filename to the shell as code.
	 */
	QMimeData quoted;
	quoted.setUrls({ QUrl::fromLocalFile(QStringLiteral("/tmp/it's here")) });
	QCOMPARE(bssh_terminal_view::text_for_drop(&quoted),
	          QStringLiteral("'/tmp/it'\\''s here'"));

	/* A shell metacharacter must not survive as one. */
	QMimeData nasty;
	nasty.setUrls({ QUrl::fromLocalFile(QStringLiteral("/tmp/$(reboot)")) });
	QCOMPARE(bssh_terminal_view::text_for_drop(&nasty),
	          QStringLiteral("'/tmp/$(reboot)'"));

	/*
	 * Several files are several ARGUMENTS to one command, so they are
	 * joined with spaces. Newlines would run each of them instead.
	 */
	QMimeData several;
	several.setUrls({
		QUrl::fromLocalFile(QStringLiteral("/a")),
		QUrl::fromLocalFile(QStringLiteral("/b c")),
	});
	QCOMPARE(bssh_terminal_view::text_for_drop(&several), QStringLiteral("/a '/b c'"));
	QVERIFY(!bssh_terminal_view::text_for_drop(&several).contains(QLatin1Char('\n')));

	/*
	 * Text is taken as it stands -- but URLs win when both are offered,
	 * which is what a file manager does: dragging a file carries its URL
	 * and the same URL as text, and pasting "file:///home/..." is not
	 * something a shell can act on.
	 */
	QMimeData text;
	text.setText(QStringLiteral("echo hello"));
	QCOMPARE(bssh_terminal_view::text_for_drop(&text), QStringLiteral("echo hello"));

	QMimeData both;
	both.setUrls({ QUrl::fromLocalFile(QStringLiteral("/etc/hosts")) });
	both.setText(QStringLiteral("file:///etc/hosts"));
	QCOMPARE(bssh_terminal_view::text_for_drop(&both), QStringLiteral("/etc/hosts"));

	/* And a payload with neither is refused, which is what lets a dragged
	 * TAB reach the window that knows how to adopt it (sec 8.4). */
	QMimeData session;
	session.setData(QStringLiteral("application/x-bssh-session"), QByteArrayLiteral("3"));
	QVERIFY(bssh_terminal_view::text_for_drop(&session).isEmpty());
	QVERIFY(bssh_terminal_view::text_for_drop(nullptr).isEmpty());
}

void terminal_view_test::a_drop_asks_the_window_to_paste_rather_than_pasting_itself() {
	const bssh_factory_reset_t factory_reset;
	bssh_terminal_view view;

	/*
	 * Registered as a drop target, which is what makes the window manager
	 * offer this pane a drag at all.
	 */
	QVERIFY2(view.acceptDrops(), "a pane that takes no drops never sees one");

	/*
	 * The delivery itself is NOT driven here, and that is measured rather
	 * than assumed: Qt swallows a synthetic QDropEvent. Sending one with
	 * QApplication::sendEvent never reaches dropEvent -- checked with a
	 * twenty-line probe against a plain QWidget with acceptDrops(true),
	 * which counted zero calls -- because QApplication::notify wants a
	 * live drag session behind it. A test that sent one and passed would
	 * be asserting nothing at all.
	 *
	 * So what is ours is tested: the pure mapping from a payload to text
	 * (above), and the wiring below that carries it into a session.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});

	bssh_profile_t profile;
	profile.name = QStringLiteral("dropped on");
	profile.host = QStringLiteral("host.example");
	profile.user = QStringLiteral("nabbe");

	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session *session = window.registry()->create(profile);
	session->connect_to_host();
	window.adopt_session(session);
	QCoreApplication::processEvents();
	QVERIFY(fake);

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	QVERIFY(!panes->views().isEmpty());

	/*
	 * What a drop does, once Qt has delivered it: the view asks, and the
	 * window pastes -- through the same door as every other paste, so a
	 * dropped path meets bracketed paste and sec 6.4's question about
	 * more than one line.
	 */
	emit panes->views().first()->paste_requested(QStringLiteral("/tmp/notes.txt"));
	QCoreApplication::processEvents();

	QVERIFY2(fake->written().contains(QByteArrayLiteral("/tmp/notes.txt")),
	          "a dropped path must reach the session as bytes");

	while (shared->count() > 0) shared->close(shared->at(0));
	bssh_ssh_session::set_factory(nullptr);
}

void terminal_view_test::an_unfocused_cursor_is_hollow() {
	/*
	 * konsole's convention, asked for directly (sec 10.2): the terminal
	 * you are typing into has a filled block and every other one has an
	 * outline. It is the same safety question sec 8.3 calls
	 * non-negotiable about the focus border, asked of the one mark that
	 * is on screen even with a single pane -- where that border is
	 * deliberately not drawn.
	 *
	 * Asserted on the PIXELS rather than on a flag, because the whole
	 * property is what somebody sees. A boolean saying "hollow" would
	 * pass with the painter drawing a filled block from it.
	 */
	/*
	 * The view is NOT the top-level here, and that is load-bearing. A
	 * shown top-level with StrongFocus is handed focus by Qt the moment
	 * its window activates, so the first version of this test grabbed
	 * two images of a FOCUSED pane and compared them to each other --
	 * identical, and it would have stayed identical with the hollow
	 * cursor deleted. Something else has to be able to hold focus.
	 */
	QWidget host;
	QHBoxLayout *row = new QHBoxLayout(&host);

	bssh_terminal_view view(&host);
	QLineEdit *elsewhere = new QLineEdit(&host);
	row->addWidget(&view);
	row->addWidget(elsewhere);

	host.resize(700, 160);

	QJsonObject settings;
	settings.insert(QStringLiteral("font_family"), QStringLiteral("jetbrains-mono"));
	settings.insert(QStringLiteral("font_size"), 12);
	settings.insert(QStringLiteral("cursor_blink"), false);
	bssh_apply_appearance(&view, settings);

	bssh_screen screen(view.cols(), view.rows());

	/*
	 * The cursor is put five columns in, on a cell left blank. Column
	 * zero is the wrong place to ask -- it is where the grid inset and
	 * the scrollbar both land -- and a cell with a glyph in it would
	 * make the centre pixel ambiguous between the block's punch-out and
	 * the character itself.
	 */
	screen.set_cursor(0, 5);
	view.attach_screen(&screen);

	host.show();
	QVERIFY(QTest::qWaitForWindowExposed(&host));
	host.activateWindow();

	elsewhere->setFocus();
	QTRY_VERIFY(!view.hasFocus());

	const QImage unfocused = view.grab().toImage();

	view.setFocus();
	QTRY_VERIFY(view.hasFocus());

	const QImage focused = view.grab().toImage();

	/*
	 * The two images must DIFFER, and that alone is the discriminating
	 * assertion: everything else on screen is identical between the two
	 * grabs, so any difference is the cursor. Asserting only that the
	 * focused one has a filled cell would pass with both filled.
	 */
	QVERIFY(unfocused != focused);

	/*
	 * And the direction. The focused cell is solid, so it has strictly
	 * more cursor-coloured pixels than the outline does -- a hollow
	 * block that was drawn filled fails here even though the images
	 * above would differ for some other reason.
	 */
	const auto ink = [](const QImage &image, const QColor &background) {
		int count = 0;
		for (int y = 0; y < image.height(); ++y) {
			for (int x = 0; x < image.width(); ++x) {
				if (image.pixelColor(x, y) != background) ++count;
			}
		}
		return count;
	};

	const QColor background = unfocused.pixelColor(unfocused.width() - 2,
	                                                unfocused.height() - 2);

	QVERIFY(ink(unfocused, background) > 0);
	QVERIFY2(ink(focused, background) > ink(unfocused, background),
	          "a filled block covers more of the cell than its outline");
}

void terminal_view_test::only_the_focused_pane_blinks() {
	/*
	 * Four panes each animating a cursor is four things moving on a
	 * screen where one of them is the one being typed into: the movement
	 * is meant to say "here", and it says nothing when everything says
	 * it. It is also four timers waking twice a second, which sec 5.1
	 * cares about for its own reasons.
	 *
	 * Two views in ONE window, because that is the arrangement the rule
	 * is about -- two top-level widgets would each be focused in their
	 * own window and the question could not arise.
	 */
	QWidget host;
	QHBoxLayout *row = new QHBoxLayout(&host);

	bssh_terminal_view *left = new bssh_terminal_view(&host);
	bssh_terminal_view *right = new bssh_terminal_view(&host);
	row->addWidget(left);
	row->addWidget(right);

	host.resize(800, 200);
	host.show();
	QVERIFY(QTest::qWaitForWindowExposed(&host));
	host.activateWindow();

	QJsonObject settings;
	settings.insert(QStringLiteral("cursor_blink"), true);
	bssh_apply_appearance(left, settings);
	bssh_apply_appearance(right, settings);

	/*
	 * Both were ASKED for a blinking cursor, and that is the half a
	 * timer-count assertion cannot see on its own: the setting must
	 * still be recorded on the pane that is not blinking, or moving
	 * focus to it would leave it steady for ever.
	 */
	QVERIFY(left->cursor_blink_wanted());
	QVERIFY(right->cursor_blink_wanted());

	left->setFocus();
	QTRY_VERIFY(left->hasFocus());

	QVERIFY2(left->cursor_blinks(), "the focused pane blinks");
	QVERIFY2(!right->cursor_blinks(), "and the one beside it does not");

	right->setFocus();
	QTRY_VERIFY(right->hasFocus());

	QVERIFY2(right->cursor_blinks(), "the blink follows focus");
	QVERIFY2(!left->cursor_blinks(), "and leaves the pane it came from");
}

void terminal_view_test::typing_and_output_bring_the_cursor_back_on() {
	/*
	 * The blink ran on its own clock, so half of all typing landed while
	 * the cursor was off: the character appeared with nothing after it,
	 * and the cursor came back when the timer said, not the typist.
	 */
	bssh_terminal_view view;
	show_and_resize(view, 400, 200);
	QVERIFY(QTest::qWaitForWindowExposed(&view));
	view.activateWindow();

	bssh_emulator emulator(view.cols(), view.rows());
	bssh_input_router router(&emulator);
	view.attach_screen(&emulator.screen());
	view.attach_input_router(&router);
	connect(&emulator, &bssh_emulator::damaged, &view, &bssh_terminal_view::on_screen_damaged);

	QJsonObject settings;
	settings.insert(QStringLiteral("cursor_blink"), true);
	bssh_apply_appearance(&view, settings);
	view.setFocus();
	QTRY_VERIFY(view.hasFocus());
	QVERIFY(view.cursor_blinks());

	/* A key, caught in the off half. */
	QTRY_VERIFY_WITH_TIMEOUT(!view.cursor_shown(), 2000);
	QTest::keyClick(&view, Qt::Key_A);
	QVERIFY2(view.cursor_shown(), "a keystroke shows the cursor");
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("a"));

	/* And output, which is how a remote's echo arrives. */
	QTRY_VERIFY_WITH_TIMEOUT(!view.cursor_shown(), 2000);
	emulator.feed(QByteArrayLiteral("a"));
	QVERIFY2(view.cursor_shown(), "output shows the cursor");
}

namespace {

/* Gathers the area of every paint a widget receives. */
class paint_recorder : public QObject {
public:
	QRegion painted;

	bool eventFilter(QObject *watched, QEvent *event) override {
		if (event->type() == QEvent::Paint)
			painted += static_cast<QPaintEvent *>(event)->region();
		return QObject::eventFilter(watched, event);
	}
};

}

void terminal_view_test::a_keystroke_repaints_only_its_row() {
	/*
	 * Every change used to repaint the whole view, so each keystroke
	 * redrew every cell and handed the X server the whole window -- 2.3 ms
	 * for a 1700x1000 upload on the holder's NVIDIA server against 0.1 ms
	 * for a row. TDE Konsole sends the cells that changed, and felt
	 * snappier for it (project.md sec 9.5a).
	 */
	bssh_terminal_view view;
	show_and_resize(view, 600, 400);
	QVERIFY(QTest::qWaitForWindowExposed(&view));

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	connect(&emulator, &bssh_emulator::rows_damaged, &view, &bssh_terminal_view::on_rows_damaged);

	paint_recorder recorder;
	view.installEventFilter(&recorder);

	const auto painted_after = [&](const QByteArray &bytes) {
		QTest::qWait(30);
		recorder.painted = QRegion();
		emulator.feed(bytes);
		/* Empty, and failing the comparison it is used in, if no paint
		 * came at all. */
		if (!QTest::qWaitFor([&] { return !recorder.painted.isEmpty(); }, 5000)) return QRect();
		QTest::qWait(30);
		return recorder.painted.boundingRect();
	};

	emulator.feed(QByteArrayLiteral("\x1b[4;1H"));

	/* A character typed on row 3: that row and nothing else. */
	const QRect typed = painted_after(QByteArrayLiteral("x"));
	QCOMPARE(typed.height(), view.cell_height());
	QVERIFY(typed.top() >= 3 * view.cell_height());

	/* A newline moves the cursor down a row: the row it left and the row
	 * it is on, so the old cursor is erased. */
	const QRect moved = painted_after(QByteArrayLiteral("\r\n"));
	QCOMPARE(moved.height(), 2 * view.cell_height());

	/* A clear is most of the screen: the whole view. */
	QCOMPARE(painted_after(QByteArrayLiteral("\x1b[2J")), view.rect());

	/* Scrolled back, the screen's rows are not where they are drawn, so a
	 * change repaints the whole view rather than a band of history. */
	for (int line = 0; line < view.rows() * 2; ++line) emulator.feed(QByteArrayLiteral("line\r\n"));
	view.set_scroll_offset(3);
	QVERIFY(view.scroll_offset() > 0);
	QCOMPARE(painted_after(QByteArrayLiteral("y")), view.rect());
}

void terminal_view_test::a_scrolled_terminal_matches_a_full_repaint() {
	/*
	 * sec 9.5a: output scrolling moves the pixels already painted and
	 * paints only the rows it exposed. Moved pixels are only right if
	 * everything else is, so what is checked is the window's own picture
	 * -- what the incremental updates left in its backing store -- against
	 * a fresh repaint of the whole view, pixel for pixel. And that the
	 * scroll painted much less than the view, or nothing was saved.
	 */
	bssh_terminal_view view;
	show_and_resize(view, 600, 400);
	QVERIFY(QTest::qWaitForWindowExposed(&view));

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	connect(&emulator, &bssh_emulator::rows_scrolled, &view, &bssh_terminal_view::on_rows_scrolled);
	connect(&emulator, &bssh_emulator::rows_damaged, &view, &bssh_terminal_view::on_rows_damaged);

	/* Every row different, so a row in the wrong place shows. */
	for (int line = 0; line < view.rows() + 3; ++line) {
		emulator.feed(QByteArray("row ") + QByteArray::number(line) + ' ' +
		              QByteArray(line % 7 + 1, char('a' + line % 26)) + "\r\n");
		QTest::qWait(5);
	}
	QTest::qWait(50);

	paint_recorder recorder;
	view.installEventFilter(&recorder);
	emulator.feed(QByteArrayLiteral("one more\r\nand another\r\n\x1b[31mred\x1b[0m"));
	QTRY_VERIFY(!recorder.painted.isEmpty());
	QTest::qWait(50);

	const int painted_rows = recorder.painted.boundingRect().height() / view.cell_height();
	QVERIFY2(painted_rows <= 5,
	         qPrintable(QStringLiteral("a scroll of two lines painted %1 rows of %2")
	                            .arg(painted_rows).arg(view.rows())));

	const auto matches_a_full_repaint = [&view](const char *what) {
		const QImage incremental = view.windowHandle()->screen()->grabWindow(view.winId())
		                                   .toImage().convertToFormat(QImage::Format_RGB32);
		const QImage full = view.grab().toImage().convertToFormat(QImage::Format_RGB32);
		if (incremental.size() == full.size() && incremental == full) return true;
		incremental.save(QStringLiteral("scroll-incremental.png"));
		full.save(QStringLiteral("scroll-full.png"));
		qWarning("%s: the window differs from a full repaint; both saved as scroll-*.png", what);
		return false;
	};
	QVERIFY(matches_a_full_repaint("two lines scrolled"));

	/*
	 * A scroll region (DECSTBM) scrolling while a status line below it
	 * changes in the same read -- what a pager or an editor with a status
	 * bar does. The changed row is apart from the rows the scroll exposed,
	 * so it is repainted only if its damage is in the right coordinates and
	 * announced after the scroll it follows; next to the exposed rows the
	 * exposed area would have covered it either way.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[1;12r\x1b[12;1Hscrolled a\r\nscrolled b\r\n"
	                                "\x1b[20;1Hstatus line\x1b[12;1H"));
	QTest::qWait(80);
	QVERIFY(matches_a_full_repaint("a scroll region with a status line below it"));
}

void terminal_view_test::the_tmux_handle_shows_under_the_pointer_and_the_grip_clears_the_scrollbar() {
	/*
	 * The holder: like the drag grip, the tmux and screen handle should not
	 * show unless the terminal is being pointed at -- on the desktop. Open,
	 * it stays; on a touch screen, with no pointer to hover, it always
	 * shows. And the grip itself sat under the scrollbar, partly hidden.
	 */
	bssh_terminal_view view;
	show_and_resize(view, 500, 300);
	QVERIFY(QTest::qWaitForWindowExposed(&view));
	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());

	/* The pointer away from the terminal, whatever showing the window did
	 * with it: the offscreen platform may enter a window under its cursor. */
	QEvent leave(QEvent::Leave);
	QCoreApplication::sendEvent(&view, &leave);

	bssh_mux_session_t work;
	work.name = QStringLiteral("work");
	view.offer_multiplexers({ work });
	QVERIFY(view.multiplexer_drawer());
	QVERIFY2(!view.multiplexer_drawer()->isVisible(), "the handle shows with no pointer over the terminal");

	QEnterEvent enter(QPointF(20, 20), QPointF(20, 20), QPointF(view.mapToGlobal(QPoint(20, 20))));
	QCoreApplication::sendEvent(&view, &enter);
	QVERIFY(view.multiplexer_drawer()->isVisible());
	QCoreApplication::sendEvent(&view, &leave);
	QVERIFY(!view.multiplexer_drawer()->isVisible());

	/* Open, it stays when the pointer leaves. */
	QCoreApplication::sendEvent(&view, &enter);
	view.multiplexer_drawer()->set_open(true);
	QCoreApplication::sendEvent(&view, &leave);
	QVERIFY2(view.multiplexer_drawer()->isVisible(), "an open drawer went when the pointer left");
	view.multiplexer_drawer()->set_open(false);
	QVERIFY(!view.multiplexer_drawer()->isVisible());

	/* On a touch screen there is no hover: always shown. */
	bssh_platform::force_touch_primary(1);
	view.offer_multiplexers({ work });
	QVERIFY2(view.multiplexer_drawer()->isVisible(), "hidden on a touch screen, where nothing can hover");
	bssh_platform::force_touch_primary(-1);

	/* The grip is clear of the scrollbar -- with one on the right made
	 * certain, or this would pass by never meeting it. */
	for (int line = 0; line < view.rows() * 3; ++line) emulator.feed("line\r\n");
	view.attach_scrollback(&emulator.scrollback());
	view.set_scrollbar_position(bssh_terminal_view::SCROLLBAR_RIGHT);
	view.set_pane_handle(true);
	QCoreApplication::sendEvent(&view, &enter);
	QTest::qWait(20);
	const QScrollBar *bar = view.findChild<QScrollBar *>();
	QVERIFY2(bar && bar->isVisible(), "no scrollbar to keep the grip clear of");
	QVERIFY2(!view.pane_grip_rect().intersects(bar->geometry()),
	         "the scrollbar covers part of the drag grip");
}

void terminal_view_test::alt_drag_asks_for_the_pane_to_be_moved() {
	/*
	 * sec 8.4's pane drag. A tab could always be pulled out of the bar,
	 * and a PANE could not be picked up at all -- so a terminal that had
	 * been tiled was stuck there, and with two panes and nothing else the
	 * window had no bar to drag from either. Alt and the left button is
	 * the gesture, chosen because it costs no screen space and is what
	 * every Linux window manager already means by "move this".
	 *
	 * The view emits and does nothing: it has no idea which session it
	 * shows or which window holds it (sec 8.4's "a window asks the
	 * registry and never reaches into another window"). Driven here
	 * rather than through the window, because the window's handler runs
	 * QDrag::exec -- a nested loop needing a real pointer grab, which the
	 * offscreen platform has not got.
	 */
	bssh_terminal_view view;
	view.resize(400, 200);

	bssh_screen screen(view.cols(), view.rows());
	view.attach_screen(&screen);
	view.show();
	QVERIFY(QTest::qWaitForWindowExposed(&view));

	QSignalSpy asked(&view, &bssh_terminal_view::pane_drag_requested);
	QVERIFY(asked.isValid());

	const QPoint from(60, 40);
	const QPoint to = from + QPoint(4 * QApplication::startDragDistance(), 0);

	/*
	 * The plain drag FIRST, and it is the assertion that matters most: a
	 * press and a sweep with no modifier is how text is selected, which
	 * is the thing a terminal does most. A gesture that tore the pane out
	 * on that would make the app unusable, and it would pass every
	 * assertion below.
	 */
	QMouseEvent plain_press(QEvent::MouseButtonPress, from, view.mapToGlobal(from),
	                         Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(&view, &plain_press);

	QMouseEvent plain_move(QEvent::MouseMove, to, view.mapToGlobal(to),
	                        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(&view, &plain_move);

	QMouseEvent plain_release(QEvent::MouseButtonRelease, to, view.mapToGlobal(to),
	                           Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QCoreApplication::sendEvent(&view, &plain_release);

	QCOMPARE(asked.count(), 0);

	/* Now with Alt held. */
	QMouseEvent press(QEvent::MouseButtonPress, from, view.mapToGlobal(from),
	                   Qt::LeftButton, Qt::LeftButton, Qt::AltModifier);
	QCoreApplication::sendEvent(&view, &press);

	/*
	 * A press on its own is not a gesture. Pressing a modifier and
	 * clicking is something people do by accident, and a window that
	 * rearranges itself on one is worse than a window that cannot be
	 * rearranged.
	 */
	QCOMPARE(asked.count(), 0);

	const QPoint barely = from + QPoint(1, 0);
	QMouseEvent twitch(QEvent::MouseMove, barely, view.mapToGlobal(barely),
	                    Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
	QCoreApplication::sendEvent(&view, &twitch);
	QVERIFY2(asked.count() == 0, "a pixel of movement is a click, not a drag");

	QMouseEvent move(QEvent::MouseMove, to, view.mapToGlobal(to),
	                  Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
	QCoreApplication::sendEvent(&view, &move);

	QCOMPARE(asked.count(), 1);

	/* And once per gesture, not once per pixel after it. */
	QMouseEvent further(QEvent::MouseMove, to + QPoint(30, 0),
	                     view.mapToGlobal(to + QPoint(30, 0)),
	                     Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
	QCoreApplication::sendEvent(&view, &further);
	QCOMPARE(asked.count(), 1);
}

namespace {

/*
 * The bounding box of everything that is not the background, over the
 * WHOLE frame. Origin-independent on purpose: the grid inset and the
 * scrollbar both move where the grid starts, and a test that computed
 * the cursor's position would be re-deriving the renderer's arithmetic
 * and agreeing with itself.
 *
 * It scanned only the first cell_height rows at first, on the assumption
 * that row 0 begins at y = 0. It does not -- BSSH_GRID_INSET_PX shifts
 * the grid down by a pixel -- so the band ended one row short of the
 * bottom of the cell, which is exactly where an underline is drawn. It
 * passed here because the cell was tall enough for the underline to be
 * two pixels thick and overlap by one, and failed in CI where the cell
 * is smaller and it is one pixel. A probe that measures a place the
 * fault may not appear in, found by a different Qt rather than by
 * reading -- the same error as sampling a corner that turned out to be
 * the scrollbar.
 *
 * Scanning everything is safe only because the caller turns the
 * scrollbar off: grab() renders child widgets, and the bar is ink.
 */
QRect ink_box(const QImage &frame, const QColor &background) {
	int left = frame.width(), right = -1, top = frame.height(), bottom = -1;

	for (int y = 0; y < frame.height(); ++y) {
		for (int x = 0; x < frame.width(); ++x) {
			if (frame.pixelColor(x, y) == background) continue;

			left = qMin(left, x);
			right = qMax(right, x);
			top = qMin(top, y);
			bottom = qMax(bottom, y);
		}
	}

	if (right < 0) return QRect();
	return QRect(QPoint(left, top), QPoint(right, bottom));
}

} /* namespace */

void terminal_view_test::the_cursor_style_reaches_the_pixels() {
	/*
	 * sec 10.2's cursor_style, which is round-tripped through JSON,
	 * through the INI, through the preset editor and through the
	 * settings resolver -- and which nothing asserted ever changes what
	 * is DRAWN. The renderer has three branches, block, underline and
	 * bar; only the block was ever painted in a test, and only by being
	 * the default.
	 *
	 * That is the class sec 10.2 already records twice: a setting
	 * stored, read and applied to nothing. The key row had three
	 * visibility modes with nothing setting one, and the override badge
	 * was built on one editor while the other resolved elsewhere. Both
	 * had round-trip tests.
	 *
	 * Found with the search key the sibling qtty derived: the parse
	 * being right is what hides the delivery being wrong. Here the
	 * "parse" is four layers of serialisation, which is exactly what
	 * made this look thoroughly covered.
	 *
	 * The assertions are on the SHAPE of the ink rather than on its
	 * presence, because "there is a cursor" is what the setting's name
	 * suggests and it passes for all three values. What separates them
	 * is where the ink is: a block fills the cell, an underline is a
	 * thin band at the bottom of it, a bar is a thin column at the left.
	 */
	/*
	 * The bar off for the whole test: grab() renders child widgets, so a
	 * scrollbar is ink and would be the bounding box. Restored below --
	 * the default is process-wide and the next test inherits it.
	 */
	const bssh_terminal_view::scrollbar_position_t original =
	        bssh_terminal_view::default_scrollbar_position();
	bssh_terminal_view::set_default_scrollbar_position(bssh_terminal_view::SCROLLBAR_NONE);

	const auto box_for = [](const QString &style) {
		bssh_terminal_view view;
		view.resize(400, 120);

		QJsonObject settings;
		settings.insert(QStringLiteral("font_family"), QStringLiteral("jetbrains-mono"));
		settings.insert(QStringLiteral("font_size"), 12);
		settings.insert(QStringLiteral("cursor_blink"), false);
		settings.insert(QStringLiteral("cursor_style"), style);
		bssh_apply_appearance(&view, settings);

		bssh_screen screen(view.cols(), view.rows());
		screen.set_cursor(0, 5);
		view.attach_screen(&screen);

		view.show();

		/*
		 * The result is read rather than discarded, which the compiler
		 * had been asking for -- qWaitForWindowExposed is nodiscard and
		 * this was the one site ignoring it. An unexposed window grabs
		 * an empty frame, so every measurement below would be about
		 * nothing, and the assertions that follow would report a cursor
		 * that drew nothing rather than a window that never appeared.
		 *
		 * A null box rather than a QVERIFY, because this is a lambda
		 * returning a value and QVERIFY returns void. The caller
		 * already asserts each box is valid, with a message naming all
		 * three and the cell -- so an unexposed window lands there,
		 * which is the failure that says the most.
		 */
		if (!QTest::qWaitForWindowExposed(&view))
			return QPair<QRect, QSize>(QRect(), QSize());

		const QImage frame = view.grab().toImage();
		const QColor background = frame.pixelColor(frame.width() / 2,
		                                            frame.height() - 3);

		return QPair<QRect, QSize>(ink_box(frame, background),
		                            QSize(view.cell_width(), view.cell_height()));
	};

	const QPair<QRect, QSize> block = box_for(QStringLiteral("block"));
	const QPair<QRect, QSize> underline = box_for(QStringLiteral("underline"));
	const QPair<QRect, QSize> bar = box_for(QStringLiteral("bar"));

	const int cw = block.second.width();
	const int ch = block.second.height();
	QVERIFY(cw > 2 && ch > 4);

	/*
	 * Each drew something, or every comparison below is between two empty
	 * rectangles and says nothing.
	 *
	 * The message carries all three boxes and the cell, because this
	 * assertion failed once in CI and not here, and "drew nothing" on its
	 * own does not say whether the cursor was absent, the cell was a
	 * different size, or the probe was looking in the wrong place.
	 */
	const auto describe = [&](const char *name, const QPair<QRect, QSize> &box) {
		return QStringLiteral("%1 %2,%3 %4x%5")
		        .arg(QString::fromLatin1(name))
		        .arg(box.first.left()).arg(box.first.top())
		        .arg(box.first.width()).arg(box.first.height());
	};
	const QString shapes = QStringLiteral("cell %1x%2; %3; %4; %5")
	        .arg(cw).arg(ch)
	        .arg(describe("block", block), describe("underline", underline),
	              describe("bar", bar));

	QVERIFY2(block.first.isValid(), qPrintable(shapes));
	QVERIFY2(underline.first.isValid(), qPrintable(shapes));
	QVERIFY2(bar.first.isValid(), qPrintable(shapes));

	/* A block fills its cell in both directions. */
	QVERIFY2(block.first.height() >= ch - 1 && block.first.width() >= cw - 1,
	          qPrintable(QStringLiteral("block ink %1x%2 in a %3x%4 cell")
	                             .arg(block.first.width()).arg(block.first.height())
	                             .arg(cw).arg(ch)));

	/* An underline is thin and at the BOTTOM -- the second half is what
	 * tells it from a bar lying on its side. */
	QVERIFY2(underline.first.height() <= ch / 3,
	          qPrintable(QStringLiteral("underline is %1 tall in a %2 cell")
	                             .arg(underline.first.height()).arg(ch)));
	QVERIFY2(underline.first.top() > ch / 2,
	          "an underline belongs at the bottom of the cell");
	QVERIFY2(underline.first.width() >= cw - 1, "an underline spans the cell");

	/* A bar is thin and at the LEFT, and full height. */
	QVERIFY2(bar.first.width() <= cw / 2,
	          qPrintable(QStringLiteral("bar is %1 wide in a %2 cell")
	                             .arg(bar.first.width()).arg(cw)));
	QVERIFY2(bar.first.height() >= ch - 1, "a bar spans the cell height");
	QVERIFY2(bar.first.left() < block.first.left() + cw / 2,
	          "a bar belongs at the left of the cell");

	bssh_terminal_view::set_default_scrollbar_position(original);
}

void terminal_view_test::a_reattached_screen_is_not_read_from_the_old_one() {
	/*
	 * The accessibility layer caches the flattened text and invalidates
	 * it on the view's content revision -- and only on_screen_damaged()
	 * used to move that. So a view pointed at a different screen went on
	 * reporting the FIRST one's text to a screen reader until something
	 * happened to damage the new one.
	 *
	 * This is not reachable in the app today and the test says so
	 * deliberately: bssh_pane_container::rebuild() constructs a fresh
	 * view per pane, so nothing is ever re-attached and the cache is new
	 * each time. The contract was true by an accident of how panes are
	 * rebuilt rather than by anything in the view -- which is sec 12.4's
	 * scrollbar that tracked its history correctly because two of three
	 * signal sites happened to damage the screen as a side effect. A
	 * consumer that is right by coincidence looks exactly like a wired
	 * one, and the way to tell them apart is to drive the path the
	 * coincidence does not cover.
	 *
	 * Found by the sibling qtty asking its own question of a GATE rather
	 * than a payload: its frame scheduler compared image COUNTS to
	 * decide whether to send a frame, so a picture repainted in place
	 * was computed correctly and never transmitted. Same fault, same
	 * position -- a change detector reading a property that does not
	 * change when the content does.
	 */
	bssh_emulator first(40, 6);
	first.feed(QByteArrayLiteral("the first terminal"));

	bssh_emulator second(40, 6);
	second.feed(QByteArrayLiteral("a different terminal"));

	bssh_terminal_view view;
	view.resize(600, 200);
	view.attach_screen(&first.screen());

	QAccessibleInterface *a = QAccessible::queryAccessibleInterface(&view);
	QVERIFY(a);

	/*
	 * Read FIRST, so the cache is populated. Without this the test would
	 * pass against the defect: nothing is cached until somebody asks,
	 * and a fresh cache reads the new screen correctly.
	 */
	QVERIFY(a->text(QAccessible::Value).contains(QStringLiteral("the first terminal")));

	view.attach_screen(&second.screen());

	const QString now = a->text(QAccessible::Value);
	QVERIFY2(now.contains(QStringLiteral("a different terminal")),
	          qPrintable(QStringLiteral("a reader was still being given the old screen: %1")
	                             .arg(now.left(120))));
	QVERIFY2(!now.contains(QStringLiteral("the first terminal")),
	          "and the old text must be gone, not merely joined by the new");
}

/*
 * OSC 8 was the only clickable link, and almost nothing emits OSC 8.
 * Every URL a person actually meets in a terminal -- curl's output, a git
 * remote, a log line, the address in an error message -- is plain text
 * that some program printed knowing nothing about the convention, and
 * none of it was clickable at all.
 *
 * These cases are about where a URL STOPS, because that is the whole
 * difficulty. Finding `https://' is trivial; handing the browser
 * `page).' is finding the address and breaking it.
 */
void terminal_view_test::a_bare_url_is_clickable_and_stops_where_it_should() {
	bssh_terminal_view view;
	show_and_resize(view, 900, 200);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	/*
	 * The top row's line index, taken the way a click takes it rather
	 * than from an internal. cell_at is what mousePressEvent uses, so the
	 * test addresses cells through the same mapping the feature does.
	 */
	qint64 first = 0;
	int ignored = 0;
	QVERIFY(view.cell_at(QPoint(2, 2), first, ignored));

	emulator.feed(QByteArrayLiteral("see https://example.com/a/page for more\r\n"));

	/* Clicked inside the address: the whole address, and not the word
	 * after it. */
	QCOMPARE(view.url_at(first, 10), QStringLiteral("https://example.com/a/page"));

	/* Clicked on the prose either side: nothing. A stray Ctrl+click on a
	 * line of output must not launch anything. */
	QVERIFY2(view.url_at(first, 1).isEmpty(), "found a URL in the word 'see'");
	QVERIFY2(view.url_at(first, 34).isEmpty(), "found a URL in the word 'more'");

	/*
	 * Sentence punctuation is not an address. This is the case that
	 * makes the difference between a link that works and one that
	 * 404s, and it is the reason trimming is done by counting brackets
	 * rather than by stripping a character class.
	 */
	emulator.feed(QByteArrayLiteral("(see https://example.com/page).\r\n"));
	QCOMPARE(view.url_at(first + 1, 12), QStringLiteral("https://example.com/page"));

	/* But a bracket whose opener is inside the address belongs to it --
	 * Wikipedia's own URLs are the everyday case. */
	emulator.feed(QByteArrayLiteral("https://en.wikipedia.org/wiki/Fo_(bar)\r\n"));
	QCOMPARE(view.url_at(first + 2, 5),
	          QStringLiteral("https://en.wikipedia.org/wiki/Fo_(bar)"));

	/*
	 * Only http and https. A marked OSC 8 link is an explicit act by the
	 * program that drew it and may carry any scheme; text that merely
	 * LOOKS like a link is not, so what is inferred is deliberately
	 * narrower than what is declared.
	 */
	emulator.feed(QByteArrayLiteral("ftp://example.com/x file:///etc/passwd\r\n"));
	QVERIFY2(view.url_at(first + 3, 3).isEmpty(), "inferred an ftp:// link from text");
	QVERIFY2(view.url_at(first + 3, 22).isEmpty(), "inferred a file:// link from text");

	/*
	 * A scheme with no host is not an address, though it matches the
	 * pattern. The first version of this case fed `https:// nothing',
	 * which never reaches the check at all: the pattern needs a non-space
	 * after the slashes, so it simply does not match and the assertion
	 * passed without exercising anything. Sabotaging the host check
	 * stayed green and said so. `https:///path' matches and has no host.
	 */
	emulator.feed(QByteArrayLiteral("https:///path here\r\n"));
	QVERIFY2(view.url_at(first + 4, 2).isEmpty(), "accepted a scheme with no host");

	/*
	 * And the CALL SITE, which everything above this misses.
	 *
	 * Every assertion so far asks url_at directly, so removing its call
	 * from mousePressEvent changes nothing they can see -- the sabotage
	 * that does exactly that stayed green. A test that calls the helper
	 * it is verifying cannot see a wrong caller, which is the same trap
	 * the session bell fell into earlier and the reason this is here:
	 * the wiring is a separate fact from the parsing.
	 */
	QSignalSpy activated(&view, &bssh_terminal_view::link_activated);

	emulator.feed(QByteArrayLiteral("go to https://example.com/clicked now\r\n"));

	const QRect target = view.cell_rect(first + 5, 12);
	QVERIFY2(target.isValid(), "the address is not on screen to be clicked");

	/* Without Ctrl nothing opens: a stray tap on output must not launch
	 * a browser at an address the remote chose. */
	QTest::mouseClick(&view, Qt::LeftButton, Qt::NoModifier, target.center());
	QCOMPARE(activated.count(), 0);

	QTest::mouseClick(&view, Qt::LeftButton, Qt::ControlModifier, target.center());
	QCOMPARE(activated.count(), 1);
	QCOMPARE(activated.first().first().toString(),
	          QStringLiteral("https://example.com/clicked"));
}

/*
 * sec 6.4's touch model: "long press then drag handles".
 *
 * It was not built. A long press opened the menu and nothing else, so
 * Copy there was enabled only if the user had already dragged out a
 * selection -- and a drag is the gesture scrolling needs, which is why
 * the table gives selection to the press instead. Building the scroller
 * without this would have left the two fighting over one gesture.
 */
void terminal_view_test::a_long_press_selects_and_handles_adjust_it() {
	const forced_touch_t arrangement(1);

	bssh_terminal_view view;
	show_and_resize(view, 640, 200);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);
	emulator.feed(QByteArrayLiteral("alpha /usr/local/bin/thing omega\r\n"));

	qint64 line = 0;
	int col = 0;
	QVERIFY(view.cell_at(QPoint(2, 2), line, col));

	QVERIFY2(!view.has_selection(), "something was already selected");

	/*
	 * A press in the middle of the path. The WORD is taken, not a point:
	 * a zero-width selection is not one, and a path is what anybody
	 * reaches for in a terminal -- the same word characters the
	 * double-click uses, because two ideas of "word" is two answers.
	 */
	const QRect middle = view.cell_rect(line, 12);
	QVERIFY(middle.isValid());

	/*
	 * A real press-and-hold, not select_word_at() directly.
	 *
	 * The first version called the helper, so the sabotage that deletes
	 * the selection FROM the long press left the suite green: a test
	 * that calls the helper it is verifying cannot see a wrong caller,
	 * which is the third time that trap has been walked into here.
	 * BSSH_HOLD_MS is 500, so the wait is deliberate rather than a
	 * sleep-and-hope.
	 */
	/*
	 * The press-and-hold is off unless something turns it on -- a pointer
	 * keeps press-and-hold meaning "start a selection and think about
	 * it", so the window enables it only where touch is primary. A
	 * standalone view in a test is nobody's window, so it is enabled
	 * here the way a touch session would.
	 */
	view.set_long_press_menu(true);

	QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, middle.center());
	QTest::qWait(700);
	QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, middle.center());

	QVERIFY2(view.has_selection(), "a long press selected nothing");
	QCOMPARE(view.selection_text(), QStringLiteral("/usr/local/bin/thing"));

	/*
	 * Both handles exist and sit at the two ends, below the text rather
	 * than over it -- a handle covering the selection hides the thing it
	 * is there to adjust.
	 */
	const QRect start = view.selection_handle_rect(true);
	const QRect end = view.selection_handle_rect(false);
	QVERIFY2(start.isValid() && end.isValid(), "a touch selection has no handles");
	QVERIFY2(start.center().x() < end.center().x(), "the handles are the wrong way round");
	QVERIFY2(start.top() >= middle.bottom(), "a handle is drawn over the text");

	/*
	 * And the target is bigger than the mark. The circle drawn is a
	 * sixth of this, deliberately: 48dp of ink over a terminal hides
	 * several cells, while 48dp of target is what a finger needs.
	 */
	QCOMPARE(start.width(), BSSH_TOUCH_TARGET_DP);

	/*
	 * Dragging the END handle leftwards shortens the selection and
	 * leaves the START where it was. Moving both is the mistake that
	 * makes a handle behave like a fresh drag.
	 */
	QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, end.center());
	QTest::mouseMove(&view, view.cell_rect(line, 16).center());
	QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier,
	                     view.cell_rect(line, 16).center());

	QVERIFY(view.has_selection());
	QVERIFY2(view.selection_text().startsWith(QStringLiteral("/usr/")),
	          qPrintable(QStringLiteral("the start moved too: %1")
	                             .arg(view.selection_text())));
	QVERIFY2(view.selection_text().size() < 20,
	          qPrintable(QStringLiteral("the end did not move: %1")
	                             .arg(view.selection_text())));

	/*
	 * And the START handle, which the case above cannot reach: dragging
	 * the end exercises one branch, and a sabotage that anchors the
	 * START to the wrong end passes a test that only ever drags the end.
	 * Two handles are two paths.
	 */
	view.select_word_at(line, 12);
	const QString whole = view.selection_text();
	const QRect start_handle = view.selection_handle_rect(true);
	QVERIFY(start_handle.isValid());

	const QPoint to = view.cell_rect(line, 10).center();
	QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, start_handle.center());
	QTest::mouseMove(&view, to);
	QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier, to);

	/* The end stayed: the selection still finishes where it did. */
	QVERIFY2(view.selection_text().endsWith(QStringLiteral("thing")),
	          qPrintable(QStringLiteral("the end moved with the start handle: %1")
	                             .arg(view.selection_text())));
	QVERIFY2(view.selection_text() != whole,
	          "the start handle moved nothing");
}

/*
 * Zoom (sec 9.5). The view's half: the size it draws at, and the two
 * arithmetic properties that were easy to get wrong -- a resolve arriving
 * under a zoom, and where the clamp is applied.
 */
void terminal_view_test::zoom_moves_the_size_the_grid_is_drawn_at() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);
	view.set_font_id(QString(), 11);

	QCOMPARE(view.font_point_size(), 11);
	const int plain_cell = view.cell_width();

	view.set_font_zoom(3);

	QCOMPARE(view.font_zoom(), 3);
	QCOMPARE(view.font_point_size(), 14);

	/*
	 * The CELL, not only the number. A zoom that moved a member and not
	 * the font would satisfy every assertion above it, and the cell width
	 * is what the grid, the remote's column count and the user all
	 * actually see.
	 */
	QVERIFY2(view.cell_width() > plain_cell,
	          qPrintable(QStringLiteral("14pt drew the same cell as 11pt: %1")
	                             .arg(view.cell_width())));

	view.set_font_zoom(0);
	QCOMPARE(view.font_point_size(), 11);
	QCOMPARE(view.cell_width(), plain_cell);
}

void terminal_view_test::a_resolve_moves_the_base_under_the_zoom() {
	/*
	 * A preset edit, a host reopened, a pane rebuilt: all of them arrive
	 * as a resolve, and a user who has zoomed has not asked for it back.
	 * The base moves and the zoom rides on top.
	 */
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);
	view.set_font_id(QString(), 11);
	view.set_font_zoom(2);
	QCOMPARE(view.font_point_size(), 13);

	QJsonObject resolved;
	resolved.insert(QStringLiteral("font_size"), 14);
	bssh_apply_appearance(&view, resolved);

	QCOMPARE(view.font_zoom(), 2);
	QCOMPARE(view.font_point_size(), 16);

	/*
	 * And a base that leaves no room re-clamps the zoom rather than
	 * carrying steps the view will not draw. Kept honest at the moment
	 * the base moves, because the alternative is a stored number that
	 * disagrees with the screen and a handful of dead keypresses the
	 * next time somebody steps back.
	 */
	QJsonObject at_the_top;
	at_the_top.insert(QStringLiteral("font_size"), BSSH_PRESET_MAX_POINT_SIZE);
	bssh_apply_appearance(&view, at_the_top);

	QCOMPARE(view.font_point_size(), BSSH_PRESET_MAX_POINT_SIZE);
	QCOMPARE(view.font_zoom(), 0);
}

void terminal_view_test::the_zoom_stops_where_the_settings_layer_does() {
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);
	view.set_font_id(QString(), 11);

	view.set_font_zoom(1000);
	QCOMPARE(view.font_point_size(), BSSH_PRESET_MAX_POINT_SIZE);

	/*
	 * And the step back MOVES, which is the whole reason the clamp is on
	 * the zoom rather than on the sum. Bound only the sum and the stored
	 * steps keep climbing while the size stands still, so the first step
	 * back does nothing -- from the keyboard, a zoom that has stopped
	 * working.
	 */
	view.set_font_zoom(view.font_zoom() - 1);
	QCOMPARE(view.font_point_size(), BSSH_PRESET_MAX_POINT_SIZE - 1);

	view.set_font_zoom(-1000);
	QCOMPARE(view.font_point_size(), BSSH_PRESET_MIN_POINT_SIZE);
}

void terminal_view_test::a_pinch_zooms_the_same_way_the_chord_does() {
	/*
	 * sec 9.5 calls pinch "the obvious next increment on touch: the menu
	 * makes the feature reachable there, a gesture would make it
	 * natural". It emits the SAME signal Ctrl+wheel and the Text Size
	 * menu emit, so everything already settled about zooming stays
	 * settled -- the session owns the number, the clamp lives in
	 * rebuild_font, and the value is not persisted.
	 *
	 * Driven through a real QGestureEvent rather than by calling the
	 * handler, because the half that fails on its own is the WIRING: a
	 * widget that never grabbed the gesture receives nothing, and a
	 * handler tested directly cannot see that.
	 */
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	QSignalSpy zooms(&view, &bssh_terminal_view::zoom_requested);

	/*
	 * The INCREMENTAL factor each update carries, which is what the view
	 * accumulates -- see handle_pinch for why it does not read the
	 * gesture's total: a gesture's state is set by Qt's recogniser
	 * through private friends, so no test can produce a GestureStarted
	 * and a design needing one would have its reset unreachable.
	 */
	const auto pinch = [&view](qreal change) {
		QPinchGesture gesture;
		gesture.setScaleFactor(change);

		QList<QGesture *> one;
		one.append(&gesture);
		QGestureEvent event(one);
		QCoreApplication::sendEvent(&view, &event);
	};

	/*
	 * Fingers apart by one step. The assertion is on the DIRECTION as
	 * well as the count, because a sign error zooms out when somebody
	 * spreads their fingers and no count would say so.
	 */
	pinch(1.2);

	QCOMPARE(zooms.count(), 1);
	QVERIFY2(zooms.at(0).at(0).toInt() > 0, "spreading two fingers zoomed OUT");

	/* And further apart keeps stepping. */
	pinch(1.2);
	QCOMPARE(zooms.count(), 2);

	/*
	 * A move too small to reach the next threshold emits nothing --
	 * without which a pinch barely held still would step on every touch
	 * update the platform happens to send.
	 */
	pinch(1.05);
	QCOMPARE(zooms.count(), 2);

	/*
	 * And back the other way. Two steps' worth, because the 1.05 above
	 * is still carried: the first undoes it and the second crosses.
	 */
	zooms.clear();
	pinch(1.0 / 1.2);
	pinch(1.0 / 1.2);

	QVERIFY2(zooms.count() >= 1, "bringing two fingers together did nothing");
	QVERIFY2(zooms.at(zooms.count() - 1).at(0).toInt() < 0,
	          "bringing two fingers together zoomed IN");

	/*
	 * WHAT THIS CANNOT REACH, said here rather than left for somebody to
	 * assume otherwise: the grabGesture() call. Qt recognises a pinch
	 * from real touch points and there is no public way to ask a widget
	 * which gestures it grabbed, so a suite can drive the handler and
	 * cannot prove the gesture would ever arrive. Sending the event
	 * directly, as above, bypasses the grab entirely.
	 *
	 * The same limit the drop tests record for Qt's own drag target,
	 * and it is why tool/sabotage/pinch-zoom.toml breaks the ARITHMETIC
	 * rather than the grab: an edit nothing can catch is not a sabotage,
	 * it is a claim.
	 */
}

void terminal_view_test::ctrl_wheel_zooms_while_the_remote_owns_the_wheel() {
	/*
	 * The case the ordering exists for: text too small to read is usually
	 * text inside less, vim or htop, and those are exactly the programs
	 * that have asked for the wheel. A zoom that worked everywhere except
	 * there would be missing where it is wanted.
	 */
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	/* 1000: report presses, which is what a pager turns on. */
	emulator.feed(QByteArrayLiteral("\x1b[?1000h"));
	QVERIFY2(emulator.mouse_reporting(),
	          "the fixture never reached the case: the remote does not own the wheel");
	emulator.take_output();

	QSignalSpy zoomed(&view, &bssh_terminal_view::zoom_requested);

	const QPointF at(20, 20);
	QWheelEvent event(at, view.mapToGlobal(at.toPoint()), QPoint(0, 0),
	                   QPoint(0, 120), Qt::NoButton, Qt::ControlModifier,
	                   Qt::NoScrollPhase, false);
	QApplication::sendEvent(&view, &event);

	QCOMPARE(zoomed.count(), 1);
	QCOMPARE(zoomed.at(0).at(0).toInt(), 1);

	/* And the remote heard nothing: a zoom is local, like a selection. */
	QCOMPARE(emulator.take_output(), QByteArray());
}

void terminal_view_test::the_zoom_chords_reach_the_focused_session() {
	/*
	 * The control. Every assertion above is about a function that is
	 * already correct; this one asks whether anything CALLS it, which is
	 * the defect this tree keeps finding -- a value with two consumers
	 * and only one of them wired.
	 *
	 * Through the router's own handle_key, which is the road a real key
	 * takes, rather than through handle_shortcut.
	 */
	bssh_main_window window;
	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);

	bssh_profile_t profile;
	profile.name = QStringLiteral("zooms");
	profile.host = QStringLiteral("zooms.example");

	bssh_session *session = registry->create(profile);
	window.adopt_session(session);

	QKeyEvent bigger(QEvent::KeyPress, Qt::Key_Equal, Qt::ControlModifier, QString());
	QVERIFY2(!session->input_router()->handle_key(&bigger),
	          "Ctrl+= was not stolen from the remote");
	QCOMPARE(session->font_zoom(), 1);

	QKeyEvent smaller(QEvent::KeyPress, Qt::Key_Minus, Qt::ControlModifier, QString());
	session->input_router()->handle_key(&smaller);
	QCOMPARE(session->font_zoom(), 0);

	session->set_font_zoom(4);
	QKeyEvent reset(QEvent::KeyPress, Qt::Key_0, Qt::ControlModifier, QString());
	session->input_router()->handle_key(&reset);
	QCOMPARE(session->font_zoom(), 0);
}

void terminal_view_test::the_terminal_menu_offers_zoom() {
	/*
	 * Asked of the MENU rather than of the window's handler, because a
	 * device with no keyboard and no wheel has no other way in: an entry
	 * that is not on the menu is not a route however well the handler
	 * works. The same reasoning as the config route's test in layout_test.
	 */
	bssh_main_window window;
	QMenu *menu = window.build_terminal_menu(&window);
	QVERIFY(menu);

	const QStringList wanted = { QStringLiteral("terminal_zoom_in"),
	                              QStringLiteral("terminal_zoom_out"),
	                              QStringLiteral("terminal_zoom_reset") };

	for (const QString &name : wanted) {
		QAction *action = menu->findChild<QAction *>(name);
		QVERIFY2(action, qPrintable(QStringLiteral("the terminal menu has no %1")
		                                    .arg(name)));
		QVERIFY2(action->isEnabled(),
		          qPrintable(QStringLiteral("%1 is on the menu and disabled").arg(name)));
	}

	/*
	 * And the chord it advertises comes from the router, like Copy's --
	 * a literal here would be a second list, free to drift the moment
	 * somebody rebinds one.
	 */
	QAction *zoom_in = menu->findChild<QAction *>(QStringLiteral("terminal_zoom_in"));
	QCOMPARE(zoom_in->shortcut(),
	          QKeySequence(bssh_input_router::binding_for(QStringLiteral("zoom-in"))));

	/*
	 * And on the menu BAR, which is the desktop's discoverable surface
	 * and had none of these. The chords worked throughout, which is what
	 * made this invisible: it costs nobody who already knew them and
	 * everybody who did not -- the same sentence this bar's Find entry
	 * was added under.
	 */
	const QStringList on_the_bar = { QStringLiteral("menu_zoom_in"),
	                                  QStringLiteral("menu_zoom_out"),
	                                  QStringLiteral("menu_zoom_reset") };

	for (const QString &name : on_the_bar) {
		QAction *action = window.findChild<QAction *>(name);
		QVERIFY2(action, qPrintable(QStringLiteral("the menu bar has no %1, so a "
		                                            "desktop user can only reach the "
		                                            "zoom by a chord nobody told them")
		                                    .arg(name)));
	}

	QAction *bar_zoom_out = window.findChild<QAction *>(QStringLiteral("menu_zoom_out"));
	QVERIFY(bar_zoom_out);
	QCOMPARE(bar_zoom_out->shortcut(),
	          QKeySequence(bssh_input_router::binding_for(QStringLiteral("zoom-out"))));

	delete menu;
}

void terminal_view_test::a_zoom_past_the_limit_still_steps_back() {
	/*
	 * Holding the key down past the limit must not cost anything on the
	 * way back. The session counts presses and the VIEW is the only one
	 * that knows the size the host resolved to, so a session left
	 * counting refusals is a session whose next several steps back
	 * change nothing on screen -- the shape of "the zoom has stopped
	 * working" that no arithmetic test on the view can see.
	 */
	bssh_main_window window;
	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);

	bssh_profile_t profile;
	profile.name = QStringLiteral("held down");
	profile.host = QStringLiteral("held.example");

	bssh_session *session = registry->create(profile);
	window.adopt_session(session);

	bssh_terminal_view *view = window.findChild<bssh_terminal_view *>();
	QVERIFY2(view, "the window built no pane to zoom");

	const int base = view->font_point_size();
	QVERIFY2(base < BSSH_PRESET_MAX_POINT_SIZE,
	          "the fixture starts at the limit and cannot reach past it");

	QKeyEvent bigger(QEvent::KeyPress, Qt::Key_Equal, Qt::ControlModifier, QString());
	for (int i = 0; i < BSSH_PRESET_MAX_POINT_SIZE * 2; ++i) {
		session->input_router()->handle_key(&bigger);
	}

	QCOMPARE(view->font_point_size(), BSSH_PRESET_MAX_POINT_SIZE);
	QCOMPARE(session->font_zoom(), BSSH_PRESET_MAX_POINT_SIZE - base);

	QKeyEvent smaller(QEvent::KeyPress, Qt::Key_Minus, Qt::ControlModifier, QString());
	session->input_router()->handle_key(&smaller);

	QCOMPARE(view->font_point_size(), BSSH_PRESET_MAX_POINT_SIZE - 1);
}

void terminal_view_test::a_cursor_under_the_floor_is_lifted_like_text_is() {
	/*
	 * sec 9.3's floor says it lifts "a drawn colour when the scheme's own
	 * value falls under it", and the cursor is a drawn colour a person
	 * picks -- the colour editor offers Background, Text and Cursor. It
	 * was the one drawn thing the floor did not reach.
	 *
	 * Unreachable with a shipped scheme, which is why nothing noticed:
	 * all five set cursor equal to foreground, so their cursor clears the
	 * floor because their text does. The custom scheme is the one a
	 * person makes by hand, and it is the case this guards.
	 */
	bssh_colour_scheme_t hidden;
	hidden.id = QStringLiteral("custom");
	hidden.display_name = QStringLiteral("Custom");
	hidden.dark = true;
	hidden.background = qRgb(0, 0, 0);
	hidden.foreground = qRgb(255, 255, 255);
	hidden.cursor = qRgb(16, 16, 16);
	for (int i = 0; i < 16; ++i) hidden.ansi[i] = qRgb(255, 255, 255);

	QVERIFY2(bssh_contrast_ratio(hidden.cursor, hidden.background) < BSSH_CONTRAST_FLOOR,
	          "the fixture's cursor already clears the floor, so it proves nothing");

	bssh_colour_scheme_catalog::instance().set_custom(hidden);

	bssh_terminal_view view;
	show_and_resize(view, 640, 320);
	view.set_colour_scheme(QStringLiteral("custom"));
	view.set_cursor_style(QStringLiteral("block"));

	/* grab() renders child widgets, and a scrollbar is not the cursor. */
	view.set_scrollbar_position(bssh_terminal_view::SCROLLBAR_NONE);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	QVERIFY(QTest::qWaitForWindowExposed(&view));
	view.activateWindow();
	view.setFocus();
	QTRY_VERIFY2(view.hasFocus(), "an unfocused view draws an outline, not a block");

	/* An empty cell under the cursor, so the block is what is sampled. */
	emulator.feed(QByteArrayLiteral("\x1b[2J\x1b[H"));

	/*
	 * Found rather than computed: with the screen cleared and no
	 * scrollbar, the block is the only thing on the widget that is not
	 * the background, so the first pixel that differs IS the cursor.
	 * Asking for its rectangle would need the grid's inset, which is the
	 * view's own business and not a thing to reproduce here.
	 */
	const auto cursor_pixel = [&view](const QColor &background) {
		const QImage frame = view.grab().toImage();
		for (int y = 0; y < frame.height(); ++y) {
			for (int x = 0; x < frame.width(); ++x) {
				const QColor here = frame.pixelColor(x, y);
				if (here != background) return here;
			}
		}
		return background;
	};

	const QColor ground(hidden.background);

	view.set_raise_contrast(true);
	const QColor lifted = cursor_pixel(ground);
	QVERIFY2(lifted != ground, "nothing but background was drawn: no cursor to measure");
	QVERIFY2(bssh_contrast_ratio(lifted.rgb(), hidden.background) >= BSSH_CONTRAST_FLOOR,
	          qPrintable(QStringLiteral("the cursor was drawn at %1:1 against its own "
	                                     "background, under the %2:1 floor")
	                             .arg(bssh_contrast_ratio(lifted.rgb(), hidden.background))
	                             .arg(BSSH_CONTRAST_FLOOR)));

	/*
	 * And with the setting OFF the faithful value is drawn, which is what
	 * says this measures the floor rather than a constant: a view that
	 * always lifted would pass the assertion above and fail this one.
	 */
	view.set_raise_contrast(false);
	const QColor faithful = cursor_pixel(ground);
	QVERIFY2(faithful != ground, "nothing but background was drawn with the setting off");
	QVERIFY2(bssh_contrast_ratio(faithful.rgb(), hidden.background) < BSSH_CONTRAST_FLOOR,
	          "the cursor was lifted with the setting off, so the setting does nothing");
}

void terminal_view_test::the_scrollback_search_is_reachable_without_a_keyboard() {
	/*
	 * sec 6.1 calls the search "the thing a long scrollback is FOR", and
	 * until now it had exactly one route: Ctrl+Shift+F. A phone has no
	 * chord to press, so the feature was built, wired, tested -- the find
	 * bar is constructed on the terminal page whatever the device -- and
	 * unreachable on the platform sec 8.7 was written about.
	 *
	 * Asked of both surfaces, because they fail differently: the menu bar
	 * is the desktop's discoverable route, and the context menu is the
	 * only route touch has.
	 */
	bssh_main_window window;

	QAction *bar_find = window.findChild<QAction *>(QStringLiteral("menu_find"));
	QVERIFY2(bar_find, "the menu bar offers no way to search the scrollback");

	QMenu *menu = window.build_terminal_menu(&window);
	QVERIFY(menu);

	QAction *menu_find = menu->findChild<QAction *>(QStringLiteral("terminal_find"));
	QVERIFY2(menu_find,
	          "the terminal menu offers no way to search: a device with no "
	          "keyboard cannot reach the scrollback search at all");

	/*
	 * And it advertises the chord the router would actually steal, like
	 * Copy does -- a literal here would be a second list, wrong the
	 * moment somebody rebinds it.
	 */
	QCOMPARE(menu_find->shortcut(),
	          QKeySequence(bssh_input_router::binding_for(QStringLiteral("find"))));

	delete menu;
}

void terminal_view_test::a_pane_can_leave_its_window_without_a_pointer() {
	/*
	 * The menu bar's own entry says it: "It is also the only route a PANE
	 * has out of a window. Dragging one out is Alt and a drag, which
	 * works and which nobody discovers." A phone has neither -- no menu
	 * bar and no Alt -- so detaching was reachable on one platform only,
	 * which is the shape sec 8.7 already found for Close and Duplicate.
	 *
	 * Asserted on the terminal menu because that is the surface touch
	 * has, and the entry is in the Pane group beside Split for the same
	 * reason the menu bar keeps Detach next to it: they are the two
	 * halves of one question about where a session lives.
	 */
	const forced_touch_t phone(1);

	bssh_main_window window;
	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);

	bssh_profile_t profile;
	profile.name = QStringLiteral("detaches");
	profile.host = QStringLiteral("detaches.example");
	window.adopt_session(registry->create(profile));

	QMenu *menu = window.build_terminal_menu(&window);
	QVERIFY(menu);

	QAction *detach = menu->findChild<QAction *>(QStringLiteral("terminal_detach"));
	QVERIFY2(detach, "the terminal menu offers no way to move a pane into its "
	                  "own window, so touch has none at all");

	/*
	 * And it is refused where it cannot act, rather than offered and
	 * silently doing nothing: tear_out declines to empty a window, so a
	 * lone session has nothing to detach.
	 */
	QVERIFY2(!detach->isEnabled(),
	          "Detach was offered for the only session in the window, which "
	          "tear_out refuses -- an entry that does nothing when chosen");

	/*
	 * Show as Tab is the same question asked of the other verb. Two
	 * sessions here share one pane -- the second opened as a tab in it
	 * (sec 8.3's bar per pane) -- and with one pane there is no neighbour
	 * whose bar it could join, so show_as_tab declines and the entry must
	 * say so rather than be offered and do nothing.
	 */
	window.adopt_session(registry->create(profile));

	delete menu;
	menu = window.build_terminal_menu(&window);
	QVERIFY(menu);

	QAction *as_tab = menu->findChild<QAction *>(QStringLiteral("terminal_show_as_tab"));
	QVERIFY(as_tab);
	QVERIFY2(!as_tab->isEnabled(),
	          "Show as Tab was offered with one pane, where choosing it does "
	          "nothing at all");

	/* And with two sessions there is something to detach. */
	QAction *detach_two = menu->findChild<QAction *>(QStringLiteral("terminal_detach"));
	QVERIFY(detach_two);
	QVERIFY2(detach_two->isEnabled(),
	          "Detach stayed refused with two sessions, where it can act");

	delete menu;
}

void terminal_view_test::a_session_can_be_closed_without_a_tab_bar() {
	/*
	 * Every route that closes ONE session went through the tab bar or
	 * the menu bar: the bar's own close button, middle-click on a tab,
	 * the tab context menu's Close, and File > Close Tab. sec 8.7
	 * dissolved the tab bar on touch -- the column holds every session,
	 * so m_tab_order is empty and there are no tabs -- and the menu bar
	 * is built only for the desktop front door.
	 *
	 * So a phone could open sessions and never close one. The section
	 * that dissolved the tabs accounted for losing the tab REORDER and
	 * replaced it with the column's Move state; Close and Duplicate went
	 * with them and were not replaced.
	 *
	 * Asserted on the terminal menu because that is the surface touch
	 * has. An entry that is not on it is not a route, however well the
	 * registry closes things.
	 */
	bssh_main_window window;
	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);

	bssh_profile_t profile;
	profile.name = QStringLiteral("closes");
	profile.host = QStringLiteral("closes.example");

	bssh_session *session = registry->create(profile);
	window.adopt_session(session);

	/*
	 * Never connected, which is the state Connect exists for -- sec 8.4's
	 * restore leaves every reopened host exactly here. Asserted, because
	 * a live session is deliberately offered no Connect and the check
	 * below would then be asking for a verb that should not be there.
	 */
	QVERIFY2(!session->is_live(), "the fixture connected, so Connect is not expected");

	QMenu *menu = window.build_terminal_menu(&window);
	QVERIFY(menu);

	QAction *close = menu->findChild<QAction *>(QStringLiteral("terminal_close_session"));
	QVERIFY2(close,
	          "the terminal menu offers no way to close this session, and on "
	          "touch there is no tab bar and no menu bar to offer one either");
	QVERIFY2(close->isEnabled(), "the only way to close a session is disabled");

	/*
	 * Duplicate went the same way and for the same reason: it lived on
	 * the tab menu, which touch no longer reaches.
	 */
	QVERIFY2(menu->findChild<QAction *>(QStringLiteral("terminal_duplicate")),
	          "the terminal menu offers no way to duplicate a session");

	/*
	 * And Connect, which the tab menu offers for a session that is not
	 * live. sec 8.4's restore reopens every remembered host and connects
	 * none of them, so without a route to connect, a restored session on
	 * a phone is the dead end that entry was written to prevent -- and
	 * the only thing carrying it was the tab bar's menu.
	 *
	 * This fixture's session was never connected, which is the state the
	 * entry exists for; asserting it on a live one would be asking for a
	 * verb that is deliberately absent.
	 */
	QVERIFY2(menu->findChild<QAction *>(QStringLiteral("terminal_connect")),
	          "a session that is not live has no way to connect: after a restore "
	          "that is a dead end on any device with no tab bar");

	delete menu;
}

void terminal_view_test::dropping_the_history_takes_the_selection_with_it() {
	/*
	 * A selection names ABSOLUTE lines, and bssh_scrollback::clear says
	 * in as many words why: "a cleared line is gone rather than
	 * renumbered, so anything still holding a name for one must fail to
	 * find it". The view holds exactly such a name and nothing told it.
	 *
	 * So: select in the history, drop the history, and Copy is still
	 * offered and hands back blanks -- the selected lines resolve to
	 * empty cells, the trailing-space trim leaves nothing, and what
	 * reaches the clipboard is newlines. Two taps apart on a phone now
	 * that Clear Scrollback is on the terminal menu (sec 6.4).
	 */
	bssh_terminal_view view;
	show_and_resize(view, 640, 96);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	for (int i = 0; i < 20; ++i) {
		emulator.feed(QByteArrayLiteral("row") + QByteArray::number(i)
		               + QByteArrayLiteral("\r\n"));
	}
	QVERIFY(emulator.scrollback().count() > 0);

	view.scroll_by(5);
	drag(view, 0, 0, 3, 0);
	QVERIFY(view.has_selection());
	QVERIFY2(view.selection_text().startsWith(QStringLiteral("row")),
	          "the fixture did not select history, so the drop has nothing "
	          "to take with it");

	emulator.clear_scrollback();
	QCoreApplication::processEvents();

	/*
	 * Asserted on has_selection rather than on the text, because the text
	 * is the symptom and the state is the defect: a view still claiming a
	 * selection keeps Copy enabled, keeps painting a highlight over rows
	 * that hold something else now, and answers the next Copy with
	 * whatever those rows contain.
	 */
	QVERIFY2(!view.has_selection(),
	          "the history went and the view still claims a selection in it");
	QCOMPARE(view.selection_text(), QString());
}

void terminal_view_test::dropping_the_history_takes_the_find_matches_too() {
	/*
	 * The same shape as the selection one beside this, in the feature on
	 * the same menu. A find match is a pair of absolute line numbers,
	 * which is another name the scrollback promises will fail to resolve
	 * rather than resolve to something else -- and the find bar keeps
	 * reporting a count that was true before the history went.
	 *
	 * What a person sees: search, drop the history, and the bar still
	 * says three matches with none of them anywhere, Next walks through
	 * lines that are gone, and the view scrolls to where they used to
	 * be.
	 */
	bssh_terminal_view view;
	show_and_resize(view, 640, 320);

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	view.attach_emulator(&emulator);

	emulator.feed(QByteArrayLiteral("an early needle\r\n"));
	emulator.feed(QByteArrayLiteral("a second needle\r\n"));
	for (int i = 0; i < view.rows() + 20; ++i)
		emulator.feed(QByteArrayLiteral("filler line\r\n"));

	/*
	 * Both matches are in the history and none is on the live screen,
	 * which is the arrangement the drop can empty completely. Asserted
	 * rather than assumed: a match still on the screen would survive and
	 * the count below would be right for the wrong reason.
	 */
	QCOMPARE(view.find_text(QStringLiteral("needle")), 2);
	QVERIFY2(emulator.scrollback().count() > 0,
	          "the fixture scrolled nothing off, so there is no history to drop");

	QSignalSpy told(&view, &bssh_terminal_view::find_matches_changed);

	emulator.clear_scrollback();
	QCoreApplication::processEvents();

	QVERIFY2(view.find_match_count() == 0,
	          "the history went and the find bar still counts matches in it");
	QCOMPARE(view.current_find_index(), 0);

	/*
	 * And the bar is TOLD, because the count is pulled rather than
	 * pushed: update_find_status runs when the query changes or a step
	 * is taken, so a stale count sits on screen until the next keystroke
	 * unless something announces the change.
	 */
	QVERIFY2(told.count() > 0,
	          "the matches went without a word, so the bar keeps its count");
}

void terminal_view_test::the_find_bars_count_is_told_when_the_matches_go() {
	/*
	 * And the route, which the view-level case above cannot see: the
	 * view emits, the pane container relays, and the window pulls the
	 * count into the bar. Every one of those is a place the signal can
	 * stop, and a signal with no listener is the defect this tree keeps
	 * finding -- so this asserts on the TEXT the bar shows a person.
	 */
	bssh_main_window window;
	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);

	bssh_profile_t profile;
	profile.name = QStringLiteral("finds");
	profile.host = QStringLiteral("finds.example");

	bssh_session *session = registry->create(profile);
	window.adopt_session(session);
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	QVERIFY(session->emulator());
	session->emulator()->feed(QByteArrayLiteral("an early needle\r\n"));
	session->emulator()->feed(QByteArrayLiteral("a second needle\r\n"));
	for (int i = 0; i < 80; ++i)
		session->emulator()->feed(QByteArrayLiteral("filler line\r\n"));
	QVERIFY2(session->emulator()->scrollback().count() > 0,
	          "the fixture scrolled nothing off, so there is no history to drop");

	/* Open the search the way touch does, then type into it. */
	QMenu *menu = window.build_terminal_menu(&window);
	QVERIFY(menu);
	QAction *find = menu->findChild<QAction *>(QStringLiteral("terminal_find"));
	QVERIFY(find);
	window.act_on_terminal_menu(menu, find);
	delete menu;

	bssh_find_bar *bar = window.findChild<bssh_find_bar *>();
	QVERIFY(bar);
	QLineEdit *field = bar->findChild<QLineEdit *>();
	QVERIFY(field);
	QLabel *status = bar->findChild<QLabel *>();
	QVERIFY(status);

	field->setText(QStringLiteral("needle"));
	QCoreApplication::processEvents();

	const QString counted = status->text();
	QVERIFY2(counted.contains(QLatin1Char('2')),
	          qPrintable(QStringLiteral("the bar did not report the two matches, "
	                                     "so there is no stale count to catch: ")
	                     + counted));

	session->emulator()->clear_scrollback();
	QCoreApplication::processEvents();

	QVERIFY2(status->text() != counted,
	          qPrintable(QStringLiteral("the history went and the find bar still "
	                                     "reads ") + status->text()));
}

void terminal_view_test::the_history_can_be_dropped_without_the_remote() {
	/*
	 * ED 3 -- what `clear` sends -- was the only way to drop the
	 * scrollback, so a user who wanted their own history gone had to
	 * hope the far end cooperated. A disconnected session cannot
	 * cooperate at all, and sec 6.4 records why somebody asks: "before
	 * pasting a password".
	 *
	 * Asserted on the emulator's own method AND on the route to it,
	 * because they fail differently: a clear nothing calls is the defect
	 * this tree keeps finding, and a menu entry calling a clear that
	 * does not announce leaves the view scrolled into a history that is
	 * gone.
	 */
	bssh_emulator emulator(80, 24);
	for (int i = 0; i < 40; ++i) {
		emulator.feed(QByteArrayLiteral("line ") + QByteArray::number(i)
		               + QByteArrayLiteral("\r\n"));
	}

	QVERIFY2(emulator.scrollback().count() > 0,
	          "the fixture scrolled nothing off, so there is no history to drop");

	QSignalSpy announced(&emulator, &bssh_emulator::scrollback_changed);
	emulator.clear_scrollback();

	QCOMPARE(emulator.scrollback().count(), 0);
	QVERIFY2(announced.count() > 0,
	          "the history went without a word, so a view stays scrolled into it");

	/* And a route a person can take, on both surfaces. */
	bssh_main_window window;
	QVERIFY2(window.findChild<QAction *>(QStringLiteral("menu_clear_scrollback")),
	          "the menu bar offers no way to drop the history");

	QMenu *menu = window.build_terminal_menu(&window);
	QVERIFY(menu);
	QVERIFY2(menu->findChild<QAction *>(QStringLiteral("terminal_clear_scrollback")),
	          "the terminal menu offers no way to drop the history, so touch has none");

	/*
	 * And the modes, which is the other half of putting a wedged
	 * terminal back in order -- and the half that cannot be done from
	 * the shell when the session has dropped, because there is no shell.
	 */
	QVERIFY2(window.findChild<QAction *>(QStringLiteral("menu_reset_terminal")),
	          "the menu bar offers no way to reset the terminal");
	QVERIFY2(menu->findChild<QAction *>(QStringLiteral("terminal_reset")),
	          "the terminal menu offers no way to reset, so a wedged session on "
	          "touch has no way back");
	delete menu;
}

void terminal_view_test::a_substituted_font_says_so_somewhere_a_user_looks() {
	/*
	 * bssh_font_catalog::substitution_note() writes a whole sentence for
	 * a person -- "X is not installed on this device yet. Showing Y until
	 * it is." -- and bssh_terminal_view::font_substitution_note() hands it
	 * on. The view's own header says who displays it: "The window shows
	 * it; the view only knows it."
	 *
	 * No window did. Nothing in src referenced either function, so a host
	 * whose font is not on this device rendered in a substitute with no
	 * explanation, and the sentence written to explain it was produced
	 * for nobody.
	 *
	 * Asserted on the status line rather than on the view, because the
	 * defect is not in the note: the note was always right, and what was
	 * missing is anywhere a user could read it.
	 */
	bssh_main_window window;
	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);

	bssh_profile_t profile;
	profile.name = QStringLiteral("odd font");
	profile.host = QStringLiteral("fonts.example");

	bssh_session *session = registry->create(profile);
	window.adopt_session(session);

	/*
	 * A font id the catalogue does not know, which is the branch that
	 * needs no missing file to reach -- and is what a preset carrying a
	 * font this build dropped would look like.
	 */
	QJsonObject settings = session->settings();
	settings.insert(QStringLiteral("font_family"), QStringLiteral("not-a-bundled-font"));
	session->set_settings(settings);

	bssh_terminal_view *view = window.findChild<bssh_terminal_view *>();
	QVERIFY2(view, "the window built no pane, so nothing resolved a font");
	QVERIFY2(!view->font_substitution_note().isEmpty(),
	          "the fixture's font resolved cleanly, so there is no note to show");

	QVERIFY2(window.status_text().contains(QStringLiteral("not-a-bundled-font")),
	          qPrintable(QStringLiteral("the status line says nothing about the "
	                                     "substitution: %1").arg(window.status_text())));
}

void terminal_view_test::choosing_a_menu_entry_does_what_it_says() {
	/*
	 * Every test of this menu until now asserted that an entry EXISTS.
	 * None could assert that choosing it does anything, because the
	 * dispatch sat after QMenu::exec() -- which runs its own event loop
	 * and returns when somebody clicks, so a headless suite could never
	 * reach past it.
	 *
	 * That is the weaker half of the day's findings: the defect being
	 * hunted was entries that reach nothing, and the guard could only
	 * see whether they were drawn. act_on_terminal_menu() is the seam,
	 * and these are the assertions it makes possible.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);

	bssh_profile_t profile;
	profile.name = QStringLiteral("acts");
	profile.host = QStringLiteral("acts.example");

	bssh_session *session = registry->create(profile);
	window.adopt_session(session);

	/* A history to drop, and a mode to clear. */
	bssh_emulator *emulator = session->emulator();
	QVERIFY(emulator);
	for (int i = 0; i < 40; ++i) {
		emulator->feed(QByteArrayLiteral("line ") + QByteArray::number(i)
		                + QByteArrayLiteral("\r\n"));
	}
	emulator->feed(QByteArrayLiteral("\x1b[?1000h"));

	QVERIFY2(emulator->scrollback().count() > 0, "nothing scrolled off to drop");
	QVERIFY2(emulator->mouse_reporting(), "no mode set, so a reset proves nothing");

	QMenu *menu = window.build_terminal_menu(&window);
	QVERIFY(menu);

	window.act_on_terminal_menu(
	        menu, menu->findChild<QAction *>(QStringLiteral("terminal_clear_scrollback")));
	QCOMPARE(emulator->scrollback().count(), 0);

	QVERIFY2(emulator->mouse_reporting(),
	          "clearing the history also cleared a mode, which is a different verb");

	window.act_on_terminal_menu(
	        menu, menu->findChild<QAction *>(QStringLiteral("terminal_reset")));
	QVERIFY2(!emulator->mouse_reporting(),
	          "Reset Terminal left the mode a dead program set");

	/*
	 * And the one that cannot be undone, last: closing takes the session
	 * out of the registry, which is what every other close route does.
	 */
	const int before = registry->count();
	QVERIFY(before > 0);

	window.act_on_terminal_menu(
	        menu, menu->findChild<QAction *>(QStringLiteral("terminal_close_session")));
	QCOMPARE(registry->count(), before - 1);

	delete menu;
}

void terminal_view_test::the_edit_verbs_on_that_menu_act_too() {
	/*
	 * The rest of what act_on_terminal_menu made reachable. Copy, Paste,
	 * Select All and Find were in the same position as Close and Clear:
	 * present, enabled, and asserted only to exist.
	 *
	 * Open link is deliberately not here. Choosing it hands a URL to
	 * QDesktopServices, which on a developer's machine opens a browser
	 * -- a test that launches an application is one nobody can run twice
	 * in a row, and the confirmation in front of it needs an answer no
	 * headless suite can give.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	/*
	 * SHOWN and sized, which the first version of this fixture was not.
	 * An unshown window lays its pane out at nothing, the pane reports a
	 * degenerate grid, and the container resizes the emulator to match --
	 * so "selectable" went into a terminal about one column wide and Copy
	 * honestly returned "s". The test measured the fixture.
	 */
	bssh_main_window window;
	window.resize(1000, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);

	bssh_profile_t profile;
	profile.name = QStringLiteral("edits");
	profile.host = QStringLiteral("edits.example");
	bssh_session *session = registry->create(profile);
	window.adopt_session(session);
	QCoreApplication::processEvents();

	bssh_terminal_view *view = window.findChild<bssh_terminal_view *>();
	QVERIFY2(view, "no pane, so no verb has anything to act on");
	QVERIFY2(view->cols() > 20,
	          qPrintable(QStringLiteral("the grid is %1 columns wide, so this fixture "
	                                     "cannot hold the word it selects")
	                             .arg(view->cols())));
	QVERIFY2(!view->has_selection(), "the fixture is already selected");

	bssh_emulator *emulator = session->emulator();
	QVERIFY(emulator);
	emulator->feed(QByteArrayLiteral("selectable"));
	QCoreApplication::processEvents();

	QMenu *menu = window.build_terminal_menu(&window);
	QVERIFY(menu);

	window.act_on_terminal_menu(
	        menu, menu->findChild<QAction *>(QStringLiteral("terminal_select_all")));
	QVERIFY2(view->has_selection(), "Select All selected nothing");

	QGuiApplication::clipboard()->clear();
	window.act_on_terminal_menu(
	        menu, menu->findChild<QAction *>(QStringLiteral("terminal_copy")));
	QVERIFY2(QGuiApplication::clipboard()->text().contains(QStringLiteral("selectable")),
	          qPrintable(QStringLiteral("Copy put %1 on the clipboard")
	                             .arg(QGuiApplication::clipboard()->text())));

	/*
	 * One line, deliberately: more than one line of content is what
	 * paste_needs_confirming asks about, and a dialog is a thing this
	 * suite cannot answer.
	 */
	emulator->take_output();
	QGuiApplication::clipboard()->setText(QStringLiteral("pasted"));
	window.act_on_terminal_menu(
	        menu, menu->findChild<QAction *>(QStringLiteral("terminal_paste")));
	QVERIFY2(emulator->take_output().contains(QByteArrayLiteral("pasted")),
	          "Paste sent the remote nothing");

	bssh_find_bar *bar = window.findChild<bssh_find_bar *>();
	QVERIFY(bar);
	QVERIFY2(!bar->isVisible(), "the find bar is up before anybody asked");

	window.act_on_terminal_menu(
	        menu, menu->findChild<QAction *>(QStringLiteral("terminal_find")));
	QVERIFY2(bar->isVisible(), "choosing Find did not open the find bar");

	delete menu;
}

void terminal_view_test::the_arrangement_verbs_on_that_menu_act_too() {
	/*
	 * Split and Show as Tab, which sec 6.4 put on this menu because a
	 * one-session window hides its tab bar and a two-pane window has no
	 * bar to drop a pane onto -- so for those arrangements this menu is
	 * the only route, and until now nothing asserted it was a working
	 * one.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	window.resize(1200, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);

	bssh_profile_t first;
	first.name = QStringLiteral("one");
	first.host = QStringLiteral("one.example");
	window.adopt_session(registry->create(first));

	bssh_profile_t second;
	second.name = QStringLiteral("two");
	second.host = QStringLiteral("two.example");
	window.adopt_session(registry->create(second));
	QCoreApplication::processEvents();

	bssh_pane_container *panes = window.panes();
	QVERIFY(panes);
	QCOMPARE(panes->pane_count(), 1);

	QMenu *menu = window.build_terminal_menu(&window);
	QVERIFY(menu);

	QAction *split = menu->findChild<QAction *>(QStringLiteral("terminal_split"));
	QVERIFY(split);
	QVERIFY2(split->isEnabled(), "this window has no room to split, so the verb proves nothing");

	window.act_on_terminal_menu(menu, split);
	QCoreApplication::processEvents();
	QCOMPARE(panes->pane_count(), 2);

	/*
	 * And back, through the entry that exists because a two-pane window
	 * has no tab bar to drop onto.
	 */
	QMenu *after = window.build_terminal_menu(&window);
	QVERIFY(after);
	window.act_on_terminal_menu(
	        after, after->findChild<QAction *>(QStringLiteral("terminal_show_as_tab")));
	QCoreApplication::processEvents();
	QCOMPARE(panes->pane_count(), 1);

	delete menu;
	delete after;
}

void terminal_view_test::an_unchosen_scheme_follows_the_app_and_a_chosen_one_stays() {
	/*
	 * sec 6.2, the holder's answer of 2026-09-29: a host that never chose
	 * a colour scheme follows the app's appearance -- white on black when
	 * it is dark, black on white when it is light -- and one that chose,
	 * even the dark "default" by name, keeps it. Driven through the theme,
	 * which is the route the desktop flipping takes.
	 */
	const bool was_light = bssh_colour_scheme_catalog::instance().default_id()
	                       == QStringLiteral("black-on-white");

	bssh_terminal_view follows;
	follows.set_colour_scheme(QString());
	bssh_terminal_view chose;
	chose.set_colour_scheme(QStringLiteral("default"));

	bssh_theme::apply(bssh_theme::MODE_DARK);
	QCOMPARE(follows.colour_scheme(), QStringLiteral("default"));
	QVERIFY(follows.terminal_background().lightness() < 128);

	bssh_theme::apply(bssh_theme::MODE_LIGHT);
	QCOMPARE(follows.colour_scheme(), QStringLiteral("black-on-white"));
	QVERIFY2(follows.terminal_background().lightness() >= 128,
	          "the id moved and the pane is still painted dark");
	QCOMPARE(chose.colour_scheme(), QStringLiteral("default"));
	QVERIFY(chose.terminal_background().lightness() < 128);

	bssh_theme::apply(bssh_theme::MODE_DARK);
	QCOMPARE(follows.colour_scheme(), QStringLiteral("default"));

	bssh_theme::apply(was_light ? bssh_theme::MODE_LIGHT : bssh_theme::MODE_DARK);
}
