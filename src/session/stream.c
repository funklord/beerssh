/*
 * The reliable stream under the session where datagrams arrive once or not
 * at all (stream.h, project.md sec 5.6).
 */

#include "session/stream.h"

#include <string.h>

#include "session/generated/frame.h"

static uint64_t at_least(uint64_t value, uint64_t floor) {
	return value < floor ? floor : value;
}

static uint64_t at_most(uint64_t value, uint64_t ceiling) {
	return value > ceiling ? ceiling : value;
}

void bssh_stream_init(bssh_stream_t *stream, uint8_t *ring, size_t ring_capacity, size_t mss,
                      uint64_t window) {
	memset(stream, 0, sizeof(*stream));
	stream->mss = mss ? mss : 1;
	stream->ring = ring;
	stream->ring_capacity = ring_capacity;
	/* Unknown until the other side speaks; the congestion window bounds it meanwhile. */
	stream->peer_window = UINT64_MAX;
	stream->cwnd = 4 * (uint64_t)stream->mss;
	stream->ssthresh = UINT64_MAX;
	stream->rto_ms = BSSH_STREAM_RTO_INITIAL_MS;
	stream->window = window;
}

size_t bssh_stream_write(bssh_stream_t *stream, const uint8_t *bytes, size_t len) {
	const size_t room = stream->ring_capacity - stream->ring_len;
	if (len > room) len = room;
	for (size_t i = 0; i < len; ++i) {
		stream->ring[(stream->ring_start + stream->ring_len + i) % stream->ring_capacity] = bytes[i];
	}
	stream->ring_len += len;
	return len;
}

size_t bssh_stream_pending(const bssh_stream_t *stream) {
	return stream->ring_len;
}

/* The end of what is held to send, as a stream offset. */
static uint64_t held_end(const bssh_stream_t *stream) {
	return stream->unacked + stream->ring_len;
}

/* How much may be in flight: the congestion window, within what the other side allows. */
static uint64_t send_limit(const bssh_stream_t *stream) {
	return at_most(stream->cwnd, at_least(stream->peer_window, stream->mss));
}

static int can_send(const bssh_stream_t *stream) {
	return stream->next < held_end(stream) && stream->next - stream->unacked < send_limit(stream);
}

/* A round trip measured: srtt, rttvar and the timeout from them (RFC 6298). */
static void measured(bssh_stream_t *stream, uint64_t sample64) {
	const uint32_t sample = (uint32_t)at_most(sample64, BSSH_STREAM_RTO_MAX_MS);
	if (!stream->have_rtt) {
		stream->srtt_ms = sample;
		stream->rttvar_ms = sample / 2;
		stream->have_rtt = 1;
	} else {
		const uint32_t gap = stream->srtt_ms > sample ? stream->srtt_ms - sample : sample - stream->srtt_ms;
		stream->rttvar_ms = (3 * stream->rttvar_ms + gap) / 4;
		stream->srtt_ms = (7 * stream->srtt_ms + sample) / 8;
	}
	const uint64_t rto = (uint64_t)stream->srtt_ms + at_least(4 * (uint64_t)stream->rttvar_ms, 1);
	stream->rto_ms = (uint32_t)at_most(at_least(rto, BSSH_STREAM_RTO_MIN_MS), BSSH_STREAM_RTO_MAX_MS);
}

/* Half of what is in flight, and never under two segments: where congestion starts again. */
static uint64_t halved(const bssh_stream_t *stream) {
	return at_least((stream->sent_max - stream->unacked) / 2, 2 * (uint64_t)stream->mss);
}

/* An acknowledgement that moved: bytes freed, the window grown, the timer kept honest. */
static void acknowledged(bssh_stream_t *stream, uint64_t ack, uint64_t now_ms) {
	const uint64_t acked = ack - stream->unacked;

	stream->ring_start = (stream->ring_start + (size_t)acked) % stream->ring_capacity;
	stream->ring_len -= (size_t)acked;
	stream->unacked = ack;
	if (stream->next < ack) stream->next = ack;

	if (stream->timing && ack >= stream->timed_end) {
		measured(stream, now_ms - stream->timed_at);
		stream->timing = 0;
	}
	if (stream->backoffs) {
		stream->backoffs = 0;
		if (stream->have_rtt) {
			const uint64_t rto = (uint64_t)stream->srtt_ms + at_least(4 * (uint64_t)stream->rttvar_ms, 1);
			stream->rto_ms = (uint32_t)at_most(at_least(rto, BSSH_STREAM_RTO_MIN_MS), BSSH_STREAM_RTO_MAX_MS);
		}
	}

	if (stream->recover && ack < stream->recover) {
		/*
		 * Still recovering from a loss: the bytes after the hole were dropped
		 * and are being sent again from it, so the window holds still.
		 */
	} else if (stream->recover) {
		stream->recover = 0;
		stream->cwnd = stream->ssthresh;
	} else if (stream->cwnd < stream->ssthresh) {
		stream->cwnd += at_most(acked, stream->mss);
	} else {
		stream->cwnd += at_least((uint64_t)stream->mss * stream->mss / stream->cwnd, 1);
	}
	stream->duplicate_acks = 0;

	if (stream->unacked < stream->sent_max) {
		stream->armed = 1;
		stream->deadline = now_ms + stream->rto_ms;
	} else {
		stream->armed = 0;
	}
}

