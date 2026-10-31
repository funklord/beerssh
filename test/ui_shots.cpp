#include <QLabel>
#include <QAbstractButton>
#include <QDir>
#include <QScrollArea>
#include <QTreeWidget>
#include <QRegularExpression>
#include <QMenu>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QtTest>

#include "model/profile.h"
#include "platform/platform.h"
#include "model/profile_store.h"
#include "ui/app_settings_view.h"
#include "ui/font_picker.h"
#include "input/input_router.h"
#include "softkeys/key_row_layout.h"
#include "ui/layout_manager.h"
#include "softkeys/key_row.h"
#include "ui/pane_container.h"
#include "softkeys/keyboard.h"
#include "softkeys/keyboard_layout.h"
#include "ui/host_editor.h"
#include "ui/host_list.h"
#include "ui/licences_view.h"
#include "ui/password_prompt.h"
#include "ui/colour_editor.h"
#include "ui/term_features_editor.h"
#include "ui/preset_editor.h"
#include "fake_ssh_session.h"
#include "model/session.h"
#include "model/session_registry.h"
#include "ui/main_window.h"
#include "term/emulator.h"
#include "term/font_catalog.h"
#include "ui/session_dialog.h"
#include "ui/terminal_view.h"
#include "ui/ssh_config_import.h"
#include "ui_shots.h"
#include "phone_metrics.h"

namespace {

/*
 * Forces the arrangement for as long as it is in scope, so an early
 * return cannot leave the next run rendering the wrong one.
 */
struct forced_touch_t {
	explicit forced_touch_t(int touch) { bssh_platform::force_touch_primary(touch); }
	~forced_touch_t() { bssh_platform::force_touch_primary(-1); }
};

QString shot_dir() {
	QString dir = qEnvironmentVariable("BSSH_SHOT_DIR");
	if (dir.isEmpty()) dir = QStringLiteral("build-shots");
	QDir().mkpath(dir);
	return dir;
}

/*
 * A host with the kind of length people really have, in every field.
 *
 * An empty store and a three-letter hostname make every screen look fine,
 * which is the trap: the layout bugs this exists to show up all needed a
 * realistic string to appear at all.
 */
bssh_profile_t sample_profile() {
	bssh_profile_t profile;
	profile.name = QStringLiteral("build server (europe)");
	profile.host = QStringLiteral("build.internal.example.com");
	profile.user = QStringLiteral("nabbe.testing");
	profile.port = 2299;
	profile.identity_file = QStringLiteral("~/.ssh/id_ed25519_work");
	profile.proxy_jump = QStringLiteral("nabbe@bastion.internal.example.com:2222");
	return profile;
}

/*
 * Renders one surface and reports what it demanded.
 *
 * The number matters as much as the picture: a surface whose MINIMUM
 * exceeds the screen it is meant for is the shape most of the layout bugs
 * in this project had, and it is visible here before anybody scrolls.
 */
void shoot(const QString &name, QWidget *widget, int width, int height) {
	/*
	 * SHOWN before it is resized, and that order is the whole of this
	 * comment. Qt does not deliver a resizeEvent to a hidden widget, so
	 * resizing first and showing second set the geometry silently and
	 * everything a widget recomputes on resize kept the answer it had at
	 * construction.
	 *
	 * Measured on the main window: its status line elides against the
	 * room it has when the text is set and re-elides from the original
	 * on resizeEvent -- correct, and never reached here, so
	 * desktop-window.png showed "nabbe.testing..." across an 800px
	 * window that had acres of room. A picture of something the app does
	 * not do, which is this directory's own rule.
	 *
	 * The split fixture below had already met this and worked around it
	 * locally, resizing and showing before its drop. The workaround was
	 * right and the place was wrong: every shot goes through here.
	 */
	widget->show();
	QVERIFY(QTest::qWaitForWindowExposed(widget));
	widget->resize(width, height);
	QTest::qWait(50);

	const QSize needed = widget->minimumSizeHint();
	const bool fits = needed.width() <= width && needed.height() <= height;

	const QString path = shot_dir() + QStringLiteral("/") + name + QStringLiteral(".png");
	widget->grab().save(path);

	QTextStream(stdout) << QStringLiteral("  %1  %2x%3  needs %4x%5%6\n")
	                               .arg(path, -44)
	                               .arg(width).arg(height)
	                               .arg(needed.width()).arg(needed.height())
	                               .arg(fits ? QString() : QStringLiteral("   <-- DOES NOT FIT"));
	widget->hide();
}

} /* namespace */

void bssh_ui_shots::initTestCase() {
	/*
	 * Before anything is drawn. The candidate paths are relative to the
	 * BINARY and the test binary lives in build-tests/, so BSSH_FONT_DIR
	 * is what points it at the checkout -- `make shots` sets it.
	 *
	 * Without this the pictures are drawn in whatever monospace the
	 * machine has, which sec 8.3 already recorded once for the terminal
	 * and fixed there only: the load sat inside render_terminal(), the
	 * last pass to run, so every widget shot before it had the bundled
	 * set absent. The font picker showed DejaVu Sans Mono where the app
	 * would have chosen Iosevka Term on touch or JetBrains Mono on the
	 * desktop -- a picture of the font-choosing UI with the app's fonts
	 * missing, which is the least interpretable shot of the lot.
	 */
	const bssh_platform *platform = bssh_platform::instance();
	const int faces = platform
	        ? bssh_font_catalog::instance().load_bundled(platform->font_directory())
	        : 0;

	QTextStream(stdout) << QStringLiteral("\nfonts: %1\n")
	                               .arg(faces > 0
	                                     ? QStringLiteral("%1 bundled faces").arg(faces)
	                                     : QStringLiteral("NONE -- these are the system's, "
	                                                       "not the app's"));
}

