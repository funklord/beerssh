#ifndef BSSH_LINE_GAPS_TEST_H
#define BSSH_LINE_GAPS_TEST_H

#include <QObject>

/* The exact shape of what the client lacks (project.md sec 5.6). */
class line_gaps_test : public QObject {
	Q_OBJECT

private slots:
	void a_snapshot_says_how_many_lines_are_missing();
	void arrivals_shrink_a_gap_from_either_end_and_split_it();
	void extending_after_a_fill_starts_a_new_gap();
	void a_fill_past_the_end_changes_nothing_beyond_it();
	void lost_lines_are_kept_apart_and_never_offered();
	void a_lost_line_cannot_be_filled();
	void lost_ranges_merge_with_their_neighbours();
	void the_line_wanted_is_the_one_nearest_the_reader();
	void running_out_of_room_leaves_the_line_missing();
	void refuses_to_start_with_nowhere_to_keep_ranges();
};

#endif
