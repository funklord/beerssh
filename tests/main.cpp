#include <QApplication>
#include <QTemporaryDir>
#include <QtTest>

#include "conn_manager_test.h"
#include "emulator_bench.h"
#include "live_ssh_test.h"
#include "ui_shots.h"
#include "emulator_test.h"
#include "graphics_test.h"
#include "platform_test.h"
#include "preset_test.h"
#include "secret_store_test.h"
#include "profile_store_test.h"
#include "session_registry_test.h"
#include "tab_status_test.h"
#include "input_router_test.h"
#include "pty_session_test.h"
#include "layout_test.h"
#include "font_catalog_test.h"
#include "font_picker_test.h"
#include "preset_editor_test.h"
#include "screen_test.h"
#include "ssh_config_test.h"
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
	/*
	 * Point every standard location at a throwaway tree, BEFORE
	 * QApplication resolves any of them.
	 *
	 * Several tests build a bssh_main_window, which owns a
	 * bssh_profile_store constructed with no directory -- the app's real
	 * one. So the suite has been writing hosts, layout and device
	 * settings into whoever ran it. That was invisible while everything
	 * lived under ~/.config/beerssh; it stopped being invisible the day
	 * the host list moved to ~/.ssh, which is not a directory a test
	 * suite may write to under any circumstances.
	 *
	 * The live-SSH harness (tools/check-ssh.sh) sets up its own HOME and
	 * needs to keep it, so it says so and this leaves it alone.
	 */
	static QTemporaryDir sandbox;
	if (qEnvironmentVariableIsEmpty("BSSH_TEST_SSH_PORT") && sandbox.isValid()) {
		qputenv("HOME", sandbox.path().toUtf8());
		qputenv("XDG_CONFIG_HOME", (sandbox.path() + QStringLiteral("/config")).toUtf8());
		qputenv("XDG_DATA_HOME", (sandbox.path() + QStringLiteral("/data")).toUtf8());
	}

	QApplication app(argc, argv);

	/*
	 * `--bench` runs the throughput measurements INSTEAD of the suite
	 * (sec 12.3). They take seconds rather than milliseconds and report
	 * numbers that depend on what else the machine is doing, so they are
	 * asked for rather than run by default -- but they are compiled and
	 * linked either way, because a benchmark that has quietly stopped
	 * building is found at exactly the wrong moment.
	 *
	 * Stripped from argv before qExec sees it, which rejects options it
	 * does not know rather than ignoring them.
	 */
	QList<char *> forwarded;
	bool bench = false;
	bool live_ssh = false;
	bool shots = false;
	for (int i = 0; i < argc; ++i) {
		if (qstrcmp(argv[i], "--bench") == 0) {
			bench = true;
			continue;
		}
		/*
		 * The transport against a real server (sec 12.3). Opt-in for the
		 * same reason as --bench and for a stronger one: it needs an
		 * sshd, which tools/check-ssh.sh supplies. Everything else here
		 * runs against the fake.
		 */
		if (qstrcmp(argv[i], "--live-ssh") == 0) {
			live_ssh = true;
			continue;
		}
		/*
		 * `--shots` renders every screen to a PNG instead of running the
		 * suite (sec 12.3). Opt-in like the others, and for the same
		 * reason they are: it writes files and answers a question nobody
		 * asked on an ordinary `make test`.
		 */
		if (qstrcmp(argv[i], "--shots") == 0) {
			shots = true;
			continue;
		}
		forwarded.append(argv[i]);
	}

	int forwarded_argc = int(forwarded.size());
	char **forwarded_argv = forwarded.data();

	if (bench) {
		return run_test<emulator_bench>(forwarded_argc, forwarded_argv) == 0 ? 0 : 1;
	}

	if (live_ssh) {
		return run_test<live_ssh_test>(forwarded_argc, forwarded_argv) == 0 ? 0 : 1;
	}

	if (shots) {
		return run_test<bssh_ui_shots>(forwarded_argc, forwarded_argv) == 0 ? 0 : 1;
	}

	argc = forwarded_argc;
	argv = forwarded_argv;

	int failures = 0;
	failures += run_test<font_catalog_test>(argc, argv);
	failures += run_test<font_picker_test>(argc, argv);
	failures += run_test<preset_editor_test>(argc, argv);
	failures += run_test<screen_test>(argc, argv);
	failures += run_test<ssh_config_test>(argc, argv);
	failures += run_test<conn_manager_test>(argc, argv);
	failures += run_test<emulator_test>(argc, argv);
	failures += run_test<graphics_test>(argc, argv);
	failures += run_test<platform_test>(argc, argv);
	failures += run_test<preset_test>(argc, argv);
	failures += run_test<secret_store_test>(argc, argv);
	failures += run_test<profile_store_test>(argc, argv);
	failures += run_test<session_registry_test>(argc, argv);
	failures += run_test<tab_status_test>(argc, argv);
	failures += run_test<pty_session_test>(argc, argv);
	failures += run_test<input_router_test>(argc, argv);
	failures += run_test<layout_test>(argc, argv);
	failures += run_test<terminal_view_test>(argc, argv);

	/*
	 * Collapsed to 1: the count is the number of failed test FUNCTIONS
	 * summed across classes, and a total that happened to land on 256
	 * would exit 0 through a POSIX shell. Anything non-zero is a failure.
	 */
	return failures == 0 ? 0 : 1;
}
