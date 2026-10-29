#include "conformance_test.h"

#include <QImage>
#include <QScreen>
#include <QWindow>
#include <QtTest>

#include "term/emulator.h"
#include "ui/terminal_view.h"

namespace {

/* What a cell is, written so that a mismatch says which part differs. */
QString describe(const bssh_cell_t &cell) {
	const auto colour = [](const bssh_color_t &c) {
		switch (c.kind) {
		case bssh_color_t::DEFAULT: return QStringLiteral("default");
		case bssh_color_t::INDEXED: return QStringLiteral("index %1").arg(c.index);
		case bssh_color_t::RGB: return QStringLiteral("#%1").arg(c.rgb & 0xffffff, 6, 16, QLatin1Char('0'));
		}
		return QStringLiteral("?");
	};
	return QStringLiteral("'%1' fg %2 bg %3 attrs 0x%4")
	        .arg(QString::fromUcs4(&cell.ch, 1), colour(cell.fg), colour(cell.bg))
	        .arg(cell.attrs, 2, 16, QLatin1Char('0'));
}

bool same(const bssh_cell_t &a, const bssh_cell_t &b) {
	return a.ch == b.ch && a.fg == b.fg && a.bg == b.bg && a.attrs == b.attrs;
}

}

void conformance_test::a_cell_keeps_what_it_is_through_scrollback_and_resize_data() {
	QTest::addColumn<QByteArray>("sgr");

	/* One row per thing a cell can carry. A new SGR is a new row. */
	QTest::newRow("16 colours") << QByteArray("\x1b[31m");
	QTest::newRow("bright") << QByteArray("\x1b[93m");
	QTest::newRow("background") << QByteArray("\x1b[44m");
	QTest::newRow("bright background") << QByteArray("\x1b[105m");
	QTest::newRow("256 colours") << QByteArray("\x1b[38;5;208m");
	QTest::newRow("256 background") << QByteArray("\x1b[48;5;22m");
	QTest::newRow("truecolour") << QByteArray("\x1b[38;2;200;10;150m");
	QTest::newRow("truecolour background") << QByteArray("\x1b[48;2;5;80;160m");
	QTest::newRow("bold") << QByteArray("\x1b[1m");
	QTest::newRow("faint") << QByteArray("\x1b[2m");
	QTest::newRow("italic") << QByteArray("\x1b[3m");
	QTest::newRow("underline") << QByteArray("\x1b[4m");
	QTest::newRow("blink") << QByteArray("\x1b[5m");
	QTest::newRow("reverse") << QByteArray("\x1b[7m");
	QTest::newRow("strike") << QByteArray("\x1b[9m");
	QTest::newRow("bold red on blue") << QByteArray("\x1b[1;31;44m");
	QTest::newRow("everything") << QByteArray("\x1b[1;3;4;7;9;38;5;46;48;2;1;2;3m");
}

void conformance_test::a_cell_keeps_what_it_is_through_scrollback_and_resize() {
	/*
	 * A styled run is written, recorded as it is on screen, scrolled into
	 * the history, and must be the same there; then the window grows, the
	 * line comes back out of the history onto the screen, and must be the
	 * same again. The holder saw lines come back dark, then come back
	 * uncoloured -- each a different half of this round trip broken.
	 */
	QFETCH(QByteArray, sgr);

	bssh_emulator emulator(40, 8);
	emulator.feed(QByteArray("plain ") + sgr + "styled" + "\x1b[0m" + " plain");
	const int at = 6; /* the 's' of "styled" */
	const bssh_cell_t on_screen = emulator.screen().cell(0, at);
	QVERIFY2(!same(on_screen, emulator.screen().cell(0, 0)),
	         qPrintable(QStringLiteral("the SGR did not style the cell at all: %1").arg(describe(on_screen))));

	for (int line = 0; line < 8; ++line) emulator.feed("\r\nmore");
	QVERIFY(emulator.scrollback().count() >= 1);
	const QList<bssh_cell_t> &history = emulator.scrollback().line(0);
	QVERIFY(history.size() > at);
	QVERIFY2(same(history.at(at), on_screen),
	         qPrintable(QStringLiteral("in the scrollback: %1, on screen it was %2")
	                            .arg(describe(history.at(at)), describe(on_screen))));

	/* Taller by more than the history holds: every line comes back. */
	emulator.resize(40, 20);
	QVERIFY2(same(emulator.screen().cell(0, at), on_screen),
	         qPrintable(QStringLiteral("brought back by a resize: %1, it was %2")
	                            .arg(describe(emulator.screen().cell(0, at)), describe(on_screen))));
}

