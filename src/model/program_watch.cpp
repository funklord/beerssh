#include "model/program_watch.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QTimer>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace {

/*
 * What tells one file at a path from the next: device and inode where
 * there are inodes, else size and time written. Empty where there is no
 * runnable file, mid-install included.
 */
QString identity_of(const QString &path) {
	const QFileInfo info(path);
	if (!info.isFile() || !info.isExecutable()) return QString();
#ifdef Q_OS_UNIX
	struct stat status;
	if (::stat(QFile::encodeName(path).constData(), &status) != 0) return QString();
	return QStringLiteral("%1:%2").arg(qulonglong(status.st_dev)).arg(qulonglong(status.st_ino));
#else
	return QStringLiteral("%1:%2").arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch());
#endif
}

}  // namespace

bssh_program_watch::bssh_program_watch(const QString &path, QObject *parent)
    : QObject(parent), m_path(path), m_identity(identity_of(path)),
      m_watcher(new QFileSystemWatcher(this)), m_settle(new QTimer(this)) {
	m_settle->setSingleShot(true);
	m_settle->setInterval(1500);
	connect(m_settle, &QTimer::timeout, this, &bssh_program_watch::look);
	const QString directory = QFileInfo(path).absolutePath();
	if (!m_identity.isEmpty() && !directory.isEmpty()) m_watcher->addPath(directory);
	connect(m_watcher, &QFileSystemWatcher::directoryChanged, m_settle, qOverload<>(&QTimer::start));
}

void bssh_program_watch::set_settle_ms(int ms) {
	m_settle->setInterval(ms);
}

void bssh_program_watch::look() {
	const QString now = identity_of(m_path);
	if (now.isEmpty() || now == m_identity) return;
	m_identity = now;
	m_replaced = true;
	emit program_replaced();
}
