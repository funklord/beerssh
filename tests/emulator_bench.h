#ifndef BSSH_EMULATOR_BENCH_H
#define BSSH_EMULATOR_BENCH_H

#include <QObject>

/*
 * Throughput through bssh_emulator::feed(), in MB/s (project.md sec 12.3).
 *
 * This is where the numbers in sec 9.5 come from. They were measured with
 * a scratch harness twice over, which meant every figure in that section
 * cited a program that no longer existed -- a measurement nobody can
 * repeat is a claim, not evidence.
 *
 * NOT run by `make test`, and deliberately so: it takes seconds rather
 * than milliseconds, and a CI machine building three things at once
 * produces numbers that vary by more than the changes being measured. Run
 * it with `make bench`, on an otherwise idle machine, and compare against
 * a run of the same binary rather than against a figure in a document.
 *
 * It is BUILT by every test build even though it is not run, because a
 * benchmark that stops compiling is discovered at exactly the moment
 * someone needs it.
 *
 * Each case asserts on the resulting screen as well as timing it. A
 * benchmark that only measures cannot tell "faster" from "stopped doing
 * the work", and the mirror is exactly the sort of thing that gets faster
 * by being wrong.
 */
class emulator_bench : public QObject {
	Q_OBJECT

private slots:
	void plain_scrolling_output();
	void coloured_scrolling_output();
	void full_screen_repaint();
};

#endif /* BSSH_EMULATOR_BENCH_H */
