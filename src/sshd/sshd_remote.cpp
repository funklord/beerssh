#include "sshd/sshd_remote.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QTimer>

#include "sshd/sshd_keeper.h"
#include "sshd/sshd_platform.h"

namespace {

const int POLL_MS = 1000;

/* A start is waited for this long before a dead process is asked again. */
const int START_GRACE_MS = 10000;

/* The log is cut back to its last LOG_KEEP bytes once it passes LOG_LIMIT. */
const qint64 LOG_LIMIT = 256 * 1024;
const qint64 LOG_KEEP = 64 * 1024;

/*
 * Android names a component's process "<package>:<name>"; the manifest
 * puts the server in ":server". Empty in the tests, where the "server"
 * is the test itself.
 */
QByteArray &process_suffix() {
	static QByteArray suffix = ":server";
	return suffix;
}

QString one_line(QString text) {
	return text.replace(QLatin1Char('\n'), QLatin1Char(' '));
}

} /* namespace */

QString bssh_sshd_remote::status_path(const bssh_sshd_platform *platform) {
	return platform->data_directory() + QStringLiteral("/server.status");
}

QString bssh_sshd_remote::log_path(const bssh_sshd_platform *platform) {
	return platform->data_directory() + QStringLiteral("/server.log");
}

void bssh_sshd_remote::set_process_suffix(const QByteArray &suffix) {
	process_suffix() = suffix;
}

bool bssh_sshd_remote::server_alive(qint64 pid) {
	if (pid <= 0) return false;
	QFile cmdline(QStringLiteral("/proc/%1/cmdline").arg(pid));
	if (!cmdline.open(QIODevice::ReadOnly)) return false;
	const QByteArray name = cmdline.readAll().split('\0').value(0);
	if (name.isEmpty()) return false;   /* a zombie has none */
	return process_suffix().isEmpty() || name.endsWith(process_suffix());
}

void bssh_sshd_remote::report_from(bssh_sshd_keeper *keeper, bssh_sshd_platform *platform) {
	QDir().mkpath(platform->data_directory());
	const QString status = status_path(platform);
	const QString log = log_path(platform);

	QFile old(log);
	if (old.size() > LOG_LIMIT && old.open(QIODevice::ReadOnly)) {
		old.seek(old.size() - LOG_KEEP);
		old.readLine();   /* from a line's start */
		const QByteArray tail = old.readAll();
		old.close();
		QSaveFile cut(log);
		if (cut.open(QIODevice::WriteOnly)) {
			cut.write(tail);
			cut.commit();
		}
	}

	auto write_status = [keeper, status] {
		QSaveFile file(status);
		if (!file.open(QIODevice::WriteOnly)) return;
		QByteArray text;
		text += "pid=" + QByteArray::number(QCoreApplication::applicationPid()) + '\n';
		text += "serving=" + QByteArray(keeper->serving() ? "1" : "0") + '\n';
		text += "status=" + one_line(keeper->status_line()).toUtf8() + '\n';
		text += "fingerprint=" + keeper->fingerprint().toUtf8() + '\n';
		file.write(text);
		file.commit();
	};
	QObject::connect(keeper, &bssh_sshd_control::changed, keeper, write_status);
	QObject::connect(keeper, &bssh_sshd_control::logged, keeper,
	                 [log](const QTime &when, const QString &line) {
		QFile file(log);
		if (!file.open(QIODevice::Append)) return;
		file.write((when.toString(QStringLiteral("HH:mm:ss ")) + one_line(line) + QLatin1Char('\n'))
		           .toUtf8());
	});
	write_status();
}

bssh_sshd_remote::bssh_sshd_remote(bssh_sshd_platform *platform, QObject *parent)
    : bssh_sshd_control(parent), m_platform(platform), m_timer(new QTimer(this)) {
	/* What was logged before this window opened is the server's past, not news. */
	m_log_offset = QFile(log_path(platform)).size();

	m_timer->setInterval(POLL_MS);
	connect(m_timer, &QTimer::timeout, this, &bssh_sshd_remote::follow);
	m_timer->start();
	follow();
}

bool bssh_sshd_remote::active() const {
	return m_platform->switched_on();
}

QString bssh_sshd_remote::status_line() const {
	if (m_process && !m_status.isEmpty()) return m_status;
	if (m_platform->switched_on()) return tr("Starting...");
	if (!m_platform->has_storage_access()) return tr("Stopped. Needs All files access to serve storage.");
	if (m_platform->listen_address().isNull()) return tr("Stopped. Connect to Wi-Fi to serve files.");
	return tr("Stopped.");
}

void bssh_sshd_remote::switch_on() {
	m_platform->set_switched_on(true);
	m_platform->request_notifications();
	m_asked = QDateTime::currentDateTimeUtc();
	m_platform->start_server_process();
	emit changed();
}

void bssh_sshd_remote::switch_off() {
	m_platform->set_switched_on(false);
	m_platform->stop_server_process();
	emit changed();
}

void bssh_sshd_remote::follow() {
	read_status();
	read_log();

	/*
	 * Switched on and no process: Android ended it, or it never came up.
	 * Asked again, but not while a start is still on its way.
	 */
	if (m_platform->switched_on() && !m_process
	    && (!m_asked.isValid() || m_asked.msecsTo(QDateTime::currentDateTimeUtc()) > START_GRACE_MS)) {
		m_asked = QDateTime::currentDateTimeUtc();
		m_platform->start_server_process();
	}
}

void bssh_sshd_remote::read_status() {
	QFile file(status_path(m_platform));
	QString status;
	QString fingerprint;
	bool process = false;
	bool running = false;
	if (file.open(QIODevice::ReadOnly)) {
		for (const QByteArray &line : file.readAll().split('\n')) {
			const int equals = line.indexOf('=');
			if (equals < 0) continue;
			const QByteArray key = line.left(equals);
			const QByteArray value = line.mid(equals + 1);
			if (key == "pid") process = server_alive(value.toLongLong());
			else if (key == "serving") running = value == "1";
			else if (key == "status") status = QString::fromUtf8(value);
			else if (key == "fingerprint") fingerprint = QString::fromUtf8(value);
		}
	}
	running = running && process;

	if (process == m_process && running == m_running && status == m_status
	    && fingerprint == m_fingerprint) {
		return;
	}
	m_process = process;
	m_running = running;
	m_status = status;
	if (!fingerprint.isEmpty()) m_fingerprint = fingerprint;
	emit changed();
}

void bssh_sshd_remote::read_log() {
	QFile file(log_path(m_platform));
	if (!file.open(QIODevice::ReadOnly)) return;
	if (file.size() < m_log_offset) m_log_offset = 0;   /* trimmed: read it from the top */
	if (file.size() == m_log_offset) return;
	file.seek(m_log_offset);
	while (!file.atEnd()) {
		const QByteArray line = file.readLine();
		if (!line.endsWith('\n')) break;   /* half written: next time */
		m_log_offset += line.size();
		const QString text = QString::fromUtf8(line).trimmed();
		const QTime when = QTime::fromString(text.left(8), QStringLiteral("HH:mm:ss"));
		emit logged(when.isValid() ? when : QTime::currentTime(), when.isValid() ? text.mid(9) : text);
	}
}
