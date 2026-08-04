#include <QSignalSpy>
#include <QtTest>

#include "emulator_test.h"
#include "term/emulator.h"

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
	wide.feed(QByteArrayLiteral("\xe4\xb8\xad\xe6\x96\x87\r\n"));   /* 中文 */
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
		emulator.feed(QByteArrayLiteral("ab\xe4\xb8\xad\r\nx\r\ny"));   /* ab中 */

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
	const QByteArray text = QByteArrayLiteral("a\xe4\xb8\xad""b");   /* a中b */

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
	 * This is the whole reason vterm-patches/0004 exists: without it
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
