#include "session/session_client.h"

#include "session/wire.h"

#include <string.h>

void bssh_session_client_init(bssh_session_client_t *client,
                              const bssh_session_client_events_t *events) {
	memset(client, 0, sizeof(*client));
	client->events = *events;
}

static uint64_t count_lines(const uint8_t *bytes, uint32_t len) {
	uint64_t lines = 0;

	for (uint32_t i = 0; i < len; ++i) lines += bytes[i] == '\n';
	return lines;
}

static int take_output(bssh_session_client_t *client, const bssh_wire_frame_t *frame) {
	uint64_t offset = 0;
	const uint8_t *bytes = NULL;
	uint32_t len = 0;

	if (bssh_wire_read_output(frame, &offset, &bytes, &len)) return -1;

	if (offset > client->next) {
		client->events.gap(client->events.context, offset - client->next, 0);
		client->next = offset;
	} else if (offset < client->next) {
		const uint64_t seen = client->next - offset;
		if (seen >= len) return 0;
		bytes += seen;
		len -= (uint32_t)seen;
	}

	if (len) client->events.output(client->events.context, bytes, len);
	client->next += len;
	client->line += count_lines(bytes, len);
	return 0;
}

static int take_lost(bssh_session_client_t *client, const bssh_wire_frame_t *frame) {
	uint64_t offset = 0;
	uint64_t lines = 0;

	if (bssh_wire_read_lost(frame, &offset, &lines)) return -1;
	if (offset > client->next) {
		/* Behind what was already counted is a host's mistake, and not a number. */
		const uint64_t missing = lines > client->line ? lines - client->line : 0;

		client->events.gap(client->events.context, offset - client->next, missing);
		client->next = offset;
		client->line = lines;
	}
	return 0;
}

static int take_restarted(bssh_session_client_t *client, const bssh_wire_frame_t *frame) {
	uint64_t offset = 0;

	if (bssh_wire_read_offset(frame, &offset)) return -1;
	client->next = offset;
	client->line = 0;
	client->acked = offset;
	client->events.restarted(client->events.context);
	return 0;
}

static int take_exited(bssh_session_client_t *client, const bssh_wire_frame_t *frame) {
	int32_t status = 0;

	if (bssh_wire_read_exited(frame, &status)) return -1;
	client->events.exited(client->events.context, status);
	return 0;
}

/* 1 when the hello is whole and taken, 0 for more bytes, -1 for not a hello. */
static int take_hello(bssh_session_client_t *client) {
	uint8_t hello[32];
	const size_t len = bssh_wire_put_hello(hello, sizeof(hello));
	const size_t have = client->in_used < len ? client->in_used : len;

	if (memcmp(client->in, hello, have) != 0) return -1;
	if (have < len) return 0;

	memmove(client->in, client->in + len, client->in_used - len);
	client->in_used -= (uint32_t)len;
	client->said_hello = 1;
	return 1;
}

static int take_frames(bssh_session_client_t *client) {
	uint32_t at = 0;

	if (!client->said_hello) {
		const int hello = take_hello(client);
		if (hello <= 0) return hello;
	}

	for (;;) {
		bssh_wire_frame_t frame;
		uint32_t used = 0;
		const int taken = bssh_wire_take(client->in + at, client->in_used - at, &frame, &used);

		if (taken < 0) return -1;
		if (taken == 0) break;

		int status = 0;
		if (frame.kind == BSSH_WIRE_OUTPUT) status = take_output(client, &frame);
		else if (frame.kind == BSSH_WIRE_LOST) status = take_lost(client, &frame);
		else if (frame.kind == BSSH_WIRE_EXITED) status = take_exited(client, &frame);
		else if (frame.kind == BSSH_WIRE_RESTARTED) status = take_restarted(client, &frame);
		else if (frame.kind == BSSH_WIRE_CAUGHT_UP) client->events.caught_up(client->events.context);
		if (status) return -1;

		at += used;
	}

	memmove(client->in, client->in + at, client->in_used - at);
	client->in_used -= at;
	return 0;
}

int bssh_session_client_feed(bssh_session_client_t *client, const uint8_t *bytes, size_t len) {
	while (len) {
		const size_t room = sizeof(client->in) - client->in_used;
		const size_t step = len < room ? len : room;

		memcpy(client->in + client->in_used, bytes, step);
		client->in_used += (uint32_t)step;
		bytes += step;
		len -= step;

		if (take_frames(client)) return -1;

		/* Room for the largest frame and still none: it can never complete. */
		if (client->in_used == sizeof(client->in)) return -1;
	}
	return 0;
}

void bssh_session_client_reset_link(bssh_session_client_t *client) {
	client->in_used = 0;
	client->said_hello = 0;
}

size_t bssh_session_client_ack_due(bssh_session_client_t *client, uint64_t every, uint8_t *out,
                                   size_t capacity) {
	if (client->next - client->acked < every) return 0;

	const size_t len = bssh_wire_put_offset(out, capacity, BSSH_WIRE_ACK, client->next);
	if (len) client->acked = client->next;
	return len;
}
