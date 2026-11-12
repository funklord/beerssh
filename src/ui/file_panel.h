#ifndef BSSH_FILE_PANEL_H
#define BSSH_FILE_PANEL_H

#include <QList>
#include <QPointer>
#include <QWidget>

#include "ssh/ssh_session.h"

class QLabel;
class QLineEdit;
class QProgressBar;
class QVBoxLayout;
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

	/*
	 * Changes in the directory shown, by name; the listing follows each.
	 * A name is one component -- no slash, not . or .. -- and a rename
	 * never replaces what is there. Removing a folder removes everything
	 * in it; the menu asks first, these do not. False when nothing was
	 * started.
	 */
	bool rename_entry(const QString &name, const QString &to);
	bool make_folder(const QString &name);
	bool remove_entries(const QStringList &names);

	/*
	 * What a drag of the selected entries out to a file manager carries
	 * (sec 6.4): sftp:// links at first, and the copies staged here from
	 * this moment once they are whole. Starts the staging; the panel's
	 * own drag takes it, and tests read it.
	 */
	class bssh_remote_drag *drag_data_for_selection();

	/*
	 * The drag ended: dropped, or let go over nothing (Qt::IgnoreAction),
	 * which stops its staging and removes what it staged -- a large file
	 * dragged and abandoned is not copied for nothing. start_drag() calls
	 * it; tests call it as a drag would.
	 */
	void drag_ended(Qt::DropAction action);

	/* Files staged for a drag only up to this many bytes in all; larger,
	 * and folders, get the link alone. 64 MiB. */
	void set_staging_limit(qint64 bytes) { m_staging_limit = bytes; }

	/* No staging at all, whatever the size or the mount: for a test of
	 * a drop's other roads, and for anyone who would rather it did not. */
	void set_staging_enabled(bool enabled) { m_staging_enabled = enabled; }

	/* The entries shown, in order: for tests and accessibility alike. */
	QList<bssh_remote_entry_t> entries() const { return m_entries; }

	/* Where a double-clicked file goes; the Downloads folder by default. */
	void set_download_directory(const QString &directory) { m_download_directory = directory; }

	/* Shown over its parent, the terminal view, and kept its size. */
	void set_overlay(bool overlay);
	bool is_overlay() const { return m_overlay; }

	/* Its width, kept within what leaves the terminal room. */
	void set_panel_width(int width);

	QSize sizeHint() const override;

signals:
	/* The close button: the container takes the panel down. */
	void close_requested();

	/* The grip was dragged and let go at this width. */
	void width_chosen(int width);

	/* A listing arrived and is shown (or its failure is). */
	void listed();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dropEvent(QDropEvent *event) override;

private:
	void show_listing(const QString &path, const QList<bssh_remote_entry_t> &entries, const QString &why);
	void activate(QTreeWidgetItem *item);
	void copy_selected_to(const QString &directory);
	void show_menu(const QPoint &global);
	void copy_selected_elsewhere();
	void say(const QString &text);
	QString remote_path_of(const bssh_remote_entry_t &entry) const;
	QString path_here(const QString &name) const;
	void changed(bool ok, const QString &path, const QString &why);

	QPointer<bssh_session> m_session;
	QString m_path;
	QList<bssh_remote_entry_t> m_entries;
	int m_listings_asked = 0;
	QString m_download_directory;
	int m_changes_asked = 0;
	QStringList m_removals;
	QString m_failure;
	QString m_staging_dir;
	QPointer<class bssh_remote_drag> m_staging_data;
	int m_drags = 0;
	qint64 m_staging_limit = 64 * 1024 * 1024;
	bool m_staging_enabled = true;
	QString m_last_staged_dir;   /* the last drag's, staged or staging */
	QString m_discard_dir;       /* an abandoned drag's, removed once stopped */
	QStringList m_staging_mounted;   /* its copies on the FUSE mount, if any */
	void end_mounted(bool whole);
	/* A copy waiting for the staging it replaces to stop: XDS's. */
	QStringList m_after_discard_paths;
	QString m_after_discard_dir;
	QByteArray direct_save(const QStringList &paths);
	bool save_into(const QStringList &paths, const QString &folder);
	QString m_ark_path;   /* the drag's D-Bus object, while it is registered */
	QPointer<QObject> m_ark_object;
	void drop_ark();
	void set_direct_save_name(const QString &name);
	QPoint m_drag_press;
	void start_drag();

	QWidget *m_grip;
	int m_drag_from = -1;
	bool m_overlay = false;
	int m_drag_width = 0;
	QLineEdit *m_location;
	QToolButton *m_up;
	QTreeWidget *m_list;
	QLabel *m_status;

	/* A copy's row: what and how far, its bar, its Stop. */
	struct copy_row {
		QWidget *row = nullptr;
		QLabel *label = nullptr;
		QProgressBar *bar = nullptr;
		QToolButton *stop = nullptr;
	};
	copy_row make_copy_row(QVBoxLayout *column, const QString &stop_name);
	void show_copy(const copy_row &row, const QString &verb, qint64 done, qint64 total);
	void end_copy(const copy_row &row, bool ok, const QString &directory, const QStringList &written,
	              const QString &why);
	copy_row m_copy_back;
	copy_row m_copy_up;
};

#endif /* BSSH_FILE_PANEL_H */
