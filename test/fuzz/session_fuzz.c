/*
 * What the network can say to a session (project.md sec 5.6). Run with `make
 * fuzz-session`: libFuzzer under ASan and UBSan, bounded by FUZZ_SECONDS.
 *
 * Two doors. Every reader over whatever frame the bytes make, and the client
 * fed them in pieces after a real hello, which is what a hostile host or a
 * corrupted link gets to do. The first byte chooses the size of the pieces, so
 * a frame split across reads is reached as often as a whole one.
 */

#include "session/session_client.h"
#include "session/wire.h"

#include <assert.h>
#include <stdlib.h>

static uint64_t delivered;

static void on_output(void *context, const uint8_t *bytes, size_t len) {
	(void)context;
	assert(len > 0);

	/* Read every byte, so a span past the buffer is caught by ASan. */
	for (size_t i = 0; i < len; ++i) delivered += bytes[i];
}

static void on_gap(void *context, uint64_t count, uint64_t lines) {
	(void)context;
	(void)lines;
	assert(count > 0);
}

static void on_exited(void *context, int32_t status) {
	(void)context;
	(void)status;
}

static void on_restarted(void *context) {
	(void)context;
}

static void read_every_way(const bssh_wire_frame_t *frame) {
	uint64_t offset = 0;
	const uint8_t *bytes = NULL;
	uint32_t len = 0;
	uint16_t columns = 0;
	uint16_t rows = 0;
	int32_t status = 0;
	uint8_t mode = 0;

	(void)bssh_wire_read_offset(frame, &offset);
	(void)bssh_wire_read_attach(frame, &offset, &columns, &rows);
	(void)bssh_wire_read_resize(frame, &columns, &rows);
	(void)bssh_wire_read_exited(frame, &status);
	if (bssh_wire_read_mode(frame, &mode) == 0) assert(mode <= BSSH_WIRE_MODE_SERIAL);

	if (bssh_wire_read_output(frame, &offset, &bytes, &len) == 0) {
		assert(len <= frame->body_len);
		for (uint32_t i = 0; i < len; ++i) delivered += bytes[i];
	}
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
	if (!size) return 0;

	const size_t piece = (size_t)data[0] + 1;
	++data;
	--size;

	/* A copy of exactly this size: a read past it is ASan's to report. */
	uint8_t *bytes = malloc(size ? size : 1);
	if (!bytes) return 0;
	for (size_t i = 0; i < size; ++i) bytes[i] = data[i];

	uint32_t at = 0;
	while (at < size) {
		bssh_wire_frame_t frame;
		uint32_t used = 0;
		const int taken = bssh_wire_take(bytes + at, (uint32_t)size - at, &frame, &used);

		if (taken <= 0) break;
		assert(used > 0 && used <= (uint32_t)size - at);
		assert(frame.body_len <= BSSH_WIRE_BODY_MAX);
		read_every_way(&frame);
		at += used;
	}

	static bssh_session_client_t client;
	const bssh_session_client_events_t events = { NULL,         on_output,    on_gap,
	                                              on_exited,    on_restarted, on_restarted };
	bssh_session_client_init(&client, &events);

	uint8_t hello[32];
	const size_t hello_len = bssh_wire_put_hello(hello, sizeof(hello));
	assert(bssh_session_client_feed(&client, hello, hello_len) == 0);

	for (size_t from = 0; from < size; from += piece) {
		const size_t len = size - from < piece ? size - from : piece;

		if (bssh_session_client_feed(&client, bytes + from, len) != 0) break;
		assert(client.in_used < sizeof(client.in));

		uint8_t ack[16];
		(void)bssh_session_client_ack_due(&client, 1, ack, sizeof(ack));
	}

	free(bytes);
	return 0;
}
