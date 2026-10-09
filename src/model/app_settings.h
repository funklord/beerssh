#ifndef BSSH_APP_SETTINGS_H
#define BSSH_APP_SETTINGS_H

#include <QList>
#include <QPair>
#include <QString>
#include <QMap>
#include <QStringList>

#include "term/colour_scheme.h"
#include <QVariant>

/*
 * bssh_app_settings -- the preferences that belong to the DEVICE rather
 * than to a host (project.md sec 6.2, sec 10.6).
 *
 * Everything a host can differ on lives in a preset, and that is most of
 * it. What is left is the handful of things that are properties of the
 * thing you are holding: whether the on-screen key row is wanted, and
 * whatever M11 adds. A preset cannot answer those -- the same host opened
 * on a phone and on a workstation wants different answers, and a preset
 * that carried one would be a preset that stops being portable (sec 10.2).
 *
 * Stored in app.ini beside hosts.ini and presets.ini, in the same plain
 * format and for the same reason: it is configuration, and configuration a
 * person can open in an editor is worth more than a database.
 */
/*
 * The dimmest the terminal's text may be set, as a percentage.
 *
 * Not a taste: at 40 per cent white on black is 3.66:1 and at 30 it is
 * 2.45:1, against sec 9.3's floor of 3:1. Below this the slider would
 * offer settings the rest of the program refuses to draw.
 */
const int BSSH_TEXT_BRIGHTNESS_MIN = 40;

/*
 * The most image history a terminal may be given, in MB: a gigabyte of
 * decoded pictures per terminal is past anything useful and short of what
 * a typo could otherwise ask for.
 */
const int BSSH_MAX_IMAGE_HISTORY_MB = 1024;

class bssh_app_settings {
public:
	/*
	 * sec 6.2's three-state key row setting. AUTO follows the primary
	 * input device, which is the right default and the wrong answer for
	 * exactly two people: someone with a convertible laptop who wants the
	 * row, and someone with a Bluetooth keyboard on a tablet who does
	 * not. Those two cases are why the spec asks for three states rather
	 * than a checkbox.
	 */
	/*
	 * How the terminal's context menu is laid out (sec 6.4).
	 *
	 * Three states rather than a checkbox, and AUTO first, for the same
	 * reason the key row and the theme have them: "follow this device" is
	 * a real answer and the one most people want, and it is not the only
	 * one. A convertible in tablet mode and a phone with a keyboard
	 * docked want opposite things from the same OS.
	 */
	enum context_menu_t {
		CONTEXT_MENU_AUTO,
		CONTEXT_MENU_DESKTOP,
		CONTEXT_MENU_TOUCH
	};

	context_menu_t context_menu() const;
	void set_context_menu(context_menu_t layout);

	enum key_row_visibility_t {
		KEY_ROW_AUTO,
		KEY_ROW_ALWAYS,
		KEY_ROW_NEVER
	};

	/*
	 * Which on-screen keyboard a touch device gets (sec 6.2).
	 *
	 * SYSTEM is what this app did until now: whatever keyboard Android
	 * supplies, with the key row above it carrying the Ctrl, Esc, Tab and
	 * arrows that no system keyboard has. It is the right answer wherever
	 * a system keyboard actually appears, and it keeps that keyboard's
	 * prediction, its languages and its layout -- none of which a
	 * terminal should be in the business of reimplementing.
	 *
	 * BUILT_IN is the keyboard this app draws. It exists because the
	 * other answer has a floor: where no system keyboard comes up, a row
	 * of modifiers has nothing to modify and the terminal cannot be typed
	 * into at all.
	 *
	 * TWO states and not the three the settings above carry, and that is
	 * a real difference rather than an oversight. AUTO means "follow the
	 * device", and the device cannot be asked this one: Android reports
	 * no capability for whether an IME will appear, so an AUTO here would
	 * be a guess wearing a detection's clothes. What it would have to
	 * guess is also the case that strands the user -- guess SYSTEM where
	 * none arrives and there is no way to type the setting back.
	 */
	enum keyboard_t {
		KEYBOARD_SYSTEM,

