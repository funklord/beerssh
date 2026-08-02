#include <QApplication>
#include <QtTest>

#include "platform/platform.h"
#include "platform_test.h"
#include "ui/main_window.h"

void platform_test::desktop_platform_is_selected_and_named() {
	bssh_platform *platform = bssh_platform::instance();
	QVERIFY(platform != nullptr);

	/* The build picked one implementation; nothing chooses at runtime. */
	QVERIFY(platform->name().startsWith(QStringLiteral("desktop/")));
	QVERIFY(!platform->is_touch_primary());
}

void platform_test::keep_alive_controls_quit_on_last_window() {
	bssh_platform *platform = bssh_platform::instance();
	const bool original = QApplication::quitOnLastWindowClosed();

	/*
	 * The desktop half of sec 5.5, and the whole mechanism: with sessions
	 * live, closing the last window must not end the process.
	 */
	platform->set_keep_alive(true);
	QCOMPARE(QApplication::quitOnLastWindowClosed(), false);

	platform->set_keep_alive(false);
	QCOMPARE(QApplication::quitOnLastWindowClosed(), true);

	QApplication::setQuitOnLastWindowClosed(original);
}

void platform_test::session_count_and_notify_are_safe_without_a_tray() {
	bssh_platform *platform = bssh_platform::instance();

	/*
	 * CI has no system tray, and neither does a minimal desktop session.
	 * Neither call may crash or refuse to return there -- a terminal that
	 * dies because the desktop has no notification area is worse than one
	 * that silently skips the notification.
	 */
	platform->set_session_count(3);
	platform->notify(QStringLiteral("BeerSSH"), QStringLiteral("reconnecting"));
	platform->set_session_count(0);

	QVERIFY(true);
}

void platform_test::close_policy_asks_only_when_something_is_live() {
	/* Nothing connected: closing the window closes the app, full stop. */
	QCOMPARE(bssh_main_window::close_action(false, false), bssh_main_window::CLOSE_AND_QUIT);
	QCOMPARE(bssh_main_window::close_action(false, true), bssh_main_window::CLOSE_AND_QUIT);

	/*
	 * Live and never asked: ask. Vanishing into the tray unannounced
	 * loses work the user thought they had closed; silently killing a
	 * running build is worse.
	 */
	QCOMPARE(bssh_main_window::close_action(true, false), bssh_main_window::ASK_FIRST);

	/* Asked once and told to keep running: do not ask again. */
	QCOMPARE(bssh_main_window::close_action(true, true), bssh_main_window::CLOSE_TO_BACKGROUND);
}
