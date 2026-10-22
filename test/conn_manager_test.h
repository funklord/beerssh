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
	void a_network_change_does_not_dial_a_session_never_connected();
	void backoff_is_jittered_within_bounds();
	void jitter_separates_sessions_dropped_together();
	void a_network_flap_staggers_the_sessions_rather_than_dialling_them();
	void a_failing_reconnect_does_not_reset_its_own_backoff();
	void an_exited_shell_is_not_reopened();
	void a_dropped_connection_is_still_retried();
	void a_user_disconnect_is_not_retried();
	void a_user_disconnect_is_not_retried_when_nobody_calls_the_setter();
	void auto_reconnect_can_be_switched_off();

	/*
	 * A password the server refused is never offered again (sec 11) --
	 * not by the session, not by a reconnect -- and a refusal is not a
	 * drop the manager dials after. Reported from a phone, where a wrong
	 * password was retried until the manager gave up: the way a server
	 * counting failures blackholes the address.
	 */
	void a_refused_password_is_never_offered_again();
	void a_refused_password_is_not_reconnected();
	void a_reconnect_offers_only_the_password_that_got_in();
	void a_password_never_answered_is_not_replayed();
};

#endif /* BSSH_CONN_MANAGER_TEST_H */
