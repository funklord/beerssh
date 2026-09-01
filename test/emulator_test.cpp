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

void emulator_test::resize_keeps_existing_text() {
	bssh_emulator emulator(20, 5);
	emulator.feed(QByteArrayLiteral("stays"));

	emulator.resize(10, 3);

	QCOMPARE(emulator.screen().cols(), 10);
	QCOMPARE(emulator.screen().rows(), 3);
	QCOMPARE(row(emulator, 0), QStringLiteral("stays"));
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

	QCOMPARE(emulator.scrollback().count(), 1);

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
	QVERIFY(damaged.count() >= lines / 4);
	QCOMPARE(row(emulator, 22), QString(width, QLatin1Char('x')));
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
		emulator.feed(QByteArrayLiteral("short\r\na\r\nb"));

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
		emulator.feed(QByteArrayLiteral("\x1b[44m  \x1b[0m\r\na\r\nb"));

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
		emulator.feed(QByteArrayLiteral("ab\xe4\xb8\xad\r\nx\r\ny"));   /* ab U+4E2D */

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
	scrolled.feed(text + QByteArrayLiteral("\r\nx\r\ny"));
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
