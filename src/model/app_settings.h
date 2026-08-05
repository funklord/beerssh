#ifndef BSSH_APP_SETTINGS_H
#define BSSH_APP_SETTINGS_H

#include <QString>

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

	/* Where app.ini lives; empty when no directory could be found. */
	QString path() const { return m_path; }

private:
	QString m_path;
};

#endif /* BSSH_APP_SETTINGS_H */
