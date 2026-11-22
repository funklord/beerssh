#ifndef BSSH_PROGRAM_WATCH_H
#define BSSH_PROGRAM_WATCH_H

#include <QObject>
#include <QString>

class QFileSystemWatcher;
class QTimer;

/*
 * bssh_program_watch -- says when the program file this process was
 * started from has been replaced: a package upgrade installed a new
 * BeerSSH over the running one (sec 8.4, upgrades). The holder,
 * 2026-11-21: "still no way to switch to an updated version?" -- File >
 * Restart, Keeping Sessions existed, and nothing said when it was wanted.
 *
 * The file is told apart by its identity, not its name: dpkg, rpm and
 * `install` write the new file beside the old and rename it over, so the
 * path names a new inode while this process still runs the old one. A
 * file rewritten in place cannot happen to a running program (ETXTBSY).
 * The directory is watched, since a rename over the file ends a watch on
 * the file itself, and a change is judged a moment after it settles, as a
 * package manager writes its temporary file first.
 */
class bssh_program_watch : public QObject {
	Q_OBJECT

public:
	explicit bssh_program_watch(const QString &path, QObject *parent = nullptr);

	/* How long after the directory last changed the file is looked at. */
	void set_settle_ms(int ms);

	QString path() const { return m_path; }

	/* True once the file has been replaced, and stays so. */
	bool replaced() const { return m_replaced; }

signals:
	/* Once per new file: a second upgrade says so again. */
	void program_replaced();

private:
	void look();

	QString m_path;
	QString m_identity;
	bool m_replaced = false;
	QFileSystemWatcher *m_watcher;
	QTimer *m_settle;
};

#endif /* BSSH_PROGRAM_WATCH_H */
