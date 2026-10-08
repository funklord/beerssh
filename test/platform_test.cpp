#include <QApplication>
#include <QLabel>
#include <QSettings>
#include <QtTest>
#include <QSignalSpy>

#include "hang_watchdog.h"
#include "platform/platform.h"
#include "model/app_settings.h"
#include <QTemporaryDir>
#include <QFile>
#include "ui/theme.h"
#include <QPalette>
#include "platform_test.h"
#include "phone_metrics.h"
#include "ui/main_window.h"
#include "ui/interface_scale.h"

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

void platform_test::the_hang_gate_fires_only_foreground_and_past_the_threshold() {
	const qint64 threshold = 30000;

	/* Foreground and well past: the one case that fires. */
	QVERIFY(bssh_hang_should_dump(true, 30000, threshold));
	QVERIFY(bssh_hang_should_dump(true, 45000, threshold));

	/* Foreground but not yet past: a slow moment, not a hang. */
	QVERIFY(!bssh_hang_should_dump(true, 29999, threshold));
	QVERIFY(!bssh_hang_should_dump(true, 0, threshold));

	/*
	 * Backgrounded, however long the stall: Qt suspends the loop when the
	 * app is put away, so this is the normal case, not a hang -- and the
	 * arm that, if it fired, would kill a live backgrounded session.
	 */
	QVERIFY(!bssh_hang_should_dump(false, 45000, threshold));
	QVERIFY(!bssh_hang_should_dump(false, 300000, threshold));
}

void platform_test::the_hang_gate_follows_the_lifecycle_not_the_last_beat() {
	const qint64 threshold = 30000;
	bssh_hang_state state;

	/* The control: foreground, and the loop stops beating -- a hang. */
	state.set_foreground(true, 0);
	state.beat(1000);
	QVERIFY(!state.should_dump(30999, threshold));
	QVERIFY(state.should_dump(31000, threshold));

	/*
	 * Put away. Qt stops timers at pause, so there is no beat after this
	 * one -- the case the old gate read as "foreground at the last beat"
	 * and dumped half a minute into every trip to the background.
	 */
	state.set_foreground(false, 2000);
	QVERIFY2(!state.should_dump(600000, threshold),
	          "a backgrounded app is not a hung one, however long it is away");

	/*
	 * Brought back after ten minutes. The loop has not beaten since the
	 * pause; a healthy resume must not be charged for the time it was away.
	 */
	state.set_foreground(true, 600000);
	QVERIFY2(!state.should_dump(600500, threshold),
	          "a resume after a long background read as a hang before its first beat");

	/* And a loop that wedges during that resume is the hang this is for. */
	QVERIFY(state.should_dump(630000, threshold));

	/* Beats after the resume keep it quiet. */
	state.beat(629000);
	QVERIFY(!state.should_dump(630000, threshold));
}

