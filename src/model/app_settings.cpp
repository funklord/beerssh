#include "model/app_settings.h"

#include <QDir>
#include <QSettings>
#include <QStandardPaths>

namespace {

/*
 * Written as words rather than as 0/1/2. The file is meant to be opened
 * and edited (sec 10.6), and "key_row=never" says what it does while
 * "key_row=2" needs this source file to decode.
 */
QString to_text(bssh_app_settings::key_row_visibility_t visibility) {
	switch (visibility) {
	case bssh_app_settings::KEY_ROW_ALWAYS: return QStringLiteral("always");
	case bssh_app_settings::KEY_ROW_NEVER: return QStringLiteral("never");
	case bssh_app_settings::KEY_ROW_AUTO: break;
	}
	return QStringLiteral("auto");
}

bssh_app_settings::key_row_visibility_t from_text(const QString &text) {
	if (text.compare(QStringLiteral("always"), Qt::CaseInsensitive) == 0) {
		return bssh_app_settings::KEY_ROW_ALWAYS;
	}
	if (text.compare(QStringLiteral("never"), Qt::CaseInsensitive) == 0) {
		return bssh_app_settings::KEY_ROW_NEVER;
	}

	/*
	 * Anything else is auto, including a typo. A hand-edited file with
	 * "key_row=alwyas" in it should behave like the default rather than
	 * refuse to start, and auto is the answer that is right for most
	 * devices anyway.
	 */
	return bssh_app_settings::KEY_ROW_AUTO;
}

} /* namespace */

bssh_app_settings::bssh_app_settings(const QString &directory) {
	QString dir = directory;
	if (dir.isEmpty()) {
		dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
	}

	if (dir.isEmpty()) return;

	QDir().mkpath(dir);
	m_path = dir + QStringLiteral("/app.ini");
}

bssh_app_settings::key_row_visibility_t bssh_app_settings::key_row_visibility() const {
	if (m_path.isEmpty()) return KEY_ROW_AUTO;

	QSettings file(m_path, QSettings::IniFormat);
	return from_text(file.value(QStringLiteral("input/key_row"),
	                             QStringLiteral("auto")).toString());
}

void bssh_app_settings::set_key_row_visibility(key_row_visibility_t visibility) {
	if (m_path.isEmpty()) return;

	QSettings file(m_path, QSettings::IniFormat);
	file.setValue(QStringLiteral("input/key_row"), to_text(visibility));
	file.sync();
}

namespace {

const char *const BSSH_CONTEXT_MENU_KEYS[] = { "auto", "desktop", "touch" };

} /* namespace */

bssh_app_settings::context_menu_t bssh_app_settings::context_menu() const {
	if (m_path.isEmpty()) return CONTEXT_MENU_AUTO;

	QSettings file(m_path, QSettings::IniFormat);
	const QString text = file.value(QStringLiteral("input/context_menu"),
	                                 QStringLiteral("auto")).toString();

	for (int i = 0; i < 3; ++i) {
		if (text == QLatin1String(BSSH_CONTEXT_MENU_KEYS[i])) return context_menu_t(i);
	}
	return CONTEXT_MENU_AUTO;
}

void bssh_app_settings::set_context_menu(context_menu_t layout) {
	if (m_path.isEmpty()) return;

	QSettings file(m_path, QSettings::IniFormat);
	file.setValue(QStringLiteral("input/context_menu"),
	               QLatin1String(BSSH_CONTEXT_MENU_KEYS[int(layout)]));
	file.sync();
}

bool bssh_app_settings::caps_as_ctrl() const {
	if (m_path.isEmpty()) return false;

	QSettings file(m_path, QSettings::IniFormat);
	return file.value(QStringLiteral("input/caps_as_ctrl"), false).toBool();
}

void bssh_app_settings::set_caps_as_ctrl(bool caps_as_ctrl) {
	if (m_path.isEmpty()) return;

	QSettings file(m_path, QSettings::IniFormat);
	file.setValue(QStringLiteral("input/caps_as_ctrl"), caps_as_ctrl);
	file.sync();
}

bool bssh_app_settings::hide_from_screenshots() const {
	if (m_path.isEmpty()) return false;

	QSettings file(m_path, QSettings::IniFormat);
	return file.value(QStringLiteral("security/hide_from_screenshots"), false).toBool();
}

void bssh_app_settings::set_hide_from_screenshots(bool hide) {
	if (m_path.isEmpty()) return;

	QSettings file(m_path, QSettings::IniFormat);
	file.setValue(QStringLiteral("security/hide_from_screenshots"), hide);
	file.sync();
}

QString bssh_app_settings::theme() const {
	if (m_path.isEmpty()) return QStringLiteral("system");

	QSettings file(m_path, QSettings::IniFormat);
	return file.value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString();
}

void bssh_app_settings::set_theme(const QString &theme) {
	if (m_path.isEmpty()) return;

	QSettings file(m_path, QSettings::IniFormat);
	file.setValue(QStringLiteral("appearance/theme"), theme);
}

bool bssh_app_settings::raise_contrast() const {
	if (m_path.isEmpty()) return true;

	QSettings file(m_path, QSettings::IniFormat);
	return file.value(QStringLiteral("appearance/raise_contrast"), true).toBool();
}

void bssh_app_settings::set_raise_contrast(bool raise) {
	if (m_path.isEmpty()) return;

	QSettings file(m_path, QSettings::IniFormat);
	file.setValue(QStringLiteral("appearance/raise_contrast"), raise);
}
