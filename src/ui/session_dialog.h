#ifndef BSSH_SESSION_DIALOG_H
#define BSSH_SESSION_DIALOG_H

#include <QWidget>

#include "model/profile.h"

class QLineEdit;
class QListWidget;
class QSpinBox;
class QStackedWidget;
class QTreeWidget;
class QTreeWidgetItem;
class bssh_profile_store;

/*
 * bssh_session_dialog -- the desktop front door, in PuTTY's shape
 * (project.md sec 8.5).
 *
 * PuTTY's configuration window is the interface a very large number of
 * people already have in their fingers: a category tree down the left, the
 * chosen category's settings on the right, the destination at the top, and
 * a saved-sessions list with Load / Save / Delete. Anyone arriving from
 * PuTTY should be able to use this without reading anything, which is the
 * entire point of borrowing it rather than inventing a layout.
 *
 * Categories beyond Session are listed but say plainly which milestone
 * fills them in. An empty pane that looks finished is worse than one that
 * admits it is not: the terminal, window and connection settings arrive
 * with the preset system at M10 (sec 10.2), and it is the same set of
 * settings a preset owns.
 */
class bssh_session_dialog : public QWidget {
	Q_OBJECT

public:
	explicit bssh_session_dialog(bssh_profile_store *store, QWidget *parent = nullptr);

	/* The destination as currently typed, saved or not. */
	bssh_profile_t current_profile() const;

	void set_current_profile(const bssh_profile_t &profile);

signals:
	/* "Open" -- connect to this. */
	void open_requested(const bssh_profile_t &profile);
	void cancelled();

private slots:
	void on_load();
	void on_save();
	void on_delete();
	void on_open();
	void on_category_changed();
	void on_session_activated();

private:
	QWidget *build_session_page();
	QWidget *build_placeholder_page(const QString &title, const QString &milestone);
	void reload_sessions();
	int selected_profile_id() const;

	bssh_profile_store *m_store;
	QTreeWidget *m_categories;
	QStackedWidget *m_pages;
	QLineEdit *m_host;
	QSpinBox *m_port;
	QLineEdit *m_user;
	QLineEdit *m_session_name;
	QListWidget *m_sessions;
	int m_current_id;
};

#endif /* BSSH_SESSION_DIALOG_H */
