#ifndef BSSH_SSHD_CONTROL_H
#define BSSH_SSHD_CONTROL_H

#include <QObject>
#include <QString>
#include <QTime>

/*
 * What beersshd's window drives (project.md sec 19.6): the server, where
 * it runs.
 *
 * Two answers. bssh_sshd_keeper runs it in this process, which is the
 * desktop's and the tests' case. bssh_sshd_remote drives the one in
 * beersshd's own :server process on Android, which exists so that the
 * server outlives the app -- and can be started at boot, when there is no
 * app: Qt restarts the whole process when an activity opens in one where
 * a service already started Qt, so the two cannot share it.
 */
class bssh_sshd_control : public QObject {
	Q_OBJECT

public:
	using QObject::QObject;

	/* Whether it is on: serving, or waiting for Wi-Fi to serve. Start/Stop shows this. */
	virtual bool active() const = 0;

	virtual QString status_line() const = 0;

	/* The host key as ssh(1) prints it, or empty before the first start. */
	virtual QString fingerprint() const = 0;

	/* The user's Start and Stop: these alone change whether it is switched on. */
	virtual void switch_on() = 0;
	virtual void switch_off() = 0;

	/* Looks again at what may have changed: the address, the other process. */
	virtual void follow() = 0;

	/*
	 * Comes to the front: Android may have said no to running, and that
	 * answer was for the app in the background.
	 */
	virtual void resumed() {}

signals:
	/* One line, stamped when it happened rather than when it was shown. */
	void logged(const QTime &when, const QString &line);

	/* active(), status_line() or fingerprint() may have changed. */
	void changed();
};

#endif /* BSSH_SSHD_CONTROL_H */
