#include "session/line_gaps.h"

#include <string.h>

int bssh_line_gaps_init(bssh_line_gaps_t *lines, bssh_line_gap_t *gaps, size_t capacity) {
	if (!lines || !gaps || !capacity) return -1;

	memset(lines, 0, sizeof(*lines));
	lines->gaps = gaps;
	lines->capacity = capacity;
	return 0;
}

static uint64_t end_of(const bssh_line_gap_t *gap) {
	return gap->first + gap->count;
}

static void remove_at(bssh_line_gaps_t *lines, size_t at) {
	memmove(&lines->gaps[at], &lines->gaps[at + 1], (lines->used - at - 1) * sizeof(lines->gaps[0]));
	--lines->used;
}

static void insert_at(bssh_line_gaps_t *lines, size_t at, bssh_line_gap_t gap) {
	memmove(&lines->gaps[at + 1], &lines->gaps[at], (lines->used - at) * sizeof(lines->gaps[0]));
	lines->gaps[at] = gap;
	++lines->used;
}

/* Keeps "never touching a like kind", so the count of ranges stays honest. */
static void merge_around(bssh_line_gaps_t *lines, size_t at) {
	if (at + 1 < lines->used && end_of(&lines->gaps[at]) == lines->gaps[at + 1].first
	    && lines->gaps[at].kind == lines->gaps[at + 1].kind) {
		lines->gaps[at].count += lines->gaps[at + 1].count;
		remove_at(lines, at + 1);
	}
	if (at > 0 && end_of(&lines->gaps[at - 1]) == lines->gaps[at].first
	    && lines->gaps[at - 1].kind == lines->gaps[at].kind) {
		lines->gaps[at - 1].count += lines->gaps[at].count;
		remove_at(lines, at);
	}
}

int bssh_line_gaps_extend(bssh_line_gaps_t *lines, uint64_t end) {
	if (end <= lines->end) return 0;

	bssh_line_gap_t *last = lines->used ? &lines->gaps[lines->used - 1] : NULL;
	if (last && last->kind == BSSH_LINE_GAP_MISSING && end_of(last) == lines->end) {
		last->count += end - lines->end;
		lines->end = end;
		return 0;
	}

	if (lines->used == lines->capacity) return -1;
	const bssh_line_gap_t gap = { lines->end, end - lines->end, BSSH_LINE_GAP_MISSING };
	insert_at(lines, lines->used, gap);
	lines->end = end;
	return 0;
}

/*
 * Whether cutting [first, last) leaves this gap as two: taking the middle
 * out of a range needs one more slot.
 */
static int splits(const bssh_line_gap_t *gap, uint64_t first, uint64_t last) {
	return gap->first < first && last < end_of(gap);
}

/*
 * Takes [first, last) out of every gap. All room is checked before anything
 * changes, so a refusal leaves the map exactly as it was.
 */
static int cut(bssh_line_gaps_t *lines, uint64_t first, uint64_t last) {
	size_t needed = 0;
	for (size_t i = 0; i < lines->used; ++i) needed += (size_t)splits(&lines->gaps[i], first, last);
	if (lines->used + needed > lines->capacity) return -1;

	for (size_t i = 0; i < lines->used;) {
		bssh_line_gap_t *gap = &lines->gaps[i];
		const uint64_t gap_end = end_of(gap);

		if (gap_end <= first || gap->first >= last) {
			++i;
		} else if (gap->first >= first && gap_end <= last) {
			remove_at(lines, i);
		} else if (splits(gap, first, last)) {
			const bssh_line_gap_t tail = { last, gap_end - last, gap->kind };
			gap->count = first - gap->first;
			insert_at(lines, i + 1, tail);
			i += 2;
		} else if (gap->first < first) {
			gap->count = first - gap->first;
			++i;
		} else {
			gap->count = gap_end - last;
			gap->first = last;
			++i;
		}
	}
	return 0;
}

int bssh_line_gaps_fill(bssh_line_gaps_t *lines, uint64_t first, uint64_t count) {
	if (first >= lines->end || !count) return 0;

	uint64_t last = count > lines->end - first ? lines->end : first + count;

	/* A lost line cannot arrive: whatever claims to be one is not filled in. */
	for (size_t i = 0; i < lines->used; ++i) {
		const bssh_line_gap_t *gap = &lines->gaps[i];
		if (gap->kind != BSSH_LINE_GAP_LOST) continue;
		if (gap->first >= last || end_of(gap) <= first) continue;

		if (first < gap->first) {
			const int status = bssh_line_gaps_fill(lines, first, gap->first - first);
			if (status) return status;
		}
		if (end_of(gap) >= last) return 0;
		first = end_of(gap);
	}
	return cut(lines, first, last);
}

int bssh_line_gaps_lose(bssh_line_gaps_t *lines, uint64_t first, uint64_t count) {
	if (first >= lines->end || !count) return 0;

	const uint64_t last = count > lines->end - first ? lines->end : first + count;

	/* The cut may split one range in two, and the lost range takes a slot. */
	size_t needed = 1;
	for (size_t i = 0; i < lines->used; ++i) needed += (size_t)splits(&lines->gaps[i], first, last);
	if (lines->used + needed > lines->capacity) return -1;

	cut(lines, first, last);

	size_t at = 0;
	while (at < lines->used && lines->gaps[at].first < first) ++at;
	const bssh_line_gap_t gap = { first, last - first, BSSH_LINE_GAP_LOST };
	insert_at(lines, at, gap);
	merge_around(lines, at);
	return 0;
}

uint64_t bssh_line_gaps_total(const bssh_line_gaps_t *lines, bssh_line_gap_kind_t kind) {
	uint64_t total = 0;
	for (size_t i = 0; i < lines->used; ++i) {
		if (lines->gaps[i].kind == kind) total += lines->gaps[i].count;
	}
	return total;
}

int bssh_line_gaps_next_wanted(const bssh_line_gaps_t *lines, uint64_t near, uint64_t *line) {
	int found = -1;
	uint64_t best_distance = 0;

	for (size_t i = 0; i < lines->used; ++i) {
		const bssh_line_gap_t *gap = &lines->gaps[i];
		if (gap->kind != BSSH_LINE_GAP_MISSING) continue;

		const uint64_t top = end_of(gap) - 1;
		const uint64_t candidate = near < gap->first ? gap->first : near > top ? top : near;
		const uint64_t distance = candidate > near ? candidate - near : near - candidate;

		if (found < 0 || distance <= best_distance) {
			*line = candidate;
			best_distance = distance;
			found = 0;
		}
	}
	return found;
}
