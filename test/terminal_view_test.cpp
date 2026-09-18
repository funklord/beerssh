#include <QHBoxLayout>
#include <QScrollBar>
#include <QUrl>
#include <QMimeData>
#include <QDropEvent>
#include <QSignalSpy>
#include <QtTest>

#include <QAccessible>
#include <QLineEdit>
#include <QSpinBox>
#include <QTemporaryDir>
#include "model/profile_store.h"
#include "ui/session_dialog.h"
#include "model/app_settings.h"
#include <QHash>
#include <QSet>
#include "term/colour_scheme.h"
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
#include "ui/interface_scale.h"
#include "term/font_catalog.h"
#include "ui/terminal_view.h"
#include "model/session_registry.h"
#include "ui/pane_container.h"


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
	 * Then the session verbs. These lived only on the TAB bar's menu,
	 * which is a target you have to know about -- and a window with one
	 * session hides its tab bar entirely, so splitting was unreachable
	 * from anywhere but a chord. Reported as exactly that: a terminal
	 * cannot be split from its right-click menu.
	 */
	QVERIFY(actions.at(4)->isSeparator());
	QCOMPARE(actions.at(5)->text(), QStringLiteral("Split &Beside Current"));

	/*
	 * The un-split, next to the split and for a reason of its own: with
	 * two panes and nothing else a window has no tab bar to drop a pane
	 * onto, so the gesture alone would leave a split with no way out.
	 * The same argument as Split's, which is on this menu because a
	 * one-session window has no tab bar either.
	 */
	QCOMPARE(actions.at(6)->text(), QStringLiteral("Show as &Tab"));

	/*
	 * Then reordering, which is beside the other two arrangement verbs
	 * because it is the third of them: Split adds a pane, Show as Tab
	 * removes one, and these move one past its neighbour. It is a menu
	 * item rather than a drag because the drag cannot complete on
	 * Android -- QDrag hands the gesture to the platform, which claims
	 * it as drag-to-split-screen (sec 8.4).
	 */
	QCOMPARE(actions.at(7)->text(), QStringLiteral("Move Pane &Left"));
	QCOMPARE(actions.at(8)->text(), QStringLiteral("Move Pane &Right"));

	QCOMPARE(actions.at(9)->text(), QStringLiteral("New &Tab"));
	QCOMPARE(actions.at(10)->text(), QStringLiteral("New &Window"));

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
	 * Access keys on every verb, which is what makes the menu usable from
	 * the keyboard and is half of what those guidelines are about.
	 */
	for (const QAction *action : actions) {
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
	QCOMPARE(actions.size(), 13);

	delete menu;
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

	bssh_main_window::apply_menu_layout(&menu, bssh_app_settings::CONTEXT_MENU_TOUCH);

	/*
	 * The chord goes: it names a key nobody holding a phone has, and the
	 * text is what makes a touch row wide as well as short.
	 */
	QVERIFY2(verb->shortcut().isEmpty(), "the touch layout must drop the chord");

	/*
	 * And the row reaches sec 6.2's floor. Asserted on the number, not
	 * merely on the stylesheet being non-empty: 48 is the whole point.
	 */
	QVERIFY2(menu.styleSheet().contains(QStringLiteral("min-height: 48px")),
	          qPrintable(QStringLiteral("touch rows must reach the 48dp floor, got: ")
	                             + menu.styleSheet()));
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
		QTest::qWaitForWindowExposed(&view);

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
