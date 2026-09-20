#include "model/app_settings.h"

#include "platform/platform.h"

#include <QColor>

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

bool bssh_app_settings::store(const QString &key, const QVariant &value) {
	if (m_path.isEmpty()) return false;

	QSettings file(m_path, QSettings::IniFormat);
	file.setValue(key, value);

	/*
	 * Synced rather than left to QSettings' own timer. Two of these
	 * objects exist at once all the time -- the settings page holds one
	 * and the window reads another -- and a write that is still in
	 * memory is a setting the other one cannot see (sec 10.6).
	 */
	file.sync();

	/*
	 * And whether it landed. A device setting that silently fails to
	 * persist merely reverts at the next start; the orphaned-secret list
	 * is the one where silence is durable, because a credential that is
	 * never recorded is never swept.
	 */
	return file.status() == QSettings::NoError;
}

bssh_app_settings::key_row_visibility_t bssh_app_settings::key_row_visibility() const {
	return from_text(value(QStringLiteral("input/key_row"),
	                        QStringLiteral("auto")).toString());
}

void bssh_app_settings::set_key_row_visibility(key_row_visibility_t visibility) {
	store(QStringLiteral("input/key_row"), to_text(visibility));
}

namespace {

const char *const BSSH_KEYBOARD_KEYS[] = { "system", "built_in", "built_in_full" };
const int BSSH_KEYBOARD_COUNT = 3;

} /* namespace */

/*
 * The default follows the PLATFORM, which is the correction this setting
 * needed: it was SYSTEM everywhere.
 *
 * On a phone that was the wrong way round. The system keyboard has no Esc,
 * no Ctrl, no arrows and no function keys -- the four things a terminal is
 * mostly made of -- so a user who installs BeerSSH and taps the terminal
 * gets a keyboard that cannot type the characters the app exists for, and
 * has to find a setting to fix it. The keyboard sec 6.2 asks for is the
 * one a touch device should start with.
 *
 * It stays SYSTEM on a desktop for the same reason it is wrong on a phone:
 * there is a real keyboard already, and drawing one on screen would take a
 * third of the terminal to duplicate it.
 *
 * A stored value always wins, so nobody's choice is changed by this -- it
 * decides only what an installation that has never been asked does.
 */
bssh_app_settings::keyboard_t bssh_app_settings::default_keyboard() {
	const bssh_platform *platform = bssh_platform::instance();
	return (platform && platform->is_touch_primary()) ? KEYBOARD_BUILT_IN
	                                                   : KEYBOARD_SYSTEM;
}

bssh_app_settings::keyboard_t bssh_app_settings::keyboard() const {
	const QVariant stored = value(QStringLiteral("input/keyboard"), QVariant());
	if (!stored.isValid()) return default_keyboard();

	const QString text = stored.toString();
	for (int i = 0; i < BSSH_KEYBOARD_COUNT; ++i) {
		if (text == QLatin1String(BSSH_KEYBOARD_KEYS[i])) return keyboard_t(i);
	}

	/*
	 * An unreadable value reads as the platform's default rather than as
	 * SYSTEM. It used to read as SYSTEM with the argument that a mistyped
	 * word in app.ini must not hand a phone a keyboard it did not ask
	 * for -- but with the default the other way round on touch, that
	 * argument now says the opposite: a typo must not take the keyboard
	 * AWAY from a device whose only way to type is the one we draw.
	 */
	return default_keyboard();
}

