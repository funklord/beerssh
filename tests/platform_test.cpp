#include <QApplication>
#include <QtTest>

#include "platform/platform.h"
#include "model/app_settings.h"
#include <QTemporaryDir>
#include <QFile>
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

void platform_test::the_window_opens_on_the_front_door() {
	bssh_main_window window;

	/*
	 * The regression this exists for: the constructor's front-door block
	 * was deleted in an unrelated rewrite, so the app opened straight
	 * into an empty terminal on both platforms for eight commits. Nothing
	 * caught it because every other test drives the widgets directly --
	 * this one asserts on the ASSEMBLED window, which is the only place
	 * the omission was visible.
	 */
	QVERIFY(window.showing_front_door());

	/*
	 * The command-line-target case is deliberately NOT exercised here: it
	 * reaches a modal password dialog, and a test that can block is worse
	 * than a gap in coverage.
	 */
}

void platform_test::the_key_row_setting_survives_a_restart() {
	QTemporaryDir dir;

	/*
	 * sec 6.2 asks for three states, not a checkbox, and names the two
	 * cases that need them: a convertible laptop whose owner wants the
	 * row, and a tablet with a keyboard docked whose owner does not. The
	 * key row has supported all three since M3 and nothing ever set one,
	 * so AUTO was the only reachable mode.
	 */
	{
		bssh_app_settings settings(dir.path());
		QCOMPARE(settings.key_row_visibility(), bssh_app_settings::KEY_ROW_AUTO);

		settings.set_key_row_visibility(bssh_app_settings::KEY_ROW_NEVER);
	}

	{
		bssh_app_settings reopened(dir.path());
		QCOMPARE(reopened.key_row_visibility(), bssh_app_settings::KEY_ROW_NEVER);
	}

	/*
	 * Written as a word, because app.ini is meant to be opened and edited
	 * (sec 10.6): "key_row=never" says what it does and "key_row=2" needs
	 * the source to decode.
	 */
	QFile file(dir.path() + QStringLiteral("/app.ini"));
	QVERIFY(file.open(QIODevice::ReadOnly));
	QVERIFY(QString::fromUtf8(file.readAll()).contains(QStringLiteral("key_row=never")));

	/* A typo in a hand-edited file behaves like the default rather than
	 * refusing to start. */
	{
		QFile broken(dir.path() + QStringLiteral("/app.ini"));
		QVERIFY(broken.open(QIODevice::WriteOnly));
		broken.write("[input]\nkey_row=alwyas\n");
	}

	bssh_app_settings mistyped(dir.path());
	QCOMPARE(mistyped.key_row_visibility(), bssh_app_settings::KEY_ROW_AUTO);
}
