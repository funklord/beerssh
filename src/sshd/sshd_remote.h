#ifndef BSSH_SSHD_REMOTE_H
#define BSSH_SSHD_REMOTE_H

#include <QDateTime>
#include <QStringList>

#include "sshd/sshd_control.h"

class QTimer;
class bssh_sshd_platform;
class bssh_sshd_keeper;

/*
 * The server in another process, as the window sees it (project.md sec
 * 19.6). On Android it runs in beersshd's :server process, where a boot
 * can start it with no app; this end starts and stops that process and
 * reads what it reports.
 *
 * Through two files in the data directory, written only by the server's
 * process: `server.status`, replaced whole at every change, and
 * `server.log`, appended. Everything else the window changes -- the keys,
 * the password, the port -- was a file the server reads already. A
 * status is believed only while the process it names is alive, so one
 * Android killed reads as stopped rather than as serving for ever.
 */
class bssh_sshd_remote : public bssh_sshd_control {
	Q_OBJECT

public:
	explicit bssh_sshd_remote(bssh_sshd_platform *platform, QObject *parent = nullptr);

	bool active() const override;
	QString status_line() const override;
	QString fingerprint() const override { return m_fingerprint; }

	void switch_on() override;
	void switch_off() override;
	void follow() override;

	static QString status_path(const bssh_sshd_platform *platform);
	static QString log_path(const bssh_sshd_platform *platform);

	/*
	 * The server process's half: keeps both files up to date from a
	 * keeper running there. Trims the log to its last part when it has
	 * grown large, so a server left on for months does not fill the
	 * app's storage with it.
	 */
	static void report_from(bssh_sshd_keeper *keeper, bssh_sshd_platform *platform);

	/* Whether `pid` is a process of this app's server, by its command line. */
	static bool server_alive(qint64 pid);

	/* The process-name suffix server_alive() wants; the tests make it any process. */
	static void set_process_suffix(const QByteArray &suffix);

private:
	void read_status();
	void read_log();

	bssh_sshd_platform *m_platform;
	QTimer *m_timer;

	bool m_running = false;      /* the process is alive and says it is serving */
	bool m_process = false;      /* the process is alive at all */
	QString m_status;
	QString m_fingerprint;
	QDateTime m_asked;           /* when the process was last asked to start */
	qint64 m_log_offset = 0;
};

#endif /* BSSH_SSHD_REMOTE_H */