/*
 * The whole of a scrolling surface, not the third of it a phone shows.
 *
 * The desktop pages have had this since the settings outgrew a screen.
 * The PHONE did not -- the form factor where a control that is too wide
 * actually breaks something -- so everything below the fold reached
 * Android having been looked at on a desktop or not at all.
 *
 * The CONTENT of the scroll area, never the page: a page that scrolls
 * reports a tiny minimum, because the scroll area is happy to be small,
 * and its height says nothing about how much there is to read.
 */
void shoot_whole_page(const QString &name, QWidget *surface) {
	QScrollArea *area = surface->findChild<QScrollArea *>();
	QWidget *content = area ? area->widget() : nullptr;
	if (!content) return;

	/*
	 * Lay the page out at the phone size and ASK the scroll area how wide
	 * its viewport came out, rather than guessing.
	 *
	 * This was BSSH_PHONE_DP - 20 until 2026-09-08, and the 20 was a fudge
	 * for a scrollbar nobody had measured. It cost a false report: the
	 * Keyboard combo was announced as clipping on the copyright holder's
	 * phone on the strength of a picture drawn at 300, where the device
	 * gives its content 306. Six pixels, and the finding was wrong.
	 *
	 * A width nobody measured is a width the picture cannot be trusted at,
	 * so the number is now taken from the widget that decides it and
	 * PRINTED beside the picture -- a shot whose width is in the log is one
	 * a reader can check against the phone, which is the whole argument for
	 * having a gallery at all.
	 */
	surface->resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	surface->show();
	QVERIFY(QTest::qWaitForWindowExposed(surface));
	QTest::qWait(50);

	const int width = area->viewport()->width();
	QVERIFY2(width > 0 && width <= BSSH_PHONE_DP,
	          qPrintable(QStringLiteral("%1: viewport came out %2 wide inside %3")
	                          .arg(name).arg(width).arg(BSSH_PHONE_DP)));

	const QSize was = content->size();

	/* Capped, so a page asking for something absurd makes a large
	 * picture rather than exhausting memory.
	 *
	 * And at the height the page needs AT THIS WIDTH: a note under a
	 * control in a labelled row wraps to the field's width, and
	 * sizeHint() asks nothing about width -- so the page came out short
	 * and the notes clipped, in a picture of something the app, whose
	 * scroll area asks heightForWidth(), never draws. */
	const int tall = content->hasHeightForWidth() ? content->heightForWidth(width) : 0;
	content->resize(width, qMin(qMax(tall, content->sizeHint().height()), 3000));
	QTest::qWait(50);

	const QString path = shot_dir() + QStringLiteral("/") + name
	                      + QStringLiteral("-full.png");
	content->grab().save(path);
	QTextStream(stdout) << QStringLiteral("  %1  the whole page (%2 wide, %3 tall)\n")
	                               .arg(path, -44).arg(width).arg(content->height());

	content->resize(was);
	surface->hide();
}