void platform_test::keeping_awake_is_asked_only_on_a_change() {
	bssh_platform *platform = bssh_platform::instance();
	QVERIFY(platform);
	QVERIFY(!platform->sleep_inhibited());

	QSignalSpy changed(platform, &bssh_platform::sleep_inhibition_changed);

	/*
	 * The window asks on every registry change -- a title, a bell -- so the
	 * same answer twice must not reach the desktop twice, or every line of
	 * output would take and drop an inhibitor.
	 */
	platform->set_sleep_inhibited(true);
	platform->set_sleep_inhibited(true);
	QCOMPARE(changed.count(), 1);
	QVERIFY(platform->sleep_inhibited());

	platform->set_sleep_inhibited(false);
	platform->set_sleep_inhibited(false);
	QCOMPARE(changed.count(), 2);
	QVERIFY(!platform->sleep_inhibited());
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
	window.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
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

void platform_test::the_caps_setting_survives_a_restart() {
	QTemporaryDir dir;

	/*
	 * sec 6.1's Caps->Ctrl, stored beside the key row and the theme in
	 * app.ini rather than on a preset: it is a fact about the keyboard in
	 * front of the user, not about the machine at the other end, so it
	 * must not change when they open a different host.
	 */
	{
		bssh_app_settings settings(dir.path());
		QVERIFY2(!settings.caps_as_ctrl(), "the default must change nothing");

		settings.set_caps_as_ctrl(true);
	}

	{
		bssh_app_settings reopened(dir.path());
		QVERIFY(reopened.caps_as_ctrl());
	}

	/* And it can be turned off again, which a write-once bug would hide. */
	{
		bssh_app_settings settings(dir.path());
		settings.set_caps_as_ctrl(false);
	}
	{
		bssh_app_settings reopened(dir.path());
		QVERIFY(!reopened.caps_as_ctrl());
	}
}

void platform_test::hiding_from_screenshots_is_not_promised_where_it_cannot_work() {
	bssh_platform *platform = bssh_platform::instance();

	/*
	 * sec 11 asks for FLAG_SECURE on Android and says in as many words
	 * that there is no portable desktop equivalent and not to promise one
	 * in the UI. X11 has no such concept, and Wayland's screencopy
	 * protocols offer no opt-out a client can rely on.
	 *
	 * So the desktop must ANSWER NO rather than accept the setting and do
	 * nothing. A security control that reports success while having no
	 * effect is worse than an absent one: it is the difference between a
	 * user knowing their terminal is in the screenshot and believing it
	 * is not.
	 */
	QVERIFY2(!platform->can_hide_from_screenshots(),
	          "the desktop cannot do this and must not claim to");

	/* And asking for it anyway is harmless rather than a crash. */
	platform->set_hide_from_screenshots(true);
	platform->set_hide_from_screenshots(false);

	/*
	 * The Android half -- FLAG_SECURE on the Activity's window -- cannot
	 * be reached from a desktop test binary, and cannot be verified off a
	 * device even there: what it governs is what the OS puts in the
	 * recents thumbnail. It is recorded as untested in sec 11 rather than
	 * claimed here.
	 */
	QTemporaryDir dir;
	{
		bssh_app_settings settings(dir.path());
		QVERIFY2(!settings.hide_from_screenshots(), "off by default: it restricts the user too");
		settings.set_hide_from_screenshots(true);
	}
	{
		bssh_app_settings reopened(dir.path());
		QVERIFY(reopened.hide_from_screenshots());
	}
}

void platform_test::the_focus_follows_mouse_setting_survives_a_restart() {
	QTemporaryDir dir;

	/*
	 * sec 8.3 asked for this to be off by default before it existed, and
	 * the default is the part worth pinning: a fresh install must behave
	 * the way every other terminal does.
	 */
	{
		bssh_app_settings settings(dir.path());
		QVERIFY2(!settings.focus_follows_mouse(), "off by default, per sec 8.3");

		settings.set_focus_follows_mouse(true);
	}
	{
		bssh_app_settings reopened(dir.path());
		QVERIFY(reopened.focus_follows_mouse());
	}
	{
		bssh_app_settings settings(dir.path());
		settings.set_focus_follows_mouse(false);
	}
	{
		bssh_app_settings reopened(dir.path());
		QVERIFY2(!reopened.focus_follows_mouse(), "and it can be turned off again");
	}
}

void platform_test::the_desktop_adds_no_interface_scale() {
	bssh_platform *platform = bssh_platform::instance();
	QVERIFY(platform);

	/*
	 * 1.0 is "already applied", not "unsupported": Qt scales the desktop's
	 * own font, so there is nothing left for this to add. Only Android,
	 * whose fontScale Qt ignores, returns anything else.
	 */
	QVERIFY2(qFuzzyCompare(platform->system_font_scale(), 1.0),
	          "the desktop must not double-apply the system font size");
}

void platform_test::the_interface_follows_the_system_font_size_live() {
	bssh_app_settings settings;
	const bool was = settings.honour_system_font_scale();
	settings.set_honour_system_font_scale(true);

	bssh_follow_interface_scale();
	const QFont start = QApplication::font();

	/*
	 * Put back however this test leaves -- a failed QVERIFY returns early,
	 * and a forced scale left behind enlarged the font for every test that
	 * ran after it, which turned one failure into seven.
	 */
	struct restore_t {
		bool honour;
		QFont font;
		~restore_t() {
			bssh_app_settings().set_honour_system_font_scale(honour);
			bssh_platform::force_system_font_scale(0);
			bssh_apply_interface_scale();
			bssh_platform::force_application_active(-1);

			/*
			 * And the font itself, directly: restoring only through the
			 * code under test leaves it enlarged whenever that code is
			 * what broke.
			 */
			QApplication::setFont(font);
		}
	} restore { was, start };
	const auto size = [](const QFont &font) {
		return font.pointSizeF() > 0 ? font.pointSizeF() : qreal(font.pixelSize());
	};
	QVERIFY(size(start) > 0);

	/*
	 * The font size is changed in Android's Settings, so the app is in
	 * the background while it changes and comes back after. The manifest
	 * handles fontScale itself, so nothing restarts the app -- which is
	 * how a change measured on the Fold went unseen until the next start.
	 */
	bssh_platform::force_application_active(0);
	bssh_platform::force_system_font_scale(1.5);
	bssh_platform::force_application_active(1);
	QVERIFY2(qAbs(size(QApplication::font()) - size(start) * 1.5) < 0.6,
	          qPrintable(QStringLiteral("%1 -> %2 at 1.5")
	                             .arg(size(start)).arg(size(QApplication::font()))));

	/* From the starting font every time: 1.5 then 1.2 is 1.2, not 1.8. */
	bssh_platform::force_application_active(0);
	bssh_platform::force_system_font_scale(1.2);
	bssh_platform::force_application_active(1);
	QVERIFY2(qAbs(size(QApplication::font()) - size(start) * 1.2) < 0.6,
	          qPrintable(QStringLiteral("%1 -> %2 at 1.2")
	                             .arg(size(start)).arg(size(QApplication::font()))));

	/*
	 * And the setting turned off puts the starting font back at once,
	 * through the path the toggle takes -- the window applying the
	 * device settings -- rather than at the next start.
	 */
	settings.set_honour_system_font_scale(false);
	{
		bssh_main_window window;
		window.apply_device_settings();
	}
	QCOMPARE(QApplication::font(), start);

}

void platform_test::the_font_scale_setting_survives_a_restart() {
	QTemporaryDir dir;

	/*
	 * On by default, because a user who enlarged their device text meant
	 * it -- the accessibility-respecting default (sec 9.5). The off state
	 * is for someone who wants this app's chrome at its own size.
	 */
	{
		bssh_app_settings settings(dir.path());
		QVERIFY2(settings.honour_system_font_scale(), "on by default, per sec 9.5");

		settings.set_honour_system_font_scale(false);
	}
	{
		bssh_app_settings reopened(dir.path());
		QVERIFY(!reopened.honour_system_font_scale());
	}
	{
		bssh_app_settings settings(dir.path());
		settings.set_honour_system_font_scale(true);
	}
	{
		bssh_app_settings reopened(dir.path());
		QVERIFY2(reopened.honour_system_font_scale(), "and it can be turned back on");
	}
}

void platform_test::the_ligatures_setting_survives_a_restart() {
	QTemporaryDir dir;

	/*
	 * Off by default, and the default is the argued position (sec 9.2): a
	 * ligature draws two characters as one shape while the grid counts
	 * two columns. On is the opt-in for a user who wants them.
	 */
	{
		bssh_app_settings settings(dir.path());
		QVERIFY2(!settings.font_ligatures(), "off by default, per sec 9.2");

		settings.set_font_ligatures(true);
	}
	{
		bssh_app_settings reopened(dir.path());
		QVERIFY(reopened.font_ligatures());
	}
	{
		bssh_app_settings settings(dir.path());
		settings.set_font_ligatures(false);
	}
	{
		bssh_app_settings reopened(dir.path());
		QVERIFY2(!reopened.font_ligatures(), "and it can be turned off again");
	}
}

void platform_test::the_key_remaps_survive_a_restart() {
	QTemporaryDir dir;

	{
		bssh_app_settings settings(dir.path());

		/* Every slot empty by default -- no remaps until a user sets one. */
		const QList<QPair<QString, QString>> fresh = settings.key_remaps();
		QCOMPARE(fresh.size(), bssh_app_settings::BSSH_KEY_REMAP_SLOTS);
		for (const QPair<QString, QString> &rule : fresh) {
			QVERIFY2(rule.first.isEmpty() && rule.second.isEmpty(),
			          "a fresh install remaps nothing");
		}

		QList<QPair<QString, QString>> remaps = fresh;
		remaps[0] = qMakePair(QStringLiteral("F13"), QStringLiteral("Esc"));
		remaps[2] = qMakePair(QStringLiteral("Home"), QStringLiteral("PgUp"));
		settings.set_key_remaps(remaps);
	}

	{
		bssh_app_settings reopened(dir.path());
		const QList<QPair<QString, QString>> remaps = reopened.key_remaps();
		QCOMPARE(remaps.at(0).first, QStringLiteral("F13"));
		QCOMPARE(remaps.at(0).second, QStringLiteral("Esc"));
		QCOMPARE(remaps.at(2).first, QStringLiteral("Home"));
		/* And an untouched slot stays empty -- a write does not fill them all. */
		QVERIFY(remaps.at(1).first.isEmpty());
	}
}

void platform_test::the_remote_cursor_shape_setting_survives_a_restart() {
	QTemporaryDir dir;

	/*
	 * On by default (sec 6.4): a program's DECSCUSR is honoured, the way
	 * vim's cursor works. Off is the opt-out for a user who wants one fixed
	 * cursor, and it has to stick.
	 */
	{
		bssh_app_settings settings(dir.path());
		QVERIFY2(settings.honour_remote_cursor_shape(), "on by default, per sec 6.4");
		settings.set_honour_remote_cursor_shape(false);
	}
	{
		bssh_app_settings reopened(dir.path());
		QVERIFY(!reopened.honour_remote_cursor_shape());
	}
	{
		bssh_app_settings settings(dir.path());
		settings.set_honour_remote_cursor_shape(true);
	}
	{
		bssh_app_settings reopened(dir.path());
		QVERIFY2(reopened.honour_remote_cursor_shape(), "and it can be turned back on");
	}
}

void platform_test::the_scrollbar_setting_survives_a_restart() {
	QTemporaryDir dir;

	{
		bssh_app_settings settings(dir.path());

		/*
		 * AUTOMATIC by default, which is the right-hand side where
		 * there is a pointer and nothing where there is not (sec 8.3).
		 * Konsole's own default is a visible bar; ours has to answer
		 * for a phone as well.
		 */
		QCOMPARE(settings.scrollbar(), bssh_app_settings::SCROLLBAR_AUTO);
		settings.set_scrollbar(bssh_app_settings::SCROLLBAR_LEFT);
	}

	{
		bssh_app_settings reopened(dir.path());
		QCOMPARE(reopened.scrollbar(), bssh_app_settings::SCROLLBAR_LEFT);
	}

	/*
	 * Written as a WORD, and read back as one. app.ini is meant to be
	 * opened in an editor (sec 10.6), and Konsole storing this exact
	 * setting as an integer is a small daily annoyance to anyone who
	 * does.
	 */
	QSettings file(dir.path() + QStringLiteral("/app.ini"), QSettings::IniFormat);
	QCOMPARE(file.value(QStringLiteral("appearance/scrollbar")).toString(),
	          QStringLiteral("left"));

	/* And a typo behaves as the default rather than refusing to start. */
	file.setValue(QStringLiteral("appearance/scrollbar"), QStringLiteral("riht"));
	file.sync();

	bssh_app_settings mistyped(dir.path());
	QCOMPARE(mistyped.scrollbar(), bssh_app_settings::SCROLLBAR_AUTO);
}

void platform_test::the_status_line_setting_survives_a_restart() {
	QTemporaryDir dir;

	{
		bssh_app_settings settings(dir.path());

		/*
		 * The session and its state by default -- not the QPA platform
		 * name, which is what this line used to open with and is a fact
		 * only a developer wants (sec 8.4).
		 */
		QCOMPARE(settings.status_line(), bssh_app_settings::STATUS_LINE_SESSION);
		settings.set_status_line(bssh_app_settings::STATUS_LINE_OFF);
	}

	bssh_app_settings reopened(dir.path());
	QCOMPARE(reopened.status_line(), bssh_app_settings::STATUS_LINE_OFF);
}

void platform_test::the_window_title_format_survives_a_restart() {
	QTemporaryDir dir;

	{
		bssh_app_settings settings(dir.path());
		QCOMPARE(settings.window_title(), QStringLiteral("%t - BeerSSH"));

		settings.set_window_title(QStringLiteral("%u@%h"));
	}

	{
		bssh_app_settings reopened(dir.path());
		QCOMPARE(reopened.window_title(), QStringLiteral("%u@%h"));

		/*
		 * Cleared means the DEFAULT, not an empty title: a window with
		 * no name is a blank row in the task switcher, and somebody who
		 * emptied the field by accident would have no way to tell their
		 * windows apart.
		 */
		reopened.set_window_title(QString());
	}

	bssh_app_settings cleared(dir.path());
	QCOMPARE(cleared.window_title(), QStringLiteral("%t - BeerSSH"));
}

void platform_test::a_chord_can_be_rebound_and_given_back() {
	QTemporaryDir dir;
	bssh_app_settings settings(dir.path());

	/*
	 * Absent and empty are different answers, and the difference is the
	 * whole feature: absent is the built-in chord, empty is one the user
	 * has handed back to the remote shell (sec 6.1). Leaning on
	 * QString's null-versus-empty to carry that through a file is leaning
	 * on something QSettings does not promise.
	 */
	QVERIFY(!settings.has_chord(QStringLiteral("copy")));

	settings.set_chord(QStringLiteral("copy"), QStringLiteral("Ctrl+Alt+C"));
	QVERIFY(settings.has_chord(QStringLiteral("copy")));
	QCOMPARE(settings.chord(QStringLiteral("copy")), QStringLiteral("Ctrl+Alt+C"));

	settings.set_chord(QStringLiteral("paste"), QString());
	QVERIFY2(settings.has_chord(QStringLiteral("paste")),
	          "a chord given back to the shell is a decision, not an absence");
	QVERIFY(settings.chord(QStringLiteral("paste")).isEmpty());
}

void platform_test::a_paste_is_questioned_only_when_it_carries_more_than_a_line() {
	/*
	 * sec 6.4 asked for this "by default (a setting)" and nothing had
	 * built it, so a paste carrying five commands ran all five.
	 */
	QTemporaryDir dir;
	{
		bssh_app_settings settings(dir.path());
		QVERIFY2(settings.warn_multiline_paste(),
		          "a paste that runs commands must be questioned unless told not to");

		settings.set_warn_multiline_paste(false);
	}
	{
		bssh_app_settings reopened(dir.path());
		QVERIFY(!reopened.warn_multiline_paste());
	}

	/* One line, however it ends, is the ordinary case and is not asked
	 * about -- a warning that fires on every paste is one people turn
	 * off, after which it protects nobody. */
	QVERIFY(!bssh_main_window::paste_needs_confirming(QStringLiteral("ls -l"), true));
	QVERIFY(!bssh_main_window::paste_needs_confirming(QStringLiteral("ls -l\n"), true));
	QVERIFY(!bssh_main_window::paste_needs_confirming(QString(), true));

	/* Two lines of content is the case that runs something unread. */
	QVERIFY(bssh_main_window::paste_needs_confirming(QStringLiteral("ls\nrm -rf /"), true));
	QVERIFY(bssh_main_window::paste_needs_confirming(QStringLiteral("ls\nrm -rf /\n"), true));

	/* A bare carriage return is Enter to a terminal too. */
	QVERIFY(bssh_main_window::paste_needs_confirming(QStringLiteral("ls\rwhoami"), true));

	/* And the setting really is what turns it off. */
	QVERIFY(!bssh_main_window::paste_needs_confirming(QStringLiteral("ls\nrm -rf /"), false));
}

/*
 * Tier 4, and why it needs a rung of its own.
 *
 * Trinity exposes no Qt 6 platform theme and runs no XDG portal, so the
 * hint answers Unknown and the portal cannot be asked -- and with nothing
 * integrated Qt hands the program its DEFAULT LIGHT PALETTE. The palette
 * rung then reports light while working perfectly: there is no dark
 * palette anywhere for it to find. A false sense of coverage rather than
 * a visible gap, which is why the file the desktop actually wrote has to
 * be read, and read BEFORE the palette rather than after it.
 *
 * The values below are the real ones, measured on the desktop that
 * prompted the rule: windowBackground=0,42,78 against
 * windowForeground=220,220,220.
 */
namespace {

QString write_kdeglobals(const QTemporaryDir &dir, const QString &name,
                          const QString &body) {
	const QString path = dir.filePath(name);
	QFile f(path);
	f.open(QIODevice::WriteOnly | QIODevice::Text);
	f.write(body.toUtf8());
	f.close();
	return path;
}

}  /* namespace */

void platform_test::the_desktops_own_colours_are_read_when_nothing_else_says() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = write_kdeglobals(dir, QStringLiteral("dark"),
	    QStringLiteral("[General]\n"
	                    "colorScheme=DarkBlue.kcsrc\n"
	                    "windowBackground=0,42,78\n"
	                    "windowForeground=220,220,220\n"));
	QCOMPARE(bssh_appearance_from_scheme_files({path}), BSSH_APPEARANCE_DARK);
}

