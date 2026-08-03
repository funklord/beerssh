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
	QString theme() const;
	void set_theme(const QString &theme);

	key_row_visibility_t key_row_visibility() const;
	void set_key_row_visibility(key_row_visibility_t visibility);

	/* Where app.ini lives; empty when no directory could be found. */
	QString path() const { return m_path; }

private:
	QString m_path;
};

#endif /* BSSH_APP_SETTINGS_H */