void bssh_app_settings::set_keyboard(keyboard_t keyboard) {
	store(QStringLiteral("input/keyboard"), QLatin1String(BSSH_KEYBOARD_KEYS[int(keyboard)]));
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

bool bssh_app_settings::honour_remote_cursor_shape() const {
	return value(QStringLiteral("appearance/honour_remote_cursor_shape"), true).toBool();
}
void bssh_app_settings::set_honour_remote_cursor_shape(bool honour) {
	store(QStringLiteral("appearance/honour_remote_cursor_shape"), honour);
}

QString bssh_app_settings::theme() const {
	return value(QStringLiteral("appearance/theme"), QStringLiteral("system")).toString();
}

QString bssh_app_settings::keyboard_layout() const {
	return value(QStringLiteral("input/keyboard_layout"),
	              QStringLiteral("us-intl")).toString();
}

void bssh_app_settings::set_keyboard_layout(const QString &id) {
	store(QStringLiteral("input/keyboard_layout"), id);
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
	 *
	 * THE FLOOR IS 40 AND WAS 10, which is the same argument finishing
	 * its own sentence. sec 9.3 sets a 3:1 contrast floor for anything
	 * this program draws, and the dim is deliberately applied AFTER that
	 * floor so a user who wants it dimmer than the default can have it.
	 * What nobody decided was how much dimmer. Measured against pure
	 * black, which is what a dark scheme's background is:
	 *
	 *     100%  #ffffff  21.00:1      40%  #666666   3.66:1
	 *      70%  #b2b2b2   9.90:1      30%  #4c4c4c   2.45:1
	 *      50%  #7f7f7f   5.24:1      10%  #191919   1.19:1
	 *
	 * So the slider could be set to a QUARTER of this program's own
	 * minimum, and 10 was the value one of these configs actually held
	 * -- reported as "the text colours are dark, impossible to read",
	 * which at 1.19:1 is a literal description. 40 is the lowest step
	 * that still clears 3:1, so every position the slider can reach is
	 * one the rest of the program would accept.
	 *
	 * Clamping on READ is what matters: a config already holding 10
	 * comes back as 40 and the terminal is readable again on the next
	 * launch, without anybody having to find the setting that made it
	 * unreadable.
	 */
	return qBound(BSSH_TEXT_BRIGHTNESS_MIN,
	               value(QStringLiteral("appearance/text_brightness"), 100).toInt(), 100);
}

void bssh_app_settings::set_text_brightness(int percent) {
	store(QStringLiteral("appearance/text_brightness"),
	       qBound(BSSH_TEXT_BRIGHTNESS_MIN, percent, 100));
}

namespace {

/*
 * The keys, in one place. Sixteen numbered entries plus the three a
 * scheme actually owns -- named rather than indexed, because "colour 17"
 * in a config file tells a reader nothing about which one it is.
 */
QString ansi_key(int index) {
	return QStringLiteral("custom_colours/ansi%1").arg(index);
}

QRgb from_name(const QString &name, QRgb fallback) {
	const QColor colour(name);
	return colour.isValid() ? colour.rgb() : fallback;
}

} /* namespace */

bssh_colour_scheme_t bssh_app_settings::custom_scheme() const {
	const bssh_colour_scheme_catalog &catalog = bssh_colour_scheme_catalog::instance();

	/*
	 * Started from the classic set rather than from nothing, so an unset
	 * or half-written custom scheme is a mostly-standard one instead of a
	 * black screen. by_id("default") is the shipped palette, which is the
	 * same sixteen every scheme carries.
	 */
	bssh_colour_scheme_t scheme;
	if (const bssh_colour_scheme_t *base = catalog.by_id(QStringLiteral("default"))) {
		scheme = *base;
	}
	scheme.id = QStringLiteral("custom");
	scheme.display_name = QStringLiteral("Custom");
	scheme.licence = QStringLiteral("This device");

	for (int i = 0; i < 16; ++i) {
		scheme.ansi[i] = from_name(value(ansi_key(i), QString()).toString(), scheme.ansi[i]);
	}

	scheme.background = from_name(
	        value(QStringLiteral("custom_colours/background"), QString()).toString(),
	        scheme.background);
	scheme.foreground = from_name(
	        value(QStringLiteral("custom_colours/foreground"), QString()).toString(),
	        scheme.foreground);
	scheme.cursor = from_name(
	        value(QStringLiteral("custom_colours/cursor"), QString()).toString(),
	        scheme.cursor);

	scheme.dark = bssh_relative_luminance(scheme.background) < 0.5;

	return scheme;
}

QString bssh_app_settings::custom_key_row() const {
	return value(QStringLiteral("keyboard/custom_key_row"), QString()).toString();
}

void bssh_app_settings::set_custom_key_row(const QString &spec) {
	store(QStringLiteral("keyboard/custom_key_row"), spec);
}

void bssh_app_settings::set_custom_scheme(const bssh_colour_scheme_t &scheme) {
	for (int i = 0; i < 16; ++i) {
		store(ansi_key(i), QColor(scheme.ansi[i]).name());
	}

	store(QStringLiteral("custom_colours/background"), QColor(scheme.background).name());
	store(QStringLiteral("custom_colours/foreground"), QColor(scheme.foreground).name());
	store(QStringLiteral("custom_colours/cursor"), QColor(scheme.cursor).name());
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

bool bssh_app_settings::honour_system_font_scale() const {
	return value(QStringLiteral("appearance/honour_system_font_scale"), true).toBool();
}

void bssh_app_settings::set_honour_system_font_scale(bool honour) {
	store(QStringLiteral("appearance/honour_system_font_scale"), honour);
}

bool bssh_app_settings::font_ligatures() const {
	return value(QStringLiteral("appearance/font_ligatures"), false).toBool();
}

void bssh_app_settings::set_font_ligatures(bool ligatures) {
	store(QStringLiteral("appearance/font_ligatures"), ligatures);
}

QList<QPair<QString, QString>> bssh_app_settings::key_remaps() const {
	QList<QPair<QString, QString>> remaps;
	for (int i = 0; i < BSSH_KEY_REMAP_SLOTS; ++i) {
		const QString from =
		        value(QStringLiteral("remap/%1_from").arg(i), QString()).toString();
		const QString to =
		        value(QStringLiteral("remap/%1_to").arg(i), QString()).toString();
		remaps.append(qMakePair(from, to));
	}
	return remaps;
}

void bssh_app_settings::set_key_remaps(const QList<QPair<QString, QString>> &remaps) {
	for (int i = 0; i < BSSH_KEY_REMAP_SLOTS; ++i) {
		const QPair<QString, QString> rule = remaps.value(i);
		store(QStringLiteral("remap/%1_from").arg(i), rule.first);
		store(QStringLiteral("remap/%1_to").arg(i), rule.second);
	}
}

QStringList bssh_app_settings::orphaned_secrets() const {
	return value(QStringLiteral("secrets/orphaned"), QStringList()).toStringList();
}

bool bssh_app_settings::set_orphaned_secrets(const QStringList &keys) {
	return store(QStringLiteral("secrets/orphaned"), keys);
}
