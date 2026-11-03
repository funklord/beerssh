#ifndef BSSH_SSHD_KEEPER_H
#define BSSH_SSHD_KEEPER_H

#include <QHostAddress>

#include "sshd/sshd.h"
#include "sshd/sshd_control.h"

class QTimer;
class bssh_sshd_platform;

/*
 * The server, run in this process and kept going (project.md sec 19.6):
 * listening on the Wi-Fi address, following it when it changes, waiting
 * when there is none, serving again on its own when it is switched on
 * and something else stopped it, and telling the platform what to show
 * in the notification.
 *
 * It was the window's until the server had to run without one -- in the
 * :server process on Android, started at boot -- and it does nothing the
 * window did not. The port is read from QSettings at each start, where
 * the window writes it.
 */
class bssh_sshd_keeper : public bssh_sshd_control {
	Q_OBJECT

public:
	explicit bssh_sshd_keeper(bssh_sshd_platform *platform, QObject *parent = nullptr);
	~bssh_sshd_keeper() override;

	bool serving() const { return m_server.running(); }
	const bssh_sshd &server() const { return m_server; }

	bool active() const override { return serving(); }
	QString status_line() const override;
	QString fingerprint() const override { return m_server.host_key_fingerprint(); }

	void switch_on() override;
	void switch_off() override;
	void follow() override { follow_address(); }
	void resumed() override;

	/* Listens, if it can; a refusal is logged. Not the user's Start: see switch_on. */
	void start();
	void stop();

	/*
	 * Follows the network. A new address is listened on instead; none at
	 * all is waited out rather than stopped for, since a server that
	 * switched itself off when the phone left home would miss that
	 * night's backup. Polled on a timer.
	 */
	void follow_address();

	/* Serves now if it is switched on; for the owner to call once its signals are wired. */
	void start_if_switched_on();

	static QString authorized_keys_path(const bssh_sshd_platform *platform);
	static QString password_path(const bssh_sshd_platform *platform);

private:
	void log(const QString &line);
	void report();

	bssh_sshd_platform *m_platform;
	bssh_sshd m_server;
	QHostAddress m_bound;
	bool m_held_by_system = false;   /* stopped by Android; see time_limit_reached */
	QTimer *m_address_timer = nullptr;
};

#endif /* BSSH_SSHD_KEEPER_H */
