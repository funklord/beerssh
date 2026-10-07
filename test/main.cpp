#include <QApplication>
#include <QTemporaryDir>
#include <QtTest>

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

#include "platform/platform.h"
#include "command_line_test.h"
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
#include "host_key_test.h"
#include "input_router_test.h"
#include "pty_session_test.h"
#include "layout_test.h"
#include "font_catalog_test.h"
#include "font_download_test.h"
#include "font_picker_test.h"
#include "preset_editor_test.h"
#include "screen_test.h"
#include "ssh_config_test.h"
#include "sshd_test.h"
#include "sshd_window_test.h"
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
/*
 * A test that does not return must not be able to fill the disk, and that
 * is an incident rather than a precaution.
 *
 * Three copies of this binary were found orphaned to init in state R,
 * seven to ten hours old, with stdout and stderr redirected into log files
 * that had since been deleted. An unlinked file that is still open belongs
 * to no directory, so `du` could not see them and deleting them freed
 * nothing -- the space returns only when the last descriptor closes.
 * Between them they held 693 GB and took a 905 GB filesystem to 100% full,
 * which is what made the suite start reporting failures that were really
 * "no room to write".
 *
 * None of the three was started by `make test`; they were run directly.
 * That is why the bound lives here rather than in the Makefile. A wrapper
 * only guards the way somebody did not run it.
 *
 * Two limits, because the failure has two shapes. A test that spins in
 * silence is caught by the clock. One that spins printing is caught by the
 * output cap first and much sooner -- 530 GB of that 693 was one process
 * writing about 15 MB/s for nine hours, which passes any sane cap within
 * seconds.
 *
 * The cap reads the SIZE of stdout and stderr rather than counting what
 * gets written, so it costs nothing per line and still sees a test that
 * writes to the descriptor behind libc's back. It applies only when they
 * are regular files: a terminal has no size to blow, and a pipe's reader
 * is somebody else's problem.
 *
 * Either limit is off with 0, for running under a debugger, where a
 * process sitting still is the whole point.
 */
static const int watchdog_default_seconds = 300;
static const long long watchdog_default_output_mb = 256;

static std::mutex watchdog_lock;
static std::condition_variable watchdog_wake;
static std::thread watchdog_thread;
static std::string watchdog_class;
static std::chrono::steady_clock::time_point watchdog_deadline;
static bool watchdog_running = false;
static int watchdog_seconds = 0;

static int watchdog_env_seconds() {
	bool ok = false;
	const int value = qEnvironmentVariableIntValue("BSSH_TEST_TIMEOUT", &ok);
	return ok ? value : watchdog_default_seconds;
}

static long long watchdog_env_output_bytes() {
	bool ok = false;
	const int value = qEnvironmentVariableIntValue("BSSH_TEST_MAX_OUTPUT", &ok);
	const long long megabytes = ok ? value : watchdog_default_output_mb;
	return megabytes <= 0 ? 0 : megabytes * 1024 * 1024;
}

/*
 * Exits 124, the code timeout(1) uses, so anything that already tells a
 * timeout apart from a test failure needs no second rule for this one.
 *
 * _exit rather than abort: abort raises SIGABRT, and QtTest's handler for
 * a fatal signal forks gdb against itself -- the behaviour that stranded
 * five processes in state `t` and is switched off in `make test` for it.
 * A watchdog that reproduced one incident while preventing the other
 * would be no bargain.
 *
 * It does leave the sandbox QTemporaryDir behind, since no destructor
 * runs. That is a few INI files, weighed against the filesystem this
 * exists to protect.
 */
static void watchdog_abort(const char *why, const std::string &which) {
	std::fprintf(stderr, "\ntest: %s\n", why);
	if (!which.empty()) {
		std::fprintf(stderr, "test:   while running %s\n", which.c_str());
	}
	std::fprintf(stderr,
	              "test:   BSSH_TEST_TIMEOUT (seconds per class) and "
	              "BSSH_TEST_MAX_OUTPUT (MB) set the budgets; 0 disables\n");
	std::fflush(stderr);
	_exit(124);
}

static bool watchdog_output_over(long long cap_bytes) {
	if (cap_bytes <= 0) {
		return false;
	}
	const int watched[] = { STDOUT_FILENO, STDERR_FILENO };
	for (int fd : watched) {
		struct stat info;
		if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
			continue;
		}
		if (info.st_size > cap_bytes) {
			return true;
		}
	}
	return false;
}

