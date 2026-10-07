#ifndef BSSH_SSHD_PLATFORM_H
#define BSSH_SSHD_PLATFORM_H

#include <QHostAddress>
#include <QObject>
#include <QString>

/*
 * What beersshd's window needs from the platform (project.md sec 19), and
 * nothing more: where shared storage is, whether the app may reach it,
 * which address to listen on, and keeping the process alive while it
 * does. One implementation for Android, over JNI, and one for the
 * desktop, where every answer is the trivial one -- which is what lets
 * the window be built and tested in the desktop suite.
 */
class bssh_sshd_platform : public QObject {
	Q_OBJECT

public:
	/* The one this build was made with. */
	static bssh_sshd_platform *instance();

	/* The folder served as "/". */
	virtual QString storage_root() const = 0;

	/* Whether storage_root() can be read and written. */
	virtual bool has_storage_access() const = 0;

	/* Sends the user to wherever access is granted; may return at once. */
	virtual void request_storage_access() = 0;

	/*
	 * Where to listen, or a null address when there is nowhere suitable.
	 * On a phone that is the Wi-Fi address and nothing else: mobile data
	 * sits behind the carrier's NAT, and listening on it would only offer
	 * the files to whoever shares that network.
	 */
	virtual QHostAddress listen_address() const = 0;

	/*
	 * Tells the platform the server is (or is no longer) listening. On
	 * Android this is the foreground service and its notification.
	 */
	virtual void set_serving(bool serving, const QString &status) = 0;

	/*
	 * Asks, where the platform makes it a question, to show the "serving"
	 * notification. Called at Start; does nothing where nothing is asked.
	 */
	virtual void request_notifications() {}

	/* Where the host key and the authorized keys live: never inside the root. */
	virtual QString data_directory() const;

signals:
	/*
	 * The platform will no longer keep the server alive: Android's
	 * six-hour limit on file-transfer services has run out.
	 */
	void time_limit_reached();
};

#endif /* BSSH_SSHD_PLATFORM_H */
