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
