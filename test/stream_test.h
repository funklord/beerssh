#ifndef BSSH_STREAM_TEST_H
#define BSSH_STREAM_TEST_H

#include <QObject>

/*
 * The reliable stream under a session over datagrams (src/session/stream.h,
 * project.md sec 5.6), between two ends over a simulated link that loses,
 * delays, reorders and repeats what it carries, on a clock the test keeps.
 */
class stream_test : public QObject {
	Q_OBJECT

private slots:
	void a_clean_link_carries_both_ways_without_resending();
	void a_bad_link_still_delivers_every_byte_once_in_order();
	void one_lost_segment_is_sent_again_before_the_timeout();
	void a_dead_link_is_tried_less_and_less_often();
	void the_round_trip_is_measured();
	void a_malformed_segment_is_refused_and_changes_nothing();
};

#endif
