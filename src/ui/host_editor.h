#ifndef BSSH_HOST_EDITOR_H
#define BSSH_HOST_EDITOR_H

#include <QWidget>

#include <QJsonObject>

#include "model/profile.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class bssh_font_picker;
class bssh_profile_store;

/*
 * bssh_host_editor -- adding or editing a host, on touch (project.md
 * sec 8.5).
 *
 * A full-screen page, not a dialog. The first Android build used a
 * QInputDialog here, and driving it on a device showed exactly what sec
 * 8.5 warns about: a small floating box with a single cramped line edit,
 * in the middle of a screen the user is holding in one hand. It worked,
 * which is the problem -- it is the desktop idiom surviving into the
 * mobile front door because nobody looked at it.
 *
 * The desktop keeps the PuTTY-shaped Session page for the same job. Same
 * profile, same store, different shape, exactly as the front doors
 * themselves differ.
 */
class bssh_host_editor : public QWidget {
	Q_OBJECT

public:
	/*
	 * The store is not optional in practice, however it looks: without it
	 * this page cannot see which preset a host follows, so it resolves
	 * against the app defaults and shows a font the host does not use
	 * (sec 10.1). It defaults to null only so the tests can build a bare
	 * editor for the fields that do not need one.
	 */
	explicit bssh_host_editor(bssh_profile_store *store = nullptr,
	                           QWidget *parent = nullptr);

	/* Empty profile to add; an existing one to edit. */
	void set_profile(const bssh_profile_t &profile);

	/*
	 * What the fields currently describe. Invalid (empty host) when there
	 * is nothing worth saving, which is what the caller checks rather
	 * than the editor refusing to be read.
	 */
	bssh_profile_t profile() const;

	/* Add or Edit -- the heading and the save button both say which. */
	void set_editing(bool editing);

signals:
	void save_requested(const bssh_profile_t &profile);
	void cancelled();

private slots:
	/*
	 * Drops the font override rather than copying the preset's current
	 * value into it (sec 10.1). Copying stops following the preset the
	 * next time it changes, which is exactly what the user asked to undo.
	 */
	void revert_font_to_preset();

private:
	/* The three layers this host resolves through (sec 10.1). */
	QJsonObject resolved_settings() const;
	void update_override_badge();

	bssh_profile_store *m_store;
	bssh_profile_t m_profile;
	QLabel *m_title;
	QLabel *m_font_scope;
	QPushButton *m_font_revert;
	QPushButton *m_save;
	QLineEdit *m_name;
	QLineEdit *m_identity;
	QLineEdit *m_jump;
	QLineEdit *m_host;
	QLineEdit *m_user;
	QSpinBox *m_port;
	bssh_font_picker *m_font_picker;
};

#endif /* BSSH_HOST_EDITOR_H */
