#ifndef BSSH_SSHD_WINDOW_H
#define BSSH_SSHD_WINDOW_H

#include <QHostAddress>
#include <QWidget>

#include "sshd/sshd.h"

class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QTimer;
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
	explicit bssh_sshd_window(bssh_sshd_platform *platform, QWidget *parent = nullptr);
	~bssh_sshd_window() override;

	bool serving() const { return m_server.running(); }
	const bssh_sshd &server() const { return m_server; }

	/* Where the authorized keys are kept: the platform's data directory. */
	QString authorized_keys_path() const;

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
	void log(const QString &line);
	void update_status();
	QString status_line() const;

	bssh_sshd_platform *m_platform;
	bssh_sshd m_server;
	QHostAddress m_bound;

	QLabel *m_status = nullptr;
	QLabel *m_fingerprint = nullptr;
	QPushButton *m_toggle = nullptr;
	QWidget *m_access = nullptr;
	QSpinBox *m_port = nullptr;
	QPlainTextEdit *m_keys = nullptr;
	QLabel *m_key_count = nullptr;
	QPlainTextEdit *m_log = nullptr;
	QTimer *m_address_timer = nullptr;
};

#endif /* BSSH_SSHD_WINDOW_H */
