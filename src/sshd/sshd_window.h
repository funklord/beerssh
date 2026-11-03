#ifndef BSSH_SSHD_WINDOW_H
#define BSSH_SSHD_WINDOW_H

#include <QHostAddress>
#include <QWidget>

#include "sshd/sshd.h"
#include "sshd/sshd_control.h"

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTime;
class QTimer;
class bssh_sshd_keeper;
class bssh_sshd_platform;

/*
 * beersshd's one screen (project.md sec 19): whether it may reach shared
 * storage, a Start/Stop, the address and host key a client needs, the
 * keys allowed in, and a log of who came.
 *
 * Built over bssh_sshd_platform rather than over Android, so the same
 * window runs in the desktop suite against a platform the test controls.
 */
class bssh_sshd_window : public QWidget {
	Q_OBJECT

public:
	/*
	 * Drives `control`, which the window takes over; with none, a keeper
	 * of its own, running the server here -- the desktop's and the tests'
	 * case. Android's window is given a bssh_sshd_remote.
	 */
	explicit bssh_sshd_window(bssh_sshd_platform *platform, bssh_sshd_control *control = nullptr,
	                          QWidget *parent = nullptr);
	~bssh_sshd_window() override;

	/* On: serving, or waiting for Wi-Fi to. */
	bool serving() const;

	/* The server, where this window runs it itself; not for a remote one. */
	const bssh_sshd &server() const;

	/* Where the authorized keys are kept: the platform's data directory. */
	QString authorized_keys_path() const;
	QString password_path() const;

public slots:
	void start();
	void stop();

	/* Re-reads everything the platform can change: access, address. */
	void refresh();

	/*
	 * Follows the network. Listening was bound to one address, so a phone
	 * that moves to another Wi-Fi is restarted on the new one, and one
	 * that loses Wi-Fi stops -- rather than going on listening on an
	 * address that is no longer its own. Polled on a timer.
	 */
	void follow_address();

	/* Shows the authorized-keys file again, unless the box has unsaved edits. */
	void reload_keys();

private:
	void save_keys();
	void set_password(const QString &password);
	void refresh_password_state();
	void log(const QString &line);
	void log_at(const QTime &when, const QString &line);

	bssh_sshd_platform *m_platform;
	bssh_sshd_control *m_control;
	bssh_sshd_keeper *m_keeper = nullptr;   /* m_control, where it runs here */

	QLabel *m_status = nullptr;
	QLineEdit *m_fingerprint = nullptr;
	QPushButton *m_toggle = nullptr;
	QWidget *m_access = nullptr;
	QWidget *m_battery = nullptr;
	QSpinBox *m_port = nullptr;
	QPlainTextEdit *m_keys = nullptr;
	QLabel *m_key_count = nullptr;
	QLineEdit *m_password = nullptr;
	QLabel *m_password_state = nullptr;
	QPlainTextEdit *m_log = nullptr;
};

#endif /* BSSH_SSHD_WINDOW_H */
