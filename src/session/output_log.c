#include "session/output_log.h"

#include <string.h>

int bssh_output_log_init(bssh_output_log_t *log, uint8_t *bytes, size_t capacity,
                         bssh_output_log_mark_t *marks, size_t marks_capacity) {
	if (!log || !bytes || !capacity || !marks || !marks_capacity) return -1;

	memset(log, 0, sizeof(*log));
	log->bytes = bytes;
	log->capacity = capacity;
	log->marks = marks;
	log->marks_capacity = marks_capacity;
	return 0;
}

static void add_mark(bssh_output_log_t *log, uint64_t offset, uint64_t at_ms) {
	/*
	 * One mark per millisecond at most: `yes` through a pty is about 276,000
	 * reads a second, and timing finer than this is worth nothing to a replay.
	 */
	if (log->marks_used > 0) {
		const size_t last = (log->marks_first + log->marks_used - 1) % log->marks_capacity;
		if (log->marks[last].at_ms == at_ms) return;
	}

	/* Out of marks costs the oldest timing, never a wrong one. */
	if (log->marks_used == log->marks_capacity) {
		log->marks_first = (log->marks_first + 1) % log->marks_capacity;
		--log->marks_used;
	}

	const size_t next = (log->marks_first + log->marks_used) % log->marks_capacity;
	log->marks[next].offset = offset;
	log->marks[next].at_ms = at_ms;
	++log->marks_used;
}

static void copy_in(bssh_output_log_t *log, uint64_t offset, const uint8_t *data, size_t len) {
	const size_t at = (size_t)(offset % log->capacity);
	const size_t first = len < log->capacity - at ? len : log->capacity - at;

	memcpy(log->bytes + at, data, first);
	memcpy(log->bytes, data + first, len - first);
}

size_t bssh_output_log_append(bssh_output_log_t *log, const uint8_t *data, size_t len,
                              uint64_t at_ms) {
	if (!log || !data || !len) return 0;

	size_t taken = len;
	if (log->faithful) {
		const size_t room = log->capacity - (size_t)(log->end - log->acked);
		if (taken > room) taken = room;
		if (!taken) return 0;
	}

	add_mark(log, log->end, at_ms);

	/* Only the tail of a write larger than the whole log can be kept. */
	const size_t kept = taken < log->capacity ? taken : log->capacity;
	copy_in(log, log->end + (taken - kept), data + (taken - kept), kept);
	log->end += taken;

	if (log->end - log->start > log->capacity) {
		const uint64_t new_start = log->end - log->capacity;

		/* Bytes the client already holds are not lost by being evicted. */
		const uint64_t from = log->start > log->acked ? log->start : log->acked;
		if (new_start > from) log->lost += new_start - from;
		log->start = new_start;
	}

	return taken;
}

bssh_output_log_answer_t bssh_output_log_read(const bssh_output_log_t *log, uint64_t offset,
                                              uint8_t *out, size_t out_capacity, size_t *out_len) {
	*out_len = 0;
	if (offset < log->start) return BSSH_OUTPUT_LOG_GONE;
	if (offset >= log->end) return BSSH_OUTPUT_LOG_ABSENT;

	const uint64_t held = log->end - offset;
	const size_t len = held < out_capacity ? (size_t)held : out_capacity;
	const size_t at = (size_t)(offset % log->capacity);
	const size_t first = len < log->capacity - at ? len : log->capacity - at;

	memcpy(out, log->bytes + at, first);
	memcpy(out + first, log->bytes, len - first);
	*out_len = len;
	return BSSH_OUTPUT_LOG_OK;
}

void bssh_output_log_ack(bssh_output_log_t *log, uint64_t offset) {
	if (offset > log->end) offset = log->end;
	if (offset > log->acked) log->acked = offset;
}

void bssh_output_log_set_faithful(bssh_output_log_t *log, int faithful) {
	log->faithful = faithful != 0;

	/*
	 * Live may have evicted past what the client acknowledged, and what is gone
	 * cannot be guarded: the room a faithful append measures starts at what is
	 * still held, or end - acked would exceed the capacity and wrap.
	 */
	if (log->faithful && log->acked < log->start) log->acked = log->start;
}

int bssh_output_log_time_of(const bssh_output_log_t *log, uint64_t offset, uint64_t *at_ms) {
	int found = -1;

	for (size_t i = 0; i < log->marks_used; ++i) {
		const bssh_output_log_mark_t *mark = &log->marks[(log->marks_first + i) % log->marks_capacity];
		if (mark->offset > offset) break;
		*at_ms = mark->at_ms;
		found = 0;
	}
	return found;
}
