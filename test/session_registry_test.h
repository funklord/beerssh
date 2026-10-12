#ifndef BSSH_SESSION_REGISTRY_TEST_H
#define BSSH_SESSION_REGISTRY_TEST_H

#include <QObject>

/*
 * The registry (project.md sec 3, core principle 2).
 *
 * The point of these is the one property the whole architecture rests on:
 * a session is not a widget's possession. If that holds, tabs, splits,
 * activity recreation and background survival are bookkeeping; if it ever
 * stops holding, all four break at once and none of them obviously.
 */
class session_registry_test : public QObject {
	Q_OBJECT

private slots:
	void creates_and_lists_sessions();
	void session_outlives_the_view_that_showed_it();
	void closing_announces_before_destroying();
	void live_count_ignores_dead_sessions();

	/* Only a CONNECTED session of a keep-awake host asks (sec 5.5, 2026-09-26). */
	void only_a_connected_keep_awake_host_keeps_the_computer_awake();
	void title_falls_back_from_osc_to_profile();
	void the_transport_reports_which_kind_of_ending_it_was();
	void a_shell_that_ends_closes_its_session();
	void a_held_session_stays_when_its_shell_ends();
	void a_password_is_requested_only_when_keys_fail();
	void a_host_key_is_never_trusted_without_an_answer();
	void an_encrypted_key_is_offered_before_a_password();
	void a_hosts_keepalive_interval_reaches_its_transport();
	void every_resolved_setting_reaches_something_that_uses_it();
	void the_on_connect_command_runs_on_every_connect();
	void a_reconnect_hands_the_new_shell_a_clean_terminal();
	void editing_a_host_reaches_the_session_already_open_on_it();
	void a_reconnected_session_shows_and_takes_the_new_shell();
	void a_silenced_bell_does_not_mark_the_tab();
	void turning_scrollback_off_actually_turns_it_off();

	/* sec 8.3's window title, which is a format rather than a choice. */
	void the_window_title_expands_what_the_session_knows();
	void the_three_events_sec_5_5_names_reach_the_platform();
};

#endif /* BSSH_SESSION_REGISTRY_TEST_H */
