#include "ui/remote_drag.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <cerrno>
#include <signal.h>

bssh_remote_drag::bssh_remote_drag(const QList<QUrl> &remote) : m_remote(remote) {}

void bssh_remote_drag::set_staged(const QStringList &local_paths) {
	m_local.clear();
	for (const QString &path : local_paths) m_local << QUrl::fromLocalFile(path);
}

QStringList bssh_remote_drag::formats() const {
	/* XDS first, as the protocol asks: a target that speaks it prefers it. */
	QStringList list;
	if (m_direct_save) list << QStringLiteral("XdndDirectSave0");
	list << QStringLiteral("text/uri-list") << QStringLiteral("text/plain");
	return list;
}

bool bssh_remote_drag::hasFormat(const QString &mime) const {
	return formats().contains(mime);
}

QVariant bssh_remote_drag::retrieveData(const QString &mime, QMetaType type) const {
	/* Asked at the drop, not at the drag's start: by then the staged copy
	 * may be whole. */
	if (mime == QLatin1String("XdndDirectSave0")) return m_direct_save ? m_direct_save() : QByteArray("E");
	const QList<QUrl> urls = urls_now();
	if (mime == QLatin1String("text/uri-list")) {
		if (type.id() == QMetaType::QVariantList) {
			QVariantList list;
			for (const QUrl &url : urls) list << url;
			return list;
		}
		QByteArray lines;
		for (const QUrl &url : urls) lines += url.toEncoded() + "\r\n";
		return lines;
	}
	if (mime == QLatin1String("text/plain")) {
		QStringList lines;
		for (const QUrl &url : urls) lines << (url.isLocalFile() ? url.toLocalFile() : url.toString());
		return lines.join(QLatin1Char('\n'));
	}
	return QVariant();
}

namespace {

QString staging_base() {
	return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/drag");
}

/* The directory is this process's, by its name; nothing else is removed. */
void remove_own_staging() {
	const QString own = staging_base() + QLatin1Char('/') + QString::number(QCoreApplication::applicationPid());
	if (QFileInfo(own).isDir()) QDir(own).removeRecursively();
}

}

QString bssh_drag_staging_root() {
	static QString root;
	if (!root.isEmpty()) return root;
	const QString base = staging_base();
	/*
	 * A process's staging is named for its pid; one whose process has
	 * gone (a crash, a kill) is no one's, and goes. Only directories
	 * named as a pid, under this app's own cache.
	 */
	const QDir dir(base);
	for (const QString &name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
		bool numeric = false;
		const qint64 pid = name.toLongLong(&numeric);
		if (!numeric || pid <= 0 || pid == QCoreApplication::applicationPid()) continue;
		if (kill(pid_t(pid), 0) != 0 && errno == ESRCH) QDir(dir.filePath(name)).removeRecursively();
	}
	root = base + QLatin1Char('/') + QString::number(QCoreApplication::applicationPid());
	QDir().mkpath(root);
	qAddPostRoutine(remove_own_staging);
	return root;
}
