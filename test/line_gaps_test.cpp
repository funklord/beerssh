#include "line_gaps_test.h"

#include "session/line_gaps.h"

#include <QTest>

namespace {

struct map_t {
	/* One slot past the capacity, so a write over the limit fails a test
	 * rather than the process. */
	bssh_line_gap_t gaps[5];
	bssh_line_gaps_t lines;

	map_t() { bssh_line_gaps_init(&lines, gaps, 4); }

	uint64_t missing() const { return bssh_line_gaps_total(&lines, BSSH_LINE_GAP_MISSING); }
	uint64_t lost() const { return bssh_line_gaps_total(&lines, BSSH_LINE_GAP_LOST); }
};

}

void line_gaps_test::a_snapshot_says_how_many_lines_are_missing() {
	map_t map;
	QCOMPARE(bssh_line_gaps_extend(&map.lines, 48000), 0);
	QCOMPARE(bssh_line_gaps_fill(&map.lines, 0, 48000), 0);
	QCOMPARE(map.missing(), uint64_t(0));

	/* The example from sec 5.6: the top of the screen is line 48213. */
	QCOMPARE(bssh_line_gaps_extend(&map.lines, 48213), 0);
	QCOMPARE(map.missing(), uint64_t(213));
	QCOMPARE(map.lines.used, size_t(1));
	QCOMPARE(map.gaps[0].first, uint64_t(48000));

	/* An older snapshot arriving late moves nothing back. */
	QCOMPARE(bssh_line_gaps_extend(&map.lines, 48100), 0);
	QCOMPARE(map.lines.end, uint64_t(48213));
	QCOMPARE(map.missing(), uint64_t(213));
}

void line_gaps_test::arrivals_shrink_a_gap_from_either_end_and_split_it() {
	map_t map;
	bssh_line_gaps_extend(&map.lines, 100);

	QCOMPARE(bssh_line_gaps_fill(&map.lines, 0, 10), 0);
	QCOMPARE(map.gaps[0].first, uint64_t(10));
	QCOMPARE(bssh_line_gaps_fill(&map.lines, 90, 10), 0);
	QCOMPARE(map.gaps[0].count, uint64_t(80));

	QCOMPARE(bssh_line_gaps_fill(&map.lines, 40, 20), 0);
	QCOMPARE(map.lines.used, size_t(2));
	QCOMPARE(map.gaps[0].first, uint64_t(10));
	QCOMPARE(map.gaps[0].count, uint64_t(30));
	QCOMPARE(map.gaps[1].first, uint64_t(60));
	QCOMPARE(map.gaps[1].count, uint64_t(30));

	/* One arrival across both, and across lines held already. */
	QCOMPARE(bssh_line_gaps_fill(&map.lines, 0, 100), 0);
	QCOMPARE(map.lines.used, size_t(0));
}

void line_gaps_test::extending_after_a_fill_starts_a_new_gap() {
	map_t map;
	bssh_line_gaps_extend(&map.lines, 10);
	bssh_line_gaps_fill(&map.lines, 5, 5);
	QCOMPARE(map.missing(), uint64_t(5));

	bssh_line_gaps_extend(&map.lines, 20);
	QCOMPARE(map.lines.used, size_t(2));
	QCOMPARE(map.gaps[1].first, uint64_t(10));
	QCOMPARE(map.missing(), uint64_t(15));

	/* And the last gap grows when it reaches the end. */
	bssh_line_gaps_extend(&map.lines, 25);
	QCOMPARE(map.lines.used, size_t(2));
	QCOMPARE(map.gaps[1].count, uint64_t(15));
}

void line_gaps_test::a_fill_past_the_end_changes_nothing_beyond_it() {
	map_t map;
	bssh_line_gaps_extend(&map.lines, 10);

	QCOMPARE(bssh_line_gaps_fill(&map.lines, 10, 5), 0);
	QCOMPARE(map.missing(), uint64_t(10));

	/* A count that would wrap is still only the lines there are. */
	QCOMPARE(bssh_line_gaps_fill(&map.lines, 8, UINT64_MAX), 0);
	QCOMPARE(map.missing(), uint64_t(8));

	/* Lines not yet announced are not held when they are announced. */
	bssh_line_gaps_extend(&map.lines, 12);
	QCOMPARE(map.missing(), uint64_t(10));
}