/* The control. Without it a parser that always says dark would pass. */
void platform_test::a_light_scheme_in_the_same_format_reads_light() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = write_kdeglobals(dir, QStringLiteral("light"),
	    QStringLiteral("[General]\n"
	                    "windowBackground=255,255,255\n"
	                    "windowForeground=0,0,0\n"));
	QCOMPARE(bssh_appearance_from_scheme_files({path}), BSSH_APPEARANCE_LIGHT);
}

/*
 * The name is not a predicate. This desktop's scheme is called
 * DarkBlue.kcsrc and IS dark, which is luck; plenty of dark schemes are
 * not named for it and plenty of light ones would match a substring
 * test. So a file whose name says dark and whose colours say light must
 * read light, or the implementation is reading the wrong thing and
 * passing the case above by coincidence.
 */
void platform_test::the_colours_decide_and_the_scheme_name_does_not() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = write_kdeglobals(dir, QStringLiteral("misnamed"),
	    QStringLiteral("[General]\n"
	                    "colorScheme=DarkBlue.kcsrc\n"
	                    "windowBackground=255,255,255\n"
	                    "windowForeground=0,0,0\n"));
	QCOMPARE(bssh_appearance_from_scheme_files({path}), BSSH_APPEARANCE_LIGHT);
}

/*
 * kdeglobals carries per-application sections using the same key names.
 * Taking whichever came last would answer about some other program's
 * colours -- here a dark [General] followed by a light section, which
 * must still read dark.
 */