void conformance_test::the_screen_is_always_what_a_full_repaint_draws_data() {
	QTest::addColumn<QList<QByteArray>>("reads");

	/*
	 * One row per way a program changes the screen. Each read is fed with
	 * a turn of the event loop after it, as reads arrive, and the window
	 * is compared with a full repaint after every one.
	 */
	QList<QByteArray> lines;
	for (int i = 0; i < 40; ++i)
		lines << QByteArray("\x1b[3") + QByteArray::number(1 + i % 6) + "mline " +
		                 QByteArray::number(i) + "\x1b[0m\r\n";
	QTest::newRow("coloured output scrolling") << lines;

	QTest::newRow("scroll region up and down")
	        << QList<QByteArray>{ "a\r\nb\r\nc\r\nd\r\ne\r\nf\r\ng\r\nh",
	                              "\x1b[2;6r\x1b[6;1H\r\n\x1b[31mscrolled up\x1b[0m",
	                              "\x1b[2;1H\x1bM\x1b[32mscrolled down\x1b[0m",
	                              "\x1b[r\x1b[8;1Hbottom" };
	QTest::newRow("insert and delete lines")
	        << QList<QByteArray>{ "1\r\n2\r\n3\r\n4\r\n5\r\n6", "\x1b[2;1H\x1b[2L\x1b[33minserted\x1b[0m",
	                              "\x1b[4;1H\x1b[1M", "\x1b[3M" };
	QTest::newRow("insert and delete characters")
	        << QList<QByteArray>{ "abcdefghij", "\x1b[1;3H\x1b[3@XYZ", "\x1b[1;2H\x1b[2P", "\x1b[1;1H\x1b[4X" };
	QTest::newRow("erase in line and display")
	        << QList<QByteArray>{ "\x1b[44mblue background line\x1b[0m\r\nsecond",
	                              "\x1b[1;5H\x1b[K", "\x1b[1;1H\x1b[1K", "\x1b[2J\x1b[Hcleared" };
	QTest::newRow("alternate screen in and out")
	        << QList<QByteArray>{ "primary text", "\x1b[?1049h\x1b[H\x1b[35malternate\x1b[0m",
	                              "\x1b[?1049l" };
	QTest::newRow("wide characters")
	        << QList<QByteArray>{ "x\xe4\xb8\xad\xe6\x96\x87y\r\n", "\x1b[1;2H\xe5\xad\x97", "\x1b[1;1H\x1b[2P" };
	QTest::newRow("synchronised frame")
	        << QList<QByteArray>{ "start\r\n", "\x1b[?2026h\x1b[2J\x1b[Hhalf",
	                              " and the rest\r\nmore\r\nlines\r\n", "\x1b[?2026l" };
	QTest::newRow("reverse video and cursor moves")
	        << QList<QByteArray>{ "\x1b[7mreversed\x1b[0m\r\n", "\x1b[5;10Hthere\x1b[1;1Hhere", "\x1b[?5h", "\x1b[?5l" };
}

void conformance_test::the_screen_is_always_what_a_full_repaint_draws() {
	/*
	 * The view paints only the rows that changed and moves pixels when
	 * output scrolls (sec 9.5a): every one of those is a way for the window
	 * to show something the screen does not hold. So after every read the
	 * window's own pixels -- what the incremental updates left in its
	 * backing store -- must equal a fresh repaint of the whole view.
	 */
	QFETCH(QList<QByteArray>, reads);

	bssh_terminal_view view;
	view.show();
	view.resize(560, 300);
	QVERIFY(QTest::qWaitForWindowExposed(&view));

	bssh_emulator emulator(view.cols(), view.rows());
	view.attach_screen(&emulator.screen());
	view.attach_scrollback(&emulator.scrollback());
	connect(&emulator, &bssh_emulator::rows_scrolled, &view, &bssh_terminal_view::on_rows_scrolled);
	connect(&emulator, &bssh_emulator::rows_damaged, &view, &bssh_terminal_view::on_rows_damaged);
	QTest::qWait(30);

	for (int index = 0; index < reads.size(); ++index) {
		emulator.feed(reads.at(index));
		QTest::qWait(25);

		/* A frame the program is still writing is held on purpose (DEC
		 * 2026): the window shows the last whole one until it is done. */
		if (emulator.synchronised_output()) continue;

		const QImage incremental = view.windowHandle()->screen()->grabWindow(view.winId())
		                                   .toImage().convertToFormat(QImage::Format_RGB32);
		const QImage full = view.grab().toImage().convertToFormat(QImage::Format_RGB32);
		if (incremental.size() != full.size() || incremental != full) {
			const QString name = QString::fromLatin1(QTest::currentDataTag()).replace(QLatin1Char(' '), QLatin1Char('-'));
			incremental.save(QStringLiteral("conformance-%1-incremental.png").arg(name));
			full.save(QStringLiteral("conformance-%1-full.png").arg(name));
			QFAIL(qPrintable(QStringLiteral("after read %1 the window differs from a full repaint; "
			                                "saved as conformance-%2-*.png").arg(index).arg(name)));
		}
	}
}
