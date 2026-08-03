#include "emulator_bench.h"

#include <QElapsedTimer>
#include <QtTest>

#include "term/emulator.h"

namespace {

/*
 * Bytes arrive from an SSH channel in reads, not all at once, and feed()
 * holds back an incomplete UTF-8 tail between them (sec 7). Measuring one
 * enormous write would skip that path entirely and flatter the result.
 */
const int READ_SIZE = 8192;

/*
 * Best of N rather than a mean: scheduler noise, page faults and a busy
 * neighbour can only ever make a run slower, so the fastest run is the
 * one least contaminated by things that are not the emulator.
 */
const int REPEATS = 5;

QByteArray plain_scrolling(int lines) {
	QByteArray out;
	for (int i = 0; i < lines; ++i) {
		out += QByteArrayLiteral("this is a fairly ordinary line of build output number ")
			      + QByteArray::number(i) + QByteArrayLiteral("\r\n");
	}
	return out;
}

/*
 * The same shape of output with an SGR change every few cells, which is
 * what a compiler, a test runner or `ls` actually emits. It is a separate
 * case because per-cell attribute conversion is the cost that survives
 * every improvement to per-callback overhead (sec 9.5).
 */
QByteArray coloured_scrolling(int lines) {
	QByteArray out;
	for (int i = 0; i < lines; ++i) {
		out += QByteArrayLiteral("\x1b[32m[ok]\x1b[0m \x1b[1;34mmodule\x1b[0m ")
			      + QByteArray::number(i)
			      + QByteArrayLiteral(" \x1b[33mcompiled\x1b[0m in 0.4s\r\n");
	}
	return out;
}

/*
 * A full-screen redraw with no scrolling at all -- vim, top, a TUI
 * installer. More bytes than the scrolling cases and a completely
 * different cost profile, which is the whole reason to measure both: the
 * gap between them is what pointed at the scroll path in the first place.
 */
QByteArray full_repaint(int frames, int cols, int rows) {
	QByteArray out;
	for (int frame = 0; frame < frames; ++frame) {
		out += QByteArrayLiteral("\x1b[H");
		for (int row = 0; row < rows; ++row) {
			out += QByteArrayLiteral("\x1b[") + QByteArray::number(row + 1)
				      + QByteArrayLiteral(";1H");
			out += QByteArray(cols - 1, char('a' + (frame + row) % 26));
		}
	}
	return out;
}

/*
 * Feeds the stream in reads, returns the fastest elapsed time, and hands
 * back the last emulator so the caller can check the screen it produced.
 */
qint64 fastest_nsecs(const QByteArray &stream, int cols, int rows,
	                    QString *final_text) {
	qint64 best = -1;

	/*
	 * One untimed pass first. Best-of-N does not rescue the first case
	 * measured after a build, because all N repeats then sit inside the
	 * same cold window -- freshly faulted-in pages and an allocator that
	 * has never served a scrollback this size. Skipping this reported
	 * 1.91 MB/s for a case that measures 3.98 a second later, which reads
	 * as a regression and is not one.
	 */
	{
		bssh_emulator warmup(cols, rows);
		for (int offset = 0; offset < stream.size(); offset += READ_SIZE) {
			warmup.feed(stream.mid(offset, READ_SIZE));
		}
	}

	for (int repeat = 0; repeat < REPEATS; ++repeat) {
		bssh_emulator emulator(cols, rows);
		QElapsedTimer timer;
		timer.start();

		for (int offset = 0; offset < stream.size(); offset += READ_SIZE) {
			emulator.feed(stream.mid(offset, READ_SIZE));
		}

		const qint64 elapsed = timer.nsecsElapsed();
		if (best < 0 || elapsed < best) best = elapsed;
		if (final_text) *final_text = emulator.screen().to_text();
	}

	return best;
}

void report(const char *what, int cols, int rows, qint64 bytes, qint64 nsecs) {
	const double seconds = double(nsecs) / 1e9;
	qInfo("%-26s %3dx%-3d  %7.2f MB/s  (%lld KB in %lld ms)",
		     what, cols, rows,
		     double(bytes) / seconds / (1024.0 * 1024.0),
		     bytes / 1024, nsecs / 1000000);
}

/* Trailing blanks are stripped by to_text(), so compare on a real row. */
QString row_of(const QString &text, int row) {
	return text.split(QLatin1Char('\n')).value(row);
}

} /* namespace */

void emulator_bench::plain_scrolling_output() {
	/*
	 * Two grid sizes, because the cost of a scroll is per-screen rather
	 * than per-line: a 200x50 grid moves five times as many cells per
	 * scrolled line as an 80x24 one, and it was that ratio -- not the
	 * absolute number -- that identified the scroll path as the problem.
	 * Measuring one size alone would have hidden it.
	 */
	const int lines = 20000;
	const QByteArray stream = plain_scrolling(lines);

	const int sizes[][2] = { { 200, 50 }, { 80, 24 } };
	for (const auto &size : sizes) {
		QString text;
		const qint64 nsecs = fastest_nsecs(stream, size[0], size[1], &text);
		report("plain scrolling", size[0], size[1], stream.size(), nsecs);

		/* The last line written is the one above the cursor's own row. */
		QCOMPARE(row_of(text, size[1] - 2),
		          QStringLiteral("this is a fairly ordinary line of build output number %1")
		              .arg(lines - 1));
	}
}

void emulator_bench::coloured_scrolling_output() {
	const int lines = 20000;
	const QByteArray stream = coloured_scrolling(lines);

	QString text;
	const qint64 nsecs = fastest_nsecs(stream, 200, 50, &text);
	report("coloured scrolling", 200, 50, stream.size(), nsecs);

	QCOMPARE(row_of(text, 48),
	          QStringLiteral("[ok] module %1 compiled in 0.4s").arg(lines - 1));
}

void emulator_bench::full_screen_repaint() {
	const int frames = 2000;
	const int cols = 200;
	const int rows = 50;
	const QByteArray stream = full_repaint(frames, cols, rows);

	QString text;
	const qint64 nsecs = fastest_nsecs(stream, cols, rows, &text);
	report("full-screen repaint", cols, rows, stream.size(), nsecs);

	/* Every row of the last frame, one repeated character per row. */
	QCOMPARE(row_of(text, 0),
	          QString(cols - 1, QLatin1Char('a' + (frames - 1) % 26)));
	QCOMPARE(row_of(text, rows - 1),
	          QString(cols - 1, QLatin1Char('a' + (frames - 1 + rows - 1) % 26)));
}