void platform_test::the_keys_are_taken_from_general_and_not_another_section() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = write_kdeglobals(dir, QStringLiteral("sectioned"),
	    QStringLiteral("[General]\n"
	                    "windowBackground=0,42,78\n"
	                    "windowForeground=220,220,220\n"
	                    "[konsole]\n"
	                    "windowBackground=255,255,255\n"
	                    "windowForeground=0,0,0\n"));
	QCOMPARE(bssh_appearance_from_scheme_files({path}), BSSH_APPEARANCE_DARK);
}

/*
 * Abstain rather than guess. The errors are not symmetric: a wrong light
 * answer is merely plain, a wrong dark one is unreadable text on a pale
 * background. So no readable file means no opinion -- NOT a coin toss,
 * and not a default of light dressed up as an answer.
 */
void platform_test::a_file_that_cannot_be_read_abstains_rather_than_guessing() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString absent = dir.filePath(QStringLiteral("no-such-file"));
	QCOMPARE(bssh_appearance_from_scheme_files({absent}), BSSH_APPEARANCE_UNKNOWN);
	QCOMPARE(bssh_appearance_from_scheme_files({}), BSSH_APPEARANCE_UNKNOWN);
}

/*
 * A file present but missing one of the pair says nothing, and the list
 * is an order of authority rather than a set: the next file gets to
 * answer. Both halves in one case because a parser that skipped the
 * first file for the wrong reason would still pass the second half.
 */
