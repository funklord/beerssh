#ifndef BSSH_OUTPUT_LOG_TEST_H
#define BSSH_OUTPUT_LOG_TEST_H

#include <QObject>

/* The raw log of a session's output (project.md sec 5.6). */
class output_log_test : public QObject {
	Q_OBJECT

private slots:
	void reads_back_what_was_written_across_the_wrap();
	void says_gone_and_absent_apart();
	void live_evicts_the_oldest_and_counts_what_the_client_lost();
	void a_write_larger_than_the_log_keeps_its_tail();
	void faithful_never_overwrites_what_the_client_lacks();
	void an_ack_never_moves_backwards_or_past_the_end();
	void moving_to_faithful_guards_what_is_still_held();
	void marks_coalesce_and_lose_only_the_oldest_timing();
	void refuses_to_start_with_nowhere_to_keep_anything();
};

#endif
