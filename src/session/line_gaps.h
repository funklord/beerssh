#ifndef BSSH_LINE_GAPS_H
#define BSSH_LINE_GAPS_H

/*
 * Which lines of a session's history the client lacks (project.md sec 5.6).
 *
 * Lines are numbered from the start of the session. A snapshot names the
 * line at the top of the screen, so a client that fell behind knows exactly
 * how many it lacks and can draw them as missing, then fill them as they
 * arrive, nearest the reader first.
 *
 * A range is MISSING, which backfill may still deliver, or LOST, which the
 * server's limit took for good. They are drawn differently and only one is
 * worth asking for: fuzznet's ABSENT and GONE again, for lines.
 *
 * Bounded by the caller's array, because a flood of scattered arrivals must
 * not grow this. Running out of room to split a range refuses the split and
 * leaves the whole range missing, so the worst outcome is asking again for a
 * line already held. Claiming to hold a line that is not there would show the
 * reader text that never arrived, or join the text either side of a gap.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum bssh_line_gap_kind {
	BSSH_LINE_GAP_MISSING = 0,
	BSSH_LINE_GAP_LOST,
} bssh_line_gap_kind_t;

typedef struct bssh_line_gap {
	uint64_t first;
	uint64_t count;
	bssh_line_gap_kind_t kind;
} bssh_line_gap_t;

typedef struct bssh_line_gaps {
	bssh_line_gap_t *gaps;   /* sorted by first, never touching a like kind */
	size_t capacity;
	size_t used;
	uint64_t end;            /* every line below this is held or in a gap */
} bssh_line_gaps_t;

/* Returns 0, or -1 with nowhere to keep ranges. */
int bssh_line_gaps_init(bssh_line_gaps_t *lines, bssh_line_gap_t *gaps, size_t capacity);

/*
 * The server says history now reaches `end`. Everything new is missing until
 * it arrives. Returns 0, or -1 when there is no room for the new range and
 * `end` has not moved, so the next snapshot tries again.
 */
int bssh_line_gaps_extend(bssh_line_gaps_t *lines, uint64_t end);

/*
 * Lines [first, first + count) arrived. Lines the client holds already, and
 * lines beyond `end`, change nothing. Returns 0, or -1 when a split found no
 * room and the range stayed missing.
 */
int bssh_line_gaps_fill(bssh_line_gaps_t *lines, uint64_t first, uint64_t count);

/*
 * The server's limit took [first, first + count) for good, whatever the
 * client thought of them. Returns 0, or -1 when a split found no room and
 * the range stayed missing.
 */
int bssh_line_gaps_lose(bssh_line_gaps_t *lines, uint64_t first, uint64_t count);

/* How many lines are in gaps of this kind. */
uint64_t bssh_line_gaps_total(const bssh_line_gaps_t *lines, bssh_line_gap_kind_t kind);

/*
 * The missing line nearest `near`, the line the reader is looking at, ties
 * going to the newer. Returns 0 and sets *line, or -1 when nothing is
 * missing. Lost lines are never offered: nobody can send them.
 */
int bssh_line_gaps_next_wanted(const bssh_line_gaps_t *lines, uint64_t near, uint64_t *line);

#ifdef __cplusplus
}
#endif

#endif
