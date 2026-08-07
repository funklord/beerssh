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

QVariant bssh_app_settings::value(const QString &key, const QVariant &fallback) const {
	if (m_path.isEmpty()) return fallback;

	QSettings file(m_path, QSettings::IniFormat);
	return file.value(key, fallback);
}

bool bssh_app_settings::contains(const QString &key) const {
	if (m_path.isEmpty()) return false;

	QSettings file(m_path, QSettings::IniFormat);
	return file.contains(key);
}

void bssh_app_settings::store(const QString &key, const QVariant &value) {
	if (m_path.isEmpty()) return;

	QSettings file(m_path, QSettings::IniFormat);
	file.setValue(key, value);

	/*
	 * Synced rather than left to QSettings' own timer. Two of these
	 * objects exist at once all the time -- the settings page holds one
	 * and the window reads another -- and a write that is still in
	 * memory is a setting the other one cannot see (sec 10.6).
	 */
	file.sync();
}

bssh_app_settings::key_row_visibility_t bssh_app_settings::key_row_visibility() const {
	return from_text(value(QStringLiteral("input/key_row"),
	                        QStringLiteral("auto")).toString());
}

void bssh_app_settings::set_key_row_visibility(key_row_visibility_t visibility) {
	store(QStringLiteral("input/key_row"), to_text(visibility));
}

namespace {

const char *const BSSH_CONTEXT_MENU_KEYS[] = { "auto", "desktop", "touch" };

} /* namespace */

bssh_app_settings::context_menu_t bssh_app_settings::context_menu() const {
	const QString text = value(QStringLiteral("input/context_menu"),
	                            QStringLiteral("auto")).toString();

	for (int i = 0; i < 3; ++i) {
		if (text == QLatin1String(BSSH_CONTEXT_MENU_KEYS[i])) return context_menu_t(i);
	}
	return CONTEXT_MENU_AUTO;
}

void bssh_app_settings::set_context_menu(context_menu_t layout) {
	store(QStringLiteral("input/context_menu"),
	       QString::fromLatin1(BSSH_CONTEXT_MENU_KEYS[int(layout)]));
}

bool bssh_app_settings::caps_as_ctrl() const {
	return value(QStringLiteral("input/caps_as_ctrl"), false).toBool();
}

void bssh_app_settings::set_caps_as_ctrl(bool caps_as_ctrl) {
	store(QStringLiteral("input/caps_as_ctrl"), caps_as_ctrl);
}

bool bssh_app_settings::hide_from_screenshots() const {
	return value(QStringLiteral("security/hide_from_screenshots"), false).toBool();
}

void bssh_app_settings::set_hide_from_screenshots(bool hide) {
	store(QStringLiteral("security/hide_from_screenshots"), hide);
}

bool bssh_app_settings::focus_follows_mouse() const {
	return value(QStringLiteral("input/focus_follows_mouse"), false).toBool();
}

void bssh_app_settings::set_focus_follows_mouse(bool follows) {
	store(QStringLiteral("input/focus_follows_mouse"), follows);
}

QString bssh_app_settings::theme() const {
	return value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString();
}

void bssh_app_settings::set_theme(const QString &theme) {
	store(QStringLiteral("appearance/theme"), theme);
}

bool bssh_app_settings::raise_contrast() const {
	return value(QStringLiteral("appearance/raise_contrast"), true).toBool();
}

void bssh_app_settings::set_raise_contrast(bool raise) {
	store(QStringLiteral("appearance/raise_contrast"), raise);
}

int bssh_app_settings::text_brightness() const {
	/*
	 * Bounded on the way OUT as well as in. A hand-edited config saying 0
	 * would otherwise render an invisible terminal, and the first thing
	 * somebody does about an invisible terminal is not open the settings.
	 */
	return qBound(10, value(QStringLiteral("appearance/text_brightness"), 100).toInt(), 100);
}

