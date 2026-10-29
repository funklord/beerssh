#include <QSignalSpy>
#include <QtTest>

#include "emulator_test.h"
#include "term/emulator.h"
#include "term/term_features.h"
#include "term/colour_scheme.h"

/* First row of the screen, which is what most of these assert on. */
static QString row(const bssh_emulator &emulator, int index) {
	return emulator.screen().to_text().section(QLatin1Char('\n'), index, index);
}

void emulator_test::plain_text_lands_on_the_first_row() {
	bssh_emulator emulator(20, 5);
	emulator.feed(QByteArrayLiteral("hello"));

	QCOMPARE(row(emulator, 0), QStringLiteral("hello"));
	QCOMPARE(emulator.screen().cursor_col(), 5);
	QCOMPARE(emulator.screen().cursor_row(), 0);
}

/*
 * A line wider than the grid, checked cell by cell rather than by eye.
 *
 * Written after a stray `<` appeared at the last column of a wrapped row
 * on the device (sec 12.3). The renderer cannot be the source -- its whole
 * paint path is background, text runs, cell backgrounds, underline, focus
 * border and cursor, and none of them draws that -- so whatever is there
 * is a cell the emulator placed. This asks the emulator directly, on the
 * host, where the grid can be read instead of photographed.
 *
 * 45 columns because that is what the phone reports at its default font
 * size, and the debris appeared at column 44.
 */
void emulator_test::a_line_longer_than_the_grid_wraps_without_debris() {
	bssh_emulator emulator(45, 5);

	emulator.feed(QByteArray(60, 'A'));

	/* 45 across, then the remaining 15 on the next row. */
	QCOMPARE(row(emulator, 0), QString(45, QLatin1Char('A')));
	QCOMPARE(row(emulator, 1), QString(15, QLatin1Char('A')));

	/* And nothing anywhere else -- the debris was on a row of its own. */
	QCOMPARE(row(emulator, 2), QString());
	QCOMPARE(row(emulator, 3), QString());
	QCOMPARE(row(emulator, 4), QString());
}

/*
 * The same question once the grid has SCROLLED, which the test above does
 * not reach: 60 characters in a 45x5 grid is two rows and never touches
 * the bottom.
 *
 * On the device the stray `<` (sec 12.3) always sits on a row that
 * continues a line whose beginning has scrolled off the top, and never on
 * a row printed whole. Scrolling is also where vterm-patch/0003 lives --
 * deferred damage flushed before a scroll -- so a stale cell surviving a
 * scroll is the shape worth asking about.
 *
 * Every row is checked for characters that were never sent. The feed is
 * only 'A', 'B' and newlines, so anything else on the grid is debris.
 */
void emulator_test::a_reset_from_the_remote_clears_the_modes_it_did_not_set() {
	bssh_emulator emulator(40, 6);

	emulator.feed(QByteArrayLiteral("\x1b[?1049h\x1b[?1000h\x1b[?1006h"));
	QVERIFY(emulator.mouse_reporting());
	QVERIFY(emulator.alternate_screen());

	/*
	 * RIS, which is what `reset` at a shell sends. libvterm cleared both
	 * of these by direct assignment, never through settermprop -- so the
	 * screen layer went on pointing at the alternate buffer while the
	 * state said primary, and this terminal kept a mouse mode libvterm no
	 * longer had, routing every click to a remote that would drop it.
	 * vterm-patch/0005 makes the reset announce what it clears.
	 */
	emulator.feed(QByteArrayLiteral("\033c"));

	QVERIFY(!emulator.mouse_reporting());
	QVERIFY(!emulator.alternate_screen());

	/*
	 * And the primary screen is really back, not merely claimed: text
	 * written now must be readable, which it would not be if the screen
	 * layer were still drawing into the alternate buffer.
	 */
	emulator.feed(QByteArrayLiteral("after the reset"));
	QCOMPARE(row(emulator, 0), QStringLiteral("after the reset"));
}

void emulator_test::decscusr_sets_the_cursor_shape_and_blink() {
	bssh_emulator emulator(40, 6);

	/*
	 * DECSCUSR (CSI Ps SP q): the remote asks for a cursor shape, the way
	 * vim does between its modes. The emulator reports it in the preset's
	 * own words so the view needs no translation. The spies are made after
	 * construction, so the shape libvterm announces at its initial reset is
	 * already past and only these deliberate changes are seen.
	 */
	QSignalSpy shape(&emulator, &bssh_emulator::cursor_shape_changed);
	QSignalSpy blink(&emulator, &bssh_emulator::cursor_blink_changed);

	/* Ps 4: steady underline. */
	emulator.feed(QByteArrayLiteral("\x1b[4 q"));
	QVERIFY2(!shape.isEmpty(), "DECSCUSR did not change the cursor shape");
	QCOMPARE(shape.takeLast().at(0).toString(), QStringLiteral("underline"));

	/* Ps 5: blinking bar -- shape and blink both move. */
	shape.clear();
	blink.clear();
	emulator.feed(QByteArrayLiteral("\x1b[5 q"));
	QVERIFY(!shape.isEmpty());
	QCOMPARE(shape.takeLast().at(0).toString(), QStringLiteral("bar"));
	QVERIFY2(!blink.isEmpty(), "DECSCUSR did not change the cursor blink");
	QCOMPARE(blink.takeLast().at(0).toBool(), true);

	/* Ps 2: steady block. */
	shape.clear();
	blink.clear();
	emulator.feed(QByteArrayLiteral("\x1b[2 q"));
	QVERIFY(!shape.isEmpty());
	QCOMPARE(shape.takeLast().at(0).toString(), QStringLiteral("block"));
	QVERIFY(!blink.isEmpty());
	QCOMPARE(blink.takeLast().at(0).toBool(), false);
}

void emulator_test::decscnm_toggles_reverse_video() {
	bssh_emulator emulator(40, 6);

	/*
	 * DECSCNM (CSI ?5h/l): the remote flips the whole screen. The emulator
	 * only reports it -- the swap is the view's -- so a spy on the signal
	 * is what there is to assert here.
	 */
	QSignalSpy reverse(&emulator, &bssh_emulator::reverse_video_changed);

	emulator.feed(QByteArrayLiteral("\x1b[?5h"));
	QVERIFY2(!reverse.isEmpty(), "DECSCNM on was not reported");
	QCOMPARE(reverse.takeLast().at(0).toBool(), true);

	reverse.clear();
	emulator.feed(QByteArrayLiteral("\x1b[?5l"));
	QVERIFY2(!reverse.isEmpty(), "DECSCNM off was not reported");
	QCOMPARE(reverse.takeLast().at(0).toBool(), false);
}

