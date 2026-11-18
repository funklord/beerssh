#ifndef BSSH_STREAM_H
#define BSSH_STREAM_H

/*
 * A reliable, ordered byte stream over datagrams that arrive once or not at
 * all, in any order (project.md sec 5.6): what the session's frames travel in
 * where the transport is fuzznet's say and heard, which send each word once
 * and leave resending to the caller.
 *
 * No I/O and no clock of its own. The caller hands in what arrived and the
 * time, takes out what to send, and asks when to come back; so it runs the
 * same in a test's simulated link as over a radio.
 *
 *   - Each side numbers its bytes from 0, and a segment says where its data
 *     begins, what of the other side's stream has arrived, and how much more
 *     may be sent (struct bssh_segment in frame.situ).
 *   - What has not been acknowledged is sent again once a retransmission
 *     timeout passes -- estimated from round trips, doubling while nothing
 *     is heard -- or at once after three acknowledgements that move nothing,
 *     which is the far side saying a segment went missing.
 *   - How much is in flight follows a congestion window that grows while
 *     acknowledgements come and halves on a loss, so a slow link is not
 *     flooded and a stalled one does not take what the user types after
 *     everything else.
 *   - A segment past the next byte expected is dropped and answered with an
 *     acknowledgement of where the receiver stands: the sender goes back to
 *     there. Keeping them instead would save resending on a link that
 *     reorders; terminal traffic is small enough not to need it yet.
 *
 * The end of a stream is not carried: the session says hangup and exited in
 * its own frames, and the transport's session ends with it.
 *
 * Each stream has an EPOCH, in every segment both ways: the client draws it
 * for each stream it opens, the host takes it from the first segment it
 * hears, and a segment of another epoch is not this stream's. That is what
 * tells a stream begun again after a drop, from offset 0, from the one before
 * it, whose segments may still be arriving.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BSSH_STREAM_RTO_INITIAL_MS 1000u
#define BSSH_STREAM_RTO_MIN_MS 200u
#define BSSH_STREAM_RTO_MAX_MS 60000u

typedef struct bssh_stream {
	/* Which stream this is, both ways (see above). */
	uint64_t epoch;

	/* Largest data a segment carries, which the transport's datagram decides. */
	size_t mss;

	/* Sent and not yet acknowledged, then not yet sent: a ring the caller owns. */
	uint8_t *ring;
	size_t ring_capacity;
	size_t ring_start;  /* where the byte at `unacked` sits */
	size_t ring_len;    /* bytes held, from `unacked` */

	uint64_t unacked;   /* oldest byte not acknowledged */
	uint64_t next;      /* next byte to send, from `unacked` up to the end held */
	uint64_t sent_max;  /* the furthest any segment has reached */
	uint64_t peer_window;

	/* Congestion, in bytes. */
	uint64_t cwnd;
	uint64_t ssthresh;
	unsigned duplicate_acks;
	uint64_t recover;   /* no second halving for losses before this */

	/* Round trips (RFC 6298), and the timer they set. */
	uint32_t srtt_ms;
	uint32_t rttvar_ms;
	uint32_t rto_ms;
	int have_rtt;
	int timing;         /* a segment's round trip is being timed */
	uint64_t timed_end; /* acknowledged up to here, it was */
	uint64_t timed_at;
	int armed;
	uint64_t deadline;
	unsigned backoffs;

	/* The other side's stream. */
	uint64_t received;  /* next byte expected */
	uint64_t window;    /* what we tell them they may send past `received` */
	int ack_due;
	unsigned duplicates_owed; /* one bare acknowledgement per segment past a hole */

	/* Counts, for tests and diagnostics. */
	uint64_t retransmitted;
	uint64_t dropped_ahead;
} bssh_stream_t;

/* `ring` holds what is written until it is acknowledged; `mss` at least 1. */
void bssh_stream_init(bssh_stream_t *stream, uint8_t *ring, size_t ring_capacity, size_t mss,
                      uint64_t window, uint64_t epoch);

/* The epoch a segment names, in `out`: 0, or -1 for one that is malformed. */
int bssh_stream_segment_epoch(const uint8_t *segment, size_t len, uint64_t *out);

/* Takes as much of `bytes` as there is room for, and says how much that was. */
size_t bssh_stream_write(bssh_stream_t *stream, const uint8_t *bytes, size_t len);

/* Bytes written and not yet acknowledged. */
size_t bssh_stream_pending(const bssh_stream_t *stream);

/*
 * A segment that arrived. Data that is next in order goes to `deliver`, which
 * takes it all; the rest is dropped. -1, and nothing changed, for one that is
 * malformed, of another epoch, or acknowledges bytes never sent.
 */
int bssh_stream_receive(bssh_stream_t *stream, const uint8_t *segment, size_t len, uint64_t now_ms,
                        void (*deliver)(void *context, const uint8_t *bytes, size_t len), void *context);

/*
 * The next segment to send now, written to `out`, and its length; 0 when
 * there is nothing. Called until it says 0, each time something arrived,
 * something was written, or the time bssh_stream_wait gave has passed.
 * `capacity` must hold `mss` and BSSH_STREAM_HEADER_MAX bytes of header.
 */
size_t bssh_stream_poll(bssh_stream_t *stream, uint64_t now_ms, uint8_t *out, size_t capacity);

/* Milliseconds until poll has something to do without anything arriving, or -1. */
int64_t bssh_stream_wait(const bssh_stream_t *stream, uint64_t now_ms);

#define BSSH_STREAM_HEADER_MAX 40u

#ifdef __cplusplus
}
#endif

#endif
