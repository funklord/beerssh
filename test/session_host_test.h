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
};

#endif
