#include <QApplication>
#include <QtTest>

#include "emulator_test.h"
#include "graphics_test.h"
#include "input_router_test.h"
#include "screen_test.h"
#include "terminal_view_test.h"

/*
 * One binary, every test class (project.md sec 12.3).
 *
 * QTEST_MAIN would generate a main() per class, which means one binary per
 * class and a build file that grows a target every time a test file
 * appears. Running them from one main keeps adding a test class down to
 * two lines: a header include and a run_test call below.
 *
 * QApplication rather than QCoreApplication because the widget tests need
 * one; under the offscreen QPA plugin it costs nothing and needs no
 * display.
 */
template <typename test_t>
static int run_test(int argc, char *argv[]) {
	test_t instance;
	return QTest::qExec(&instance, argc, argv);
}

int main(int argc, char *argv[]) {
	QApplication app(argc, argv);

	int failures = 0;
	failures += run_test<screen_test>(argc, argv);
	failures += run_test<emulator_test>(argc, argv);
	failures += run_test<graphics_test>(argc, argv);
	failures += run_test<input_router_test>(argc, argv);
	failures += run_test<terminal_view_test>(argc, argv);

	/*
	 * Collapsed to 1: the count is the number of failed test FUNCTIONS
	 * summed across classes, and a total that happened to land on 256
	 * would exit 0 through a POSIX shell. Anything non-zero is a failure.
	 */
	return failures == 0 ? 0 : 1;
}
