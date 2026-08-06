#include <QAbstractButton>
#include <QDir>
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
#include "ui/preset_editor.h"
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

	bssh_preset_editor presets(&store, bssh_preset_editor::TOUCH);
	shoot(QStringLiteral("touch-preset-editor"), &presets, BSSH_PHONE_W, BSSH_PHONE_H);

	bssh_app_settings_view settings;
	shoot(QStringLiteral("touch-settings"), &settings, BSSH_PHONE_W, BSSH_PHONE_H);

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

	bssh_ssh_config_import import(&store);
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
	 * Every page, not just the one that opens. The touch preset editor
	 * sat on the front door's Presets page for as long as it did because
	 * nobody had looked past the first screen.
	 */
	QTextStream(stdout) << QStringLiteral("\nfront door pages, at %1x%2:\n")
	                               .arg(BSSH_DESKTOP_W).arg(BSSH_DESKTOP_H);

	for (int i = 0; i < pages->count(); ++i) {
		pages->setCurrentIndex(i);
		QTest::qWait(50);

		const QString path = shot_dir() +
		                      QStringLiteral("/desktop-page-%1.png").arg(i, 2, 10, QLatin1Char('0'));
		front_door.grab().save(path);

		const QSize needed = pages->widget(i)->minimumSizeHint();
		QTextStream(stdout) << QStringLiteral("  %1  page needs %2x%3\n")
		                               .arg(path, -44).arg(needed.width()).arg(needed.height());
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
	};

	const sample_t samples[] = {
		{QStringLiteral("terminal-default"), QStringLiteral("default"), true},
		{QStringLiteral("terminal-default-no-floor"), QStringLiteral("default"), false},
		{QStringLiteral("terminal-solarized-dark"), QStringLiteral("solarized-dark"), true},
		{QStringLiteral("terminal-solarized-light"), QStringLiteral("solarized-light"), true},
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
	const bssh_platform *platform = bssh_platform::instance();
	const int faces = platform
	        ? bssh_font_catalog::instance().load_bundled(platform->font_directory())
	        : 0;

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

	QTextStream(stdout) << QStringLiteral("\nterminal, in %1 (%2):\n")
	                               .arg(family,
	                                     faces > 0
	                                             ? QStringLiteral("%1 bundled faces").arg(faces)
	                                             : QStringLiteral("NO bundled fonts -- these "
	                                                               "are the system's, not the "
	                                                               "app's"));

	for (const sample_t &sample : samples) {
		bssh_emulator emulator(52, 12);
		emulator.feed(QByteArray(BSSH_SAMPLE));

		bssh_terminal_view view;
		view.attach_screen(&emulator.screen());
		view.set_colour_scheme(sample.scheme);
		view.set_raise_contrast(sample.raise);
		view.resize(52 * 10, 12 * 20);
		view.show();
		QTest::qWaitForWindowExposed(&view);
		QTest::qWait(50);

		const QString path = shot_dir() + QStringLiteral("/") + sample.name +
		                      QStringLiteral(".png");
		view.grab().save(path);

		QTextStream(stdout) << QStringLiteral("  %1  %2, contrast floor %3\n")
		                               .arg(path, -44)
		                               .arg(sample.scheme)
		                               .arg(sample.raise ? QStringLiteral("on")
		                                                  : QStringLiteral("off"));
		view.hide();
	}
}
