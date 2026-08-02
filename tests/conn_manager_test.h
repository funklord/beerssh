#ifndef BSSH_CONN_MANAGER_TEST_H
#define BSSH_CONN_MANAGER_TEST_H

#include <QObject>

/*
 * Reconnection (project.md sec 5.3).
 *
 * The backoff schedule is a pure function taking its randomness as an
 * argument, precisely so these do not have to sit through it: a policy
 * that can only be observed by waiting sixty seconds is a policy nobody
 * ever checks.
 */
class conn_manager_test : public QObject {
	Q_OBJECT

private slots:
	void backoff_grows_and_then_stops_growing();
	void backoff_is_jittered_within_bounds();
	void jitter_separates_sessions_dropped_together();
	void a_user_disconnect_is_not_retried();
	void auto_reconnect_can_be_switched_off();
};

#endif /* BSSH_CONN_MANAGER_TEST_H */