void bssh_app_settings::set_text_brightness(int percent) {
	store(QStringLiteral("appearance/text_brightness"), qBound(10, percent, 100));
}

namespace {

/*
 * Words, not numbers, for the reason the key row's are words: app.ini is
 * meant to be opened and edited (sec 10.6), and "scrollbar=left" says
 * what it does where "scrollbar=2" needs this file to decode.
 *
 * Konsole stores this exact setting as an integer and it is a small
 * daily annoyance to anyone reading the file.
 */
const char *const BSSH_SCROLLBAR_KEYS[] = { "auto", "right", "left", "hidden" };
const char *const BSSH_STATUS_LINE_KEYS[] = { "session", "full", "off" };

/*
 * The default window title, in the same placeholder language Konsole,
 * xterm and tmux all grew: %t is what the session calls itself, which is
 * the OSC title the remote set or the profile name until it does.
 */
const char *const BSSH_DEFAULT_WINDOW_TITLE = "%t - BeerSSH";

} /* namespace */

bssh_app_settings::scrollbar_t bssh_app_settings::scrollbar() const {
	const QString text = value(QStringLiteral("appearance/scrollbar"),
	                            QStringLiteral("auto")).toString();

	for (int i = 0; i < 4; ++i) {
		if (text.compare(QLatin1String(BSSH_SCROLLBAR_KEYS[i]), Qt::CaseInsensitive) == 0) {
			return scrollbar_t(i);
		}
	}

	/* A typo behaves as the default rather than refusing to start. */
	return SCROLLBAR_AUTO;
}

void bssh_app_settings::set_scrollbar(scrollbar_t where) {
	store(QStringLiteral("appearance/scrollbar"),
	       QString::fromLatin1(BSSH_SCROLLBAR_KEYS[int(where)]));
}

bssh_app_settings::status_line_t bssh_app_settings::status_line() const {
	const QString text = value(QStringLiteral("appearance/status_line"),
	                            QStringLiteral("session")).toString();

	for (int i = 0; i < 3; ++i) {
		if (text.compare(QLatin1String(BSSH_STATUS_LINE_KEYS[i]), Qt::CaseInsensitive) == 0) {
			return status_line_t(i);
		}
	}

	return STATUS_LINE_SESSION;
}

void bssh_app_settings::set_status_line(status_line_t what) {
	store(QStringLiteral("appearance/status_line"),
	       QString::fromLatin1(BSSH_STATUS_LINE_KEYS[int(what)]));
}

QString bssh_app_settings::window_title() const {
	const QString format = value(QStringLiteral("appearance/window_title"),
	                              QString::fromLatin1(BSSH_DEFAULT_WINDOW_TITLE)).toString();

	/*
	 * An EMPTY format is not an empty title: a window with no name at all
	 * is one the task switcher lists as a blank row, and a user who
	 * cleared the field by accident would have no way to tell which
	 * window they were looking at. It means the default.
	 */
	return format.trimmed().isEmpty() ? QString::fromLatin1(BSSH_DEFAULT_WINDOW_TITLE)
	                                   : format;
}

void bssh_app_settings::set_window_title(const QString &format) {
	store(QStringLiteral("appearance/window_title"), format);
}

QString bssh_app_settings::chord(const QString &action) const {
	return value(QStringLiteral("chords/") + action, QString()).toString();
}

bool bssh_app_settings::has_chord(const QString &action) const {
	return contains(QStringLiteral("chords/") + action);
}

void bssh_app_settings::set_chord(const QString &action, const QString &binding) {
	store(QStringLiteral("chords/") + action, binding);
}

bool bssh_app_settings::warn_multiline_paste() const {
	return value(QStringLiteral("input/warn_multiline_paste"), true).toBool();
}

void bssh_app_settings::set_warn_multiline_paste(bool warn) {
	store(QStringLiteral("input/warn_multiline_paste"), warn);
}
