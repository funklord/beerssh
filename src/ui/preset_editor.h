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
	explicit bssh_preset_editor(bssh_profile_store *store, QWidget *parent = nullptr);

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

private slots:
	void on_selection_changed();
	void on_save();
	void on_clone();
	void on_delete();

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
	QLineEdit *m_on_connect;

	QPushButton *m_save;
	QPushButton *m_clone;
	QPushButton *m_delete;

	bssh_preset_t m_current;
};

#endif /* BSSH_PRESET_EDITOR_H */