		/* The compact one: three groups of four rows, thumb-sized. */
		KEYBOARD_BUILT_IN,

		/*
		 * A full-size keyboard in a real keyboard's proportions -- every
		 * key a 104-key board has, at the width it has there, with the
		 * clusters that do not fit on a second page (sec 6.2).
		 */
		KEYBOARD_BUILT_IN_FULL,

		/*
		 * The compact keyboard in two halves beside the terminal, always.
		 * Compact and full turn split by themselves on a short wide
		 * screen (sec 6.2); this is for wanting it everywhere.
		 */
		KEYBOARD_BUILT_IN_SPLIT
	};

	/*
	 * What an installation that has never been asked gets. Public because
	 * the settings page shows which one that is, and a second copy of the
	 * rule would be a second thing to be wrong.
	 */
	static keyboard_t default_keyboard();

	/*
	 * How terminals occupy the window (sec 8.7's ARRANGEMENT dimension).
	 *
	 * sec 8.7 says the idiom picks the default "and then lets go":
	 * mobile defaults to a column and desktop to tiles, and either can
	 * be set to anything -- for a stated reason, that "`tiled` has to be
	 * runnable on a phone and `column` on a desktop, or neither can be
	 * tested without two devices". Nothing could set it: the constructor
	 * chose from is_touch_primary() once and offered no way back.
	 *
	 * Only this dimension, deliberately. sec 8.7 asks that the three-way
	 * split land WITH the feature that makes a dimension diverge and
	 * introduce "only the dimension that feature needs, not as a
	 * standalone refactor first" -- so the idiom and the metrics stay on
	 * is_touch_primary() exactly as they were.
	 */
	enum arrangement_t {
		/* Whatever this device's idiom picks -- the behaviour so far. */
		ARRANGEMENT_AUTO,

		/* sec 8.4's side by side, which a desktop opens with. */
		ARRANGEMENT_TILED,

		/* sec 8.7's stack, which a phone opens with. */
		ARRANGEMENT_COLUMN
	};

	arrangement_t arrangement() const;
	void set_arrangement(arrangement_t arrangement);

	/*
	 * Where the tab bar sits beside the panes, for the side-by-side
	 * arrangement (sec 8.7: the holder asked for tabs on any side, each
	 * arrangement with its own). Keyed per arrangement for that reason,
	 * though only side by side has one: a column shows every session as
	 * a pane, so it never shows a tab bar to place. Top, as it always
	 * was, until chosen.
	 */
	/*
	 * How much decoded image each terminal keeps after pictures scroll off
	 * the top, in MB (sec 7.1). What is on screen is never dropped; this
	 * bounds the history, oldest first. Per terminal and set by hand -- the
	 * holder's answer of 2026-09-26, with loading pictures back on demand
	 * named as the better thing later. 32 until changed, 0 keeps none.
	 */
	int image_history_mb() const;
	void set_image_history_mb(int megabytes);

	enum tab_position_t { TABS_TOP, TABS_BOTTOM, TABS_LEFT, TABS_RIGHT };
	tab_position_t tab_position() const;
	void set_tab_position(tab_position_t position);

	/*
	 * How close to the halfway line the column's divider is pulled onto
	 * it, in pixels (sec 8.7: "snap radius about 5-10px and
	 * configurable"). The default is BSSH_COLUMN_SNAP_RADIUS, which is 8
	 * and sits in that range.
	 *
	 * Zero is the meaningful end of the range rather than a degenerate
	 * value: it turns the magnet off, which is the reason somebody would
	 * come looking for this at all. Clamped to
	 * BSSH_MAX_COLUMN_SNAP_RADIUS rather than trusted, because a number
	 * in app.ini larger than the panes would pin the divider to the
	 * halfway line and read as a divider that cannot be moved.
	 */
	int column_snap_radius() const;
	void set_column_snap_radius(int pixels);

	keyboard_t keyboard() const;
	void set_keyboard(keyboard_t keyboard);

