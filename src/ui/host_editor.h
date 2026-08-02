#ifndef BSSH_HOST_EDITOR_H
#define BSSH_HOST_EDITOR_H

#include <QWidget>

#include "model/profile.h"

class QLineEdit;
class bssh_font_picker;
class QSpinBox;

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
	explicit bssh_host_editor(QWidget *parent = nullptr);

	/* Empty profile to add; an existing one to edit. */
	void set_profile(const bssh_profile_t &profile);

	/*
	 * What the fields currently describe. Invalid (empty host) when there
	 * is nothing worth saving, which is what the caller checks rather
	 * than the editor refusing to be read.
	 */
	bssh_profile_t profile() const;

signals:
	void save_requested(const bssh_profile_t &profile);
	void cancelled();

private:
	bssh_profile_t m_profile;
	QLineEdit *m_name;
	QLineEdit *m_host;
	QLineEdit *m_user;
	QSpinBox *m_port;
	bssh_font_picker *m_font_picker;
};

#endif /* BSSH_HOST_EDITOR_H */
