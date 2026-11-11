#ifndef BSSH_FILE_PANEL_H
#define BSSH_FILE_PANEL_H

#include <QList>
#include <QPointer>
#include <QWidget>

#include "ssh/ssh_session.h"

class QLabel;
class QLineEdit;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class bssh_session;

/*
 * The files on the other end of a session, beside its terminal (project.md
 * sec 6.4, *A file panel beside each pane*): the holder's "a built in file
 * browser using the same connection simultaneously to sftp/scp send and
 * receive files". Over SFTP on the session's own connection, through
 * bssh_session's list_directory(), upload() and download().
 *
 * It opens at the shell's directory, found as a dropped file's is. A
 * directory opens on a double click or Enter; a file is copied into the
 * local Downloads folder the same way, or anywhere from its menu. Files
 * dropped on it are copied into the directory it shows, as a drop on the
 * terminal copies into the shell's. Nothing is replaced on either side: a
 * name taken gets a free one beside it.
 *
 * Built by the pane container beside the view, and rebuilt with it on
 * every layout change; the session keeps which directory it showed.
 */
class bssh_file_panel : public QWidget {
	Q_OBJECT

public:
	explicit bssh_file_panel(bssh_session *session, QWidget *parent = nullptr);

	/* The directory shown, as the server spells it; empty before the first. */
	QString path() const { return m_path; }

	/* Lists `path` (empty for home); the shell's directory with no argument. */
	void open_directory(const QString &path);
	void open_shells_directory();

	/* Copies local files into the directory shown, as a drop on it does;
	 * false when nothing was started. */
	bool copy_here(const QStringList &paths);

	/* The entries shown, in order: for tests and accessibility alike. */
	QList<bssh_remote_entry_t> entries() const { return m_entries; }

	/* Where a double-clicked file goes; the Downloads folder by default. */
	void set_download_directory(const QString &directory) { m_download_directory = directory; }

	QSize sizeHint() const override;

signals:
	/* The close button: the container takes the panel down. */
	void close_requested();

	/* A listing arrived and is shown (or its failure is). */
	void listed();

protected:
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dropEvent(QDropEvent *event) override;

private:
	void show_listing(const QString &path, const QList<bssh_remote_entry_t> &entries, const QString &why);
	void activate(QTreeWidgetItem *item);
	void copy_selected_to(const QString &directory);
	void show_menu(const QPoint &at);
	void say(const QString &text, bool stoppable = false);
	QString remote_path_of(const bssh_remote_entry_t &entry) const;

	QPointer<bssh_session> m_session;
	QString m_path;
	QList<bssh_remote_entry_t> m_entries;
	int m_listings_asked = 0;
	QString m_download_directory;

	QLineEdit *m_location;
	QToolButton *m_up;
	QTreeWidget *m_list;
	QLabel *m_status;
	QToolButton *m_stop;
};

#endif /* BSSH_FILE_PANEL_H */