	explicit bssh_app_settings(const QString &directory = QString());

	/*
	 * sec 6.2's light/dark, stored as text rather than as the enum so the
	 * file stays readable and a hand-edited "dark" works. The vocabulary
	 * is bssh_theme's; this only remembers the choice.
	 */
	/*
	 * Whether to lift a drawn colour to sec 9.3's contrast floor when the
	 * scheme's own value falls under it. ON by default: the schemes are
	 * faithful reproductions and several of them, xterm's blue loudest,
	 * are genuinely hard to read as published.
	 */
	bool raise_contrast() const;
	void set_raise_contrast(bool raise);

	/*
	 * How bright terminal text is drawn, 10-100 (sec 9.3). Device-wide
	 * rather than per host: it answers "what is the room like", which is
	 * a property of where you are sitting and not of what you connected
	 * to.
	 */
	int text_brightness() const;
	void set_text_brightness(int percent);

	/*
	 * Text brightness and the contrast floor last only as long as the
	 * program runs; every start is at full brightness with the floor on.
	 * This takes the keys older versions stored out of app.ini, once, at
	 * startup -- a stored 10 was what kept bringing the dim back.
	 */
	void forget_session_appearance();

	/*
	 * The user's own palette, for the "custom" scheme (sec 9.3). Stored
	 * as #rrggbb strings rather than integers so the config file stays
	 * something a person can read and edit, which is most of the point of
	 * having an escape hatch at all.
	 *
	 * Reading one returns the classic ANSI values for anything unset, so
	 * a half-written custom scheme is a mostly-standard one rather than a
	 * black screen.
	 */
	bssh_colour_scheme_t custom_scheme() const;
	void set_custom_scheme(const bssh_colour_scheme_t &scheme);

	/*
	 * The custom key row, as the words sk_parse_key_row reads (sec 6.2).
	 *
	 * A string rather than a parsed row, because this is the layer that
	 * talks to app.ini and the softkeys catalogue is the layer that knows
	 * what a key is -- the same division the custom colour scheme keeps,
	 * and the reason neither of them reaches across it.
	 *
	 * Empty means the user has not composed one, which is different from
	 * having composed an empty one: the catalogue's "custom" entry starts
	 * as a copy of the default, so empty leaves that copy alone.
	 */
	QString custom_key_row() const;
	void set_custom_key_row(const QString &spec);

	/*
	 * The custom keyboard layout (sec 6.3), as one entry per XKB slot.
	 *
	 * A map rather than a string, unlike the key row: a layout is ~47
	 * rows and app.ini is meant to be opened and edited (sec 10.6), so
	 * it gets a section of its own where each line is a slot a person
	 * can find and change. Keyed by the slot name, valued as the four
	 * levels the keyboard catalogue parses.
	 *
	 * Empty means none has been written, which is different from one
	 * that failed: the catalogue refuses a layout that cannot type, and
	 * a refusal leaves whatever worked before in place.
	 */
	QMap<QString, QString> custom_keyboard_layout() const;
	QString custom_keyboard_layout_name() const;
	void set_custom_keyboard_layout(const QString &name,
	                                 const QMap<QString, QString> &rows);

	QString theme() const;
	void set_theme(const QString &theme);

	/*
	 * Which keyboard layout the built-in keyboard draws (sec 6.2).
	 *
	 * GLOBAL rather than preset-owned, unlike the colour scheme and the
	 * key row beside it, and the difference is what the setting is about.
	 * Those suit a HOST -- a network switch wants different keys from a
	 * Linux box. A keyboard layout suits a PERSON: it is where their
	 * hands expect the letters to be, and a different alphabet per server
	 * would be a strange thing to want. Set by the copyright holder
	 * 2026-09-04.
	 */
	QString keyboard_layout() const;
	void set_keyboard_layout(const QString &id);

	key_row_visibility_t key_row_visibility() const;
	void set_key_row_visibility(key_row_visibility_t visibility);