void platform_test::a_half_written_scheme_abstains_and_the_next_file_answers() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString half = write_kdeglobals(dir, QStringLiteral("half"),
	    QStringLiteral("[General]\n"
	                    "windowBackground=0,42,78\n"));
	QCOMPARE(bssh_appearance_from_scheme_files({half}), BSSH_APPEARANCE_UNKNOWN);

	const QString whole = write_kdeglobals(dir, QStringLiteral("whole"),
	    QStringLiteral("[General]\n"
	                    "windowBackground=0,42,78\n"
	                    "windowForeground=220,220,220\n"));
	QCOMPARE(bssh_appearance_from_scheme_files({half, whole}),
	          BSSH_APPEARANCE_DARK);
}

/*
 * The second dialect. A desktop with no dark-mode status is the normal
 * case, and each spells the statement differently: LXQt uses [Palette]
 * and #rrggbb where TDE uses [General] and decimal triples. Both
 * sessions are installed on the machine this was written on, and before
 * this the parser found NOTHING in an LXQt config -- not a wrong answer,
 * which would have been noticed, but silence, which reads as no opinion.
 */
void platform_test::an_lxqt_palette_is_read_though_it_spells_it_differently() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = write_kdeglobals(dir, QStringLiteral("pal-dark"),
	    QStringLiteral("[Palette]\n"
	                    "base_color=#282828\n"
	                    "window_color=#232323\n"
	                    "window_text_color=#e1e6e6\n"));
	QCOMPARE(bssh_appearance_from_scheme_files({path}), BSSH_APPEARANCE_DARK);
}