void bssh_ui_shots::render_touch_surfaces() {
	/*
	 * The TOUCH arrangement, forced.
	 *
	 * Fourteen widgets decide their shape from is_touch_primary(), and on
	 * a desktop build it is false -- so these pictures were the DESKTOP
	 * arrangement squeezed into 360px. The licences view looked broken in
	 * a way it is not: side by side at that width its text arrives eight
	 * characters wide, which is exactly the failure its own code already
	 * avoids by stacking, on a phone, where the answer is true.
	 *
	 * A picture that shows something the app never does is worse than no
	 * picture, in a tool whose whole argument is that you can trust it.
	 */
	const forced_touch_t arrangement(1);

	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));
	const bssh_profile_t profile = sample_profile();
	store.save(profile);

	QTextStream(stdout) << QStringLiteral("touch, at %1x%2:\n")
	                               .arg(BSSH_PHONE_DP).arg(BSSH_PHONE_TALL_DP);

	bssh_host_list list(&store);
	shoot(QStringLiteral("touch-host-list"), &list, BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);

	bssh_host_editor editor(&store);
	editor.set_profile(profile);
	shoot(QStringLiteral("touch-host-editor"), &editor, BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	shoot_whole_page(QStringLiteral("touch-host-editor"), &editor);

	bssh_preset_editor presets(&store, bssh_preset_editor::TOUCH);
	shoot(QStringLiteral("touch-preset-editor"), &presets, BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	shoot_whole_page(QStringLiteral("touch-preset-editor"), &presets);

	/*
	 * ONE SHOT PER SECTION, because that is what a phone shows now
	 * (sec 8.5). Rendering the whole page here would be a picture of a
	 * surface touch never displays -- this file's own rule, and the
	 * reason the stale screens were deleted from the gallery.
	 *
	 * The whole page is still built, and still shot: it is the DESKTOP's
	 * "This device" category, which desktop_pages() renders.
	 */
	const struct { bssh_app_settings_view::section_t which; const char *name; } sections[] = {
		{ bssh_app_settings_view::SECTION_KEYBOARD,   "touch-settings-keyboard" },
		{ bssh_app_settings_view::SECTION_APPEARANCE, "touch-settings-appearance" },
		{ bssh_app_settings_view::SECTION_TERMINAL,   "touch-settings-terminal" },
		{ bssh_app_settings_view::SECTION_PASSWORDS,  "touch-settings-passwords" },
		{ bssh_app_settings_view::SECTION_SHORTCUTS,  "touch-settings-shortcuts" },
	};
	for (const auto &one : sections) {
		bssh_app_settings_view section(one.which, nullptr);
		shoot(QString::fromLatin1(one.name), &section, BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
		shoot_whole_page(QString::fromLatin1(one.name), &section);
	}

	/*
	 * THE SETTINGS INDEX (sec 8.5), which is a main-window page rather
	 * than a widget of its own -- so it is shot through the window, the
	 * way desktop-window is, instead of being constructed standalone.
	 *
	 * It had no picture at all when it was built, which is the gap this
	 * gallery exists to close: a new surface nobody can look at is one
	 * whose layout is checked only by assertions, and sec 12.3c records
	 * what that misses.
	 */
	{
		bssh_main_window window;
		window.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
		window.show();
		if (QTest::qWaitForWindowExposed(&window)) {
			QWidget *index = window.findChild<QWidget *>(
			        QStringLiteral("settings-index"));
			QStackedWidget *pages = window.findChild<QStackedWidget *>();
			if (index && pages) {
				pages->setCurrentWidget(index);
				QTest::qWait(50);
				shoot(QStringLiteral("touch-settings-index"), &window,
				       BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
			}
		}
		window.hide();
	}

	/*
	 * The on-screen key row (sec 6.2), which nothing had ever rendered.
	 *
	 * It is the surface a phone user touches most and the only one in
	 * this app that exists ONLY for touch -- every other screen has a
	 * desktop form somebody looks at, so this one is seen on a device or
	 * not at all. It asked for 652px of a 360px screen for as long as
	 * sec 6.2's key list and its 48dp floor were both true, and that was
	 * found by measuring rather than by looking, because there was
	 * nothing to look at.
	 *
	 * Every layout, not just the default: they are different key SETS,
	 * and a row is a thin strip, so the whole catalogue costs five
	 * pictures of 360x64 and answers "does this layout hold what its
	 * name promises" in a way no assertion does.
	 */
	bssh_emulator key_emulator(80, 24);
	bssh_input_router key_router(&key_emulator);

	/*
	 * The built-in keyboards, which had no picture at all.
	 *
	 * A surface the user's thumbs live on, and until now nothing rendered
	 * it -- so its geometry was held by an assertion that every row adds
	 * to the grid's width and by nothing that could see whether the
	 * result looks like a keyboard. The full-size one especially: its
	 * widths are ANSI's in quarter keys, and "Shift is the right size" is
	 * exactly the claim sec 17 says a render answers and a number does
	 * not.
	 *
	 * Every group of both styles, because the page button is the whole
	 * design and a picture of page one says nothing about page two.
	 */
	struct keyboard_shot_t {
		sk_catalog::style_t style;
		const char *name;
	};
	const keyboard_shot_t keyboards[] = {
		{ sk_catalog::STYLE_COMPACT, "compact" },
		{ sk_catalog::STYLE_FULL,    "full" }
	};

	for (const keyboard_shot_t &which : keyboards) {
		const QList<sk_group_t> &groups =
		        sk_catalog::instance().all(which.style);

		for (int g = 0; g < groups.size(); ++g) {
			sk_keyboard keys(&key_router);
			keys.set_style(which.style);
			keys.set_group(g);

			const QString name = QStringLiteral("touch-keyboard-%1-%2")
			        .arg(QLatin1String(which.name), groups.at(g).id);
			shoot(name, &keys, BSSH_PHONE_DP, keys.sizeHint().height());

			/*
			 * And the full one again at a tablet's width, because a
			 * phone is not where it is meant to be used and a picture
			 * of it there answers the wrong question.
			 *
			 * 15u of keyboard plus the page button needs about 770dp
			 * to give every key sec 6.2's 48dp target; a 360dp phone
			 * gives each one about 21dp, which is why the labels
			 * elide there. This is the picture that says whether the
			 * proportions are right.
			 */
			if (which.style == sk_catalog::STYLE_FULL) {
				shoot(name + QStringLiteral("-wide"), &keys,
				       800, keys.sizeHint().height());
			}

			/*
			 * And once with AltGr armed, which is the state the third
			 * level exists for and the one no other picture shows.
			 *
			 * Idle, a cap reads its letter with the accented character
			 * small and dimmed in the corner; armed, it reads the
			 * accented character alone at full weight. That swap is a
			 * paint rather than a string (sk_key_cap), so a render is
			 * the only thing that can say whether it came out legible
			 * on a key this size -- which is sec 17's whole argument,
			 * and the reason the elided labels were found here and not
			 * in an assertion.
			 */
			if (groups.at(g).id.contains(QStringLiteral("letters"))
			    || groups.at(g).id.contains(QStringLiteral("main"))) {
				key_router.cycle_modifier(bssh_input_router::MOD_ALTGR);
				shoot(name + QStringLiteral("-altgr"), &keys,
				       which.style == sk_catalog::STYLE_FULL
				               ? 800
				               : BSSH_PHONE_DP,
				       keys.sizeHint().height());

				/* Armed, locked, off: leave the router as it was found. */
				key_router.cycle_modifier(bssh_input_router::MOD_ALTGR);
				key_router.cycle_modifier(bssh_input_router::MOD_ALTGR);
			}
		}
	}

	/*
	 * The custom row is born as a copy of Default (see the catalogue's
	 * constructor), so shooting it as found produced a picture
	 * BYTE-IDENTICAL to touch-key-row-default.png -- two of these images
	 * saying the same thing, and the one named "custom" saying nothing
	 * at all about what a custom row is. A reader browsing the directory
	 * would conclude the custom row IS the default.
	 *
	 * Given one written the way a person writes one, as words (sec 6.2),
	 * and with a `gap` in it: that token is reachable only here, so this
	 * is also the only picture in which it can be seen holding its
	 * space.
	 *
	 * Put back afterwards, like the modifier cycling above: the
	 * catalogue is process-wide and the next thing to ask it for a
	 * custom row must not get this one.
	 */
	const QList<sk_key_t> found_custom = [] {
		for (const sk_key_row_layout_t &layout
		      : sk_key_row_catalog::instance().all()) {
			if (layout.id == QStringLiteral("custom")) return layout.keys;
		}
		return QList<sk_key_t>();
	}();

	sk_key_row_catalog::instance().set_custom(
	        sk_parse_key_row(QStringLiteral("esc tab ctrl alt gap left down up right")));

	for (const sk_key_row_layout_t &layout : sk_key_row_catalog::instance().all()) {
		sk_key_row row(&key_router);
		row.set_layout(layout.id);

		shoot(QStringLiteral("touch-key-row-") + layout.id, &row,
		       BSSH_PHONE_DP, row.sizeHint().height());

		/*
		 * And the whole row, not the seven keys a phone shows.
		 *
		 * This surface's design is that it holds more keys than fit and
		 * scrolls sideways (sec 6.2), so a 360px picture shows the
		 * first seven of every layout -- and the first seven of
		 * "minimal" and of "default" are the same four keys and three
		 * arrows. Two layouts rendered identically while differing
		 * entirely, which is a picture answering a question it was not
		 * asked.
		 *
		 * The scroll area's CONTENT at its natural width, the same
		 * answer the scrolling pages got vertically.
		 */
		if (QScrollArea *area = row.findChild<QScrollArea *>()) {
			if (QWidget *keys = area->widget()) {
				const QSize was = keys->size();
				keys->resize(qMin(keys->sizeHint().width(), 3000),
				              keys->sizeHint().height());
				QTest::qWait(20);

				const QString path = shot_dir()
				        + QStringLiteral("/touch-key-row-") + layout.id
				        + QStringLiteral("-full.png");
				keys->grab().save(path);
				QTextStream(stdout)
				        << QStringLiteral("  %1  every key (%2 wide)\n")
				                   .arg(path, -44).arg(keys->width());

				keys->resize(was);
			}
		}
	}

	/* And the catalogue back as it was found. */
	if (!found_custom.isEmpty()) sk_key_row_catalog::instance().set_custom(found_custom);

	/*
	 * The same menu with a thumb in front of it (sec 6.4).
	 *
	 * Its claim is that touch changes the METRICS and nothing else: the
	 * same verbs in the same order, rows grown to sec 6.2's floor, and
	 * the chords dropped -- they tell somebody with no keyboard which
	 * key to press. Two pictures side by side are what make that claim
	 * checkable by a person; the assertion alongside them only counts
	 * rows.
	 */
	{
		bssh_main_window touch_window;
		QMenu *menu = touch_window.build_terminal_menu(&touch_window);
		menu->ensurePolished();
		menu->resize(menu->sizeHint());
		QTest::qWait(20);

		const QString path = shot_dir() + QStringLiteral("/touch-terminal-menu.png");
		menu->grab().save(path);
		QTextStream(stdout) << QStringLiteral("  %1  %2x%3\n")
		                               .arg(path, -44).arg(menu->width()).arg(menu->height());
		delete menu;
	}

	bssh_font_picker picker;
	shoot(QStringLiteral("touch-font-picker"), &picker, BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);

	bssh_password_prompt prompt(
	        QStringLiteral("nabbe.testing@build.internal.example.com:2299"), true);
	shoot(QStringLiteral("touch-password-prompt"), &prompt, BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);

	bssh_licences_view licences;
	shoot(QStringLiteral("touch-licences"), &licences, BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);

}

void bssh_ui_shots::render_desktop_surfaces() {
	const bssh_factory_reset_t factory_reset;
	/* And the desktop arrangement, said rather than assumed. */
	const forced_touch_t arrangement(0);

	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));
	store.save(sample_profile());

	QTextStream(stdout) << QStringLiteral("\ndesktop, at %1x%2:\n")
	                               .arg(BSSH_SMALL_DESKTOP_W).arg(BSSH_SMALL_DESKTOP_H);

	bssh_session_dialog front_door(&store);
	shoot(QStringLiteral("desktop-front-door"), &front_door,
	       BSSH_SMALL_DESKTOP_W, BSSH_SMALL_DESKTOP_H);

	bssh_preset_editor presets(&store, bssh_preset_editor::DESKTOP);
	shoot(QStringLiteral("desktop-preset-editor"), &presets,
	       BSSH_SMALL_DESKTOP_W, BSSH_SMALL_DESKTOP_H);

	/*
	 * The assembled WINDOW, which nothing rendered until now -- every
	 * desktop shot above is one widget on its own. So the menu bar, the
	 * tab strip and its "+" button, the terminal and the status line had
	 * never been seen together, which is the arrangement a user actually
	 * meets and the only place their spacing can be judged.
	 *
	 * Two sessions, because one hides the tab strip: a shot of a window
	 * with its tab bar deliberately absent says nothing about the tab
	 * bar.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_ssh_session::set_factory([](QObject *parent) -> bssh_ssh_session * {
		return new bssh_fake_ssh_session(parent);
	});

	bssh_main_window window;
	window.adopt_session(window.registry()->create(sample_profile()));

	bssh_profile_t second = sample_profile();
	second.name = QStringLiteral("web front end");
	second.host = QStringLiteral("web.internal.example.com");
	window.adopt_session(window.registry()->create(second));

	shoot(QStringLiteral("desktop-window"), &window, BSSH_SMALL_DESKTOP_W, BSSH_SMALL_DESKTOP_H);

	/*
	 * And the same window after "Detach to a Window", which is the one
	 * thing about this surface a number cannot answer: whether the
	 * session you were LOOKING at is the one that left.
	 *
	 * The item used to read the current TAB, and on a wide screen a
	 * split shows sessions as panes that are not in the bar -- so it
	 * acted on something else, or on nothing. Both look identical from
	 * the outside, which is why this is a picture: the source window
	 * should be left holding one session and the new one should hold
	 * the other, and only two images side by side say so.
	 *
	 * Triggered through the QAction the menu holds, which is exactly
	 * what clicking the menu does.
	 */
	window.split_beside_current();

	QAction *detach = nullptr;
	for (QAction *action : window.findChildren<QAction *>()) {
		if (action->text().contains(QStringLiteral("Detach"))) detach = action;
	}
	QVERIFY2(detach && detach->isEnabled(), "the Detach item is missing or refused");

	detach->trigger();

	bssh_main_window *torn = nullptr;
	for (QWidget *widget : QApplication::topLevelWidgets()) {
		bssh_main_window *other = qobject_cast<bssh_main_window *>(widget);
		if (other && other != &window) torn = other;
	}
	QVERIFY2(torn, "Detach produced no window");

	shoot(QStringLiteral("desktop-detach-source"), &window,
	       BSSH_SMALL_DESKTOP_W, BSSH_SMALL_DESKTOP_H);
	shoot(QStringLiteral("desktop-detach-torn-out"), torn,
	       BSSH_SMALL_DESKTOP_W, BSSH_SMALL_DESKTOP_H);

	delete torn;

	/*
	 * And a pane mid-drag, which is the state a user is in when they
	 * most need to be told something: the pointer has left, and nothing
	 * else on screen says which terminal is about to move. A net over it
	 * and a border around it, drawn over everything including the focus
	 * ring.
	 */
	window.adopt_session(window.registry()->create(sample_profile()));
	window.split_beside_current();

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY2(panes, "the window has no pane container");
	QVERIFY2(panes->views().size() >= 2, "the split produced fewer than two panes");
	panes->views().first()->set_being_dragged(true);

	shoot(QStringLiteral("desktop-pane-being-dragged"), &window,
	       BSSH_SMALL_DESKTOP_W, BSSH_SMALL_DESKTOP_H);

	panes->views().first()->set_being_dragged(false);

	/*
	 * And what the pointer carries while that net is up. QDrag::exec()
	 * cannot be driven headlessly, so the pixmap is rendered on its own
	 * -- which is the only way anybody sees it before shipping it.
	 */
	QLabel carried;
	carried.setPixmap(window.session_drag_pixmap(window.registry()->at(0)));
	carried.setContentsMargins(6, 6, 6, 6);
	shoot(QStringLiteral("desktop-session-drag-cursor"), &carried,
	       carried.sizeHint().width() + 12, carried.sizeHint().height() + 12);

	/*
	 * And at the size the window SAYS it can be.
	 *
	 * It reports a minimum of about 214x77, which cannot be true of a
	 * thing holding a menu bar, a tab strip, a terminal and a status
	 * line -- and a minimum that is too small is not a harmless number:
	 * sec 8.3 records that too-tall content makes Qt squeeze rows past
	 * their minimums until they draw on top of each other, which is what
	 * a window manager honouring this figure would produce. Rendered at
	 * its own claim, so the claim can be looked at rather than trusted.
	 */
	/*
	 * The terminal's context menu, in both arrangements.
	 *
	 * Sec 6.4's claim is that touch changes the METRICS and nothing else
	 * -- same verbs, same order, bigger rows and no chords printed. That
	 * is a claim about what a menu looks like, it has had a test since it
	 * was written, and it has never had a picture. The rows either clear
	 * sec 6.2's floor or they do not, and a menu whose items move is one
	 * nobody can use without reading it every time.
	 *
	 * Grabbed from the menu widget rather than popped up: a popup is a
	 * window of its own and the offscreen platform has nowhere to put it,
	 * but the widget lays itself out the same either way.
	 */
	{
		QMenu *desktop_menu = window.build_terminal_menu(&window);
		desktop_menu->ensurePolished();
		desktop_menu->resize(desktop_menu->sizeHint());
		QTest::qWait(20);

		const QString path = shot_dir()
		        + QStringLiteral("/desktop-terminal-menu.png");
		desktop_menu->grab().save(path);
		QTextStream(stdout) << QStringLiteral("  %1  %2x%3\n")
		                               .arg(path, -44)
		                               .arg(desktop_menu->width())
		                               .arg(desktop_menu->height());
		delete desktop_menu;
	}

	const QSize window_floor = window.minimumSizeHint();
	shoot(QStringLiteral("desktop-window-minimum"), &window,
	       window_floor.width(), window_floor.height());

	/*
	 * And SPLIT, which nothing had ever rendered.
	 *
	 * Panes are sec 8.2's whole feature -- the divider, the focused
	 * pane's border, the grid each pane ends up with -- and every picture
	 * of this app showed one terminal filling the window. The one part of
	 * the layout that can look wrong in a way no size check sees is two
	 * panes beside each other, and it was the part with no picture.
	 *
	 * Shown and laid out at the size it is shot at, which is not
	 * ceremony: the size class comes from a resizeEvent, and Qt does not
	 * deliver one to a hidden widget. A window resized while hidden
	 * keeps the previous shot's geometry, and everything the layout
	 * decides from width is then decided from the wrong number.
	 */
	window.resize(BSSH_SMALL_DESKTOP_W, BSSH_SMALL_DESKTOP_H);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	QTest::qWait(50);

	/*
	 * THE DROP IS GONE, and the reason is the picture it produced.
	 *
	 * This fixture dropped a third session to create the split. By the
	 * time it runs the window is ALREADY split -- the drag shot above
	 * calls split_beside_current() and asserts two panes -- and an
	 * 800px window is MEDIUM, which sec 8.1 caps at two. So the drop
	 * found no room, took drop_session's documented fallback, and
	 * show_session() replaced the intent with the dropped session
	 * alone: the split this picture is named for was destroyed by the
	 * line that was supposed to create it.
	 *
	 * Measured rather than reasoned about: at the drop the layout held
	 * width=800, max=2, intent=2, and afterwards intent=1 with four
	 * tabs. The saved image was one terminal filling the window, under a
	 * name promising two, for as long as the shot has existed.
	 *
	 * The assertion below is what stops that recurring, and it is the
	 * one the drag shot fifty lines up already carries. A picture is
	 * evidence only if something checked it was of the thing it is
	 * named for.
	 */
	bssh_pane_container *split_panes = window.findChild<bssh_pane_container *>();
	QVERIFY2(split_panes, "the window has no pane container");
	QVERIFY2(split_panes->views().size() >= 2,
	          "the split picture would show fewer than two panes");

	shoot(QStringLiteral("desktop-window-split"), &window,
	       BSSH_SMALL_DESKTOP_W, BSSH_SMALL_DESKTOP_H);

	while (shared->count() > 0) shared->close(shared->at(0));
	bssh_ssh_session::set_factory(nullptr);

	/*
	 * The custom palette editor (sec 9.3). Shot at its own size rather
	 * than the desktop's, because it is a dialog and its minimum is the
	 * measurement worth having -- nineteen swatches and their labels
	 * either fit in a sensible box or they do not.
	 */
	const bssh_colour_scheme_t custom =
	        *bssh_colour_scheme_catalog::instance().by_id(QStringLiteral("default"));
	bssh_colour_editor colours(custom);
	/*
	 * Its MINIMUM, not its sizeHint: the report below measures against
	 * minimumSizeHint, so shooting at the smaller of the two reported
	 * a dialog that does not fit inside itself.
	 */
	const QSize wanted = colours.minimumSizeHint();
	shoot(QStringLiteral("desktop-colour-editor"), &colours,
	       wanted.width(), wanted.height());

	/*
	 * sec 7.1's features dialog, on the same footing and for the same
	 * reason: it is a dialog, so its MINIMUM is the measurement worth
	 * having -- fourteen checkboxes and a note either fit in a sensible
	 * box or they do not.
	 *
	 * Shot with two turned off rather than bare, because the state is
	 * the thing a reader is looking at: a picture of every box ticked
	 * shows the list and nothing about what a reduced terminal looks
	 * like here.
	 */
	bssh_term_features_editor features(QStringLiteral("-sixel,-kitty-graphics"));
	const QSize features_wanted = features.minimumSizeHint();
	shoot(QStringLiteral("desktop-term-features"), &features,
	       features_wanted.width(), features_wanted.height());

	/*
	 * Fed a parsed config, because the surface's whole job is the
	 * ticklist and an empty one shows none of it.
	 *
	 * Shot bare, this dialog said "These hosts were found" over a blank
	 * box -- a state the app refuses to produce: run() reports an empty
	 * parse with a message and never opens the dialog at all. Another
	 * picture of something the app never does, and one that also happened
	 * to hide the only part worth looking at.
	 *
	 * The three entries are the cases sec 10.5 says a real config
	 * contains: an ordinary host, one carrying an IdentityFile, and one
	 * behind a jump host.
	 */
	QList<bssh_ssh_config_host_t> parsed;

	bssh_ssh_config_host_t plain;
	plain.alias = QStringLiteral("web");
	plain.hostname = QStringLiteral("web01.internal.example.com");
	plain.user = QStringLiteral("nabbe");
	plain.port = 22;
	parsed.append(plain);

	bssh_ssh_config_host_t keyed;
	keyed.alias = QStringLiteral("build");
	keyed.hostname = QStringLiteral("build.internal.example.com");
	keyed.user = QStringLiteral("nabbe.testing");
	keyed.port = 2299;
	keyed.identity_file = QStringLiteral("~/.ssh/id_ed25519_work");
	parsed.append(keyed);

	bssh_ssh_config_host_t jumped;
	jumped.alias = QStringLiteral("db");
	jumped.hostname = QStringLiteral("db01.internal.example.com");
	jumped.user = QStringLiteral("nabbe");
	jumped.port = 22;
	jumped.proxy_jump = QStringLiteral("nabbe@bastion.internal.example.com:2222");
	parsed.append(jumped);

	bssh_ssh_config_import import(&store);
	import.set_hosts(parsed);
	shoot(QStringLiteral("desktop-config-import"), &import,
	       BSSH_SMALL_DESKTOP_W, BSSH_SMALL_DESKTOP_H);

	/*
	 * And the front door at its own minimum, which is where a layout
	 * stops degrading gracefully and starts clipping.
	 */
	bssh_session_dialog squeezed(&store);
	const QSize floor = squeezed.minimumSizeHint();
	shoot(QStringLiteral("desktop-front-door-minimum"), &squeezed,
	       floor.width(), floor.height());

	bssh_licences_view licences;
	shoot(QStringLiteral("desktop-licences"), &licences,
	       BSSH_SMALL_DESKTOP_W, BSSH_SMALL_DESKTOP_H);

}

void bssh_ui_shots::render_front_door_pages() {
	const forced_touch_t arrangement(0);

	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));
	store.save(sample_profile());

	bssh_session_dialog front_door(&store);
	front_door.resize(BSSH_SMALL_DESKTOP_W, BSSH_SMALL_DESKTOP_H);
	front_door.show();
	QVERIFY(QTest::qWaitForWindowExposed(&front_door));

	QStackedWidget *pages = front_door.findChild<QStackedWidget *>();
	QVERIFY(pages);

	/*
	 * Driven through the CATEGORY TREE, not by setting the stack's index.
	 *
	 * Setting the index directly leaves the tree highlighting whatever it
	 * highlighted before -- so every one of these pictures showed
	 * "Session" selected beside a pane full of something else, which is a
	 * state the app cannot produce. A picture that shows something the
	 * app never does is worse than no picture, in a tool whose whole
	 * argument is that you can trust it; the same rule that made this
	 * pass force the touch arrangement rather than inherit it.
	 *
	 * Walking the tree also names the shots after the categories, so a
	 * reader knows which page they are looking at without counting.
	 */
	QTreeWidget *categories = front_door.findChild<QTreeWidget *>();
	QVERIFY(categories);

	QList<QTreeWidgetItem *> items;
	for (int i = 0; i < categories->topLevelItemCount(); ++i) {
		QTreeWidgetItem *top = categories->topLevelItem(i);
		items.append(top);
		for (int j = 0; j < top->childCount(); ++j) items.append(top->child(j));
	}
	QVERIFY(!items.isEmpty());

	/*
	 * Every page, not just the one that opens. The touch preset editor
	 * sat on the front door's Presets page for as long as it did because
	 * nobody had looked past the first screen.
	 */
	QTextStream(stdout) << QStringLiteral("\nfront door pages, at %1x%2:\n")
	                               .arg(BSSH_SMALL_DESKTOP_W).arg(BSSH_SMALL_DESKTOP_H);

	for (QTreeWidgetItem *item : items) {
		categories->setCurrentItem(item);
		QTest::qWait(50);

		QString slug = item->text(0).toLower();
		slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")),
		              QStringLiteral("-"));

		const QString path = shot_dir() + QStringLiteral("/desktop-page-")
		                      + slug + QStringLiteral(".png");
		front_door.grab().save(path);

		const QSize needed = pages->currentWidget()
		        ? pages->currentWidget()->minimumSizeHint()
		        : QSize();
		QTextStream(stdout) << QStringLiteral("  %1  page needs %2x%3\n")
		                               .arg(path, -44).arg(needed.width()).arg(needed.height());

		/*
		 * And the WHOLE page where it is taller than the window, which
		 * is the half nobody could look at.
		 *
		 * The shot above is the dialog, so it stops at 600 pixels and
		 * everything past the fold goes unseen -- which is exactly the
		 * failure the comment above this loop describes, arriving one
		 * level down. This project's own settings page grew a scrollbar
		 * control, a status-line control, a title format and eight
		 * rebindable chords in one sitting, and not one of them below
		 * the fold would have appeared in any picture.
		 *
		 * Capped, because a page that asks for something absurd should
		 * produce a large picture rather than exhaust memory.
		 */
		/*
		 * The CONTENT of the scroll area, not the page: a page that
		 * scrolls reports a tiny minimum -- 68x68 here -- because the
		 * scroll area is happy to be small, and its height says nothing
		 * about how much there is to read.
		 */
		QScrollArea *area = pages->currentWidget()
		        ? pages->currentWidget()->findChild<QScrollArea *>()
		        : nullptr;
		QWidget *content = area ? area->widget() : nullptr;

		if (content && content->sizeHint().height() > BSSH_SMALL_DESKTOP_H) {
			const QSize was = content->size();

			/* The width the scroll area actually gives it, for the
			 * reason shoot_whole_page() gives: a picture whose width
			 * nobody stated is evidence about the picture. */
			const int width = area->viewport()->width() > 0
			                   ? area->viewport()->width()
			                   : BSSH_SMALL_DESKTOP_W - 220;

			/* At the height for this width, as shoot_whole_page() says. */
			const int needed = content->hasHeightForWidth() ? content->heightForWidth(width) : 0;
			content->resize(width, qMin(qMax(needed, content->sizeHint().height()), 3000));
			QTest::qWait(50);

			const QString tall = shot_dir() + QStringLiteral("/desktop-page-")
			                      + slug + QStringLiteral("-full.png");
			content->grab().save(tall);
			QTextStream(stdout) << QStringLiteral("  %1  the whole page (%2 wide, %3 tall)\n")
			                               .arg(tall, -44).arg(width).arg(content->height());

			content->resize(was);
		}
	}

}

