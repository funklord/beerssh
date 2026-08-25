#include <QtTest>

#include "screen_test.h"
#include "term/screen.h"


static void write_text(bssh_screen &screen, int row, int col, const QString &text) {
	for (int i = 0; i < text.size(); ++i) {
		bssh_cell_t cell;
		cell.ch = text.at(i).unicode();
		screen.set_cell(row, col + i, cell);
	}
}

void screen_test::starts_blank_at_the_requested_size() {
	const bssh_screen screen(80, 24);

	QCOMPARE(screen.cols(), 80);
	QCOMPARE(screen.rows(), 24);
	QCOMPARE(screen.cell(0, 0).ch, U' ');
	QCOMPARE(screen.to_text(), QString(23, QLatin1Char('\n')));
}

void screen_test::resize_preserves_the_top_left() {
	bssh_screen screen(80, 24);
	write_text(screen, 0, 0, QStringLiteral("keep me"));
	write_text(screen, 23, 0, QStringLiteral("bottom row"));

	screen.resize(40, 12);

	QCOMPARE(screen.cols(), 40);
	QCOMPARE(screen.rows(), 12);
	QCOMPARE(screen.to_text().section(QLatin1Char('\n'), 0, 0), QStringLiteral("keep me"));

	/*
	 * Row-major storage means a width change moves every row's start, so
	 * this is really asserting that the copy is done cell by cell rather
	 * than by truncating the backing list -- the latter would shear each
	 * row further left than the one above it.
	 */
	write_text(screen, 5, 3, QStringLiteral("indented"));
	screen.resize(20, 8);
	QCOMPARE(screen.to_text().section(QLatin1Char('\n'), 5, 5), QStringLiteral("   indented"));
}

void screen_test::resize_keeps_the_cursor_on_the_grid() {
	bssh_screen screen(80, 24);
	screen.set_cursor(20, 70);

	screen.resize(40, 10);

	QCOMPARE(screen.cursor_row(), 9);
	QCOMPARE(screen.cursor_col(), 39);
}

void screen_test::out_of_bounds_access_is_harmless() {
	bssh_screen screen(10, 4);

	/* A parser that miscounts must not take the process with it. */
	screen.set_cell(-1, 0, bssh_cell_t());
	screen.set_cell(0, 99, bssh_cell_t());
	screen.clear_row(99);

	QCOMPARE(screen.cell(-1, -1).ch, U' ');
	QCOMPARE(screen.cell(99, 99).ch, U' ');
	QCOMPARE(screen.cols(), 10);
}

void screen_test::to_text_trims_trailing_blanks() {
	bssh_screen screen(20, 2);
	write_text(screen, 0, 0, QStringLiteral("hello"));

	/* Not "hello" followed by fifteen spaces -- a golden file full of
	 * invisible padding is a golden file nobody can read a diff of. */
	QCOMPARE(screen.to_text(), QStringLiteral("hello\n"));
}

void screen_test::to_text_skips_double_width_continuations() {
	bssh_screen screen(10, 1);

	bssh_cell_t wide;
	wide.ch = U'你'; /* CJK, two cells */
	wide.width = 2;
	screen.set_cell(0, 0, wide);

	bssh_cell_t continuation;
	continuation.width = 0;
	screen.set_cell(0, 1, continuation);

	bssh_cell_t narrow;
	narrow.ch = U'!';
	screen.set_cell(0, 2, narrow);

	/* One glyph, one continuation cell that must not become a space. */
	QCOMPARE(screen.to_text(), QStringLiteral("你!"));
}