static void watchdog_main(long long cap_bytes) {
	std::unique_lock<std::mutex> lock(watchdog_lock);
	while (watchdog_running) {
		/*
		 * A second, not the whole budget: the output cap is the limit
		 * that matters for the disk, and it has to be noticed while a
		 * class is still inside its time.
		 */
		watchdog_wake.wait_for(lock, std::chrono::seconds(1));
		if (!watchdog_running) {
			break;
		}
		const std::string which = watchdog_class;
		const bool expired = !which.empty()
		                     && std::chrono::steady_clock::now() > watchdog_deadline;
		lock.unlock();
		if (expired) {
			watchdog_abort("a test class ran past its time budget", which);
		}
		if (watchdog_output_over(cap_bytes)) {
			watchdog_abort("a test wrote past its output cap", which);
		}
		lock.lock();
	}
}

static void watchdog_start() {
	watchdog_seconds = watchdog_env_seconds();
	const long long cap_bytes = watchdog_env_output_bytes();
	if (watchdog_seconds <= 0 && cap_bytes <= 0) {
		return;
	}
	watchdog_running = true;
	watchdog_thread = std::thread(watchdog_main, cap_bytes);
}

static void watchdog_stop() {
	if (!watchdog_thread.joinable()) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(watchdog_lock);
		watchdog_running = false;
	}
	watchdog_wake.notify_all();
	watchdog_thread.join();
}

static void watchdog_enter(const char *name) {
	if (watchdog_seconds <= 0) {
		return;
	}
	std::lock_guard<std::mutex> lock(watchdog_lock);
	watchdog_class = name;
	watchdog_deadline = std::chrono::steady_clock::now()
	                    + std::chrono::seconds(watchdog_seconds);
}

static void watchdog_leave() {
	std::lock_guard<std::mutex> lock(watchdog_lock);
	watchdog_class.clear();
}

/* Started and stopped with the run, so every return below is covered. */
struct watchdog_guard {
	watchdog_guard() { watchdog_start(); }
	~watchdog_guard() { watchdog_stop(); }
};

template <typename test_t>
static int run_test(int argc, char *argv[]) {
	test_t instance;
	/*
	 * The class names itself through QMetaObject, so the budget covers
	 * every class with no second list to keep in step with the calls in
	 * main -- and a run that dies says which class it died in, which is
	 * the thing nobody could tell from 693 GB of deleted log.
	 */
	watchdog_enter(instance.metaObject()->className());
	const int failed = QTest::qExec(&instance, argc, argv);
	watchdog_leave();
	return failed;
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
	 * The live-SSH harness (tool/check-ssh.sh) sets up its own HOME and
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
	 * In front, as an app being used is: the offscreen platform never
	 * activates anything, and with "behind another program" counting as
	 * away (sec 8.3) every pane would otherwise be away for the whole run.
	 * The one test about that answer changes it and puts it back.
	 */
	bssh_platform::force_application_active(1);

	/*
	 * Before any test runs and after QApplication, which is the only
	 * thing above that can block: --bench, --live-ssh and --shots all
	 * run test classes too, and a runaway is not fussy about which
	 * entry point produced it.
	 */
	watchdog_guard watchdog;

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
		 * sshd, which tool/check-ssh.sh supplies. Everything else here
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
	failures += run_test<font_download_test>(argc, argv);
	failures += run_test<font_picker_test>(argc, argv);
	failures += run_test<preset_editor_test>(argc, argv);
	failures += run_test<screen_test>(argc, argv);
	failures += run_test<ssh_config_test>(argc, argv);
	failures += run_test<sshd_test>(argc, argv);
	failures += run_test<sshd_window_test>(argc, argv);
	failures += run_test<command_line_test>(argc, argv);
	failures += run_test<conn_manager_test>(argc, argv);
	failures += run_test<emulator_test>(argc, argv);
	failures += run_test<graphics_test>(argc, argv);
	failures += run_test<platform_test>(argc, argv);
	failures += run_test<preset_test>(argc, argv);
	failures += run_test<secret_store_test>(argc, argv);
	failures += run_test<profile_store_test>(argc, argv);
	failures += run_test<session_registry_test>(argc, argv);
	failures += run_test<tab_status_test>(argc, argv);
	failures += run_test<host_key_test>(argc, argv);
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
