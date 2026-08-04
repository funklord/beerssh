#include <QAbstractButton>
#include <QDir>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QtTest>

#include "model/profile.h"
#include "model/profile_store.h"
#include "ui/app_settings_view.h"
#include "ui/font_picker.h"
#include "ui/host_editor.h"
#include "ui/host_list.h"
#include "ui/licences_view.h"
#include "ui/password_prompt.h"
#include "ui/preset_editor.h"
#include "ui/session_dialog.h"
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
}

void bssh_ui_shots::render_front_door_pages() {
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