	/*
	 * Caps Lock sends Ctrl instead (sec 6.1), off by default.
	 *
	 * A plain bool rather than the three-state shape the others use:
	 * there is no device that can be asked whether its owner wants this.
	 * It is a preference about a keyboard habit, and the only honest
	 * default is the one that changes nothing.
	 */
	bool caps_as_ctrl() const;
	void set_caps_as_ctrl(bool caps_as_ctrl);

	/*
	 * The built-in keyboard's size, as its resize bars left it (sec 6.2):
	 * a split half's width, and the keys' height under the terminal for
	 * compact and for full, in dp. 0 is "not chosen", which draws the
	 * default -- 22% of the window per half, and the keys' own height.
	 * Device-level, like the keyboard choice: it is about the hands and
	 * the screen, not about any host.
	 */
	bool keyboard_grips() const;
	void set_keyboard_grips(bool shown);
	int keyboard_split_width() const;
	void set_keyboard_split_width(int width_dp);
	int keyboard_height(bool full) const;
	void set_keyboard_height(bool full, int height_dp);

	/* Back to the defaults: every chosen size forgotten. */
	void reset_keyboard_size();

	/*
	 * Keep the terminal out of the OS screenshot and recents-thumbnail
	 * path (sec 11, sec 13). Off by default: it is a real restriction --
	 * the user cannot screenshot their own terminal either -- and a
	 * default that silently disables a system feature is one nobody
	 * connects to a setting they never touched.
	 */
	bool hide_from_screenshots() const;
	void set_hide_from_screenshots(bool hide);

	/*
	 * The pane under the pointer takes the keyboard, without a click
	 * (sec 8.3). Off by default, which that section asked for before it
	 * was built: it surprises anyone who expects click-to-focus, and the
	 * cost of the surprise is typing into the wrong shell.
	 */
	bool focus_follows_mouse() const;
	void set_focus_follows_mouse(bool follows);

	/*
	 * Where the scrollbar goes (sec 8.3), Konsole's own three answers
	 * plus the one this app keeps asking: AUTOMATIC is the right-hand
	 * side where there is a pointer and hidden where there is not,
	 * because a scrollbar is a thing you drag with a mouse and a thumb
	 * scrolls the terminal itself (sec 8.5).
	 */
	enum scrollbar_t {
		SCROLLBAR_AUTO,
		SCROLLBAR_RIGHT,
		SCROLLBAR_LEFT,
		SCROLLBAR_HIDDEN
	};

	scrollbar_t scrollbar() const;
	void set_scrollbar(scrollbar_t where);

	/*
	 * How much the status line says (sec 8.4), or nothing at all.
	 *
	 * Konsole has no status line and this had one that could not be
	 * turned off, whose first field was the QPA platform name -- which is
	 * a thing a developer wants and a user has no use for. So: OFF is a
	 * terminal and nothing else, SESSION is where you are connected and
	 * how it is going, and FULL adds the grid size and the platform for
	 * the person diagnosing a build.
	 */
	enum status_line_t {
		STATUS_LINE_SESSION,
		STATUS_LINE_FULL,
		STATUS_LINE_OFF
	};

	status_line_t status_line() const;
	void set_status_line(status_line_t what);

	/*
	 * What the window is called (sec 8.3), as a format rather than a
	 * choice from a list -- which is what Konsole, xterm and tmux all
	 * settled on, because everybody wants a different one of the same
	 * four facts. bssh_window_title() expands it.
	 */
	QString window_title() const;
	void set_window_title(const QString &format);

	/*
	 * The chords this app keeps for itself (sec 6.1), as the user has
	 * bound them. Empty means the built-in binding; a binding set to
	 * nothing at all gives the chord back to the remote shell, which is
	 * the whole point of making them editable.
	 *
	 * Keyed by the action's stable name -- "copy", "new-tab" -- so a file
	 * somebody has hand-edited survives the list being reordered.
	 */
	QString chord(const QString &action) const;
	void set_chord(const QString &action, const QString &binding);

