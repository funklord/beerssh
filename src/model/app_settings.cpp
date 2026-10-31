#include "model/app_settings.h"

#include "platform/platform.h"
#include "ui/pane_snap.h"

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

const char *const BSSH_KEYBOARD_KEYS[] = { "system", "built_in", "built_in_full",
                                            "built_in_split" };
const char *const BSSH_ARRANGEMENT_KEYS[] = { "auto", "tiled", "column" };
const int BSSH_ARRANGEMENT_COUNT = 3;
const char *const BSSH_TAB_POSITION_KEYS[] = { "top", "bottom", "left", "right", "hidden" };
const int BSSH_TAB_POSITION_COUNT = 5;
const int BSSH_KEYBOARD_COUNT = 4;

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

bssh_app_settings::arrangement_t bssh_app_settings::arrangement() const {
	const QString text = value(QStringLiteral("layout/arrangement"), QString()).toString();
	for (int i = 0; i < BSSH_ARRANGEMENT_COUNT; ++i) {
		if (text == QLatin1String(BSSH_ARRANGEMENT_KEYS[i])) return arrangement_t(i);
	}

	/*
	 * Unset or unreadable is AUTO, which is the behaviour this setting
	 * was added on top of: a typo in app.ini leaves the window arranged
	 * the way the device would have arranged it anyway.
	 */
	return ARRANGEMENT_AUTO;
}

void bssh_app_settings::set_arrangement(arrangement_t arrangement) {
	store(QStringLiteral("layout/arrangement"),
	       QString::fromLatin1(BSSH_ARRANGEMENT_KEYS[int(arrangement)]));
}

int bssh_app_settings::image_history_mb() const {
	const QVariant stored = value(QStringLiteral("terminal/image_history_mb"), QVariant());
	bool ok = false;
	const int megabytes = stored.toInt(&ok);
	if (!stored.isValid() || !ok) return 32;
	return qBound(0, megabytes, BSSH_MAX_IMAGE_HISTORY_MB);
}

void bssh_app_settings::set_image_history_mb(int megabytes) {
	store(QStringLiteral("terminal/image_history_mb"),
	       qBound(0, megabytes, BSSH_MAX_IMAGE_HISTORY_MB));
}

bssh_app_settings::tab_position_t bssh_app_settings::tab_position() const {
	const QString text = value(QStringLiteral("layout/tabs_tiled"), QString()).toString();
	for (int i = 0; i < BSSH_TAB_POSITION_COUNT; ++i) {
		if (text == QLatin1String(BSSH_TAB_POSITION_KEYS[i])) return tab_position_t(i);
	}
	return TABS_TOP;
}

void bssh_app_settings::set_tab_position(tab_position_t position) {
	store(QStringLiteral("layout/tabs_tiled"),
	       QString::fromLatin1(BSSH_TAB_POSITION_KEYS[int(position)]));
}

bool bssh_app_settings::menu_bar_shown() const {
	return value(QStringLiteral("layout/menu_bar"), true).toBool();
}

void bssh_app_settings::set_menu_bar_shown(bool shown) {
	store(QStringLiteral("layout/menu_bar"), shown);
}

int bssh_app_settings::column_snap_radius() const {
	const QVariant stored = value(QStringLiteral("layout/column_snap_radius"),
	                               QVariant());
	if (!stored.isValid()) return BSSH_COLUMN_SNAP_RADIUS;

	bool ok = false;
	const int pixels = stored.toInt(&ok);
	if (!ok) return BSSH_COLUMN_SNAP_RADIUS;

	return qBound(0, pixels, BSSH_MAX_COLUMN_SNAP_RADIUS);
}

