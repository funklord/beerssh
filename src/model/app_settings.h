#ifndef BSSH_APP_SETTINGS_H
#define BSSH_APP_SETTINGS_H

#include <QString>
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

	QString theme() const;
	void set_theme(const QString &theme);

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
	void store(const QString &key, const QVariant &value);
	bool contains(const QString &key) const;

	QString m_path;
};

#endif /* BSSH_APP_SETTINGS_H */