	/*
	 * Whether this action has been bound at all, which is a different
	 * question from what it is bound TO.
	 *
	 * Absent means the built-in chord; present and empty means the user
	 * gave that chord back to the remote shell. Leaning on QString's
	 * null-versus-empty to tell those apart would be leaning on a
	 * distinction QSettings does not promise to preserve through a file.
	 */
	bool has_chord(const QString &action) const;

	/*
	 * Ask before pasting more than one line (sec 6.4). ON by default,
	 * which is what that section asks for and the only defensible
	 * default: a paste containing a newline RUNS the command, and the
	 * one it runs is one the user has not read.
	 */
	bool warn_multiline_paste() const;
	void set_warn_multiline_paste(bool warn);

	/*
	 * Whether the interface follows the system font-scale accessibility
	 * setting (sec 9.5). On by default, because a user who enlarged their
	 * device's text meant it; off is for someone who wants this app's
	 * chrome at its own size regardless. The terminal grid never follows
	 * it either way.
	 */
	bool honour_system_font_scale() const;
	void set_honour_system_font_scale(bool honour);

	/*
	 * Font ligatures in the terminal (sec 9.2). OFF by default, and the
	 * default is the argued position: a ligature draws two characters as
	 * one shape while the grid still counts two columns, so a search that
	 * highlights "!=" highlights half a glyph. On is for the user who
	 * knows that and wants them anyway. Effective only on Qt 6.7+, where
	 * per-font feature control exists.
	 */
	bool font_ligatures() const;
	void set_font_ligatures(bool ligatures);

	/*
	 * Whether the remote may change the cursor shape with DECSCUSR (sec
	 * 6.4). On by default -- vim and friends expect it -- but a user who
	 * wants one fixed cursor from the preset can turn the override off.
	 */
	bool honour_remote_cursor_shape() const;
	void set_honour_remote_cursor_shape(bool honour);

	/*
	 * General key remapping (sec 6.1): a small set of "when I press this
	 * key, send that key instead" rules, so CapsLock can be Escape or an
	 * F-key a navigation key without touching the OS layout. Each rule is
	 * a pair of QKeySequence portable strings (from, to); a rule with
	 * either half empty is an unused slot. A fixed number of slots rather
	 * than an add/remove list -- enough for the handful of remaps a person
	 * keeps, and no dynamic table to build or a device to test one on.
	 *
	 * Key-to-key rather than key-to-bytes, so the emulator's own
	 * translation renders the result and no escape sequence has to be
	 * hand-typed; a rule whose target this app steals as a chord is left
	 * to the chord, which is checked first.
	 */
	static const int BSSH_KEY_REMAP_SLOTS = 8;
	QList<QPair<QString, QString>> key_remaps() const;
	void set_key_remaps(const QList<QPair<QString, QString>> &remaps);

	/*
	 * sec 11: secret-store keys whose host row is gone but whose secret
	 * could not be erased, because the passphrase-derived store was
	 * locked at the time and erasing means re-encrypting the whole blob.
	 *
	 * Not a secret and deliberately not in the secret store: it is a list
	 * of key NAMES, which is exactly the thing that store cannot be asked
	 * for while locked. It says a host with that id once had a password,
	 * which the hosts file said in plainer terms while the host existed.
	 */
	QStringList orphaned_secrets() const;
	bool set_orphaned_secrets(const QStringList &keys);

	/* Where app.ini lives; empty when no directory could be found. */
	QString path() const { return m_path; }

private:
	/*
	 * One reader and one writer, because every setting above was eleven
	 * lines of the same QSettings dance and this file is going to keep
	 * growing. The default is passed rather than assumed, so a setting's
	 * default still lives with the setting.
	 */
	QVariant value(const QString &key, const QVariant &fallback) const;

	/*
	 * False when the write did not reach app.ini. QSettings reports that
	 * through status() and this asked nothing, exactly as profile_store's
	 * three writers did -- the same fault in the sibling file, fixed in
	 * only one of them at the time.
	 */
	bool store(const QString &key, const QVariant &value);
	bool contains(const QString &key) const;

	QString m_path;
};

#endif /* BSSH_APP_SETTINGS_H */
