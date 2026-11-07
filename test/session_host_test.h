#ifndef BSSH_SESSION_HOST_TEST_H
#define BSSH_SESSION_HOST_TEST_H

#include <QObject>

/*
 * The session host and its frames (project.md sec 5.6), over a real pty and
 * real sockets: a dropped connection is a closed socket here, and the shell
 * that must survive it is a real shell.
 */
class session_host_test : public QObject {
	Q_OBJECT

private slots:
	void a_frame_round_trips_and_says_how_much_it_used();
	void a_frame_can_arrive_a_byte_at_a_time();
	void a_kind_nobody_knows_is_a_frame_to_skip();
	void a_length_past_the_limit_is_refused_at_once();
	void a_varint_with_a_second_spelling_is_refused();
	void a_shell_survives_losing_its_client();
	void output_written_while_detached_arrives_on_return();
	void a_client_too_far_behind_is_told_what_was_lost();
	void a_faithful_mode_pauses_the_program_and_loses_nothing();
	void a_second_client_replaces_the_first();
	void a_program_ending_is_reported_and_ends_the_host();
	void nobody_coming_back_ends_the_host_after_the_linger();
	void garbage_loses_the_client_and_not_the_session();
	void the_client_delivers_output_split_anywhere();
	void the_client_never_joins_text_across_a_gap();
	void the_client_is_told_what_a_lost_frame_took();
	void a_kept_session_shows_a_hole_as_lines();
	void the_client_drops_what_it_has_already_seen();
	void the_client_keeps_its_place_across_a_new_link();
	void the_client_acknowledges_when_due_and_not_before();
	void the_client_refuses_a_peer_not_speaking_the_protocol();
	void a_client_and_a_real_host_agree_from_the_first_byte();
	void a_client_asking_past_the_end_is_told_it_is_a_new_session();
	void a_hangup_ends_the_program_and_the_host();
	void a_host_not_asked_is_not_probed();
	void a_host_without_the_program_gets_a_plain_shell();
	void a_host_with_the_program_gets_a_kept_session();
	void a_reconnect_returns_to_the_same_place_with_modes_kept();
	void a_banner_ahead_of_the_program_falls_back_on_the_same_connection();
	void closing_a_kept_session_hangs_it_up();
	void a_new_shell_after_a_reboot_is_shown_and_said();
	void a_client_is_told_when_it_has_caught_up();
	void a_restored_session_finds_its_shell_and_replays_in_silence();
	void a_session_is_named_before_it_is_asked_anything();
	void keeping_every_update_switches_during_a_session();
	void a_mode_set_with_nothing_kept_waits_for_the_attach();
};

#endif