void platform_test::an_lxqt_light_palette_reads_light() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = write_kdeglobals(dir, QStringLiteral("pal-light"),
	    QStringLiteral("[Palette]\n"
	                    "window_color=#efefef\n"
	                    "window_text_color=#000000\n"));
	QCOMPARE(bssh_appearance_from_scheme_files({path}), BSSH_APPEARANCE_LIGHT);
}

/*
 * Of the twelve palettes LXQt ships, luminance classifies all twelve
 * correctly while EIGHT are named something that says nothing about
 * which they are. Here the file is called "Light" and holds Silver's
 * colours, which are dark -- so a substring test gets it backwards.
 */
void platform_test::an_lxqt_palette_file_named_light_holding_dark_reads_dark() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = write_kdeglobals(dir, QStringLiteral("Light"),
	    QStringLiteral("[Palette]\n"
	                    "window_color=#636464\n"
	                    "window_text_color=#f9f9f9\n"));
	QCOMPARE(bssh_appearance_from_scheme_files({path}), BSSH_APPEARANCE_DARK);
}

/*
 * **The applied palette lives in lxqt.conf, not in the library.** This
 * first read <data>/lxqt/palettes/<theme=>, wrong twice over: that file
 * is loaded only when palette_override is on, so it can describe a
 * palette nobody is using; and LXQt title-cases the name before building
 * the path, so seven of the twelve themes installed here -- ambiance,
 * dark, frost, kvantum, light, silver, system -- did not resolve at all.
 * The shipped /etc/xdg/lxqt/lxqt.conf says theme=frost, so a DEFAULT
 * install silently found nothing: the failure this rung removes.
 */
void platform_test::the_applied_lxqt_palette_is_read_from_lxqt_conf_itself() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString conf = write_kdeglobals(dir, QStringLiteral("lxqt.conf"),
	    QStringLiteral("[General]\n"
	                    "theme=frost\n"
	                    "icon_theme=oxygen\n"
	                    "[Palette]\n"
	                    "window_color=#232323\n"
	                    "window_text_color=#e1e6e6\n"
	                    "[Qt]\n"
	                    "style=Fusion\n"));
	/* theme= is lowercase and names no palette file; the answer is in
	 * this file regardless. */
	QCOMPARE(bssh_appearance_from_scheme_files({conf}), BSSH_APPEARANCE_DARK);
}
