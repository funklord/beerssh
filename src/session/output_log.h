#ifndef BSSH_OUTPUT_LOG_H
#define BSSH_OUTPUT_LOG_H

/*
 * Every byte a session's program wrote, numbered from the start of the
 * session, kept up to a limit (project.md sec 5.6). It is the raw log: the
 * ground truth a faithful replay is made from, and what a client asks for
 * by offset after a drop.
 *
 * Plain C with the caller's memory, no Qt and no allocation, because this
 * layer may move into fuzznet. fuzznet's log/ has the design and cannot
 * carry this traffic: sec 5.6 has the measurement.
 *
 * THE ANSWER TO A READ SAYS WHY IT HOLDS NOTHING. "Retention took it" and
 * "it has not been written yet" call for opposite things: a client told the
 * first stops asking and draws the loss, and a client told the second waits.
 * Left as one answer, a client that fell behind asks for ever. fuzznet's
 * FZN_LOG_ERR_GONE and FZN_LOG_ERR_ABSENT make the same split, for the same
 * reason.
 *
 * TWO WAYS TO BE FULL, chosen by bssh_output_log_set_faithful().
 *
 * Not faithful (Live): the oldest bytes go and are counted, so a program
 * printing forever costs a bounded amount and never waits for the network.
 *
 * Faithful (Serial, Eventual): bytes the client has not acknowledged are
 * never overwritten. Append takes what fits and reports it, the caller stops
 * reading the pty, the pty's buffer fills, and the program blocks in
 * write(). That pause is what makes "every state" true. Dropping here would
 * promise something not delivered.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum bssh_output_log_answer {
	BSSH_OUTPUT_LOG_OK = 0,
	/* Retention took it; it will never be readable. */
	BSSH_OUTPUT_LOG_GONE,
	/* Not written yet; asking again later may help. */
	BSSH_OUTPUT_LOG_ABSENT,
} bssh_output_log_answer_t;

/* From this offset on, the bytes were written at about this time. */
typedef struct bssh_output_log_mark {
	uint64_t offset;
	uint64_t at_ms;
} bssh_output_log_mark_t;

typedef struct bssh_output_log {
	uint8_t *bytes;
	size_t capacity;
	bssh_output_log_mark_t *marks;
	size_t marks_capacity;
	size_t marks_used;
	size_t marks_first;
	uint64_t start;    /* oldest offset held */
	uint64_t end;      /* offset the next byte will have */
	uint64_t acked;    /* the client holds everything below this */
	uint64_t lost;     /* bytes evicted before the client had them */
	uint64_t start_line;  /* newlines before `start`: its line, from the session's start */
	int faithful;
} bssh_output_log_t;

/*
 * Both arrays are the caller's and outlive the log. Returns 0, or -1 when
 * there is nowhere to keep bytes or marks.
 */
int bssh_output_log_init(bssh_output_log_t *log, uint8_t *bytes, size_t capacity,
                         bssh_output_log_mark_t *marks, size_t marks_capacity);

/*
 * Takes bytes written at `at_ms` and returns how many it took: all of them
 * unless faithful, and in faithful as many as leave every unacknowledged
 * byte in place. 0 from a faithful log means stop reading the pty until
 * bssh_output_log_ack() makes room.
 */
size_t bssh_output_log_append(bssh_output_log_t *log, const uint8_t *data, size_t len,
                              uint64_t at_ms);

/*
 * Copies up to `out_capacity` bytes from `offset`, setting *out_len. OK means
 * at least one byte, or an empty read of an empty buffer.
 */
bssh_output_log_answer_t bssh_output_log_read(const bssh_output_log_t *log, uint64_t offset,
                                              uint8_t *out, size_t out_capacity, size_t *out_len);

/* The client holds everything below `offset`. Never moves backwards, and
 * never past what was written: a client cannot acknowledge the future. */
void bssh_output_log_ack(bssh_output_log_t *log, uint64_t offset);

/*
 * Switching takes effect at the next append. Moving to faithful guards only
 * what is still held: what Live already evicted stays lost, and `start` says
 * from where the record is complete.
 */
void bssh_output_log_set_faithful(bssh_output_log_t *log, int faithful);

/*
 * When the byte at `offset` was written, to the resolution marks allow.
 * Returns 0 and sets *at_ms, or -1 when no held mark reaches back that far.
 */
int bssh_output_log_time_of(const bssh_output_log_t *log, uint64_t offset, uint64_t *at_ms);

#ifdef __cplusplus
}
#endif

#endif