void emulator_test::a_reconnect_keeps_the_history_and_drops_the_modes() {
	bssh_emulator emulator(40, 6);

	emulator.feed(QByteArrayLiteral("history worth keeping\r\n"));

	/*
	 * What a full-screen program leaves behind when the link dies under
	 * it: it never got to disable any of this. sec 5.3 keeps the emulator
	 * across a reconnect so the user keeps that history -- which means
	 * the modes come too unless something takes them.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[?1049h"));
	emulator.feed(QByteArrayLiteral("\x1b[?1000h\x1b[?1006h"));
	emulator.feed(QByteArrayLiteral("\x1b[?2004h"));
	QVERIFY(emulator.mouse_reporting());
	QVERIFY(emulator.alternate_screen());
	emulator.take_output();

	emulator.reset_modes();

	QVERIFY(!emulator.mouse_reporting());
	QVERIFY(!emulator.alternate_screen());

	/*
	 * And the history survived, which is the whole reason the emulator is
	 * kept rather than rebuilt. Leaving the alternate screen is what
	 * brings it back into view: while that buffer was up it was there but
	 * not visible, so this checks the screen rather than the scrollback.
	 */
	QVERIFY2(emulator.screen().to_text().contains(QStringLiteral("history worth keeping")),
	          qPrintable(emulator.screen().to_text()));

	/*
	 * A paste is no longer wrapped in markers the new shell would print
	 * as text. Asked through send_paste rather than by reading a flag,
	 * because the flag is not what a shell sees.
	 */
	emulator.send_paste(QStringLiteral("ls"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("ls"));
}

void emulator_test::a_wrapped_line_redrawn_shorter_leaves_no_debris() {
	/*
	 * What a line EDITOR does, which neither wrap test above does: it
	 * rewrites a line in place, and the rewrite is often shorter. sec
	 * 12.3's stray `<` sits at the last column of such a row with blanks
	 * between it and the new text, and the device says the character is
	 * in the GRID rather than on the glass -- so if it is reproducible at
	 * all it is reproducible here, with no device and no shell.
	 *
	 * The row is filled exactly to its last column, which is the case
	 * worth asking about: writing the last cell leaves the cursor there
	 * with autowrap PENDING rather than moving it, and an erase issued
	 * afterwards has to account for that.
	 *
	 * It passes, all four shapes -- the erase before the write, after it,
	 * whole-line, and issued from the pending position.
	 *
	 * The `<` it was written to hunt turned out to be mksh's own
	 * horizontal-scroll marker, sent as a literal character (sec 12.3),
	 * so there was never anything here to find. Kept anyway: a full row
	 * redrawn shorter is the shape every line editor produces, the
	 * pending-wrap state is the fiddliest thing on that path, and neither
	 * of the two wrap tests beside it covers either.
	 */
	const int cols = 45;

	struct { const char *name; QByteArray redraw; } cases[] = {
		{ "cr, write, erase to end", QByteArrayLiteral("\rSHORT\x1b[K") },
		{ "cr, erase to end, write", QByteArrayLiteral("\r\x1b[KSHORT") },
		{ "cr, write, erase whole",  QByteArrayLiteral("\rSHORT\x1b[2K") },
		{ "erase to end from here",  QByteArrayLiteral("\x1b[K") },
	};

	for (const auto &shape : cases) {
		bssh_emulator emulator(cols, 5);

		/* Exactly one row, ending in the character being hunted. */
		QByteArray line(cols - 1, 'A');
		line.append('<');
		emulator.feed(line);
		QCOMPARE(row(emulator, 0).size(), cols);
		QCOMPARE(row(emulator, 0).at(cols - 1), QLatin1Char('<'));

		emulator.feed(shape.redraw);

		const QString text = row(emulator, 0);
		QVERIFY2(!text.contains(QLatin1Char('<')),
		          qPrintable(QStringLiteral("%1: row 0 kept a '<': \"%2\"")
		                      .arg(QLatin1String(shape.name)).arg(text)));
	}
}

void emulator_test::wrapped_lines_scrolling_off_leave_no_debris() {
	bssh_emulator emulator(45, 5);

	/* Eight wrapped lines through a five-row grid: it scrolls repeatedly. */
	for (int i = 0; i < 8; ++i) {
		emulator.feed(QByteArray(60, char('A' + (i % 2))));
		emulator.feed(QByteArrayLiteral("\r\n"));
	}

	const QString grid = emulator.screen().to_text();
	for (int i = 0; i < grid.size(); ++i) {
		const QChar ch = grid.at(i);
		const bool sent = ch == QLatin1Char('A') || ch == QLatin1Char('B')
		                   || ch == QLatin1Char('\n') || ch == QLatin1Char(' ');
		QVERIFY2(sent, qPrintable(QStringLiteral("debris on the grid at %1: U+%2 in [%3]")
		                                   .arg(i)
		                                   .arg(ch.unicode(), 4, 16, QLatin1Char('0'))
		                                   .arg(grid)));
	}
}

void emulator_test::newline_and_carriage_return_move_as_expected() {
	bssh_emulator emulator(20, 5);

	/*
	 * A bare LF moves down but NOT to column 0 -- that is the distinction
	 * between LF and CRLF, and getting it wrong produces the staircase
	 * output every terminal bug report includes a screenshot of.
	 */
	emulator.feed(QByteArrayLiteral("ab\ncd"));
	QCOMPARE(row(emulator, 0), QStringLiteral("ab"));
	QCOMPARE(row(emulator, 1), QStringLiteral("  cd"));

	emulator.feed(QByteArrayLiteral("\r\nefg"));
	QCOMPARE(row(emulator, 2), QStringLiteral("efg"));
}

void emulator_test::cursor_addressing_places_text() {
	bssh_emulator emulator(20, 5);

	/* CUP is 1-based on the wire, 0-based in the grid. */
	emulator.feed(QByteArrayLiteral("\x1b[3;5Hx"));

	QCOMPARE(row(emulator, 2), QStringLiteral("    x"));
	QCOMPARE(emulator.screen().cursor_row(), 2);
}

void emulator_test::erase_display_clears_everything() {
	bssh_emulator emulator(20, 5);
	emulator.feed(QByteArrayLiteral("keep\r\nthis\r\naround"));
	QCOMPARE(row(emulator, 1), QStringLiteral("this"));

	emulator.feed(QByteArrayLiteral("\x1b[2J"));

	QCOMPARE(emulator.screen().to_text(), QString(4, QLatin1Char('\n')));
}

void emulator_test::scrolling_drops_the_top_line() {
	bssh_emulator emulator(20, 3);

	emulator.feed(QByteArrayLiteral("one\r\ntwo\r\nthree\r\nfour"));

	/* The screen scrolled: "one" is gone, "four" is on the last row. */
	QCOMPARE(row(emulator, 0), QStringLiteral("two"));
	QCOMPARE(row(emulator, 2), QStringLiteral("four"));
}

void emulator_test::sgr_sets_colour_and_attributes() {
	bssh_emulator emulator(20, 3);

	/* Bold + red foreground, then reset, then a plain cell. */
	emulator.feed(QByteArrayLiteral("\x1b[1;31mR\x1b[0mp"));

	const bssh_cell_t &red = emulator.screen().cell(0, 0);
	QVERIFY(red.attrs & bssh_cell_t::BOLD);
	QCOMPARE(red.fg.kind, bssh_color_t::INDEXED);
	QCOMPARE(int(red.fg.index), 1);

	const bssh_cell_t &plain = emulator.screen().cell(0, 1);
	QCOMPARE(plain.attrs, quint8(bssh_cell_t::NONE));

	/*
	 * The reset must restore DEFAULT, not "index 7 which looks white".
	 * A cell that remembers an explicit colour it was never given stops
	 * following the colour scheme (see bssh_color_t).
	 */
	QCOMPARE(plain.fg.kind, bssh_color_t::DEFAULT);
}

void emulator_test::wide_characters_take_two_cells() {
	bssh_emulator emulator(20, 3);

	emulator.feed(QString::fromUtf8("你好!").toUtf8());

	QCOMPARE(int(emulator.screen().cell(0, 0).width), 2);
	QCOMPARE(int(emulator.screen().cell(0, 1).width), 0);
	QCOMPARE(int(emulator.screen().cell(0, 2).width), 2);
	QCOMPARE(int(emulator.screen().cell(0, 4).width), 1);

	/* Two glyphs occupying four columns, then the exclamation mark. */
	QCOMPARE(row(emulator, 0), QString::fromUtf8("你好!"));
}

void emulator_test::utf8_split_across_chunks_still_composes() {
	bssh_emulator emulator(20, 3);

	/*
	 * A multi-byte character split between two reads is the normal case
	 * on a socket, not an edge case. Feeding the halves separately is
	 * exactly what broke the M1 naive view, which decoded each chunk on
	 * its own.
	 */
	const QByteArray utf8 = QString::fromUtf8("héllo").toUtf8();
	emulator.feed(utf8.left(2));
	emulator.feed(utf8.mid(2));

	QCOMPARE(row(emulator, 0), QString::fromUtf8("héllo"));
}

void emulator_test::device_status_report_produces_a_reply() {
	bssh_emulator emulator(20, 5);
	QSignalSpy spy(&emulator, &bssh_emulator::output_ready);

	emulator.feed(QByteArrayLiteral("\x1b[5;3H"));
	emulator.feed(QByteArrayLiteral("\x1b[6n")); /* report cursor position */

	QVERIFY(spy.count() > 0);

	/*
	 * A terminal that never answers this hangs any program that asks and
	 * waits -- which is most full-screen editors, during startup.
	 */
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[5;3R"));

	/* Drained, not duplicated on the next call. */
	QCOMPARE(emulator.take_output(), QByteArray());
}

void emulator_test::title_is_reported_from_osc() {
	bssh_emulator emulator(20, 5);
	QSignalSpy spy(&emulator, &bssh_emulator::title_changed);

	emulator.feed(QByteArrayLiteral("\x1b]0;user@host: ~\x07"));

	QCOMPARE(spy.count(), 1);
	QCOMPARE(spy.first().first().toString(), QStringLiteral("user@host: ~"));
}

/*
 * A glyph in the LAST column survives the canonical erase, and that is
 * correct rather than a defect. Pinned because it looks exactly like a
 * bug on screen and the obvious fix would break conformance.
 *
 * Seen on a phone: eight characters typed at a local shell, backspace
 * held until they were gone, and one of them left sitting at the right
 * edge of the line -- surviving the shell redrawing the line and fresh
 * output scrolling past below it.
 *
 * The mechanism, from the vendored library rather than from reasoning
 * about the spec. libvterm's own t/20state_wrapping.test pins that
 * after a glyph is written into the 80th column the cursor still
 * reports `0,79`: the wrap is PENDING, held in an `at_phantom` flag,
 * and the cursor has not left the last column. `on_control` ends with
 * `updatecursor(state, &oldpos, 1)`, so BS is an explicit cursor move
 * and cancels that flag -- which upstream's last case calls "phantom
 * cancelled by explicit cursor move". So BS from the last column lands
 * on N-2, the space rewrites N-2, and nothing the sequence does can
 * reach N-1 again.
 *
 * `\b \b` is therefore incapable of erasing the last column on any
 * terminal that follows this model, xterm included.
 *
 * WHY IT IS RARELY SEEN, which is the part that decides whether this is
 * worth chasing: a line editor with terminfo emits `\b ESC[K` instead,
 * and clear-to-end-of-line takes the last column with it. sec 12.3's
 * pty test records the same shell emitting `\b ESC[K` with TERM=xterm
 * and falling back to the canonical `\b \b` with TERM unset. Android
 * ships no terminfo database, so the local shell there gets the
 * fallback and the artifact is visible.
 *
 * That is a question about what the app gives a LOCAL shell on Android,
 * not about this emulator, and it is recorded in project.md rather than
 * answered here.
 */
void emulator_test::the_last_column_survives_the_canonical_erase() {
	/*
	 * A 7-character prompt in 20 columns, so fill=13 puts the last
	 * character in column 19 and 11 and 12 do not. The two that do not
	 * are the control: without them a clean erase everywhere and a
	 * broken erase everywhere are the same result.
	 */
	struct { int fill; int survivors; } cases[] = {
		{ 11, 0 }, { 12, 0 }, { 13, 1 },
	};

	for (const auto &one : cases) {
		bssh_emulator emulator(20, 5);
		emulator.feed(QByteArrayLiteral("READY> "));
		emulator.feed(QByteArray(one.fill, 'z'));

		for (int i = 0; i < one.fill; ++i)
			emulator.feed(QByteArrayLiteral("\b \b"));

		const QString left = row(emulator, 0);
		QCOMPARE(left.count(QLatin1Char('z')), one.survivors);
		QVERIFY2(left.startsWith(QStringLiteral("READY>")),
		          qPrintable(QStringLiteral("the erase ate into the prompt: \"%1\"")
		                             .arg(left)));
	}
}

void emulator_test::resize_keeps_existing_text() {
	bssh_emulator emulator(20, 5);
	emulator.feed(QByteArrayLiteral("stays"));

	emulator.resize(10, 3);

	QCOMPARE(emulator.screen().cols(), 10);
	QCOMPARE(emulator.screen().rows(), 3);
	QCOMPARE(row(emulator, 0), QStringLiteral("stays"));
}

/*
 * The Fold's cover screen turned to landscape with the full built-in
 * keyboard up: the grid went from 49x40 to 120x2 and back, and afterwards
 * the shell's prompt was gone, neither on screen nor in the scrollback.
 * This asks the emulator's half of that: a prompt on the top row with the
 * cursor after it must survive the round trip, so that what is left to
 * explain is what the shell did on SIGWINCH.
 */
void emulator_test::a_rotation_and_back_keeps_the_prompt() {
	bssh_emulator emulator(49, 40);
	emulator.feed(QByteArrayLiteral(":/data/data/se.vibes.beerssh/files $ "));

	emulator.resize(120, 2);
	QCOMPARE(row(emulator, 0), QStringLiteral(":/data/data/se.vibes.beerssh/files $"));

	emulator.resize(49, 40);
	QCOMPARE(row(emulator, 0), QStringLiteral(":/data/data/se.vibes.beerssh/files $"));
	QCOMPARE(emulator.screen().cursor_row(), 0);
}

void emulator_test::scrolled_lines_enter_the_scrollback() {
	bssh_emulator emulator(20, 3);
	QSignalSpy spy(&emulator, &bssh_emulator::scrollback_changed);

	QCOMPARE(emulator.scrollback().count(), 0);

	emulator.feed(QByteArrayLiteral("one\r\ntwo\r\nthree\r\nfour\r\nfive"));

	/* Two lines pushed off the top of a three-row screen. */
	QCOMPARE(emulator.scrollback().count(), 2);
	QVERIFY(spy.count() >= 2);

	/* Oldest first, and the text is intact. */
	const QList<bssh_cell_t> &oldest = emulator.scrollback().line(0);
	QCOMPARE(QString::fromUcs4(&oldest.at(0).ch, 1), QStringLiteral("o"));
	QCOMPARE(QString::fromUcs4(&oldest.at(1).ch, 1), QStringLiteral("n"));
	QCOMPARE(QString::fromUcs4(&oldest.at(2).ch, 1), QStringLiteral("e"));
}

void emulator_test::a_row_scrolled_off_before_its_damage_is_flushed_is_still_intact() {
	bssh_emulator emulator(20, 3);

	/*
	 * The scrollback is taken from the MIRROR, and the mirror is only as
	 * current as the last damage flush. Damage is merged per ROW, so a
	 * row stays pending until something touches a different row -- which
	 * means a scroll that happens while the TOP row is the pending one
	 * reads a row the mirror has not been told about yet.
	 *
	 * Ordinary output never hits this: writing has moved down the screen
	 * long before the scroll, so row 0 was flushed ages ago. Writing to
	 * row 0 and scrolling immediately is the case that does.
	 */
	emulator.feed(QByteArrayLiteral("HELLO\x1b[S"));

	/* A second line, so the first has been trimmed (bssh_scrollback::push). */
	emulator.feed(QByteArrayLiteral("\x1b[S"));
	QCOMPARE(emulator.scrollback().count(), 2);

	const QList<bssh_cell_t> &pushed = emulator.scrollback().line(0);
	QString text;
	for (const bssh_cell_t &cell : pushed) text += QString::fromUcs4(&cell.ch, 1);

	QCOMPARE(text, QStringLiteral("HELLO"));
}

void emulator_test::scrollback_is_capped() {
	bssh_emulator emulator(20, 2);
	emulator.set_scrollback_lines(3);

	for (int i = 0; i < 20; ++i) {
		emulator.feed(QByteArray::number(i) + QByteArrayLiteral("\r\n"));
	}

	/*
	 * A terminal left on a chatty log is the normal case. Without the cap
	 * this list grows for as long as the session lives, which on a phone
	 * is the difference between working and being killed.
	 */
	QCOMPARE(emulator.scrollback().count(), 3);

	/* The cap keeps the NEWEST lines, not the first three ever seen. */
	const QList<bssh_cell_t> &newest = emulator.scrollback().line(2);
	QCOMPARE(QString::fromUcs4(&newest.at(0).ch, 1), QStringLiteral("1"));
}

void emulator_test::scrollback_keeps_colour() {
	bssh_emulator emulator(20, 2);

	emulator.feed(QByteArrayLiteral("\x1b[31mred\x1b[0m\r\na\r\nb"));

	QVERIFY(emulator.scrollback().count() >= 1);

	/*
	 * Cells, not text: scrolled-back output that loses its colours is
	 * scrolled-back output nobody can read a log in.
	 */
	const bssh_cell_t &cell = emulator.scrollback().line(0).at(0);
	QCOMPARE(cell.fg.kind, bssh_color_t::INDEXED);
	QCOMPARE(int(cell.fg.index), 1);
}

void emulator_test::alternate_screen_does_not_pollute_the_scrollback() {
	bssh_emulator emulator(20, 3);

	emulator.feed(QByteArrayLiteral("keep\r\nthis\r\nhistory\r\nplease"));
	const int before = emulator.scrollback().count();
	QVERIFY(before > 0);

	/*
	 * The alternate screen is what vim and less run on, and its scrolling
	 * must not end up in history -- otherwise quitting an editor leaves
	 * the scrollback full of its screen redraws.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[?1049h"));
	emulator.feed(QByteArrayLiteral("a\r\nb\r\nc\r\nd\r\ne\r\nf"));
	emulator.feed(QByteArrayLiteral("\x1b[?1049l"));

	QCOMPARE(emulator.scrollback().count(), before);
}

void emulator_test::synchronised_output_holds_the_repaint() {
	/* Wide enough that the test string does not wrap: a wrapped line puts
	 * a newline through the middle of it, and the assertion would fail on
	 * the screen's width rather than on the behaviour under test. */
	bssh_emulator emulator(40, 5);
	QSignalSpy damaged(&emulator, &bssh_emulator::damaged);

	emulator.feed(QByteArrayLiteral("before"));
	QVERIFY(damaged.count() > 0);
	damaged.clear();

	/* DEC 2026 on: the remote is rewriting the screen. */
	emulator.feed(QByteArrayLiteral("\x1b[?2026h"));
	QVERIFY(emulator.synchronised_output());

	emulator.feed(QByteArrayLiteral("\x1b[2J\x1b[Hhalf a frame"));
	emulator.feed(QByteArrayLiteral(" and the rest"));

	/*
	 * Nothing painted yet -- that is the whole point. Painting here is
	 * exactly the torn frame the mode exists to prevent.
	 */
	QCOMPARE(damaged.count(), 0);

	emulator.feed(QByteArrayLiteral("\x1b[?2026l"));
	QVERIFY(!emulator.synchronised_output());

	/* One repaint for the whole frame, not one per write. */
	QCOMPARE(damaged.count(), 1);
	QVERIFY(emulator.screen().to_text().contains(QStringLiteral("half a frame and the rest")));
}

/* Every rows_damaged() since the spy was cleared, as one range. */
static QPair<int, int> damaged_rows(const QSignalSpy &spy) {
	int first = INT_MAX;
	int last = -1;
	for (const QList<QVariant> &call : spy) {
		first = qMin(first, call.at(0).toInt());
		last = qMax(last, call.at(1).toInt());
	}
	return qMakePair(first, last);
}

void emulator_test::damage_names_the_rows_it_touched() {
	/*
	 * A view repaints only the rows this names (project.md sec 9.5a), so
	 * a row it leaves out keeps what it showed before: a character typed
	 * and never drawn, or a cursor drawn in two places.
	 */
	bssh_emulator emulator(20, 10);
	QSignalSpy rows(&emulator, &bssh_emulator::rows_damaged);

	emulator.feed(QByteArrayLiteral("\x1b[3;1H"));
	rows.clear();

	/* Text on row 2, and the cursor moving along it. */
	emulator.feed(QByteArrayLiteral("ab"));
	QCOMPARE(damaged_rows(rows), qMakePair(2, 2));

	/* The cursor moving down names the row it LEFT as well, or the old
	 * cursor stays drawn there. */
	rows.clear();
	emulator.feed(QByteArrayLiteral("\x1b[6;1H"));
	QCOMPARE(damaged_rows(rows), qMakePair(2, 5));

	/* Held across synchronised output, and released as the union. */
	rows.clear();
	emulator.feed(QByteArrayLiteral("\x1b[?2026h\x1b[1;1Hx\x1b[9;1Hy"));
	QCOMPARE(rows.count(), 0);
	emulator.feed(QByteArrayLiteral("\x1b[?2026l"));
	QCOMPARE(rows.count(), 1);
	QCOMPARE(damaged_rows(rows), qMakePair(0, 8));

	/* What is not a run of rows reports all of them. */
	rows.clear();
	emulator.reset_modes();
	QVERIFY(damaged_rows(rows).first <= 0);
	QVERIFY(damaged_rows(rows).second >= 9);
}

void emulator_test::synchronised_output_releases_itself_if_abandoned() {
	bssh_emulator emulator(20, 5);
	QSignalSpy damaged(&emulator, &bssh_emulator::damaged);

	emulator.feed(QByteArrayLiteral("\x1b[?2026h"));
	emulator.feed(QByteArrayLiteral("stuck?"));
	QCOMPARE(damaged.count(), 0);

	/*
	 * The remote never sends the closing sequence -- it crashed, or the
	 * connection dropped mid-frame. Without a timeout the display would
	 * be frozen for the rest of the session, which is a worse failure
	 * than the tearing this mode prevents.
	 */
	QTest::qWait(250);

	QVERIFY(!emulator.synchronised_output());
	QCOMPARE(damaged.count(), 1);
	QVERIFY(emulator.screen().to_text().contains(QStringLiteral("stuck?")));
}

void emulator_test::osc8_marks_cells_with_a_link() {
	bssh_emulator emulator(40, 5);

	emulator.feed(QByteArrayLiteral("see \x1b]8;;https://example.com/x\x07"
	                                 "this link\x1b]8;;\x07 and after"));

	QCOMPARE(row(emulator, 0), QStringLiteral("see this link and after"));

	/* The four cells before the link are plain. */
	QCOMPARE(int(emulator.screen().cell(0, 0).link), 0);

	/* "this link" carries a link id, and the id resolves to the URI. */
	const quint16 id = emulator.screen().cell(0, 4).link;
	QVERIFY(id != 0);
	QCOMPARE(emulator.link_uri(id), QStringLiteral("https://example.com/x"));
	QCOMPARE(emulator.screen().cell(0, 12).link, id);

	/*
	 * And the text after the closing sequence is NOT part of it. A link
	 * that swallows the rest of the line is the failure mode here, and it
	 * looks fine until someone clicks the wrong word.
	 */
	QCOMPARE(int(emulator.screen().cell(0, 14).link), 0);
	QCOMPARE(int(emulator.screen().cell(0, 20).link), 0);
}

void emulator_test::the_hyperlink_table_is_bounded_and_a_reset_empties_it() {
	/*
	 * BSSH_MAX_LINK_CHARS bounds ONE uri and the id wrap bounds the count
	 * at 65535. The product is not a bound anybody would choose: about
	 * 536 MB, kept for the life of the session, because `links` was never
	 * cleared -- not by the reset that clears every other piece of state
	 * this file owns.
	 *
	 * The per-uri cap landed earlier tonight and its own message said
	 * "65535 times that is not a limit" about the cap it replaced, then
	 * left the aggregate exactly where it was. This is that half.
	 */
	bssh_emulator emulator(40, 5);

	const QByteArray uri(2048, 'a');

	/* Comfortably past the budget: 8000 x 2048 chars is ~32 MB of
	 * QChar, four times BSSH_MAX_LINK_BYTES. */
	for (int i = 0; i < 8000; ++i) {
		emulator.feed(QByteArrayLiteral("\x1b]8;;https://example.com/") + uri
		               + QByteArrayLiteral("\x07x\x1b]8;;\x07"));
	}

	const qint64 held = qint64(emulator.link_count()) * 2048 * 2;
	QVERIFY2(held <= BSSH_MAX_LINK_BYTES * 2,
	          qPrintable(QStringLiteral("the table holds about %1 bytes of uri")
	                             .arg(held)));

	/*
	 * And it still WORKS: the most recent link resolves, so the bound is
	 * dropping old entries rather than refusing to store anything.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[H"));   /* 8000 lines of output have scrolled */
	emulator.feed(QByteArrayLiteral("\x1b]8;;https://recent.example/\x07"
	                                 "here\x1b]8;;\x07"));
	const quint16 id = emulator.screen().cell(0, 0).link;
	QVERIFY(id != 0);
	QCOMPARE(emulator.link_uri(id), QStringLiteral("https://recent.example/"));

	/*
	 * A reset drops them, like every other piece of state this file owns.
	 * The screen the links were anchored to is gone, so keeping the uris
	 * keeps the one thing nothing can reach.
	 */
	/*
	 * \033c, not \x1bc: a hex escape eats as many digits as it can, so
	 * \x1bc is the single value 0x1bc rather than ESC followed by c.
	 * The RIS test at the top of this file uses octal for the same
	 * reason -- and this one fed garbage and reported the reset as not
	 * clearing anything.
	 */
	emulator.feed(QByteArrayLiteral("\033c"));
	QCOMPARE(emulator.link_count(), 0);
}

void emulator_test::the_kitty_flags_stack_is_bounded() {
	/*
	 * CSI > flags u pushes a kitty keyboard mode. The pop beside it is
	 * guarded so the stack never empties, and the set writes in place --
	 * the PUSH had no bound at all, and the stack is cleared only by a
	 * reset. Five bytes on the wire per entry, held for the life of the
	 * session, with the remote deciding how many.
	 *
	 * That is the shape this file bounds everywhere else: the title
	 * buffer, OSC 52, the DCS and APC payloads, the hyperlink URIs. Its
	 * own words for it -- "accumulating them without a bound of our own
	 * would put the cap back where it started: the remote decides how
	 * much memory this costs".
	 */
	bssh_emulator emulator(40, 5);

	/* One entry to begin with, which is the base the pop refuses to
	 * remove. */
	QCOMPARE(emulator.kitty_stack_depth(), 1);

	for (int i = 0; i < 5000; ++i) {
		emulator.feed(QByteArrayLiteral("\x1b[>1u"));
	}

	QVERIFY2(emulator.kitty_stack_depth() <= BSSH_MAX_KITTY_STACK,
	          qPrintable(QStringLiteral("the stack grew to %1 entries")
	                             .arg(emulator.kitty_stack_depth())));

	/*
	 * And popping past the bottom still lands on the base rather than on
	 * whatever eviction left behind -- which is why the cap drops the
	 * entry ABOVE the base rather than the base itself.
	 */
	for (int i = 0; i < 5000; ++i) {
		emulator.feed(QByteArrayLiteral("\x1b[<1u"));
	}

	QCOMPARE(emulator.kitty_stack_depth(), 1);
	QCOMPARE(emulator.kitty_keyboard_flags(), 0);

	/* An ordinary push and pop still round-trips, so this is a check on
	 * the bound and not on the protocol having been switched off. */
	emulator.feed(QByteArrayLiteral("\x1b[>1u"));
	QCOMPARE(emulator.kitty_keyboard_flags(), 1);
	emulator.feed(QByteArrayLiteral("\x1b[<1u"));
	QCOMPARE(emulator.kitty_keyboard_flags(), 0);
}

void emulator_test::erase_saved_lines_actually_empties_the_scrollback() {
	/*
	 * CSI 3 J is what `clear` sends as its E3 capability, after homing
	 * the cursor and erasing the screen. The callback for it was declared
	 * in the table and left null, so the sequence arrived and nothing
	 * happened: measured at 16 scrollback lines before and 16 after, for
	 * the bare sequence and for the whole of what `clear` sends.
	 *
	 * A terminal that keeps the history through `clear` is not a cosmetic
	 * problem: running it before pasting something private is exactly
	 * what people do.
	 */
	bssh_emulator emulator(40, 5);

	for (int i = 0; i < 20; ++i) {
		emulator.feed(QByteArrayLiteral("line") + QByteArray::number(i)
		               + QByteArrayLiteral("\r\n"));
	}
	const int filled = emulator.scrollback().count();
	QVERIFY2(filled > 0, "the fixture built no history, so this proves nothing");

	const qint64 base_before = emulator.scrollback().base();

	/*
	 * Erasing the SCREEN is not erasing the history, and a fix that
	 * cleared on any ED would take the scrollback away from every
	 * full-screen program that repaints with CSI 2 J.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[2J"));
	QCOMPARE(emulator.scrollback().count(), filled);

	emulator.feed(QByteArrayLiteral("\x1b[3J"));
	QCOMPARE(emulator.scrollback().count(), 0);

	/*
	 * And the names go with them. bssh_scrollback::clear advances the
	 * base by what it dropped, so a selection anchored in the erased
	 * history fails to find its line rather than finding a different one
	 * under the same name.
	 */
	QCOMPARE(emulator.scrollback().base(), base_before + filled);
}

void emulator_test::an_enormous_link_is_dropped_whole_and_shown_short() {
	/*
	 * The link ids wrap at 65535 and the comment beside them said the
	 * allocation was therefore bounded. It bounded the COUNT. Each entry
	 * is a URI straight from the remote, `links` is never cleared, and
	 * the only thing limiting one of them was the OSC payload cap -- so
	 * 65535 times that was the real bound, which is not one.
	 */
	bssh_emulator emulator(40, 5);

	const QByteArray huge(200 * 1024, 'a');
	emulator.feed(QByteArrayLiteral("\x1b]8;;https://example.com/") + huge +
	               QByteArrayLiteral("\x07") + QByteArrayLiteral("text") +
	               QByteArrayLiteral("\x1b]8;;\x07"));

	/*
	 * No link at all, rather than a shortened one. Half a URI is a
	 * DIFFERENT address and this one is offered to a browser, so the
	 * refusal has to be total -- the same call the clipboard cap makes,
	 * and the opposite of the title's, which truncates because nothing
	 * executes a label.
	 */
	QCOMPARE(int(emulator.screen().cell(0, 0).link), 0);

	/*
	 * And a link that was already open is CLOSED by the refusal, not left
	 * standing. Returning with it open would attribute the text after it
	 * to the PREVIOUS address -- a refusal turned into a wrong
	 * destination, which is the outcome all of this exists to avoid.
	 */
	bssh_emulator second(40, 5);
	second.feed(QByteArrayLiteral("\x1b]8;;https://first.example/\x07"));
	second.feed(QByteArrayLiteral("\x1b]8;;https://second.example/") + huge +
	             QByteArrayLiteral("\x07"));
	second.feed(QByteArrayLiteral("after"));

	QCOMPARE(int(second.screen().cell(0, 0).link), 0);

	/* An ordinary address is untouched by any of it. */
	bssh_emulator ordinary(40, 5);
	ordinary.feed(QByteArrayLiteral("\x1b]8;;https://example.com/x\x07"
	                                 "ok\x1b]8;;\x07"));
	const quint16 id = ordinary.screen().cell(0, 0).link;
	QVERIFY(id != 0);
	QCOMPARE(ordinary.link_uri(id), QStringLiteral("https://example.com/x"));

	/*
	 * The other half: what is SHOWN when the user is asked whether to
	 * open one. Put in verbatim, a 2083-character address -- which the
	 * convention asks emulators to support -- makes a dialog 14,211
	 * pixels wide on an 800-pixel screen, so the box that exists for
	 * reading an address stops being able to show one.
	 */
	const QString host = QStringLiteral("https://good.example/");
	const QString tail = QStringLiteral("/final-name.tar.gz");
	const QString long_uri = host + QString(2000, QLatin1Char('a')) + tail;

	const QString shown = bssh_uri_for_prompt(long_uri, BSSH_URI_PROMPT_CHARS);

	QCOMPARE(shown.size(), BSSH_URI_PROMPT_CHARS);

	/*
	 * Elided in the MIDDLE: the scheme and host say who is being talked
	 * to and the tail says what is being fetched. Eliding at the end
	 * would hide the tail; eliding at the start would hide the HOST,
	 * which is the most important thing on the screen at that moment.
	 */
	QVERIFY2(shown.startsWith(host), qPrintable(shown));
	QVERIFY2(shown.endsWith(tail), qPrintable(shown));

	/* An address short enough to read is passed through untouched. */
	const QString plain = QStringLiteral("https://example.com/a/b");
	QCOMPARE(bssh_uri_for_prompt(plain, BSSH_URI_PROMPT_CHARS), plain);
}

void emulator_test::osc8_survives_a_wrap_and_ends_where_told() {
	/* Narrow enough that the link text wraps to the next row. */
	bssh_emulator emulator(10, 5);

	emulator.feed(QByteArrayLiteral("\x1b]8;;https://example.com\x07"
	                                 "aaaaaaaaaabbbb\x1b]8;;\x07"));

	const quint16 id = emulator.screen().cell(0, 0).link;
	QVERIFY(id != 0);

	/* The wrapped remainder belongs to the same link, not to nothing. */
	QCOMPARE(emulator.screen().cell(1, 0).link, id);
	QCOMPARE(emulator.screen().cell(1, 3).link, id);

	/* But not the cells past where it ended. */
	QCOMPARE(int(emulator.screen().cell(1, 5).link), 0);
	QCOMPARE(emulator.link_uri(id), QStringLiteral("https://example.com"));

	/* An unknown id resolves to nothing rather than to a stale URL. */
	QCOMPARE(emulator.link_uri(9999), QString());
}

void emulator_test::the_remote_may_ask_for_the_clipboard_but_never_read_it() {
	bssh_emulator emulator(80, 24);
	QSignalSpy asked(&emulator, &bssh_emulator::clipboard_write_requested);

	/* OSC 52 ; c ; base64("hello") ST */
	emulator.feed(QByteArrayLiteral("\x1b]52;c;aGVsbG8=\x1b\\"));

	QCOMPARE(asked.count(), 1);
	QCOMPARE(asked.at(0).at(0).toString(), QStringLiteral("hello"));

	/*
	 * A REQUEST, not an instruction: the emulator reports what was asked
	 * for and the window decides, because whether this host is trusted is
	 * a setting the emulator does not have (sec 7.1).
	 */

	/*
	 * The read query sends NOTHING back. "OSC 52 ; c ; ?" asks the
	 * terminal to transmit the local clipboard to the remote, which turns
	 * every connection into an exfiltration channel for whatever the user
	 * last copied -- a password, a token, someone else's message. No
	 * setting makes that acceptable: the risk is not the user's to accept
	 * on behalf of something they copied ten minutes ago.
	 */
	emulator.take_output();
	emulator.feed(QByteArrayLiteral("\x1b]52;c;?\x1b\\"));

	QCOMPARE(asked.count(), 1);
	QVERIFY2(emulator.take_output().isEmpty(),
	         "a clipboard read query must send nothing back");

	/*
	 * A payload too large to be a yank is dropped rather than decoded:
	 * the bytes are attacker-controlled, and without a cap a host can
	 * spend the terminal's memory for the price of writing to a socket.
	 */
	QByteArray huge = QByteArrayLiteral("\x1b]52;c;");
	huge += QByteArray(200 * 1024, 'A');       /* ~150 KB once decoded */
	huge += QByteArrayLiteral("\x1b\\");
	emulator.feed(huge);

	/*
	 * Dropped whole, not truncated. libvterm decodes into a fixed buffer
	 * and hands it over in pieces as that buffer fills, so an oversized
	 * payload arrives as several fragments rather than being refused --
	 * and half a clipboard is not a smaller version of what was asked
	 * for, it is a different string that someone might paste somewhere.
	 */
	QCOMPARE(asked.count(), 1);
}

void emulator_test::scrolling_moves_the_grid_without_losing_anything() {
	/*
	 * A scroll used to be handled by re-reading the whole destination out
	 * of libvterm, which was correct and cost a full-screen copy per
	 * scrolled line: plain scrolling output ran at 0.2 MB/s while a
	 * full-screen TUI repaint managed 21. The mirror moves its own rows
	 * now, so this checks the move keeps what the copy kept.
	 */
	bssh_emulator emulator(20, 4);

	for (int i = 1; i <= 6; ++i) {
		emulator.feed(QByteArrayLiteral("line ") + QByteArray::number(i) +
		               QByteArrayLiteral("\r\n"));
	}

	/* Six lines through a four-row screen: the last four survive, in order. */
	const QStringList rows = emulator.screen().to_text().split(QLatin1Char('\n'));
	QCOMPARE(rows.value(0).trimmed(), QStringLiteral("line 4"));
	QCOMPARE(rows.value(1).trimmed(), QStringLiteral("line 5"));
	QCOMPARE(rows.value(2).trimmed(), QStringLiteral("line 6"));

	/* And the ones that fell off are in the scrollback, in order. */
	QVERIFY(emulator.scrollback().count() >= 3);

	/*
	 * A double-width glyph scrolled up the screen. The old path advanced
	 * cell by cell precisely so a continuation cell would not be
	 * overwritten with a blank (sec 9.3); a row move has to preserve the
	 * same markers, or a CJK character survives a scroll as a stray half.
	 */
	bssh_emulator wide(10, 4);
	wide.feed(QByteArrayLiteral("a\r\n"));
	wide.feed(QByteArrayLiteral("\xe4\xb8\xad\xe6\x96\x87\r\n"));   /* U+4E2D U+6587 */
	wide.feed(QByteArrayLiteral("b\r\n"));
	wide.feed(QByteArrayLiteral("c\r\n"));   /* one scroll: the CJK row MOVES up */

	const bssh_cell_t first = wide.screen().cell(0, 0);
	const bssh_cell_t second = wide.screen().cell(0, 1);

	QCOMPARE(first.ch, U'中');
	QCOMPARE(int(first.width), 2);
	QVERIFY2(second.width == 0, "the continuation cell must survive a scroll");
}

void emulator_test::a_write_is_on_the_screen_when_it_returns() {
	/*
	 * Damage is merged per row rather than per cell, which means libvterm
	 * holds a row until something asks for it -- so feed() flushes, and
	 * without that the tail of every write would sit invisible until the
	 * next one arrived. On a shell prompt, which is a write with no
	 * trailing newline and then silence, "the next one" is whenever the
	 * user next types.
	 */
	bssh_emulator emulator(20, 4);
	emulator.feed(QByteArrayLiteral("$ "));

	QCOMPARE(emulator.screen().to_text().split(QLatin1Char('\n')).value(0), QStringLiteral("$"));

	/*
	 * And the same across a scroll, which is where the merging is not
	 * merely deferred but ordered: a row printed just before a scroll is
	 * still owed when the scroll arrives, so it has to be delivered while
	 * it still describes the grid. Reading it back afterwards yields the
	 * row that scrolled INTO those coordinates -- the text is not lost,
	 * it is replaced by its neighbour, which no blank-screen check finds.
	 */
	bssh_emulator scrolled(20, 3);
	scrolled.feed(QByteArrayLiteral("alpha\r\nbravo\r\ncharlie\r\ndelta"));

	const QStringList rows = scrolled.screen().to_text().split(QLatin1Char('\n'));
	QCOMPARE(rows.value(0), QStringLiteral("bravo"));
	QCOMPARE(rows.value(1), QStringLiteral("charlie"));
	QCOMPARE(rows.value(2), QStringLiteral("delta"));
}

void emulator_test::damage_is_reported_per_row_not_per_cell() {
	/*
	 * libvterm's default reports one damage rectangle per printed CELL, so
	 * ordinary output reached the mirror a character at a time: 20,000
	 * lines of build output arrived as 1,188,841 callbacks where 20,001
	 * would do, each one paying for a copy and a repaint request to carry
	 * a single character (sec 9.5).
	 *
	 * This asserts on a COUNT rather than on a clock, which is the whole
	 * reason it can live in the suite: it is the same number on a fast
	 * machine, a loaded CI runner and a phone, so it cannot flake. The
	 * throughput this protects is measured by `make bench` (sec 12.3),
	 * which for the opposite reason cannot be a gate at all.
	 */
	const int lines = 500;
	const int width = 60;

	bssh_emulator emulator(80, 24);
	QSignalSpy damaged(&emulator, &bssh_emulator::damaged);

	QByteArray stream;
	for (int i = 0; i < lines; ++i) {
		stream += QByteArray(width, 'x') + QByteArrayLiteral("\r\n");
	}
	emulator.feed(stream);

	/*
	 * Measured: 2001 merged, 31977 on libvterm's default -- a factor of
	 * 16, so the ceiling sits between them rather than on either. Pinning
	 * the exact number would turn a libvterm update into a failing test
	 * for no reason.
	 *
	 * Four per line rather than one because this counts repaint requests,
	 * not damage rectangles, and a row's worth of text also moves the
	 * cursor two or three times. Worth knowing before reading the number:
	 * once damage is merged, what remains is mostly cursor movement.
	 */
	QVERIFY2(damaged.count() <= lines * 8,
	          qPrintable(QStringLiteral("%1 repaint requests for %2 lines -- damage merging "
	                                     "is off, or something below it regressed")
	                         .arg(damaged.count()).arg(lines)));

	/* And it did not get "efficient" by not reporting the output at all. */
	/*
	 * One announcement for the whole read since sec 9.5a: damage and
	 * scrolls are gathered and said once at the end of feed(), so five
	 * hundred lines in one read are one repaint. The lower bound this
	 * used to set -- a request per four lines -- guarded against damage
	 * being lost; that it arrives at all is what is asserted now.
	 */
	QCOMPARE(damaged.count(), 1);
	QCOMPARE(row(emulator, 22), QString(width, QLatin1Char('x')));
}

void emulator_test::scrolling_output_is_one_scroll_and_the_rows_it_exposed() {
	/*
	 * sec 9.5a: lines scrolling up are announced as one scroll of the rows
	 * that moved, and only the rows the scroll exposed are damage -- in
	 * the coordinates after it -- so a view moves its pixels and paints
	 * those few rows rather than the whole terminal.
	 */
	bssh_emulator emulator(20, 6);
	for (int line = 0; line < 6; ++line)
		emulator.feed(QByteArray("line ") + QByteArray::number(line) + (line < 5 ? "\r\n" : ""));

	QSignalSpy scrolled(&emulator, &bssh_emulator::rows_scrolled);
	QSignalSpy rows(&emulator, &bssh_emulator::rows_damaged);
	emulator.feed(QByteArrayLiteral("\r\nA\r\nB\r\nC"));

	QCOMPARE(scrolled.count(), 1);
	QCOMPARE(scrolled.first().at(0).toInt(), 0);
	QCOMPARE(scrolled.first().at(1).toInt(), 5);
	QCOMPARE(scrolled.first().at(2).toInt(), -3);
	QCOMPARE(rows.count(), 1);
	QVERIFY2(rows.first().at(0).toInt() >= 2,
	         qPrintable(QStringLiteral("damage from row %1: rows that only moved were repainted")
	                            .arg(rows.first().at(0).toInt())));
	QCOMPARE(rows.first().at(1).toInt(), 5);
	QCOMPARE(row(emulator, 0), QStringLiteral("line 3"));
	QCOMPARE(row(emulator, 5), QStringLiteral("C"));

	/* Further than the screen is tall: nothing left to move, all damage. */
	scrolled.clear();
	rows.clear();
	QByteArray many;
	for (int line = 0; line < 10; ++line) many += "\r\nmore";
	emulator.feed(many);
	QCOMPARE(scrolled.count(), 0);
	QCOMPARE(rows.first().at(0).toInt(), 0);

	/* Inside a synchronised frame: damage, released whole with the frame. */
	scrolled.clear();
	emulator.feed(QByteArrayLiteral("\x1b[?2026h\r\nx\r\ny\x1b[?2026l"));
	QCOMPARE(scrolled.count(), 0);
}

void emulator_test::scrollback_drops_trailing_blanks_but_not_blank_colour() {
	/*
	 * libvterm hands over a full grid row per scrolled line, so a short
	 * line on a wide grid was mostly padding -- 2.82 of 3.99 million
	 * converted cells on a 200-column run (sec 9.5). The tail is dropped
	 * now, and every reader already bounds its column against size().
	 */
	{
		bssh_emulator emulator(40, 2);
		/* The newest line keeps its blanks until the next one shows it
		 * ended (bssh_scrollback::push), so there is a line after it. */
		emulator.feed(QByteArrayLiteral("short\r\na\r\nb\r\nc"));

		QVERIFY(emulator.scrollback().count() >= 1);
		QCOMPARE(emulator.scrollback().line(0).size(), 5);
	}

	/*
	 * But "blank" is about what DRAWS, not about which character is in
	 * the cell. A space carrying a background colour is how half the TUIs
	 * in existence draw a bar across the screen, and trimming it would
	 * turn a status line into nothing on the way into history.
	 */
	{
		bssh_emulator emulator(10, 2);
		emulator.feed(QByteArrayLiteral("\x1b[44m  \x1b[0m\r\na\r\nb\r\nc"));

		const QList<bssh_cell_t> line = emulator.scrollback().line(0);
		QCOMPARE(line.size(), 2);
		QCOMPARE(line.at(1).ch, U' ');
		QCOMPARE(line.at(1).bg.kind, bssh_color_t::INDEXED);
		QCOMPARE(int(line.at(1).bg.index), 4);
	}

	/*
	 * And a double-width glyph in the last columns keeps both of its
	 * cells. libvterm reports the continuation half as a character of its
	 * own rather than as a blank, which is what stops the trim there --
	 * if that ever changed, a CJK character at the end of a line would go
	 * into history as a stray half.
	 */
	{
		bssh_emulator emulator(4, 2);
		emulator.feed(QByteArrayLiteral("ab\xe4\xb8\xad\r\nx\r\ny\r\nz"));   /* ab U+4E2D */

		const QList<bssh_cell_t> line = emulator.scrollback().line(0);
		QCOMPARE(line.size(), 4);
		QCOMPARE(line.at(2).ch, U'中');
	}
}

void emulator_test::a_wide_glyph_scrolls_back_the_way_it_was_on_screen() {
	/*
	 * The screen and the scrollback are drawn by the same painter, so
	 * they have to spell a wide glyph the same way. They did not: the
	 * screen synthesised the far half as sec 9.3 asks -- no character,
	 * width 0, which the renderer skips -- while the scrollback converted
	 * whatever libvterm had left in that column, which is its own marker,
	 * chars[0] == (uint32_t)-1.
	 *
	 * That is not a character. It has width 1, so the renderer did not
	 * skip it; it is not blank, so it was not treated as empty; and it is
	 * not valid Unicode, so the font catalog was asked which family
	 * covers it and the answer was drawn -- a box over the right half of
	 * every CJK character in the history.
	 */
	const QByteArray text = QByteArrayLiteral("a\xe4\xb8\xad""b");   /* a U+4E2D b */

	bssh_emulator live(6, 2);
	live.feed(text);

	bssh_emulator scrolled(6, 2);
	scrolled.feed(text + QByteArrayLiteral("\r\nx\r\ny\r\nz"));
	const QList<bssh_cell_t> stored = scrolled.scrollback().line(0);

	QCOMPARE(stored.size(), 4);
	for (int col = 0; col < 4; ++col) {
		const bssh_cell_t &on_screen = live.screen().cell(0, col);
		QCOMPARE(stored.at(col).ch, on_screen.ch);
		QCOMPARE(int(stored.at(col).width), int(on_screen.width));
	}

	/* Named explicitly, so the contract is readable without the loop. */
	QCOMPARE(stored.at(1).ch, U'中');
	QCOMPARE(int(stored.at(1).width), 2);
	QCOMPARE(stored.at(2).ch, U'\0');
	QVERIFY2(stored.at(2).width == 0, "the far half must be skippable, not drawable");
}

void emulator_test::a_line_written_then_immediately_scrolled_lands_intact() {
	bssh_emulator emulator(20, 3);

	/* Something to be the stale previous occupant of row 0. */
	emulator.feed(QByteArrayLiteral("STALE-ONE\r\nSTALE-TWO\r\nSTALE-THREE"));

	/*
	 * Home, overwrite row 0, and force a scroll immediately -- without
	 * ever writing to another row, which is what would flush row 0's
	 * damage on the way past.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[H\x1b[2KFRESH-ROW-ZERO"));
	emulator.feed(QByteArrayLiteral("\x1b[3;1H\r\n"));

	const bssh_scrollback &scrollback = emulator.scrollback();
	QVERIFY2(scrollback.count() > 0, "the scroll must have filed something");

	QString filed;
	for (const bssh_cell_t &cell : scrollback.line(scrollback.count() - 1)) {
		filed.append(cell.ch ? QString::fromUcs4(&cell.ch, 1) : QStringLiteral(" "));
	}

	QVERIFY2(filed.startsWith(QStringLiteral("FRESH-ROW-ZERO")),
	          qPrintable(QStringLiteral("the scrollback filed \"%1\" instead of the line that "
	                                     "was actually on row 0").arg(filed)));
}

void emulator_test::decrqm_answers_for_modes_it_knows() {
	bssh_emulator emulator(20, 4);

	/*
	 * 2026 is the mode this terminal implements and libvterm does not
	 * model, and it is the one clients actually probe. "2" means
	 * understood and currently off, which is how a client learns
	 * synchronised output is available before it uses it.
	 *
	 * This is the whole reason vterm-patch/0004 exists: without it
	 * libvterm answers the query itself with "0 -- not recognised",
	 * telling every client the terminal lacks a feature it has.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[?2026$p"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?2026;2$y"));

	/* And "1" once it is on. */
	emulator.feed(QByteArrayLiteral("\x1b[?2026h\x1b[?2026$p"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?2026;1$y"));
}

void emulator_test::decrqm_says_nothing_it_cannot_stand_behind() {
	bssh_emulator emulator(20, 4);

	/*
	 * Everything libvterm models is libvterm's to answer, and it does so
	 * from the state it is actually keeping -- a second opinion here
	 * would be two places computing one thing. These assert that the
	 * answers come out right, not which layer produced them.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[?2004$p"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?2004;2$y"));

	emulator.feed(QByteArrayLiteral("\x1b[?2004h\x1b[?2004$p"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?2004;1$y"));

	emulator.feed(QByteArrayLiteral("\x1b[?25$p"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?25;1$y"));

	/*
	 * Mouse reporting answers for the mode that is actually set rather
	 * than for "some mouse mode is on": a terminal that says 1000 while
	 * 1003 is running has told the remote something worse than nothing.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[?1003h"));
	emulator.feed(QByteArrayLiteral("\x1b[?1003$p"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?1003;1$y"));
	emulator.feed(QByteArrayLiteral("\x1b[?1000$p"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?1000;2$y"));

	/*
	 * And a mode nobody here models is answered "0 -- not recognised",
	 * which is the useful answer: a client told that knows to stop
	 * asking, where one told "reset" would set the mode and then wonder
	 * why nothing happened.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[?7727$p"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?7727;0$y"));
}

void emulator_test::xtgettcap_reports_what_this_terminal_is() {
	bssh_emulator emulator(20, 4);
	emulator.set_term_name(QStringLiteral("xterm-256color"));

	/*
	 * "TN" is 544E in hex, and the answer carries the name back hex-coded
	 * too. It must be the TERM the app actually told the remote: naming
	 * itself one thing in the environment and another when asked is worse
	 * than not answering.
	 */
	emulator.feed(QByteArrayLiteral("\x1bP+q544E\x1b\\"));
	const QByteArray tn = emulator.take_output();
	QVERIFY2(tn.startsWith(QByteArrayLiteral("\x1bP1+r544E=")), tn.constData());
	QVERIFY2(tn.contains(QByteArray("xterm-256color").toHex().toUpper()), tn.constData());

	/* Truecolor, which is true of this terminal wherever it runs. */
	emulator.feed(QByteArrayLiteral("\x1bP+q524742\x1b\\"));   /* RGB */
	QVERIFY2(emulator.take_output().startsWith(QByteArrayLiteral("\x1bP1+r524742=")),
	          "RGB must be answered");

	/*
	 * And a capability this terminal will not vouch for comes back as
	 * invalid rather than as a guess. "bce" is the trap: most terminals
	 * like this one have it, and a client told so will use it.
	 */
	emulator.feed(QByteArrayLiteral("\x1bP+q626365\x1b\\"));   /* bce */
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1bP0+r626365\x1b\\"));
}

void emulator_test::scrolling_past_a_full_screen_keeps_the_rows_in_order() {
	const int rows = 4;
	bssh_emulator emulator(20, rows);

	/*
	 * Fifty lines through a four-row screen: twelve full turns of the
	 * ring rather than the two or three the other scroll tests do. An
	 * offset that is right for the first lap and wrong after it -- the
	 * classic ring mistake -- survives a short test and dies here.
	 */
	for (int i = 0; i < 50; ++i) {
		emulator.feed(QByteArrayLiteral("line") + QByteArray::number(i)
		               + QByteArrayLiteral("\r\n"));
	}

	const QStringList visible = emulator.screen().to_text().split(QLatin1Char('\n'));

	/*
	 * Three lines and a blank, not four lines. Every line here ends in a
	 * newline, including the last, so the final one scrolls the screen
	 * once more and leaves the cursor on a fresh blank row -- which is
	 * what a shell prompt does too.
	 *
	 * Checked against the pre-ring implementation rather than reasoned
	 * about: with the rotation disabled and the old row-by-row memmove
	 * doing the work, the screen reads identically. That is what says
	 * this expectation describes the terminal rather than the ring.
	 */
	QCOMPARE(visible.value(0), QStringLiteral("line47"));
	QCOMPARE(visible.value(1), QStringLiteral("line48"));
	QCOMPARE(visible.value(2), QStringLiteral("line49"));
	QCOMPARE(visible.value(3), QString());
}

void emulator_test::scrolling_backwards_keeps_the_rows_in_order() {
	bssh_emulator emulator(20, 4);

	emulator.feed(QByteArrayLiteral("one\r\ntwo\r\nthree\r\nfour"));

	/*
	 * Reverse index at the top of the screen scrolls the other way, which
	 * is the branch of the rotation that subtracts rather than adds. It
	 * is the half a forward-only test never reaches, and getting its
	 * modulo wrong puts the rows in a plausible but wrong order rather
	 * than crashing.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[H\x1bM"));

	const QStringList visible = emulator.screen().to_text().split(QLatin1Char('\n'));
	QCOMPARE(visible.value(0), QString());
	QCOMPARE(visible.value(1), QStringLiteral("one"));
	QCOMPARE(visible.value(2), QStringLiteral("two"));
	QCOMPARE(visible.value(3), QStringLiteral("three"));
}

void emulator_test::a_margin_scroll_leaves_the_rows_outside_it_alone() {
	bssh_emulator emulator(20, 5);

	emulator.feed(QByteArrayLiteral("top\r\nA\r\nB\r\nC\r\nbottom"));

	/*
	 * A scroll region covering rows 2-4 only (DECSTBM is 1-based), then a
	 * scroll inside it. Rotating the whole ring would be wrong here: the
	 * rows outside the margins must not move, so this has to fall back to
	 * moving rows one at a time.
	 *
	 * "top" and "bottom" staying put is the whole assertion.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[2;4r"));
	emulator.feed(QByteArrayLiteral("\x1b[4;1H\r\n"));

	const QStringList visible = emulator.screen().to_text().split(QLatin1Char('\n'));
	QCOMPARE(visible.value(0), QStringLiteral("top"));
	QCOMPARE(visible.value(1), QStringLiteral("B"));
	QCOMPARE(visible.value(2), QStringLiteral("C"));
	QCOMPARE(visible.value(3), QString());
	QCOMPARE(visible.value(4), QStringLiteral("bottom"));
}

void emulator_test::focus_is_reported_only_when_the_remote_asked() {
	/*
	 * sec 7.1 tier 1's focus reporting, DEC mode 1004. libvterm parsed
	 * the mode from the start and emits the reports; nothing ever told it
	 * the focus had moved, so a remote could enable the mode and never
	 * hear anything -- indistinguishable, from the far end, from a
	 * terminal that does not support it at all.
	 *
	 * What wants it: an editor reloading a file changed underneath it.
	 * vim's autoread does nothing useful without this, because it only
	 * checks when something tells it to look.
	 */
	bssh_emulator emulator(80, 24);

	/*
	 * Silent until asked. A terminal that volunteers CSI I to a shell
	 * that never enabled 1004 is writing stray input into that shell --
	 * the bytes land in the command line.
	 */
	emulator.set_focused(true);
	emulator.set_focused(false);
	QCOMPARE(emulator.take_output(), QByteArray());

	/* The remote turns it on. */
	emulator.feed(QByteArrayLiteral("\x1b[?1004h"));
	emulator.take_output();

	emulator.set_focused(true);
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[I"));

	emulator.set_focused(false);
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[O"));

	/* And off again means silent again, which is the half people forget. */
	emulator.feed(QByteArrayLiteral("\x1b[?1004l"));
	emulator.take_output();

	emulator.set_focused(true);
	emulator.set_focused(false);
	QCOMPARE(emulator.take_output(), QByteArray());
}

void emulator_test::a_feature_spec_reads_left_to_right() {
	/*
	 * The spec is applied word by word rather than collected into a set,
	 * because that is the only order in which "none,+x" and "all,-x"
	 * both mean what they look like. A set-based reader has to invent a
	 * precedence rule, and whichever one it picks surprises somebody.
	 */
	QString error;

	const bssh_term_features_t all = bssh_term_features_t::from_spec(QString(), error);
	QVERIFY(error.isEmpty());
	for (const QString &name : bssh_term_features_t::every_name()) {
		bssh_term_features_t::feature_t feature = bssh_term_features_t::SIXEL;
		QVERIFY(bssh_term_features_t::from_name(name, feature));
		QVERIFY2(all.enabled(feature),
		          qPrintable(QStringLiteral("%1 must be on by default").arg(name)));
	}

	const bssh_term_features_t none =
	        bssh_term_features_t::from_spec(QStringLiteral("none"), error);
	QVERIFY(error.isEmpty());
	QVERIFY(!none.enabled(bssh_term_features_t::SIXEL));
	QVERIFY(!none.enabled(bssh_term_features_t::KITTY_KEYBOARD));

	const bssh_term_features_t only = bssh_term_features_t::from_spec(
	        QStringLiteral("none,+bracketed-paste"), error);
	QVERIFY(error.isEmpty());
	QVERIFY(only.enabled(bssh_term_features_t::BRACKETED_PASTE));
	QVERIFY(!only.enabled(bssh_term_features_t::SIXEL));

	const bssh_term_features_t most =
	        bssh_term_features_t::from_spec(QStringLiteral("-sixel,-kitty-graphics"), error);
	QVERIFY(error.isEmpty());
	QVERIFY(!most.enabled(bssh_term_features_t::SIXEL));
	QVERIFY(!most.enabled(bssh_term_features_t::KITTY_GRAPHICS));
	QVERIFY2(most.enabled(bssh_term_features_t::ITERM2_IMAGES),
	          "turning two off must not turn a third off");

	/*
	 * Order matters, and this is the case that proves it is read left to
	 * right rather than gathered: the same two words in the other order
	 * mean opposite things.
	 */
	const bssh_term_features_t last_wins =
	        bssh_term_features_t::from_spec(QStringLiteral("-sixel,all"), error);
	QVERIFY(error.isEmpty());
	QVERIFY(last_wins.enabled(bssh_term_features_t::SIXEL));

	/* And the round trip, which is what a stored preset writes back. */
	QCOMPARE(most.to_spec(), QStringLiteral("-sixel,-kitty-graphics"));
	QCOMPARE(all.to_spec(), QString());
	QCOMPARE(bssh_term_features_t::from_spec(most.to_spec(), error), most);
	QVERIFY(error.isEmpty());
}

void emulator_test::an_unknown_feature_name_is_refused() {
	/*
	 * Named rather than ignored, and this is the assertion that earns
	 * the type its error argument at all.
	 *
	 * A harness that misspells a feature -- "sixels", "kitty_keyboard" --
	 * would otherwise run its entire suite against a terminal that still
	 * had that feature switched on, and report a pass for a
	 * configuration nobody ever tested. That is worse than a crash: it
	 * is a green result about the wrong thing, which is the failure this
	 * project keeps paying for elsewhere.
	 */
	QString error;
	const bssh_term_features_t refused =
	        bssh_term_features_t::from_spec(QStringLiteral("-sixels"), error);

	QVERIFY2(!error.isEmpty(), "a name that is not a feature must be refused");
	QVERIFY2(error.contains(QStringLiteral("sixels")), "the message must name what was wrong");
	QVERIFY2(error.contains(QStringLiteral("sixel")),
	          "and list what would have been accepted");

	/*
	 * And it comes back with everything ON rather than half-applied. A
	 * refused spec must not leave the terminal in the state the first
	 * two words of it asked for.
	 */
	QCOMPARE(refused, bssh_term_features_t());
}

void emulator_test::a_switched_off_feature_is_not_advertised_either() {
	/*
	 * The rule the whole switch exists for, and the half a behaviour
	 * test cannot see: turning an extension off has to change what the
	 * terminal ANSWERS about it, not only what it does.
	 *
	 * sec 7.1 has already paid for both polarities of getting this
	 * wrong. DEC 2026 answered "0 -- not recognised" while the
	 * implementation existed, so clients fell back from a feature that
	 * was there; and it records the opposite for XTGETTCAP -- "a
	 * capability database is a promise: a client told bce will use it".
	 * A feature switched off but still advertised is that promise
	 * broken, and it is the more expensive direction, because the client
	 * goes ahead and nothing happens.
	 *
	 * Every case below asserts the PAIR: the extension does nothing, and
	 * the probe for it says so.
	 */
	QString error;
	const bssh_term_features_t none =
	        bssh_term_features_t::from_spec(QStringLiteral("none"), error);
	QVERIFY(error.isEmpty());

	/*
	 * Synchronised output is the sharpest of the four, because it has a
	 * real machine-readable "no": DECRPM 0 means "not recognised", which
	 * is the truth for a terminal that does not have it. With the feature
	 * on this same query answers 2.
	 */
	{
		bssh_emulator emulator(20, 4);
		emulator.set_features(none);

		emulator.feed(QByteArrayLiteral("\x1b[?2026$p"));
		QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?2026;0$y"));

		emulator.feed(QByteArrayLiteral("\x1b[?2026h"));
		QVERIFY2(!emulator.synchronised_output(),
		          "and the mode itself must not engage");
	}

	/*
	 * Kitty's keyboard protocol answers nothing at all, which is what a
	 * terminal without it does -- kitty's own probe is "ask, and if no
	 * answer arrives before the DA1 you sent after it, there is none".
	 * Answering the query with zero flags would say something different
	 * and wrong: that the protocol is spoken and sitting at its lowest
	 * tier.
	 */
	{
		bssh_emulator emulator(20, 4);
		emulator.set_features(none);

		emulator.feed(QByteArrayLiteral("\x1b[>1u\x1b[?u"));
		QCOMPARE(emulator.take_output(), QByteArray());
	}

	/* Same shape for XTQMODKEYS. */
	{
		bssh_emulator emulator(20, 4);
		emulator.set_features(none);

		emulator.feed(QByteArrayLiteral("\x1b[>4;2m\x1b[?4m"));
		QCOMPARE(emulator.take_output(), QByteArray());
	}

	/*
	 * XTGETTCAP goes silent rather than answering the invalid form for
	 * every name. The two say different things: DCS 0 + r means "I speak
	 * XTGETTCAP and do not have that one", which would still advertise
	 * the protocol.
	 *
	 * And the query must still be CONSUMED. Both XTGETTCAP and sixel are
	 * DCS sequences ending in q, and sec 7.1 records what happens when
	 * one reaches the other's parser -- so the assertion is that nothing
	 * comes back AND nothing lands on the screen.
	 */
	{
		bssh_emulator emulator(20, 4);
		emulator.set_features(none);
		emulator.set_term_name(QStringLiteral("xterm-256color"));

		emulator.feed(QByteArrayLiteral("\x1bP+q544E\x1b\\"));
		QCOMPARE(emulator.take_output(), QByteArray());
		QCOMPARE(emulator.screen().to_text().trimmed(), QString());
	}

	/*
	 * And with everything ON, the same four probes answer -- which is
	 * what stops this test passing against an emulator that had simply
	 * stopped working. Without it, deleting the feature outright would
	 * be indistinguishable from switching it off.
	 */
	{
		bssh_emulator emulator(20, 4);

		emulator.feed(QByteArrayLiteral("\x1b[?2026$p"));
		QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?2026;2$y"));

		emulator.feed(QByteArrayLiteral("\x1b[>1u\x1b[?u"));
		QVERIFY2(!emulator.take_output().isEmpty(), "kitty must answer when it is on");

		emulator.feed(QByteArrayLiteral("\x1b[>4;2m\x1b[?4m"));
		QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[>4;2m"));

		emulator.set_term_name(QStringLiteral("xterm-256color"));
		emulator.feed(QByteArrayLiteral("\x1bP+q544E\x1b\\"));
		QVERIFY2(!emulator.take_output().isEmpty(), "XTGETTCAP must answer when it is on");
	}
}

void emulator_test::the_kitty_graphics_query_is_answered() {
	/*
	 * kitty's "can you hear me" -- a=q with a one-pixel image, asking
	 * only whether anything is listening. It is the ONLY way a client
	 * can learn this protocol is available: there is no terminfo
	 * capability for kitty graphics and no DA parameter.
	 *
	 * It was unanswered. The query fell through to the transmit path,
	 * which decoded the four bytes into a 1x1 image and placed it, so a
	 * client that probes before using the protocol concluded the
	 * terminal had none while transmission worked perfectly.
	 *
	 * The exact bytes are the sibling qtty TUI's own probe, copied from
	 * its `src/backend/ansi/term_caps.cpp` rather than composed here --
	 * a probe written from this side would be testing what we think a
	 * client sends, which is the assumption that produced the gap.
	 */
	bssh_emulator emulator(20, 4);

	emulator.feed(QByteArrayLiteral("\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\"));

	const QByteArray answer = emulator.take_output();
	QVERIFY2(!answer.isEmpty(), "the query must be answered, or the protocol is invisible");

	/*
	 * The id is echoed so a client with more than one question in flight
	 * can match the answer, exactly as DECRPM echoes its mode. Asserted
	 * because an answer that dropped it would still look like a reply
	 * and would still be useless to a client that sent two.
	 */
	QCOMPARE(answer, QByteArrayLiteral("\x1b_Gi=31;OK\x1b\\"));

	/*
	 * And it stored NOTHING. A query that also places an image is worse
	 * than one that is ignored: the probe would leave a stray pixel on
	 * screen at whatever the cursor was.
	 */
	QCOMPARE(emulator.images().placements().size(), 0);
}

void emulator_test::decrqm_answers_for_the_alternate_screen() {
	/*
	 * libvterm implements 1049 and does not answer DECRQM for it: its
	 * table knows 1047 and stops there, so `CSI ? 1049 $p` came back
	 * "0 -- not recognised" while `CSI ? 1049 h` worked perfectly.
	 *
	 * The same defect sec 7.1 records for DEC 2026, and it costs more
	 * here: 1049 is what a full-screen program uses to avoid destroying
	 * the user's scrollback, so a client told the terminal lacks it
	 * either gives up the alternate screen or falls back to clearing the
	 * primary one.
	 */
	bssh_emulator emulator(20, 4);

	emulator.feed(QByteArrayLiteral("\x1b[?1049$p"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?1049;2$y"));

	/* And "1" once it is on, which is the half that proves the answer is
	 * read from the state rather than hardcoded. */
	emulator.feed(QByteArrayLiteral("\x1b[?1049h"));
	QVERIFY(emulator.alternate_screen());

	emulator.feed(QByteArrayLiteral("\x1b[?1049$p"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?1049;1$y"));

	emulator.feed(QByteArrayLiteral("\x1b[?1049l"));
	emulator.feed(QByteArrayLiteral("\x1b[?1049$p"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?1049;2$y"));
}

void emulator_test::the_terminal_answers_for_itself_not_for_libvterm() {
	/*
	 * Two questions about WHICH TERMINAL this is, both of which libvterm
	 * answered for itself -- reasonably, since it has no way to know what
	 * is built on top of it, and wrongly for every embedder. Patch 0006
	 * hands both to the fallback first.
	 *
	 * DA1 is the conventional place to advertise sixel, as parameter 4.
	 * libvterm answers "?1;2c" and cannot say more, so a client probing
	 * for sixel the standard way was told there was none while this
	 * terminal decodes it. qtty reads DA1 for exactly that parameter, so
	 * it was compositing in half-blocks against a terminal that can draw
	 * pixels.
	 */
	bssh_emulator emulator(20, 4);

	emulator.feed(QByteArrayLiteral("\x1b[c"));

#ifdef BSSH_NO_SIXEL
	/*
	 * Built without a decoder, which is the Android build. Claiming 4
	 * there would be this same defect pointed the other way, so the
	 * answer falls back to libvterm's.
	 */
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?1;2c"));
#else
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?1;2;4c"));

	/*
	 * And it stops claiming sixel when sixel is switched off. This is
	 * the pair sec 7.1 insists on -- a switch that silences the
	 * behaviour and leaves the advertisement is the expensive direction,
	 * and DA1 is the one advertisement that is a positive claim rather
	 * than an answer to a direct question.
	 */
	QString error;
	bssh_emulator quiet(20, 4);
	quiet.set_features(bssh_term_features_t::from_spec(QStringLiteral("-sixel"), error));
	QVERIFY(error.isEmpty());

	quiet.feed(QByteArrayLiteral("\x1b[c"));
	QCOMPARE(quiet.take_output(), QByteArrayLiteral("\x1b[?1;2c"));
#endif

	/*
	 * XTVERSION named the library rather than the program the user ran,
	 * which is precisely what XTVERSION exists to prevent. Asserted on
	 * the NAME rather than on the whole string, because the version
	 * moves with every release and a test that pinned it would fail for
	 * the one reason that is not a defect.
	 */
	bssh_emulator named(20, 4);
	named.feed(QByteArrayLiteral("\x1b[>q"));

	const QByteArray version = named.take_output();
	QVERIFY2(version.startsWith(QByteArrayLiteral("\x1bP>|BeerSSH(")), version.constData());
	QVERIFY2(version.endsWith(QByteArrayLiteral(")\x1b\\")), version.constData());
	QVERIFY2(!version.contains(QByteArrayLiteral("libvterm")),
	          "the answer must not name the library this is built on");
}

void emulator_test::the_terminal_answers_the_questions_a_compositing_client_asks() {
	/*
	 * Three questions asked by the sibling qtty TUI, reported from its
	 * end with what each silence costs it. None is required of a
	 * terminal; all three are cheap and all three are true of this one,
	 * which is sec 7.1's whole test for whether to answer.
	 */
	bssh_emulator emulator(20, 4);

	/*
	 * OSC 11, ranked first by the asker. Every graphics tier below kitty
	 * composites an image's alpha against the terminal background, and
	 * with no answer it has to assume one -- dark grey, which haloes
	 * every translucent edge on a light theme. A visible artefact caused
	 * entirely by a question going unanswered.
	 *
	 * Each channel is DOUBLED to sixteen bits rather than shifted, which
	 * is the trap in xterm's format: 0xff must come back 0xffff, not
	 * 0xff00, or white is not quite white. The value below is chosen so
	 * the two would differ.
	 */
	emulator.set_colours(qRgb(0xff, 0x80, 0x00), qRgb(0x11, 0x22, 0x33));

	emulator.feed(QByteArrayLiteral("\x1b]11;?\x1b\\"));
	QCOMPARE(emulator.take_output(),
	          QByteArrayLiteral("\x1b]11;rgb:1111/2222/3333\x1b\\"));

	emulator.feed(QByteArrayLiteral("\x1b]10;?\x1b\\"));
	QCOMPARE(emulator.take_output(),
	          QByteArrayLiteral("\x1b]10;rgb:ffff/8080/0000\x1b\\"));

	/*
	 * A payload that SETS a colour is a different thing and is not
	 * honoured: a remote repainting the terminal it was given is not a
	 * power this needs to grant, and the assertion is that it neither
	 * answers nor takes effect.
	 */
	emulator.feed(QByteArrayLiteral("\x1b]11;#ff0000\x1b\\"));
	QCOMPARE(emulator.take_output(), QByteArray());

	emulator.feed(QByteArrayLiteral("\x1b]11;?\x1b\\"));
	QCOMPARE(emulator.take_output(),
	          QByteArrayLiteral("\x1b]11;rgb:1111/2222/3333\x1b\\"));

	/*
	 * CSI 16 t, the cell in pixels. Without it an image's aspect ratio
	 * cannot be preserved: a half-block pixel is one cell wide and half
	 * a cell tall, so assuming 1:2 squashes every picture on a terminal
	 * whose cells are not that shape.
	 *
	 * HEIGHT BEFORE WIDTH, which is xterm's order and the reverse of how
	 * everything else in this file reads. The cell here is deliberately
	 * not square, so a reply with the two the other way round fails.
	 */
	emulator.set_cell_pixels(9, 20);

	emulator.feed(QByteArrayLiteral("\x1b[16t"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[6;20;9t"));

	/* CSI 14 t, the text area: the grid multiplied by the cell. */
	emulator.feed(QByteArrayLiteral("\x1b[14t"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[4;80;180t"));

	/*
	 * And every OTHER window op stays silent, permanently. The rest of
	 * that family moves, resizes, raises and iconifies the window, and a
	 * remote host does not get to rearrange the user's desktop --
	 * reporting a size and acting on one are different powers that
	 * happen to share a final byte. Asserted here because the two above
	 * are the first CSI t this terminal has ever answered, and the
	 * cheapest way to grow that into something worse is a later `t`
	 * handler written beside them.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[1t"));      /* de-iconify */
	emulator.feed(QByteArrayLiteral("\x1b[3;10;10t")); /* move */
	emulator.feed(QByteArrayLiteral("\x1b[8;24;80t")); /* resize the grid */
	emulator.feed(QByteArrayLiteral("\x1b[11t"));     /* report state */
	QCOMPARE(emulator.take_output(), QByteArray());
	QCOMPARE(emulator.screen().rows(), 4);
	QCOMPARE(emulator.screen().cols(), 20);

	/*
	 * Tc, which is the same claim as RGB in the other spelling. Both are
	 * true of this terminal (sec 7); answering one and declining the
	 * other told a client that asked the second way there was no
	 * truecolor. A boolean, so the reply carries the name and no value.
	 */
	emulator.feed(QByteArrayLiteral("\x1bP+q5463\x1b\\"));   /* Tc */
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1bP1+r5463\x1b\\"));

	/* And declining still works, or the change above would have made
	 * every capability true. */
	emulator.feed(QByteArrayLiteral("\x1bP+q626365\x1b\\"));  /* bce */
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1bP0+r626365\x1b\\"));

	/*
	 * Both new answers are switchable, and switching them off silences
	 * them -- the pair sec 7.1 insists on.
	 */
	QString error;
	bssh_emulator quiet(20, 4);
	quiet.set_features(bssh_term_features_t::from_spec(
	        QStringLiteral("-colour-query,-window-size"), error));
	QVERIFY(error.isEmpty());

	quiet.feed(QByteArrayLiteral("\x1b]11;?\x1b\\"));
	quiet.feed(QByteArrayLiteral("\x1b[16t"));
	QCOMPARE(quiet.take_output(), QByteArray());
}

void emulator_test::a_mode_libvterm_models_can_still_be_declined() {
	/*
	 * The three tier-1 extensions -- bracketed paste, the mouse family
	 * and focus reporting -- are modes libvterm models: it parses them,
	 * holds their state and answers DECRQM from it. Declining one was
	 * therefore impossible from above, and suppressing only the
	 * behaviour would have left this terminal reporting the mode as
	 * present while ignoring it.
	 *
	 * Patches 0002 and 0004 now consult the fallback BEFORE libvterm's
	 * own switch, which is the whole of what makes this reachable.
	 *
	 * The distinction asserted here is the one the consumer asked for by
	 * name: "0" is not-recognised and is the only definite no, while "2"
	 * is recognised-but-reset and leaves a client believing the mode is
	 * there and enabling it again. A veto that produced 2 would be no
	 * better than none.
	 */
	QString error;

	{
		bssh_emulator emulator(20, 4);
		emulator.set_features(bssh_term_features_t::from_spec(
		        QStringLiteral("-sgr-mouse"), error));
		QVERIFY(error.isEmpty());

		/*
		 * Enabled first, exactly as a client does before asking -- the
		 * answer must not depend on the terminal never having been
		 * told to turn the mode on.
		 */
		emulator.feed(QByteArrayLiteral("\x1b[?1006h\x1b[?1006$p"));
		QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?1006;0$y"));

		/* The whole family, not just the encoding. */
		emulator.feed(QByteArrayLiteral("\x1b[?1000h\x1b[?1000$p"));
		QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?1000;0$y"));

		/*
		 * And the behaviour is gone with it: the mode never reached
		 * libvterm, so nothing downstream believes the mouse is on.
		 */
		emulator.feed(QByteArrayLiteral("\x1b[?1002h"));
		QVERIFY2(!emulator.mouse_reporting(),
		          "a declined mode must not take effect either");
	}

	{
		bssh_emulator emulator(20, 4);
		emulator.set_features(bssh_term_features_t::from_spec(
		        QStringLiteral("-bracketed-paste"), error));
		QVERIFY(error.isEmpty());

		emulator.feed(QByteArrayLiteral("\x1b[?2004h\x1b[?2004$p"));
		QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?2004;0$y"));
	}

	{
		bssh_emulator emulator(20, 4);
		emulator.set_features(bssh_term_features_t::from_spec(
		        QStringLiteral("-focus-reporting"), error));
		QVERIFY(error.isEmpty());

		emulator.feed(QByteArrayLiteral("\x1b[?1004h\x1b[?1004$p"));
		QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?1004;0$y"));

		/*
		 * And nothing is volunteered on focus. A terminal that writes
		 * CSI I to a shell which never enabled 1004 is putting stray
		 * bytes on that shell's command line -- and here the shell
		 * DID ask, and was declined, which is the case most likely to
		 * be got wrong.
		 */
		emulator.set_focused(true);
		emulator.set_focused(false);
		QCOMPARE(emulator.take_output(), QByteArray());
	}

	/*
	 * With everything on, the same three answer "1" -- enabled. Without
	 * this the whole test would pass against an emulator that had
	 * stopped answering DECRQM at all, which is the vacuous form of
	 * every assertion above.
	 */
	{
		bssh_emulator emulator(20, 4);

		emulator.feed(QByteArrayLiteral("\x1b[?1006h\x1b[?1006$p"));
		QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?1006;1$y"));

		emulator.feed(QByteArrayLiteral("\x1b[?2004h\x1b[?2004$p"));
		QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?2004;1$y"));

		emulator.feed(QByteArrayLiteral("\x1b[?1004h\x1b[?1004$p"));
		QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?1004;1$y"));
	}
}

void emulator_test::the_palette_query_reports_the_hosts_own_sixteen() {
	/*
	 * OSC 4, ranked first by the sibling qtty TUI with a cost worth
	 * recording because it is the kind nobody reports. Its colour model
	 * quantises into the 256-colour palette and assumed xterm's default
	 * table. Indices 16 to 255 are a formula every terminal computes
	 * identically, so those were right; 0 to 15 are the user's scheme,
	 * so every quantised colour landing there was computed against the
	 * wrong table. The output does not look broken, it looks slightly
	 * off.
	 *
	 * The low sixteen here are deliberately nothing like xterm's, so a
	 * reply that fell back to the standard table fails.
	 */
	QRgb scheme[16];
	for (int i = 0; i < 16; ++i) scheme[i] = qRgb(i * 16, 255 - i * 16, 0x40);

	bssh_emulator emulator(20, 4);
	emulator.set_palette(scheme);

	emulator.feed(QByteArrayLiteral("\x1b]4;1;?\x1b\\"));
	QCOMPARE(emulator.take_output(),
	          QByteArrayLiteral("\x1b]4;1;rgb:1010/efef/4040\x1b\\"));

	/*
	 * Several pairs in one query, answered one reply each -- the same
	 * reason XTGETTCAP answers one name at a time: a single reply
	 * covering several indices has to say one thing about all of them.
	 */
	emulator.feed(QByteArrayLiteral("\x1b]4;0;?;15;?\x1b\\"));
	QCOMPARE(emulator.take_output(),
	          QByteArrayLiteral("\x1b]4;0;rgb:0000/ffff/4040\x1b\\")
	          + QByteArrayLiteral("\x1b]4;15;rgb:f0f0/0f0f/4040\x1b\\"));

	/*
	 * And above 15 the answer is the standard formula, NOT the scheme --
	 * a program asking for colour 137 has asked for a particular RGB
	 * value rather than for the user's taste, and this is the half a
	 * client could already compute for itself. Checked against the
	 * shared helper the renderer paints with, so the two cannot drift:
	 * a client quantising into this palette relies on them agreeing.
	 */
	emulator.feed(QByteArrayLiteral("\x1b]4;137;?\x1b\\"));

	const QRgb expected = bssh_palette_colour(scheme, 137);
	QCOMPARE(emulator.take_output(),
	          QByteArrayLiteral("\x1b]4;137;rgb:")
	          + QByteArray::number(qRed(expected), 16).rightJustified(2, '0').repeated(2)
	          + QByteArrayLiteral("/")
	          + QByteArray::number(qGreen(expected), 16).rightJustified(2, '0').repeated(2)
	          + QByteArrayLiteral("/")
	          + QByteArray::number(qBlue(expected), 16).rightJustified(2, '0').repeated(2)
	          + QByteArrayLiteral("\x1b\\"));

	/*
	 * A payload that SETS a colour is not honoured and not answered, the
	 * same line OSC 10 and OSC 11 take: a remote repainting the terminal
	 * it was given is not a power this grants.
	 */
	emulator.feed(QByteArrayLiteral("\x1b]4;1;#ff0000\x1b\\"));
	QCOMPARE(emulator.take_output(), QByteArray());

	emulator.feed(QByteArrayLiteral("\x1b]4;1;?\x1b\\"));
	QCOMPARE(emulator.take_output(),
	          QByteArrayLiteral("\x1b]4;1;rgb:1010/efef/4040\x1b\\"));

	/* An index outside the palette is declined rather than clamped: a
	 * client told colour 999 is grey would believe it exists. */
	emulator.feed(QByteArrayLiteral("\x1b]4;999;?\x1b\\"));
	QCOMPARE(emulator.take_output(), QByteArray());

	/* And it goes quiet with the feature off, like its two siblings. */
	QString error;
	bssh_emulator quiet(20, 4);
	quiet.set_palette(scheme);
	quiet.set_features(bssh_term_features_t::from_spec(
	        QStringLiteral("-colour-query"), error));
	QVERIFY(error.isEmpty());

	quiet.feed(QByteArrayLiteral("\x1b]4;1;?\x1b\\"));
	QCOMPARE(quiet.take_output(), QByteArray());
}

void emulator_test::a_remote_cannot_grow_the_title_without_limit() {
	/*
	 * The title arrives as fragments and needs no terminator to keep
	 * arriving, so a host that opens an OSC title and never closes it grew
	 * this buffer for as long as it kept typing -- and the string reaches
	 * the tab label, the window title and, on Android, the notification.
	 *
	 * on_selection_set already caps OSC 52 for exactly this reason, six
	 * hundred lines away in the same file: libvterm hands a long string
	 * over in pieces as its own buffer fills, so accumulating them without
	 * a bound of our own lets the REMOTE decide how much memory this
	 * costs. The title was the one remote-controlled buffer with no cap,
	 * next to a scrollback with a line limit and an image store with a
	 * memory budget.
	 */
	bssh_emulator emulator(80, 24);
	QSignalSpy spy(&emulator, &bssh_emulator::title_changed);

	emulator.feed(QByteArrayLiteral("\x1b]0;"));
	for (int i = 0; i < 64; ++i) {
		emulator.feed(QByteArray(4096, 'A'));
	}
	emulator.feed(QByteArrayLiteral("\x1b\\"));

	QCOMPARE(spy.count(), 1);
	const QString title = spy.first().first().toString();

	QVERIFY2(title.size() <= 1024,
	          qPrintable(QStringLiteral("a quarter-megabyte title survived at %1 characters")
	                      .arg(title.size())));

	/*
	 * The control, so this cannot pass against a terminal that dropped
	 * titles altogether: an ordinary one arrives whole.
	 */
	QSignalSpy ordinary(&emulator, &bssh_emulator::title_changed);
	emulator.feed(QByteArrayLiteral("\x1b]0;a sensible name\x1b\\"));
	QCOMPARE(ordinary.count(), 1);
	QCOMPARE(ordinary.first().first().toString(), QStringLiteral("a sensible name"));
}

void emulator_test::a_switched_off_image_protocol_costs_no_memory() {
	/*
	 * The half a feature switch did not cover. sixel, kitty graphics and
	 * iTerm2 images each tested features.enabled AFTER buffering the whole
	 * payload, so a host could spend this terminal's memory on a protocol
	 * the user had turned OFF -- and on Android, which builds without
	 * libsixel entirely, on one it cannot even decode.
	 *
	 * Asserted through what the terminal does rather than by reading a
	 * buffer: with sixel off, a large payload must leave no image and must
	 * not disturb the screen, and the terminal must still be working
	 * afterwards. A buffer this test cannot see is exactly the thing the
	 * cap exists to bound, so the observable claim is that the session
	 * survives an oversized one and keeps its senses.
	 */
	bssh_emulator emulator(80, 24);

	bssh_term_features_t off;
	off.set(bssh_term_features_t::SIXEL, false);
	emulator.set_features(off);

	emulator.feed(QByteArrayLiteral("\x1bPq"));
	for (int i = 0; i < 64; ++i) {
		emulator.feed(QByteArray(4096, '?'));
	}
	emulator.feed(QByteArrayLiteral("\x1b\\"));

	QCOMPARE(emulator.images().count(), 0);

	/*
	 * And the terminal still works, which is the control: a parser left
	 * wedged by the oversized payload would fail here rather than above.
	 */
	emulator.feed(QByteArrayLiteral("after"));
	QVERIFY2(emulator.screen().to_text().contains(QStringLiteral("after")),
	          qPrintable(QStringLiteral("the terminal has to keep working after an "
	                                     "oversized payload; screen holds: ")
	                      + emulator.screen().to_text().left(80)));
}

void emulator_test::the_kitty_query_reports_only_the_flags_in_force() {
	/*
	 * The query answered with whatever had been pushed, so a program that
	 * asked for flags this terminal did not have was told it had them.
	 * What is reported is what is in force: the five the protocol defines
	 * (all implemented since 2026-09-30), and nothing above them.
	 */
	bssh_emulator emulator(40, 5);
	emulator.set_features(bssh_term_features_t());

	emulator.feed(QByteArrayLiteral("\x1b[>127u"));
	emulator.take_output();
	emulator.feed(QByteArrayLiteral("\x1b[?u"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?31u"));

	/* Set, and add with mode 2: an undefined bit stays out. */
	emulator.feed(QByteArrayLiteral("\x1b[=1u"));
	emulator.feed(QByteArrayLiteral("\x1b[=64;2u"));
	QCOMPARE(emulator.kitty_keyboard_flags(), 1);

	/*
	 * A push of an undefined flag alone leaves nothing in force, so
	 * Escape stays the legacy byte rather than a CSI u the program was
	 * never promised.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[>64u"));
	QCOMPARE(emulator.kitty_keyboard_flags(), 0);
	emulator.take_output();
	emulator.send_key(bssh_emulator::KEY_ESCAPE, 0);
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b"));
}

/*
 * Moving a session to another process (project.md sec 8.4) carries the
 * terminal as bytes: export_state() in one emulator, import_state() into a
 * fresh one of the same size. The second must be indistinguishable from
 * the first -- the same cells, hyperlinks, history, picture and modes --
 * and must then react to the same input the same way, including a
 * sequence and a character the stream stopped in the middle of.
 */
namespace {

/* Every observable of two terminals, compared; a description of the first
 * difference, or empty when there is none. */
QString first_difference(const bssh_emulator &a, const bssh_emulator &b) {
	const bssh_screen &one = a.screen();
	const bssh_screen &two = b.screen();
	if (one.cursor_row() != two.cursor_row() || one.cursor_col() != two.cursor_col())
		return QStringLiteral("cursor %1,%2 against %3,%4").arg(one.cursor_row())
		        .arg(one.cursor_col()).arg(two.cursor_row()).arg(two.cursor_col());
	if (one.cursor_visible() != two.cursor_visible()) return QStringLiteral("cursor visibility");
	for (int row = 0; row < one.rows(); ++row) {
		for (int col = 0; col < one.cols(); ++col) {
			const bssh_cell_t &x = one.cell(row, col);
			const bssh_cell_t &y = two.cell(row, col);
			if (x.ch != y.ch || !(x.fg == y.fg) || !(x.bg == y.bg) || x.attrs != y.attrs ||
			    x.width != y.width || x.link != y.link)
				return QStringLiteral("cell %1,%2: '%3' against '%4'").arg(row).arg(col)
				        .arg(QString::fromUcs4(&x.ch, 1)).arg(QString::fromUcs4(&y.ch, 1));
			if (x.link && a.link_uri(x.link) != b.link_uri(y.link)) return QStringLiteral("link uri");
		}
	}
	if (a.scrollback().count() != b.scrollback().count()) return QStringLiteral("history length");
	if (a.scrollback().base() != b.scrollback().base()) return QStringLiteral("history base");
	for (int i = 0; i < a.scrollback().count(); ++i) {
		const QList<bssh_cell_t> &x = a.scrollback().line(i);
		const QList<bssh_cell_t> &y = b.scrollback().line(i);
		if (x.size() != y.size()) return QStringLiteral("history line %1 length").arg(i);
		for (int j = 0; j < x.size(); ++j)
			if (x.at(j).ch != y.at(j).ch || !(x.at(j).fg == y.at(j).fg))
				return QStringLiteral("history line %1 cell %2").arg(i).arg(j);
	}
	if (a.alternate_screen() != b.alternate_screen()) return QStringLiteral("alternate screen");
	if (a.mouse_reporting() != b.mouse_reporting()) return QStringLiteral("mouse reporting");
	if (a.kitty_keyboard_flags() != b.kitty_keyboard_flags()) return QStringLiteral("kitty flags");
	if (a.kitty_stack_depth() != b.kitty_stack_depth()) return QStringLiteral("kitty stack");
	if (a.link_count() != b.link_count())
		return QStringLiteral("%1 links against %2").arg(a.link_count()).arg(b.link_count());
	if (a.images().count() != b.images().count()) return QStringLiteral("picture count");
	for (int i = 0; i < a.images().count(); ++i) {
		const bssh_image_placement_t &x = a.images().placements().at(i);
		const bssh_image_placement_t &y = b.images().placements().at(i);
		if (x.id != y.id || x.row != y.row || x.col != y.col || x.cols != y.cols ||
		    x.rows != y.rows || a.images().image(x) != b.images().image(y))
			return QStringLiteral("picture %1").arg(i);
	}
	if (a.images().memory_used() != b.images().memory_used()) {
		return QStringLiteral("pictures take %1 bytes against %2")
		        .arg(a.images().memory_used()).arg(b.images().memory_used());
	}
	return QString();
}

}  // namespace

void emulator_test::a_terminal_moves_whole_into_another_emulator() {
	bssh_emulator from(40, 10);
	/* A short history, so lines leave its top and its numbering moves:
	 * at the default 5000 the base stays 0 on both sides and a move that
	 * dropped it passed. */
	from.set_scrollback_lines(10);
	QString title;
	connect(&from, &bssh_emulator::title_changed, this, [&title](const QString &t) { title = t; });

	/* History, colours, a hyperlink and a title on the primary screen. */
	for (int i = 0; i < 25; ++i) from.feed(QByteArray("\x1b[3") + char('1' + i % 7) + "mline " + QByteArray::number(i) + "\r\n");
	from.feed(QByteArrayLiteral("\x1b[0m\x1b]8;;https://example.org/moved\x1b\\a link\x1b]8;;\x1b\\ \x1b]2;moved title\x1b\\"));
	/* A picture, by kitty's protocol: 2x2 raw RGB. */
	from.feed(QByteArrayLiteral("\x1b_Ga=T,f=24,s=2,v=2,i=7;/wAAAP8AAAD/////\x1b\\"));
	/* Modes a program leaves set: cursor keys, mouse, a kitty keyboard
	 * push, a cursor shape, then the alternate screen with text on it. */
	from.feed(QByteArrayLiteral("\x1b[?1h\x1b[?1000h\x1b[?1006h\x1b[>1u\x1b[5 q"));
	from.feed(QByteArrayLiteral("\x1b[?1049h\x1b[2;3Hon the alternate screen \x1b[1;4mbold"));
	/* A link where it is SEEN: the primary screen's is behind the
	 * alternate one, and a move that dropped the links passed. */
	from.feed(QByteArrayLiteral("\x1b[0m\x1b[4;1H\x1b]8;;https://example.org/alt\x1b\\alt link\x1b]8;;\x1b\\"));
	/*
	 * And the stream stopped part-way through a CSI. libvterm holds that
	 * one itself -- this layer holds back only a UTF-8 tail and a lone
	 * ESC -- so it is libvterm's parser that has to arrive mid-sequence.
	 * The character split is a move of its own, below: both at once
	 * garble the continuation identically on both sides, and the first
	 * version of this test passed with the parser's place thrown away.
	 */
	from.feed(QByteArrayLiteral("\x1b[3"));
	from.take_output();
	QVERIFY2(from.images().count() == 1, "the picture never landed, so it would not be tested");
	QVERIFY(from.alternate_screen());
	QVERIFY(from.mouse_reporting());
	QVERIFY(from.scrollback().count() > 0);
	QVERIFY2(from.scrollback().base() > 0, "no line left the history, so its numbering is untested");
	QVERIFY2(from.screen().cell(3, 0).link != 0, "the link on the alternate screen is not there to test");

	const QByteArray state = from.export_state();
	QVERIFY(!state.isEmpty());

	bssh_emulator to(40, 10);
	to.set_scrollback_lines(10);
	QString to_title, to_cursor;
	bool to_alternate = false;
	connect(&to, &bssh_emulator::title_changed, this, [&to_title](const QString &t) { to_title = t; });
	connect(&to, &bssh_emulator::cursor_shape_changed, this, [&to_cursor](const QString &s) { to_cursor = s; });
	connect(&to, &bssh_emulator::alternate_screen_changed, this, [&to_alternate](bool on) { to_alternate = on; });
	QVERIFY(to.import_state(state));

	const QString moved = first_difference(from, to);
	QVERIFY2(moved.isEmpty(), qPrintable(QStringLiteral("after the move: ") + moved));
	QCOMPARE(to_title, QStringLiteral("moved title"));
	QCOMPARE(to_title, title);
	QVERIFY(to_alternate);
	QCOMPARE(to_cursor, QStringLiteral("bar"));

	/* The same continuation into both: the rest of the CSI, which makes it
	 * SGR 31 -- red -- rather than text, then more text. */
	const QByteArray rest = QByteArrayLiteral("1mred \x1b[0mafter");
	from.feed(rest);
	to.feed(rest);
	const QString continued = first_difference(from, to);
	QVERIFY2(continued.isEmpty(), qPrintable(QStringLiteral("after the same input: ") + continued));
	QVERIFY2(to.screen().to_text().contains(QStringLiteral("red after")),
	          "the CSI's tail was printed rather than finishing the sequence");

	/* Cursor-key mode moved too: Up is ESC O A in both, not ESC [ A. */
	from.take_output();
	to.take_output();
	from.send_key(bssh_emulator::KEY_UP, 0);
	to.send_key(bssh_emulator::KEY_UP, 0);
	const QByteArray up = from.take_output();
	QCOMPARE(to.take_output(), up);
	QCOMPARE(up, QByteArrayLiteral("\x1bOA"));

	/* And leaving the alternate screen finds the primary one intact. */
	from.feed(QByteArrayLiteral("\x1b[?1049l"));
	to.feed(QByteArrayLiteral("\x1b[?1049l"));
	const QString primary = first_difference(from, to);
	QVERIFY2(primary.isEmpty(), qPrintable(QStringLiteral("back on the primary screen: ") + primary));

	/* A second move, stopped in the middle of a euro sign's three bytes. */
	from.feed(QByteArray("cost \xe2\x82", 7));
	bssh_emulator again(40, 10);
	again.set_scrollback_lines(10);
	QVERIFY(again.import_state(from.export_state()));
	from.feed(QByteArray("\xac", 1));
	again.feed(QByteArray("\xac", 1));
	const QString split = first_difference(from, again);
	QVERIFY2(split.isEmpty(), qPrintable(QStringLiteral("after a split character: ") + split));
	QVERIFY2(again.screen().to_text().contains(QString::fromUtf8("cost \xe2\x82\xac")),
	          "the character split across the move did not arrive whole");
}

void emulator_test::a_moved_terminal_refuses_what_it_cannot_take() {
	bssh_emulator from(40, 10);
	from.feed(QByteArrayLiteral("kept text"));
	const QByteArray state = from.export_state();

	/* Another size: refused, and the terminal untouched. */
	bssh_emulator other(41, 10);
	other.feed(QByteArrayLiteral("mine"));
	QVERIFY(!other.import_state(state));
	QCOMPARE(other.screen().cell(0, 0).ch, U'm');

	/* Damaged at either end, or not a terminal's state at all. */
	bssh_emulator same(40, 10);
	same.feed(QByteArrayLiteral("mine"));
	QVERIFY(!same.import_state(state.left(state.size() / 2)));
	QVERIFY(!same.import_state(state + QByteArray("x")));
	QVERIFY(!same.import_state(QByteArrayLiteral("not a terminal")));
	QCOMPARE(same.screen().cell(0, 0).ch, U'm');
	QCOMPARE(same.screen().cell(0, 3).ch, U'e');

	/* And the whole of it is still taken afterwards. */
	QVERIFY(same.import_state(state));
	QCOMPARE(same.screen().cell(0, 0).ch, U'k');
}

void emulator_test::growing_the_terminal_brings_back_scrollback_as_it_was() {
	/*
	 * Making the window taller brings lines back out of the scrollback to
	 * fill the new rows (libvterm's sb_popline). The holder saw them come
	 * back dark -- black on black, which the contrast boost lifts to grey
	 * -- and wrong. Each restored row must be the line it was, in order,
	 * in the default colours.
	 */
	bssh_emulator emulator(60, 10);
	for (int line = 1; line <= 30; ++line) {
		emulator.feed(QByteArray("line ") + QByteArray::number(line) + (line < 30 ? "\r\n" : ""));
	}
	QCOMPARE(row(emulator, 9), QStringLiteral("line 30"));

	emulator.resize(60, 16);

	for (int index = 0; index < 16; ++index) {
		QCOMPARE(row(emulator, index), QStringLiteral("line %1").arg(15 + index));
		const bssh_cell_t first = emulator.screen().cell(index, 0);
		QVERIFY2(first.fg.kind == bssh_color_t::DEFAULT && first.bg.kind == bssh_color_t::DEFAULT,
		         qPrintable(QStringLiteral("row %1 came back in colours of its own: fg %2, bg %3")
		                            .arg(index).arg(int(first.fg.kind)).arg(int(first.bg.kind))));
	}
}

void emulator_test::a_terminal_resized_several_ways_keeps_its_lines_in_order() {
	/*
	 * The holder's window, as it went: taller, then shorter and narrower,
	 * then taller and wider than either. Lines went into the scrollback
	 * and came back, and the window showed one line repeated where a dozen
	 * different ones belonged. Every row must still be the line it was.
	 */
	bssh_emulator emulator(80, 24);
	for (int line = 1; line <= 60; ++line) {
		emulator.feed(QByteArray("line ") + QByteArray::number(line) +
		              ": the quick brown fox jumps over the lazy dog" + (line < 60 ? "\r\n" : ""));
	}

	const QList<QPair<int, int>> sizes = { { 100, 31 }, { 67, 19 }, { 105, 33 } };
	for (const auto &size : sizes) emulator.resize(size.first, size.second);

	const int rows = 33;
	for (int index = 0; index < rows; ++index) {
		QCOMPARE(row(emulator, index),
		         QStringLiteral("line %1: the quick brown fox jumps over the lazy dog")
		                 .arg(60 - (rows - 1) + index));
	}
}

namespace {

/* A comma would split QFETCH's arguments. */
using sizes_t = QList<QPair<int, int>>;

/*
 * The text a terminal holds, history and screen, as the lines the program
 * wrote: a row that continues the one before it is joined to it, and
 * trailing blanks are dropped, since a cell a resize cleared and a cell
 * nobody wrote are the same thing.
 */
QStringList logical_lines(const bssh_emulator &emulator) {
	QStringList out;
	const auto take = [&out](const QString &text, bool continuation) {
		if (continuation && !out.isEmpty()) out.last() += text;
		else out.append(text);
	};
	const auto text_of = [](const QList<bssh_cell_t> &cells) {
		QString text;
		for (const bssh_cell_t &cell : cells)
			if (cell.width != 0) text += QString::fromUcs4(&cell.ch, 1);
		return text;
	};
	const bssh_scrollback &history = emulator.scrollback();
	for (int index = 0; index < history.count(); ++index)
		take(text_of(history.line(index)), history.is_continuation(index));
	const bssh_screen &screen = emulator.screen();
	for (int index = 0; index < screen.rows(); ++index) {
		QList<bssh_cell_t> cells;
		for (int col = 0; col < screen.cols(); ++col) cells.append(screen.cell(index, col));
		take(text_of(cells), emulator.row_is_continuation(index));
	}
	for (QString &line : out) {
		while (line.endsWith(QLatin1Char(' '))) line.chop(1);
	}
	while (!out.isEmpty() && out.last().isEmpty()) out.removeLast();
	return out;
}

}

void emulator_test::a_terminal_made_very_small_and_back_keeps_every_line_data() {
	QTest::addColumn<sizes_t>("sizes");

	QTest::newRow("narrow") << sizes_t{ { 12, 10 } };
	QTest::newRow("narrow and short") << sizes_t{ { 12, 4 } };
	QTest::newRow("tiny") << sizes_t{ { 5, 3 } };
	QTest::newRow("down in steps") << sizes_t{ { 30, 8 }, { 20, 6 }, { 10, 4 }, { 4, 2 } };
	QTest::newRow("down and up in steps")
	        << sizes_t{ { 20, 6 }, { 6, 3 }, { 25, 7 }, { 3, 2 }, { 35, 9 } };
}

void emulator_test::a_terminal_made_very_small_and_back_keeps_every_line() {
	/*
	 * The holder, 2026-10-27, after reflow: "most lines are brought back
	 * now, but when made really small the lines get slightly messed up."
	 * Whatever sizes the window goes through, every line the program
	 * wrote must still be there afterwards, whole and in order.
	 */
	QFETCH(sizes_t, sizes);

	bssh_emulator emulator(40, 10);
	QStringList written;
	for (int line = 1; line <= 30; ++line) {
		QString text = QStringLiteral("%1: ").arg(line);
		const int repeat = (line % 4 == 0) ? 12 : (line % 3 == 0) ? 0 : 3;
		text += QStringLiteral("word ").repeated(repeat) + QStringLiteral("end");
		/* Wide glyphs, which a narrow edge must not cut in half. */
		if (line == 10) text = QStringLiteral("10: \u4e2d\u6587 wide \u5b57 and \u6f22\u5b57 again");
		/* Spaces where the narrow widths cut, which are text. */
		if (line == 17) text = QStringLiteral("17:  a  b  c  d  e  f  g  h");
		written << text;
		const QByteArray bytes = text.toUtf8();
		emulator.feed(line == 20 ? "\x1b[31m" + bytes + "\x1b[0m" : bytes);
		if (line < 30) emulator.feed("\r\n");
	}
	/* The last line is longer than a small screen is tall, with the cursor
	 * at its end: libvterm aborted the process resizing that. */
	written.last() += QStringLiteral(" and the prompt line goes on long past the edge").repeated(3);
	emulator.feed(QStringLiteral(" and the prompt line goes on long past the edge").repeated(3).toUtf8());
	QCOMPARE(logical_lines(emulator), written);

	for (const auto &size : sizes) {
		emulator.resize(size.first, size.second);
		QCOMPARE(logical_lines(emulator), written);

		/* And the history reads at this width: a row wider than the
		 * window is drawn cut at its edge when scrolled back to. */
		const bssh_scrollback &history = emulator.scrollback();
		for (int index = 0; index < history.count(); ++index)
			QVERIFY2(history.line(index).size() <= size.first,
			         qPrintable(QStringLiteral("history row %1 is %2 wide in a %3-column window")
			                            .arg(index).arg(history.line(index).size()).arg(size.first)));
	}
	emulator.resize(40, 10);
	QCOMPARE(logical_lines(emulator), written);

	/* The cursor is still where the program left it. */
	emulator.feed("!");
	written.last() += QLatin1Char('!');
	QCOMPARE(logical_lines(emulator), written);

	/* And the colour went round with the text. */
	const bssh_scrollback &history = emulator.scrollback();
	for (int index = 0; index < history.count(); ++index) {
		const QList<bssh_cell_t> &cells = history.line(index);
		if (cells.size() > 1 && cells.at(0).ch == U'2' && cells.at(1).ch == U'0' && !history.is_continuation(index)) {
			QCOMPARE(int(cells.at(0).fg.kind), int(bssh_color_t::INDEXED));
			QCOMPARE(int(cells.at(0).fg.index), 1);
		}
	}
}

void emulator_test::a_one_column_terminal_survives_a_wide_glyph() {
	/*
	 * A double-width glyph on a screen one column wide: libvterm marked
	 * its second cell through a NULL getcell() and the process crashed
	 * (vterm-patch/0013). Found by fuzzing under ASan. A pane can be
	 * one column -- the view floors its grid there -- and an emoji in a
	 * prompt is a double-width glyph.
	 */
	bssh_emulator emulator(1, 4);
	emulator.feed("\xe4\xb8\xad\xf0\x9f\x8d\xba""a");   /* U+4E2D, U+1F37A, a */
	emulator.feed("\x1b[?1049h\xe4\xb8\xad\x1b[?1049l");
	QCOMPARE(emulator.screen().cols(), 1);

	/* And narrowed to one column with wide text already on the screen. */
	bssh_emulator wide(20, 4);
	wide.feed("\xe4\xb8\xad\xe6\x96\x87 and \xf0\x9f\x8d\xba");
	wide.resize(1, 4);
	wide.feed("\xe5\xad\x97");
	wide.resize(20, 4);
	QVERIFY(row(wide, 0).contains(QStringLiteral("\u4e2d")));
}

void emulator_test::a_wide_glyph_at_the_edge_moves_whole_and_joins_back_without_a_gap() {
	/*
	 * A double-width glyph whose first half a reflow would put in the last
	 * column starts the next row instead (vterm-patch/0012). Cut there, its
	 * halves landed on two rows -- one cell of a glyph at the edge and a
	 * blank starting the next. And the empty cell it leaves at the edge is
	 * not text: joined back by a wider window, it became a space.
	 */
	bssh_emulator emulator(10, 4);
	emulator.feed("abc\xe4\xb8\xad""def");   /* abc U+4E2D def */
	emulator.resize(4, 4);
	QCOMPARE(emulator.screen().cell(1, 0).ch, U'\u4e2d');
	QCOMPARE(int(emulator.screen().cell(1, 0).width), 2);
	QCOMPARE(int(emulator.screen().cell(1, 1).width), 0);

	emulator.resize(10, 4);
	QCOMPARE(row(emulator, 0), QStringLiteral("abc\u4e2d""def"));

	/* The same through the history: narrow, scrolled out, widened. */
	emulator.resize(4, 4);
	emulator.feed("\r\n1\r\n2\r\n3\r\n4");
	QVERIFY(emulator.scrollback().count() >= 3);
	emulator.resize(10, 4);
	QStringList lines = logical_lines(emulator);
	QVERIFY2(lines.contains(QStringLiteral("abc\u4e2d""def")), qPrintable(lines.join(QLatin1Char('|'))));
}

void emulator_test::a_line_pulled_back_by_a_grow_narrows_without_blank_rows() {
	/*
	 * A line a grow pulled back out of the history went back to libvterm
	 * padded with typed spaces, so its reflow counted it the full width:
	 * narrowed from 40 columns to 10, "hello" took four rows, three of
	 * them blank, and the top of the screen was a blank row continuing a
	 * line in the history. Cells nothing wrote go back as empty now
	 * (bssh_cell_t::UNWRITTEN).
	 */
	bssh_emulator emulator(40, 3);
	emulator.feed("hello\r\na\r\nb\r\nc");
	emulator.resize(40, 6);
	QCOMPARE(row(emulator, 0), QStringLiteral("hello"));

	emulator.resize(10, 6);
	QCOMPARE(emulator.screen().to_text().section(QLatin1Char('\n'), 0, 3),
	         QStringLiteral("hello\na\nb\nc"));
	QVERIFY(!emulator.row_is_continuation(0));
	QCOMPARE(emulator.scrollback().count(), 0);
}

void emulator_test::a_narrowed_terminal_rewraps_and_a_widened_one_joins_again() {
	/*
	 * Reflow (project.md sec 9.5c): narrowing used to cut every line at the
	 * new width, and widening could not bring the cut part back. A line
	 * that wrapped at the edge is rewrapped onto more rows instead, and
	 * joined again when there is room -- with its colours. A line ended by
	 * the program stays its own line.
	 */
	bssh_emulator emulator(40, 10);
	const QByteArray long_line = "\x1b[32mgreen\x1b[0m and a line long enough to wrap twice when narrow";
	emulator.feed("short one\r\n" + long_line + "\r\nlast");
	QCOMPARE(row(emulator, 0), QStringLiteral("short one"));
	const bssh_cell_t green = emulator.screen().cell(1, 0);

	emulator.resize(20, 10);
	QCOMPARE(row(emulator, 0), QStringLiteral("short one"));
	QCOMPARE(row(emulator, 1), QStringLiteral("green and a line lon"));
	QVERIFY2(!row(emulator, 2).isEmpty(), "the end of the line was cut off, not rewrapped");
	QCOMPARE(emulator.screen().cell(1, 0).fg, green.fg);

	emulator.resize(70, 10);
	QCOMPARE(row(emulator, 0), QStringLiteral("short one"));
	QCOMPARE(row(emulator, 1), QStringLiteral("green and a line long enough to wrap twice when narrow"));
	QCOMPARE(row(emulator, 2), QStringLiteral("last"));
	QCOMPARE(emulator.screen().cell(1, 0).fg, green.fg);
}
