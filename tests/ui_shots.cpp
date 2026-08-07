#include <QAbstractButton>
#include <QDir>
#include <QScrollArea>
#include <QTreeWidget>
#include <QRegularExpression>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QtTest>

#include "model/profile.h"
#include "platform/platform.h"
#include "model/profile_store.h"
#include "ui/app_settings_view.h"
#include "ui/font_picker.h"
#include "ui/host_editor.h"
#include "ui/host_list.h"
#include "ui/licences_view.h"
#include "ui/password_prompt.h"
#include "ui/colour_editor.h"
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

namespace {

/*
 * The two shapes worth looking at, and the sizes the tests hold them to:
 * a narrow phone (sec 8.2's COMPACT class) and a small desktop window --
 * the half-screen column sec 8.2 names when it talks about tiling.
 */
const int BSSH_PHONE_W = 360;
const int BSSH_PHONE_H = 640;
const int BSSH_DESKTOP_W = 800;
const int BSSH_DESKTOP_H = 600;

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
	widget->resize(width, height);
	widget->show();
	QTest::qWaitForWindowExposed(widget);
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

	const QSize was = content->size();

	/* Capped, so a page asking for something absurd makes a large
	 * picture rather than exhausting memory. */
	content->resize(BSSH_PHONE_W - 20, qMin(content->sizeHint().height(), 3000));
	QTest::qWait(50);

	const QString path = shot_dir() + QStringLiteral("/") + name
	                      + QStringLiteral("-full.png");
	content->grab().save(path);
	QTextStream(stdout) << QStringLiteral("  %1  the whole page (%2 tall)\n")
	                               .arg(path, -44).arg(content->height());

	content->resize(was);
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
	                               .arg(BSSH_PHONE_W).arg(BSSH_PHONE_H);

	bssh_host_list list(&store);
	shoot(QStringLiteral("touch-host-list"), &list, BSSH_PHONE_W, BSSH_PHONE_H);

	bssh_host_editor editor(&store);
	editor.set_profile(profile);
	shoot(QStringLiteral("touch-host-editor"), &editor, BSSH_PHONE_W, BSSH_PHONE_H);
	shoot_whole_page(QStringLiteral("touch-host-editor"), &editor);

	bssh_preset_editor presets(&store, bssh_preset_editor::TOUCH);
	shoot(QStringLiteral("touch-preset-editor"), &presets, BSSH_PHONE_W, BSSH_PHONE_H);
	shoot_whole_page(QStringLiteral("touch-preset-editor"), &presets);

	bssh_app_settings_view settings;
	shoot(QStringLiteral("touch-settings"), &settings, BSSH_PHONE_W, BSSH_PHONE_H);

	shoot_whole_page(QStringLiteral("touch-settings"), &settings);

	bssh_font_picker picker;
	shoot(QStringLiteral("touch-font-picker"), &picker, BSSH_PHONE_W, BSSH_PHONE_H);

	bssh_password_prompt prompt(
	        QStringLiteral("nabbe.testing@build.internal.example.com:2299"), true);
	shoot(QStringLiteral("touch-password-prompt"), &prompt, BSSH_PHONE_W, BSSH_PHONE_H);

	bssh_licences_view licences;
	shoot(QStringLiteral("touch-licences"), &licences, BSSH_PHONE_W, BSSH_PHONE_H);

}

void bssh_ui_shots::render_desktop_surfaces() {
	/* And the desktop arrangement, said rather than assumed. */
	const forced_touch_t arrangement(0);

	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));
	store.save(sample_profile());

	QTextStream(stdout) << QStringLiteral("\ndesktop, at %1x%2:\n")
	                               .arg(BSSH_DESKTOP_W).arg(BSSH_DESKTOP_H);

	bssh_session_dialog front_door(&store);
	shoot(QStringLiteral("desktop-front-door"), &front_door,
	       BSSH_DESKTOP_W, BSSH_DESKTOP_H);

	bssh_preset_editor presets(&store, bssh_preset_editor::DESKTOP);
	shoot(QStringLiteral("desktop-preset-editor"), &presets,
	       BSSH_DESKTOP_W, BSSH_DESKTOP_H);

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

	shoot(QStringLiteral("desktop-window"), &window, BSSH_DESKTOP_W, BSSH_DESKTOP_H);

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
	       BSSH_DESKTOP_W, BSSH_DESKTOP_H);

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
	       BSSH_DESKTOP_W, BSSH_DESKTOP_H);

}

void bssh_ui_shots::render_front_door_pages() {
	const forced_touch_t arrangement(0);

	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));
	store.save(sample_profile());

	bssh_session_dialog front_door(&store);
	front_door.resize(BSSH_DESKTOP_W, BSSH_DESKTOP_H);
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
	                               .arg(BSSH_DESKTOP_W).arg(BSSH_DESKTOP_H);

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

		if (content && content->sizeHint().height() > BSSH_DESKTOP_H) {
			const QSize was = content->size();

			content->resize(content->width() > 0 ? content->width()
			                                      : BSSH_DESKTOP_W - 220,
			                 qMin(content->sizeHint().height(), 3000));
			QTest::qWait(50);

			const QString tall = shot_dir() + QStringLiteral("/desktop-page-")
			                      + slug + QStringLiteral("-full.png");
			content->grab().save(tall);
			QTextStream(stdout) << QStringLiteral("  %1  the whole page (%2 tall)\n")
			                               .arg(tall, -44).arg(content->height());

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
		QTest::qWaitForWindowExposed(&view);
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