void line_gaps_test::lost_lines_are_kept_apart_and_never_offered() {
	map_t map;
	bssh_line_gaps_extend(&map.lines, 100);

	QCOMPARE(bssh_line_gaps_lose(&map.lines, 0, 90), 0);
	QCOMPARE(map.lost(), uint64_t(90));
	QCOMPARE(map.missing(), uint64_t(10));

	uint64_t line = 0;
	QCOMPARE(bssh_line_gaps_next_wanted(&map.lines, 5, &line), 0);
	QCOMPARE(line, uint64_t(90));

	bssh_line_gaps_fill(&map.lines, 90, 10);
	QCOMPARE(bssh_line_gaps_next_wanted(&map.lines, 5, &line), -1);
	QCOMPARE(map.lost(), uint64_t(90));
}

void line_gaps_test::a_lost_line_cannot_be_filled() {
	map_t map;
	bssh_line_gaps_extend(&map.lines, 100);
	bssh_line_gaps_lose(&map.lines, 40, 20);

	/* Across the lost range: what surrounds it arrives, the range stays. */
	QCOMPARE(bssh_line_gaps_fill(&map.lines, 30, 40), 0);
	QCOMPARE(map.lost(), uint64_t(20));
	QCOMPARE(map.missing(), uint64_t(60));

	/* Entirely inside it. */
	QCOMPARE(bssh_line_gaps_fill(&map.lines, 45, 5), 0);
	QCOMPARE(map.lost(), uint64_t(20));
}

void line_gaps_test::lost_ranges_merge_with_their_neighbours() {
	map_t map;
	bssh_line_gaps_extend(&map.lines, 30);

	bssh_line_gaps_lose(&map.lines, 0, 10);
	bssh_line_gaps_lose(&map.lines, 20, 10);
	QCOMPARE(map.lines.used, size_t(3));

	bssh_line_gaps_lose(&map.lines, 10, 10);
	QCOMPARE(map.lines.used, size_t(1));
	QCOMPARE(map.gaps[0].first, uint64_t(0));
	QCOMPARE(map.gaps[0].count, uint64_t(30));
	QCOMPARE(map.gaps[0].kind, BSSH_LINE_GAP_LOST);
}

void line_gaps_test::the_line_wanted_is_the_one_nearest_the_reader() {
	map_t map;
	bssh_line_gaps_extend(&map.lines, 100);
	bssh_line_gaps_fill(&map.lines, 20, 61);

	uint64_t line = 0;
	QCOMPARE(bssh_line_gaps_next_wanted(&map.lines, 25, &line), 0);
	QCOMPARE(line, uint64_t(19));
	QCOMPARE(bssh_line_gaps_next_wanted(&map.lines, 75, &line), 0);
	QCOMPARE(line, uint64_t(81));

	/* Inside a gap, the line itself. */
	QCOMPARE(bssh_line_gaps_next_wanted(&map.lines, 5, &line), 0);
	QCOMPARE(line, uint64_t(5));

	/* Equally far, the newer: the end of history is what people read. */
	QCOMPARE(bssh_line_gaps_next_wanted(&map.lines, 50, &line), 0);
	QCOMPARE(line, uint64_t(81));
}

void line_gaps_test::running_out_of_room_leaves_the_line_missing() {
	map_t map;
	bssh_line_gaps_extend(&map.lines, 100);
	bssh_line_gaps_fill(&map.lines, 10, 10);
	bssh_line_gaps_fill(&map.lines, 30, 10);
	bssh_line_gaps_fill(&map.lines, 50, 10);
	QCOMPARE(map.lines.used, size_t(4));
	const uint64_t before = map.missing();

	/* A fourth split has nowhere to go: refused, and nothing has changed. */
	QCOMPARE(bssh_line_gaps_fill(&map.lines, 70, 10), -1);
	QCOMPARE(map.missing(), before);
	QCOMPARE(bssh_line_gaps_lose(&map.lines, 70, 10), -1);
	QCOMPARE(map.missing(), before);
	QCOMPARE(map.lost(), uint64_t(0));

	/* A fill that needs no split still works, and frees the room. */
	QCOMPARE(bssh_line_gaps_fill(&map.lines, 0, 10), 0);
	QCOMPARE(bssh_line_gaps_fill(&map.lines, 70, 10), 0);
}

void line_gaps_test::refuses_to_start_with_nowhere_to_keep_ranges() {
	bssh_line_gaps_t lines;
	bssh_line_gap_t gaps[1];

	QCOMPARE(bssh_line_gaps_init(&lines, nullptr, 1), -1);
	QCOMPARE(bssh_line_gaps_init(&lines, gaps, 0), -1);
	QCOMPARE(bssh_line_gaps_init(&lines, gaps, 1), 0);
}
