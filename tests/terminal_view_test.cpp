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
#include "term/colour_scheme.h"
#include <QContextMenuEvent>
#include <QMenu>
#include "ui/main_window.h"
#include "model/session.h"
#include "terminal_view_test.h"

#include "fake_ssh_session.h"
#include "input/input_router.h"
#include "term/emulator.h"
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

void terminal_view_test::the_terminal_menu_offers_the_three_verbs_in_order() {
	bssh_main_window window;

	QMenu *menu = window.build_terminal_menu(&window);
	QVERIFY(menu);

	const QList<QAction *> actions = menu->actions();
	QCOMPARE(actions.size(), 10);

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
	QCOMPARE(actions.at(6)->text(), QStringLiteral("New &Tab"));
	QCOMPARE(actions.at(7)->text(), QStringLiteral("New &Window"));

	/*
	 * And the way to the configuration, behind its own separator (sec 2):
	 * with the desktop opening straight into a local shell, this menu is
	 * a pointer's only route to the host list and the settings. It is a
	 * different kind of thing again, and separated for the same reason
	 * Select All is separated from Copy.
	 */
	QVERIFY(actions.at(8)->isSeparator());
	QCOMPARE(actions.at(9)->text(), QStringLiteral("&Hosts and Settings"));

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
	QCOMPARE(actions.size(), 10);

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
