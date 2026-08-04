#include "emulator_bench.h"

#include <QElapsedTimer>
#include <QtTest>

#include "term/emulator.h"
#include "ui/terminal_view.h"

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

/*
 * A screenful of the kind of thing a terminal actually shows: prose, an
 * SGR change every few cells, box drawing, and enough distinct characters
 * that a glyph cache has real work to do.
 */
QByteArray painting_sample(int cols, int rows) {
	QByteArray out(QByteArrayLiteral("\x1b[H"));
	for (int row = 0; row < rows; ++row) {
		out += QByteArrayLiteral("\x1b[") + QByteArray::number(row + 1)
			      + QByteArrayLiteral(";1H");
		out += QByteArrayLiteral("\x1b[3") + QByteArray::number(row % 8)
			      + QByteArrayLiteral("m[ok]\x1b[0m \x1b[1mmodule\x1b[0m ");
		out += QByteArray::number(row);
		out += QByteArrayLiteral(" \xe2\x94\x82 ");

		while (out.size() % 97 != 0 && cols > 40) {
			out += char('a' + (row + out.size()) % 26);
		}
	}
	return out;
}

/*
 * The fastest of N full repaints of a view, in milliseconds.
 *
 * render() rather than grab(): grab() allocates a pixmap per call, which
 * is a measurement of the allocator as much as of the renderer. The
 * target is reused and the first frame is thrown away, because the first
 * one pays for every glyph in the sample.
 */
double fastest_paint_ms(const QList<bssh_terminal_view *> &views, int frames) {
	QList<QPixmap *> targets;
	for (bssh_terminal_view *view : views) {
		targets.append(new QPixmap(view->size()));
	}

	double best = -1;
	for (int frame = 0; frame < frames + 1; ++frame) {
		QElapsedTimer timer;
		timer.start();

		for (int i = 0; i < views.size(); ++i) {
			views.at(i)->render(targets.at(i));
		}

		const double ms = double(timer.nsecsElapsed()) / 1e6;
		if (frame > 0 && (best < 0 || ms < best)) best = ms;
	}

	qDeleteAll(targets);
	return best;
}

void report_paint(const char *what, int panes, int cols, int rows, double ms) {
	qInfo("%-26s %d x %3dx%-3d  %7.2f ms/frame  (%5.1f fps)",
		     what, panes, cols, rows, ms, ms > 0 ? 1000.0 / ms : 0.0);
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

void emulator_bench::painting_one_pane() {
	/*
	 * The case sec 9.5's numbers came from: one full-screen pane, every
	 * cell repainted. 200x50 because that is a maximised terminal, and
	 * because the per-cell costs this is looking for scale with the grid.
	 */
	const int cols = 200;
	const int rows = 50;

	bssh_emulator emulator(cols, rows);
	emulator.feed(painting_sample(cols, rows));

	bssh_terminal_view view;
	view.attach_screen(&emulator.screen());
	view.resize(cols * 9, rows * 18);

	report_paint("paint, one pane", 1, cols, rows, fastest_paint_ms({&view}, 20));
}

void emulator_bench::painting_four_panes() {
	/*
	 * sec 8.4's four-pane case, which is the one the shared atlas was
	 * proposed for: four views drawing at once, all in the same font.
	 */
	const int cols = 100;
	const int rows = 25;

	QList<bssh_emulator *> emulators;
	QList<bssh_terminal_view *> views;
	for (int i = 0; i < 4; ++i) {
		bssh_emulator *emulator = new bssh_emulator(cols, rows);
		emulator->feed(painting_sample(cols, rows));
		emulators.append(emulator);

		bssh_terminal_view *view = new bssh_terminal_view();
		view->attach_screen(&emulator->screen());
		view->resize(cols * 9, rows * 18);
		views.append(view);
	}

	/*
	 * And whether the glyphs are ALREADY shared between them, which is
	 * what sec 9.5 asks for by name.
	 *
	 * A size nothing has rasterised, then the first frame of each pane in
	 * turn. If every view rasterised its own glyphs, the four first
	 * frames would each pay the full cost; if the cache is shared, the
	 * first pane pays and the other three do not. That is the whole
	 * question, and it is answerable in four numbers.
	 */
	QPixmap probe(views.first()->size());
	QList<double> first_frames;
	for (bssh_terminal_view *view : views) {
		view->set_font_id(QString(), 15);
	}
	for (bssh_terminal_view *view : views) {
		QElapsedTimer timer;
		timer.start();
		view->render(&probe);
		first_frames.append(double(timer.nsecsElapsed()) / 1e6);
	}

	qInfo("first frame per pane, cold font: %.2f, %.2f, %.2f, %.2f ms",
	       first_frames.at(0), first_frames.at(1),
	       first_frames.at(2), first_frames.at(3));

	report_paint("paint, four panes", 4, cols, rows, fastest_paint_ms(views, 20));

	qDeleteAll(views);
	qDeleteAll(emulators);
}

void emulator_bench::painting_with_a_cold_glyph_cache() {
	/*
	 * The measurement that answers the atlas question, and the reason
	 * these three exist rather than one.
	 *
	 * Qt rasterises a glyph once and keeps it, keyed by font and size, so
	 * a steady-state repaint never rasterises anything. The FIRST paint
	 * at a size nothing has drawn yet pays for every glyph in the sample;
	 * the ones after it pay for none. The gap between them is the whole
	 * of what a glyph atlas of our own could win back, and only on the
	 * frame that produces it.
	 *
	 * First-frame-versus-rest, rather than changing the font every frame,
	 * which is what this measured first and why it reported the cold case
	 * as FASTER than the warm one: a different point size is a different
	 * cell size, so a bigger font simply drew fewer cells into the same
	 * pixels. It was measuring the grid, not the cache.
	 */
	const int cols = 200;
	const int rows = 50;

	bssh_emulator emulator(cols, rows);
	emulator.feed(painting_sample(cols, rows));

	bssh_terminal_view view;
	view.attach_screen(&emulator.screen());

	/* A size nothing in this process has rasterised yet. */
	view.set_font_id(QString(), 13);
	view.resize(cols * 9, rows * 18);

	QPixmap target(view.size());

	QElapsedTimer timer;
	timer.start();
	view.render(&target);
	const double first = double(timer.nsecsElapsed()) / 1e6;

	double best = -1;
	for (int frame = 0; frame < 20; ++frame) {
		QElapsedTimer again;
		again.start();
		view.render(&target);
		const double ms = double(again.nsecsElapsed()) / 1e6;
		if (best < 0 || ms < best) best = ms;
	}

	report_paint("paint, first frame (cold)", 1, cols, rows, first);
	report_paint("paint, then warm", 1, cols, rows, best);

	qInfo("rasterisation costs %.2f ms of the first frame and nothing after it",
	       first - best);
}