int bssh_stream_receive(bssh_stream_t *stream, const uint8_t *segment, size_t len, uint64_t now_ms,
                        void (*deliver)(void *context, const uint8_t *bytes, size_t len), void *context) {
	situ_msg_t msg;
	situ_view_t view;
	uint64_t offset, ack, window;

	if (len == 0 || len > UINT32_MAX) return -1;
	situ_msg_init(&msg, (uint8_t *)(uintptr_t)segment, (uint32_t)len);
	if (situ_bssh_segment_view(&msg, 0, (uint32_t)len, &view) != SITU_OK) return -1;
	if (situ_bssh_segment_validate(view) != SITU_OK) return -1;
	if (situ_bssh_segment_offset_get(view, &offset) != SITU_OK) return -1;
	if (situ_bssh_segment_ack_get(view, &ack) != SITU_OK) return -1;
	if (situ_bssh_segment_window_get(view, &window) != SITU_OK) return -1;
	const uint32_t head = situ_bssh_segment_window_offset(view) + situ_bssh_segment_window_len(view);
	if (head > len) return -1;
	const uint8_t *data = segment + head;
	const size_t data_len = len - head;

	/* Nobody acknowledges what was never sent, and offsets do not wrap. */
	if (ack > stream->sent_max) return -1;
	if (offset > UINT64_MAX - data_len) return -1;

	if (ack > stream->unacked) {
		acknowledged(stream, ack, now_ms);
	} else if (ack == stream->unacked && data_len == 0 && stream->unacked < stream->sent_max) {
		/*
		 * The same place again, carrying nothing: the far side heard a
		 * segment past a hole. Three, and the hole is sent at once rather
		 * than at the timeout.
		 */
		if (++stream->duplicate_acks == 3 && !stream->recover) {
			stream->ssthresh = halved(stream);
			stream->cwnd = stream->ssthresh;
			stream->recover = stream->sent_max;
			stream->next = stream->unacked;
			stream->timing = 0;
		}
	}
	stream->peer_window = window;

	if (data_len) {
		stream->ack_due = 1;
		if (offset <= stream->received && offset + data_len > stream->received) {
			const size_t skip = (size_t)(stream->received - offset);
			deliver(context, data + skip, data_len - skip);
			stream->received = offset + data_len;
		} else if (offset > stream->received) {
			/*
			 * Each answered on its own, bare: three of them is how the
			 * sender learns of the hole, and a burst that arrives at once
			 * would otherwise be answered once.
			 */
			++stream->dropped_ahead;
			if (stream->duplicates_owed < 3) ++stream->duplicates_owed;
		}
	}
	return 0;
}

static size_t put_header(const bssh_stream_t *stream, uint64_t offset, uint8_t *out, size_t capacity) {
	if (capacity < BSSH_STREAM_HEADER_MAX) return 0;
	size_t len = situ_varint_put(out, 10, offset);
	len += situ_varint_put(out + len, 10, stream->received);
	len += situ_varint_put(out + len, 10, stream->window);
	return len;
}

size_t bssh_stream_poll(bssh_stream_t *stream, uint64_t now_ms, uint8_t *out, size_t capacity) {
	if (stream->armed && now_ms >= stream->deadline) {
		/* Nothing heard for a whole timeout: back to the oldest byte, and slowly. */
		stream->ssthresh = halved(stream);
		stream->cwnd = stream->mss;
		stream->recover = stream->sent_max;
		stream->next = stream->unacked;
		stream->rto_ms = (uint32_t)at_most(2 * (uint64_t)stream->rto_ms, BSSH_STREAM_RTO_MAX_MS);
		++stream->backoffs;
		stream->timing = 0;
		stream->duplicate_acks = 0;
		stream->armed = 0;
	}

	if (stream->duplicates_owed) {
		--stream->duplicates_owed;
		if (!stream->duplicates_owed) stream->ack_due = 0;
		return put_header(stream, stream->next, out, capacity);
	}

	if (can_send(stream)) {
		const size_t head = put_header(stream, stream->next, out, capacity);
		if (!head) return 0;
		uint64_t n = at_most(held_end(stream) - stream->next, stream->mss);
		n = at_most(n, send_limit(stream) - (stream->next - stream->unacked));
		n = at_most(n, capacity - head);
		const size_t from = (size_t)(stream->next - stream->unacked);
		for (size_t i = 0; i < (size_t)n; ++i) {
			out[head + i] = stream->ring[(stream->ring_start + from + i) % stream->ring_capacity];
		}

		if (stream->next < stream->sent_max) {
			stream->retransmitted += at_most(stream->sent_max - stream->next, n);
		}
		/* Karn: a round trip is timed only on bytes sent once. */
		if (stream->next >= stream->sent_max && !stream->timing && !stream->backoffs) {
			stream->timing = 1;
			stream->timed_end = stream->next + n;
			stream->timed_at = now_ms;
		} else if (stream->timing && stream->next < stream->timed_end) {
			stream->timing = 0;
		}

		stream->next += n;
		if (stream->next > stream->sent_max) stream->sent_max = stream->next;
		if (!stream->armed) {
			stream->armed = 1;
			stream->deadline = now_ms + stream->rto_ms;
		}
		stream->ack_due = 0;
		return head + (size_t)n;
	}

	if (stream->ack_due) {
		stream->ack_due = 0;
		return put_header(stream, stream->next, out, capacity);
	}
	return 0;
}

int64_t bssh_stream_wait(const bssh_stream_t *stream, uint64_t now_ms) {
	if (stream->ack_due || stream->duplicates_owed || can_send(stream)) return 0;
	if (!stream->armed) return -1;
	return stream->deadline > now_ms ? (int64_t)(stream->deadline - now_ms) : 0;
}
