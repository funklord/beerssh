#ifndef BSSH_CONN_MANAGER_H
#define BSSH_CONN_MANAGER_H

#include <QObject>

class QTimer;
class bssh_session;

/*
 * bssh_conn_manager -- decides when to try again (project.md sec 5.3).
 *
 * One per session, so a split view can show one pane healthy and another
 * reconnecting. It consumes the platform's network_changed signal, which
 * on desktop is mostly a laptop waking from suspend and on Android is a
 * Wi-Fi/cellular switch -- both leave TCP connections dead in a way that
 * looks identical from here.
 *
 * **The honest limitation, stated once and loudly:** plain SSH cannot
 * resume a dropped shell. A reconnect opens a NEW remote shell; the
 * running program and the working directory are gone. Scrollback survives
 * because it is local. Mosh is the only real fix and is a v2 item; the
 * available answer today is a preset's on-connect command attaching to
 * tmux (sec 5.3, sec 10.2). Nothing in this class pretends otherwise.
 */
class bssh_conn_manager : public QObject {
	Q_OBJECT

public:
	explicit bssh_conn_manager(bssh_session *session, QObject *parent = nullptr);

	/*
	 * Delay before attempt N, in milliseconds. Pure, and deliberately
	 * takes its randomness as an argument: a backoff schedule that can
	 * only be observed by waiting for it is a backoff schedule nobody
	 * tests.
	 *
	 * Exponential from base, capped, then spread by jitter. The jitter is
	 * not decoration -- with several sessions dropped by one network
	 * flap, an unjittered backoff reconnects them all in the same
	 * millisecond, which is the storm sec 5.3 warns about.
	 */
	static int backoff_delay_ms(int attempt, int random_percent);

	static int base_delay_ms() { return 1000; }
	static int max_delay_ms() { return 60000; }

	void set_auto_reconnect(bool enabled);
	bool auto_reconnect() const { return m_auto_reconnect; }

	bool is_reconnecting() const { return m_reconnecting; }
	int attempt() const { return m_attempt; }

	/*
	 * The user asked to disconnect. Suppresses reconnection until the
	 * next deliberate connect -- retrying a connection someone just
	 * closed is the most annoying possible bug in this area.
	 */
	void note_user_disconnect();

	/* Connectivity changed: retry now rather than waiting out the backoff. */
	void note_network_changed();

signals:
	void reconnect_scheduled(int delay_ms, int attempt);
	void reconnect_started(int attempt);
	void gave_up();

private slots:
	void on_session_state_changed();
	void attempt_reconnect();

private:
	void schedule_reconnect();
	void cancel();

	bssh_session *m_session;
	QTimer *m_timer;
	bool m_auto_reconnect;
	bool m_reconnecting;
	bool m_user_disconnected;
	int m_attempt;
};

#endif /* BSSH_CONN_MANAGER_H */
