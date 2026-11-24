#include "model/store_lock.h"

#include <QElapsedTimer>
#include <QFile>
#include <QSysInfo>

bssh_store_lock::bssh_store_lock(const QString &path, const char *label) : m_lock(path) {
	m_lock.setStaleLockTime(10000);

	QElapsedTimer waited;
	waited.start();
	qint64 stopped_since = -1;
	while (!(m_held = m_lock.tryLock(250))) {
		if (m_lock.error() != QLockFile::LockFailedError) {
			qWarning("%s: the store's lock could not be taken (error %d), changing it anyway", label,
			         int(m_lock.error()));
			return;
		}
		qint64 pid = 0;
		QString host, application;
		const bool known = m_lock.getLockInfo(&pid, &host, &application);
		/* A pid is this machine's only: a home shared over a network holds others'. */
		const bool here = host.isEmpty() || host == QSysInfo::machineHostName();
		if (known && here && pid > 0 && process_is_stopped(pid)) {
			if (stopped_since < 0) stopped_since = waited.elapsed();
			if (waited.elapsed() - stopped_since >= stopped_ms) {
				qWarning("%s: the BeerSSH holding the store (pid %lld) is stopped, changing it anyway",
				         label, static_cast<long long>(pid));
				return;
			}
		} else {
			stopped_since = -1;
		}
		if (waited.elapsed() >= ceiling_ms) {
			qWarning("%s: another BeerSSH has held the store for a minute, changing it anyway", label);
			return;
		}
	}
}

bool bssh_store_lock::process_is_stopped(qint64 pid) {
	/*
	 * /proc/<pid>/stat's third field: T stopped, t traced. The command
	 * name before it is in parentheses and may hold spaces or parentheses
	 * of its own, so the state is read after the LAST ')'.
	 */
	QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
	if (!stat.open(QIODevice::ReadOnly)) return false;
	const QByteArray line = stat.readAll();
	const int close = line.lastIndexOf(')');
	if (close < 0 || close + 2 >= line.size()) return false;
	const char state = line.at(close + 2);
	return state == 'T' || state == 't';
}
