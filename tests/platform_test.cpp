#include <QApplication>
#include <QLabel>
#include <QtTest>

#include "platform/platform.h"
#include "model/app_settings.h"
#include <QTemporaryDir>
#include <QFile>
#include "ui/theme.h"
#include <QPalette>
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

void platform_test::going_out_of_sight_is_the_platforms_to_answer_for() {
	bssh_platform *platform = bssh_platform::instance();

	/*
	 * On desktop, out of sight means the window hides and the tray icon
	 * is the way back -- so the platform has nothing of its own to do and
	 * must say so, or the window never hides and "keep running" leaves it
	 * sitting there.
	 *
	 * The Android half -- moveTaskToBack, and true -- cannot be reached
	 * from here: it is a JNI call into an Activity that does not exist in
	 * a desktop test binary. It was checked on a device instead, which is
	 * also where the bug it fixes was found: hiding the window there left
	 * a blank Activity that relaunching could not recover.
	 */
	QVERIFY2(!platform->send_to_background(),
	          "the desktop must leave the hiding to the window that closed");

	/* And asking twice does not start meaning something else. */
	QVERIFY(!platform->send_to_background());
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

void platform_test::a_long_failure_does_not_widen_the_window() {
	bssh_main_window window;
	window.resize(360, 640);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	const int before = window.minimumSizeHint().width();

	/*
	 * One of the app's own messages, at the length the app's own messages
	 * reach. Made up text would only prove something about made up text.
	 */
	const QString reason = QStringLiteral(
	        "the jump host bastion.example.com is not a known host yet -- ssh to it "
	        "once so its key is recorded, then try again");
	QVERIFY(QMetaObject::invokeMethod(&window, "on_failed", Qt::DirectConnection,
	                                   Q_ARG(QString, reason)));

	QCOMPARE(window.minimumSizeHint().width(), before);

	QLabel *status = nullptr;
	for (QLabel *candidate : window.findChildren<QLabel *>()) {
		if (candidate->toolTip().contains(reason)) status = candidate;
	}
	QVERIFY2(status, "the failure must reach the status line");

	/*
	 * Shortened, not wrapped, and the whole thing still reachable: a
	 * status line that grows takes the space from the terminal, and one
	 * that silently drops the end tells the user less than it knows.
	 */
	QVERIFY2(status->text() != reason, "a long reason must be shortened to fit");
	QVERIFY2(status->toolTip().contains(reason), "the full reason must stay reachable");
	QVERIFY2(status->sizeHint().width() <= window.width(),
	          "the status line must never ask for more room than the window has");
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

void platform_test::the_system_scheme_is_never_unknown() {
	/*
	 * Qt's own hint is not enough on its own, which is the whole reason
	 * bssh_platform answers this. Measured on this machine: with no
	 * platform-theme plugin engaged QStyleHints reports Unknown while the
	 * XDG portal reports "prefer dark" -- so trusting Qt alone draws a
	 * light window on a dark desktop.
	 *
	 * What this asserts is the contract callers rely on: whatever the
	 * sources say, a caller gets a usable answer and never has to invent
	 * one for itself.
	 */
	const bssh_appearance_t resolved = bssh_theme::resolved_system_appearance();
	QVERIFY2(resolved == BSSH_APPEARANCE_LIGHT || resolved == BSSH_APPEARANCE_DARK,
	          "resolving the system scheme must always land on light or dark");

	const bssh_platform *platform = bssh_platform::instance();
	QVERIFY(platform);

	/* Stable between calls -- a theme that flickers is worse than a wrong one. */
	QCOMPARE(platform->system_appearance(), platform->system_appearance());
}

void platform_test::the_palettes_are_actually_light_and_dark() {
	const QPalette dark = bssh_theme::palette_for(BSSH_APPEARANCE_DARK);
	const QPalette light = bssh_theme::palette_for(BSSH_APPEARANCE_LIGHT);

	QVERIFY(dark.color(QPalette::Window).lightness() < 128);
	QVERIFY(light.color(QPalette::Window).lightness() >= 128);

	/* Text has to contrast with the window it sits on, in both. */
	QVERIFY(dark.color(QPalette::WindowText).lightness() > 128);
	QVERIFY(light.color(QPalette::WindowText).lightness() < 128);

	/*
	 * Disabled text needs its own colour in both. Qt derives one by
	 * blending toward the window colour, which on a dark palette lands
	 * close enough to the enabled colour that a greyed-out control looks
	 * enabled.
	 */
	for (const QPalette &p : { dark, light }) {
		QVERIFY(p.color(QPalette::Disabled, QPalette::WindowText) !=
		         p.color(QPalette::Active, QPalette::WindowText));
	}
}

void platform_test::an_explicit_mode_ignores_the_system() {
	QCOMPARE(bssh_theme::effective_appearance(bssh_theme::MODE_LIGHT), BSSH_APPEARANCE_LIGHT);
	QCOMPARE(bssh_theme::effective_appearance(bssh_theme::MODE_DARK), BSSH_APPEARANCE_DARK);

	/* System follows detection, whatever that turns out to be here. */
	QCOMPARE(bssh_theme::effective_appearance(bssh_theme::MODE_SYSTEM),
	          bssh_theme::resolved_system_appearance());

	/* Applying a mode really repaints, rather than only recording it. */
	bssh_theme::apply(bssh_theme::MODE_DARK);
	QVERIFY(QApplication::palette().color(QPalette::Window).lightness() < 128);
	bssh_theme::apply(bssh_theme::MODE_LIGHT);
	QVERIFY(QApplication::palette().color(QPalette::Window).lightness() >= 128);
}

void platform_test::the_theme_setting_survives_a_restart() {
	QTemporaryDir dir;

	{
		bssh_app_settings settings(dir.path());
		/* The default is to follow the desktop, not to pick one. */
		QCOMPARE(settings.theme(), QStringLiteral("system"));
		settings.set_theme(QStringLiteral("dark"));
	}

	bssh_app_settings reopened(dir.path());
	QCOMPARE(reopened.theme(), QStringLiteral("dark"));
	QCOMPARE(bssh_theme::from_text(reopened.theme()), bssh_theme::MODE_DARK);

	/* Unreadable text in the file means "follow the system", not a crash. */
	QCOMPARE(bssh_theme::from_text(QStringLiteral("chartreuse")), bssh_theme::MODE_SYSTEM);
	QCOMPARE(bssh_theme::to_text(bssh_theme::MODE_SYSTEM), QStringLiteral("system"));
}