void bssh_app_settings::set_column_snap_radius(int pixels) {
	store(QStringLiteral("layout/column_snap_radius"),
	       qBound(0, pixels, BSSH_MAX_COLUMN_SNAP_RADIUS));
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

bool bssh_app_settings::keyboard_grips() const {
	return value(QStringLiteral("keyboard/grips"), true).toBool();
}

void bssh_app_settings::set_keyboard_grips(bool shown) {
	store(QStringLiteral("keyboard/grips"), shown);
}

int bssh_app_settings::keyboard_split_width() const {
	return qMax(0, value(QStringLiteral("keyboard/split_width"), 0).toInt());
}

void bssh_app_settings::set_keyboard_split_width(int width_dp) {
	store(QStringLiteral("keyboard/split_width"), qMax(0, width_dp));
}

int bssh_app_settings::keyboard_height(bool full) const {
	const QString key = full ? QStringLiteral("keyboard/height_full")
	                         : QStringLiteral("keyboard/height_compact");
	return qMax(0, value(key, 0).toInt());
}

void bssh_app_settings::set_keyboard_height(bool full, int height_dp) {
	store(full ? QStringLiteral("keyboard/height_full")
	           : QStringLiteral("keyboard/height_compact"),
	      qMax(0, height_dp));
}

void bssh_app_settings::reset_keyboard_size() {
	set_keyboard_split_width(0);
	set_keyboard_height(false, 0);
	set_keyboard_height(true, 0);
}

bool bssh_app_settings::hide_from_screenshots() const {
	return value(QStringLiteral("security/hide_from_screenshots"), false).toBool();
}

void bssh_app_settings::set_hide_from_screenshots(bool hide) {
	store(QStringLiteral("security/hide_from_screenshots"), hide);
}

bool bssh_app_settings::focus_follows_mouse() const {
	return value(QStringLiteral("input/focus_follows_mouse"), true).toBool();
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

/*
 * Brightness and the contrast floor belong to this RUN, not to app.ini (the
 * holder, 2026-10-09: "I want the software to always start with full
 * brightness/contrast. It is not worth it if it's going to have this bug
 * all the time").
 *
 * The bug in question had come back three ways, each time from a value
 * that outlived the moment it was set in. A slider that went down to 10 and
 * a config that kept 10; the wheel walking the slider while scrolling the
 * settings page, written straight to disk; and a floor of 40 clamping the
 * stored 10 on read, which made it readable and still dim on every start.
 * Each was fixed where it was found and the dim came back by the next door,
 * because what made it a bug rather than a choice was always the same
 * thing: it survived a restart nobody associated with it.
 *
 * So the slider and the checkbox still work, for as long as the program
 * runs, and every start is at full brightness with the floor on. Kept in
 * the process rather than per settings object, so every window and every
 * reader agrees. forget_session_appearance() takes the old keys out of
 * app.ini, so a stored value cannot come back through a reader written
 * later.
 */
namespace {
int g_text_brightness = 100;
bool g_raise_contrast = true;
}

bool bssh_app_settings::raise_contrast() const {
	return g_raise_contrast;
}

void bssh_app_settings::set_raise_contrast(bool raise) {
	g_raise_contrast = raise;
}

int bssh_app_settings::text_brightness() const {
	return g_text_brightness;
}

/*
 * Bounded below at BSSH_TEXT_BRIGHTNESS_MIN: sec 9.3's 3:1 floor, measured
 * against black --
 *
 *     100%  #ffffff  21.00:1      40%  #666666   3.66:1
 *      70%  #b2b2b2   9.90:1      30%  #4c4c4c   2.45:1
 *      50%  #7f7f7f   5.24:1      10%  #191919   1.19:1
 *
 * -- so even within a run, no position is one the rest of the program
 * would refuse.
 */
void bssh_app_settings::set_text_brightness(int percent) {
	g_text_brightness = qBound(BSSH_TEXT_BRIGHTNESS_MIN, percent, 100);
}

void bssh_app_settings::forget_session_appearance() {
	if (m_path.isEmpty()) return;
	QSettings file(m_path, QSettings::IniFormat);
	file.remove(QStringLiteral("appearance/text_brightness"));
	file.remove(QStringLiteral("appearance/raise_contrast"));
	file.sync();
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

QMap<QString, QString> bssh_app_settings::custom_keyboard_layout() const {
	QMap<QString, QString> rows;

	/*
	 * Read as a GROUP rather than by asking for slots by name. The slot
	 * vocabulary belongs to softkeys, and a list of names here would be
	 * a second copy of it -- one that goes stale the first time a layout
	 * uses a slot this file has not heard of.
	 */
	if (m_path.isEmpty()) return rows;

	QSettings file(m_path, QSettings::IniFormat);
	file.beginGroup(QStringLiteral("keyboard_custom_layout"));
	for (const QString &key : file.childKeys()) {
		if (key == QStringLiteral("name")) continue;
		rows.insert(key, file.value(key).toString());
	}
	file.endGroup();

	return rows;
}

QString bssh_app_settings::custom_keyboard_layout_name() const {
	return value(QStringLiteral("keyboard_custom_layout/name"), QString()).toString();
}

void bssh_app_settings::set_custom_keyboard_layout(const QString &name,
                                                    const QMap<QString, QString> &rows) {
	if (m_path.isEmpty()) return;

	/*
	 * The whole group is replaced, not merged. A layout is one table: a
	 * slot removed from it has to disappear rather than survive from the
	 * last one and leave a keyboard nobody wrote.
	 */
	QSettings file(m_path, QSettings::IniFormat);
	file.beginGroup(QStringLiteral("keyboard_custom_layout"));
	file.remove(QString());
	if (!name.isEmpty()) file.setValue(QStringLiteral("name"), name);
	for (QMap<QString, QString>::const_iterator it = rows.cbegin();
	     it != rows.cend(); ++it) {
		file.setValue(it.key(), it.value());
	}
	file.endGroup();
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
	/*
	 * Hidden unless chosen (the holder, 2026-10-30): "connected" says
	 * nothing on a local terminal and little on a healthy one, and what
	 * matters -- disconnected, retrying -- is written over the terminal
	 * it is about instead (sec 8.7a).
	 */
	const QString text = value(QStringLiteral("appearance/status_line"),
	                            QStringLiteral("off")).toString();

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

bool bssh_app_settings::keep_sessions_on_close() const {
	const bssh_platform *platform = bssh_platform::instance();
	const bool touch = platform && platform->is_touch_primary();
	return value(QStringLiteral("window/keep_sessions_on_close"), touch).toBool();
}

void bssh_app_settings::set_keep_sessions_on_close(bool keep) {
	store(QStringLiteral("window/keep_sessions_on_close"), keep);
}

bool bssh_app_settings::offer_multiplexer_sessions() const {
	return value(QStringLiteral("terminal/offer_multiplexer_sessions"), true).toBool();
}

void bssh_app_settings::set_offer_multiplexer_sessions(bool offer) {
	store(QStringLiteral("terminal/offer_multiplexer_sessions"), offer);
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