void bssh_ui_shots::render_terminal() {
	/*
	 * What a terminal has to get right, in one screen. Each line is here
	 * because getting it wrong is a bug this project has either had or
	 * spent a section of project.md avoiding.
	 */
	static const char *const BSSH_SAMPLE =
	        "\x1b[1mBeerSSH\x1b[0m  the sixteen colours:\r\n"
	        "\x1b[30m##\x1b[31m##\x1b[32m##\x1b[33m##\x1b[34m##\x1b[35m##\x1b[36m##\x1b[37m##"
	        "\x1b[90m##\x1b[91m##\x1b[92m##\x1b[93m##\x1b[94m##\x1b[95m##\x1b[96m##\x1b[97m##"
	        "\x1b[0m\r\n"
	        "\x1b[7minverse\x1b[0m \x1b[4munderline\x1b[0m \x1b[1mbold\x1b[0m "
	        "\x1b[2mfaint\x1b[0m\r\n"
	        "\r\n"
	        "box drawing:  \xe2\x94\x8c\xe2\x94\x80\xe2\x94\xac\xe2\x94\x80\xe2\x94\x90"
	        "  \xe2\x94\x9c\xe2\x94\x80\xe2\x94\xbc\xe2\x94\x80\xe2\x94\xa4"
	        "  \xe2\x96\x91\xe2\x96\x92\xe2\x96\x93\xe2\x96\x88\r\n"
	        "wide glyphs:  \xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e "
	        "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4\r\n"
	        "disambiguate: 0O1lI5S2Z {}[]() `\xc2\xb4\r\n"
	        "\r\n"
	        "\x1b[32mnabbe@build\x1b[0m:\x1b[34m~\x1b[0m$ ";

	struct sample_t {
		QString name;
		QString scheme;
		bool raise;
		int brightness;
	};

	const sample_t samples[] = {
		{QStringLiteral("terminal-default"), QStringLiteral("default"), true, 100},
		{QStringLiteral("terminal-default-no-floor"), QStringLiteral("default"), false, 100},
		{QStringLiteral("terminal-green-on-black"), QStringLiteral("green-on-black"), true, 100},
		{QStringLiteral("terminal-yellow-on-black"), QStringLiteral("yellow-on-black"), true, 100},
		{QStringLiteral("terminal-black-on-white"), QStringLiteral("black-on-white"), true, 100},

		/*
		 * The night knob, at a setting somebody would actually use. Worth
		 * a picture rather than a number: whether 50% is comfortable or
		 * murky is not a thing the code can tell you.
		 */
		{QStringLiteral("terminal-dimmed-50"), QStringLiteral("default"), true, 50},
		{QStringLiteral("terminal-dimmed-25"), QStringLiteral("default"), true, 25},
	};

	/*
	 * The bundled set, exactly as main() loads it, and said out loud.
	 *
	 * Without this the terminal shots are drawn in whatever monospace the
	 * machine happens to have -- so CJK arrives as tofu and the box
	 * drawing is whatever the system font thinks, neither of which is
	 * what the app does. A picture of a renderer is uninterpretable
	 * without knowing which font drew it, and the same trap as rendering
	 * the desktop arrangement under a "touch-" name.
	 */
	/*
	 * Asked of a view built exactly as the sample views below are, not
	 * re-derived from the catalog. family_for(QString()) answers with the
	 * fallback chain's first entry, while a view resolves an empty id
	 * through default_id() -- so the caption named DejaVu Sans Mono over
	 * pictures drawn in JetBrains Mono, which is the one thing this line
	 * exists to get right.
	 */
	const bssh_terminal_view probe;
	const QString family = probe.font_family();

	QTextStream(stdout) << QStringLiteral("\nterminal, in %1:\n").arg(family);

	for (const sample_t &sample : samples) {
		bssh_emulator emulator(52, 12);
		emulator.feed(QByteArray(BSSH_SAMPLE));

		bssh_terminal_view view;
		view.attach_screen(&emulator.screen());
		view.set_colour_scheme(sample.scheme);
		view.set_raise_contrast(sample.raise);
		view.set_text_brightness(sample.brightness);
		view.resize(52 * 10, 12 * 20);
		view.show();
		QVERIFY(QTest::qWaitForWindowExposed(&view));
		QTest::qWait(50);

		const QString path = shot_dir() + QStringLiteral("/") + sample.name +
		                      QStringLiteral(".png");
		view.grab().save(path);

		QTextStream(stdout) << QStringLiteral("  %1  %2, contrast floor %3, text at %4%\n")
		                               .arg(path, -44)
		                               .arg(sample.scheme)
		                               .arg(sample.raise ? QStringLiteral("on")
		                                                  : QStringLiteral("off"))
		                               .arg(sample.brightness);
		view.hide();
	}
}
