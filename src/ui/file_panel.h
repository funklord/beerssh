#ifndef BSSH_FILE_PANEL_H
#define BSSH_FILE_PANEL_H

#include <QList>
#include <QHash>
#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>

#include "model/session.h"
#include "ssh/ssh_session.h"

class QLabel;
class QFrame;
class QLineEdit;
class QProgressBar;
class QVBoxLayout;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class bssh_session;
class bssh_media_bytes;
class QUrl;

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

	/*
	 * Plays the one file selected (sec 6.4, *Playing a file*): audio or
	 * video, by its name. In BeerSSH's own player where the build has one
	 * (bssh_in_app_player), above the list; otherwise in the desktop's,
	 * opened at once on the FUSE mount -- whose reads wait for bytes still
	 * coming -- or once the copy is whole where there is no mount. A
	 * local shell's file is opened where it is. False when nothing began.
	 */
	bool play_selected();
	/* Whether play_selected() would begin: one file, and playable here. */
	bool can_play_selected() const;

	/* How the desktop's player is asked; QDesktopServices by default.
	 * Tests set their own, to see what would have opened. */
	void set_opener(std::function<bool(const QUrl &)> opener) { m_opener = std::move(opener); }

	/* Where BeerSSH's own player goes: above the list. */
	QWidget *media_area() const { return m_media_area; }

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
	QStringList m_last_mounted;   /* the last drag's, kept past its staging */
	/* Stagings put away with their tiles: a tile still to finish goes too. */
	QSet<QString> m_put_away;
	void put_away_staging(const QString &dir);
	void put_away_finished_staging();
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
	QString m_direct_save_name;   /* XDS's proposal while a drag offers it */
	/* The file being played, staged as a drag's is but kept apart from it. */
	QString m_play_dir;
	QString m_play_file;
	QString m_play_mounted;
	bool m_play_open_when_whole = false;
	std::shared_ptr<bssh_media_bytes> m_play_bytes;
	std::function<bool(const QUrl &)> m_opener;
	QWidget *m_media_area = nullptr;
	bool m_play_in_app = false;   /* handed to BeerSSH's own player */
	/* Subtitles beside the played file, fetched before its copy starts. */
	QStringList subtitles_beside(const bssh_remote_entry_t &entry) const;
	bool start_play_copy(const bssh_remote_entry_t &entry);
	bssh_remote_entry_t m_play_waiting;
	bool m_play_fetching_subtitles = false;
	QStringList m_play_subtitles;
	void end_play(bool whole);
	void put_away_play();
	/*
	 * Files played in BeerSSH's own player, kept past the next Play: its
	 * tiles go on reading them, one per file, and each goes once nothing
	 * holds its bytes any more -- looked at as each Play begins.
	 */
	struct kept_play_t {
		QString dir;
		QString mounted;
		std::weak_ptr<bssh_media_bytes> bytes;
	};
	QList<kept_play_t> m_kept_plays;
	void sweep_kept_plays();
	void close_playing();
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

	/* A copy's tile: what and where, its bar, Pause and X (sec 6.4). */
	struct tile_t {
		QFrame *frame = nullptr;
		QLabel *label = nullptr;
		QLabel *status = nullptr;
		QProgressBar *bar = nullptr;
		QToolButton *pause = nullptr;
		QToolButton *close = nullptr;
		QString destination;
		bool over = false;
	};
	QVBoxLayout *m_tile_column = nullptr;
	QHash<int, tile_t> m_tiles;
	tile_t &tile_for(int id);
	QString transfer_label(const bssh_session::transfer_t &transfer) const;
	void show_tile_state(tile_t &tile, bssh_session::transfer_state_t state);
	void transfer_changed(int id);
	void transfer_progress(int id, qint64 done, qint64 total);
	void transfer_finished(int id, bool ok, const QString &destination, const QStringList &written,
	                       const QString &why);
};

#endif /* BSSH_FILE_PANEL_H */
