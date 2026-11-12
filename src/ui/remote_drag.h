#ifndef BSSH_REMOTE_DRAG_H
#define BSSH_REMOTE_DRAG_H

#include <QMimeData>
#include <QStringList>
#include <QUrl>

/*
 * What a drag out of the file panel carries (project.md sec 6.4, "Dragging
 * a file out"). The files are on the host, not here, so the URL list is
 * answered only when the target asks for it -- at the drop -- and answered
 * with the best thing there is by then: the copy the panel began staging
 * here the moment the drag started, as file:// URLs, once it has arrived
 * whole; and sftp:// URLs, which the file manager fetches itself over a
 * connection of its own, only while it has not. A small file is staged
 * before it is dropped; the link is the last resort for a large one.
 */
class bssh_remote_drag : public QMimeData {
	Q_OBJECT

public:
	/* `remote` as sftp:// URLs; the staged copies arrive later. */
	explicit bssh_remote_drag(const QList<QUrl> &remote);

	/* The staged copies, whole: from now on the drop gets these. */
	void set_staged(const QStringList &local_paths);
	bool staged() const { return !m_local.isEmpty(); }

	/* What a drop asked for now would get. */
	QList<QUrl> urls_now() const { return m_local.isEmpty() ? m_remote : m_local; }

	QStringList formats() const override;
	bool hasFormat(const QString &mime) const override;

protected:
	QVariant retrieveData(const QString &mime, QMetaType type) const override;

private:
	QList<QUrl> m_remote;
	QList<QUrl> m_local;
};

/*
 * Where a drag's copies are staged: a directory of this process's own
 * under the cache, removed when the process ends, since a file manager may
 * still be copying from it long after the drag. Directories a process that
 * has gone left behind are removed the first time this is asked.
 */
QString bssh_drag_staging_root();

#endif
