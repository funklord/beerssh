#ifndef BSSH_PRESET_EDITOR_H
#define BSSH_PRESET_EDITOR_H

#include <QWidget>

#include "model/preset.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class bssh_font_picker;
class bssh_profile_store;

/*
 * bssh_preset_editor -- editing the middle layer (project.md sec 10).
 *
 * The preset system had every piece except this one: presets are stored,
 * resolved, layered under per-host overrides and counted by hosts_using(),
 * and none of it could be changed by a user. A settings model nobody can
 * edit is a settings model that only ever holds what was shipped.
 *
 * Two rules from sec 10 shape it, and both are about not surprising
 * someone:
 *
 *   Editing a preset changes every host that follows it. That IS the
 *   feature -- and it is also the surprise, so the count goes in front of
 *   the save, not in a tooltip: "used by 7 hosts" before the change lands,
 *   never after.
 *
 *   Shipped presets are cloned, not edited (sec 10.4), so a user cannot
 *   damage the starting points everyone else's clones came from. The store
 *   already refuses; refusing in the store alone would mean a Save button
 *   that does nothing, so the editor says why instead.
 */
class bssh_preset_editor : public QWidget {
	Q_OBJECT

public:
	/*
	 * touch picks the SHAPE, not the contents (sec 8.5). The desktop gets
	 * PuTTY's two columns; a phone gets one column, a short list above the
	 * fields, controls that clear sec 6.2's 48dp floor, and a way back --
	 * which the two-column version does not need because a dialog has a
	 * category tree to click away to and a full-screen page has nothing.
	 */
	/*
	 * Which arrangement to build, as a type rather than as a bool.
	 *
	 * It was a bool, and the desktop dialog said
	 *   new bssh_preset_editor(m_store, this)
	 * -- the QWidget* landing in the bool, converting to true, and giving
	 * the desktop front door the TOUCH editor: list stacked above the
	 * fields, controls sized for a thumb, and a "Back to hosts" button in
	 * a dialog whose navigation is a category tree. It compiled, it ran,
	 * and it took looking at the thing to notice. A pointer does not
	 * convert to an enum, so the same slip is now a build error.
	 */
	enum layout_t {
		DESKTOP,  /* list beside the fields, as PuTTY's pages are */
		TOUCH     /* one column, thumb-sized, with its own way out */
	};

	explicit bssh_preset_editor(bssh_profile_store *store, layout_t layout = DESKTOP,
	                             QWidget *parent = nullptr);

	/* Re-reads the store -- after a save, or after a host changed which
	 * preset it follows. */
	void reload();

	int current_preset_id() const;

signals:
	/*
	 * A preset changed, so anything resolved from it is now stale. The
	 * window re-resolves its open sessions on this: propagation that only
	 * reaches the next connection is propagation the user cannot see
	 * working.
	 */
	void preset_saved(int preset_id);
	void preset_removed(int preset_id);

	/* Touch layout only: the page is done and the caller should go back. */
	void back_requested();

private slots:
	void on_selection_changed();
	void on_save();
	void on_clone();
	void on_delete();
	void on_export();
	void on_import();

private:
	void show_preset(const bssh_preset_t &preset);
	bssh_preset_t edited_preset() const;
	void update_controls();

	bssh_profile_store *m_store;

	QListWidget *m_presets;
	QLabel *m_usage;

	QLineEdit *m_name;
	QLineEdit *m_term;
	QSpinBox *m_scrollback;
	QCheckBox *m_bell;
	QComboBox *m_cursor_style;
	QCheckBox *m_cursor_blink;
	bssh_font_picker *m_font;
	QSpinBox *m_keepalive;
	QCheckBox *m_compression;
	QCheckBox *m_clipboard;
	QLineEdit *m_on_connect;
	QComboBox *m_key_row;

	QPushButton *m_save;
	QPushButton *m_clone;
	QPushButton *m_delete;
	QPushButton *m_export;
	QPushButton *m_import;

	bssh_preset_t m_current;
	bool m_touch;
};

#endif /* BSSH_PRESET_EDITOR_H */
