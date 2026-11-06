#include "session/wire.h"

#include "session/generated/frame.h"

#include <string.h>

int bssh_wire_take(uint8_t *data, uint32_t have, bssh_wire_frame_t *frame, uint32_t *used) {
	/*
	 * The largest legal length is three bytes. A fourth continuation is garbage
	 * now, and would otherwise read as "not finished yet" and wait on the peer.
	 */
	if (have > 3 && (data[1] & data[2] & data[3] & 0x80)) return -1;

	uint32_t need = 0;
	const situ_err_t err = situ_bssh_frame_required(data, have, &need);

	if (err == SITU_ERR_TRUNCATED) {
		/* Waiting for a frame nobody may send would wait for ever. */
		return need > BSSH_WIRE_FRAME_MAX ? -1 : 0;
	}
	if (err != SITU_OK || need > BSSH_WIRE_FRAME_MAX) return -1;

	situ_msg_t msg;
	situ_view_t view;
	situ_msg_init(&msg, data, need);
	if (situ_bssh_frame_view(&msg, 0, need, &view) != SITU_OK) return -1;
	if (situ_bssh_frame_validate(view) != SITU_OK) return -1;

	frame->kind = situ_bssh_frame_kind_get(view);
	frame->body = situ_bssh_frame_body_ptr(view);
	frame->body_len = situ_bssh_frame_body_len(view);
	*used = need;
	return 1;
}

size_t bssh_wire_put(uint8_t *out, size_t capacity, uint8_t kind, const void *body, size_t len) {
	if (len > BSSH_WIRE_BODY_MAX || capacity < 1) return 0;

	const uint32_t room = capacity > UINT32_MAX ? UINT32_MAX : (uint32_t)capacity;
	const uint32_t length_len = situ_varint_put(out + 1, room - 1, len);
	if (!length_len || (size_t)1 + length_len + len > capacity) return 0;

	out[0] = kind;
	if (len) memcpy(out + 1 + length_len, body, len);
	return 1 + length_len + len;
}

size_t bssh_wire_put_offset(uint8_t *out, size_t capacity, uint8_t kind, uint64_t offset) {
	uint8_t body[10];
	const uint32_t len = situ_varint_put(body, sizeof(body), offset);

	return bssh_wire_put(out, capacity, kind, body, len);
}

size_t bssh_wire_put_output(uint8_t *out, size_t capacity, uint64_t offset, const uint8_t *bytes,
                            size_t len) {
	uint8_t head[10];
	const uint32_t head_len = situ_varint_put(head, sizeof(head), offset);
	const size_t body_len = head_len + len;

	if (body_len > BSSH_WIRE_BODY_MAX || capacity < 4 + body_len) return 0;

	const uint32_t length_len = situ_varint_put(out + 1, 3, body_len);
	out[0] = BSSH_WIRE_OUTPUT;
	memcpy(out + 1 + length_len, head, head_len);
	memcpy(out + 1 + length_len + head_len, bytes, len);
	return 1 + length_len + body_len;
}

static void put_u16(uint8_t *at, uint16_t value) {
	at[0] = (uint8_t)(value >> 8);
	at[1] = (uint8_t)value;
}

size_t bssh_wire_put_attach(uint8_t *out, size_t capacity, uint64_t offset, uint16_t columns,
                            uint16_t rows) {
	uint8_t body[14];
	const uint32_t len = situ_varint_put(body, 10, offset);

	put_u16(body + len, columns);
	put_u16(body + len + 2, rows);
	return bssh_wire_put(out, capacity, BSSH_WIRE_ATTACH, body, len + 4);
}

size_t bssh_wire_put_resize(uint8_t *out, size_t capacity, uint16_t columns, uint16_t rows) {
	uint8_t body[4];

	put_u16(body, columns);
	put_u16(body + 2, rows);
	return bssh_wire_put(out, capacity, BSSH_WIRE_RESIZE, body, sizeof(body));
}

size_t bssh_wire_put_exited(uint8_t *out, size_t capacity, int32_t status) {
	const uint32_t value = (uint32_t)status;
	const uint8_t body[4] = { (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8),
	                          (uint8_t)value };

	return bssh_wire_put(out, capacity, BSSH_WIRE_EXITED, body, sizeof(body));
}

size_t bssh_wire_put_hello(uint8_t *out, size_t capacity) {
	return bssh_wire_put(out, capacity, BSSH_WIRE_HELLO, BSSH_WIRE_HELLO_TEXT,
	                     sizeof(BSSH_WIRE_HELLO_TEXT) - 1);
}

/* A view over a body, which situ's accessors bound by what really arrived. */
static int body_message(const bssh_wire_frame_t *frame, situ_msg_t *msg) {
	situ_msg_init(msg, (uint8_t *)(uintptr_t)frame->body, frame->body_len);
	return frame->body_len ? 0 : -1;
}

int bssh_wire_read_offset(const bssh_wire_frame_t *frame, uint64_t *offset) {
	situ_msg_t msg;
	situ_view_t view;

	if (body_message(frame, &msg)) return -1;
	if (situ_bssh_frame_offset_view(&msg, 0, frame->body_len, &view) != SITU_OK) return -1;
	return situ_bssh_frame_offset_offset_get(view, offset) == SITU_OK ? 0 : -1;
}

int bssh_wire_read_output(const bssh_wire_frame_t *frame, uint64_t *offset, const uint8_t **bytes,
                          uint32_t *len) {
	uint64_t raw = 0;
	const uint32_t used = situ_varint_get(frame->body, frame->body_len, 10u, &raw);

	if (!used || used != situ_varint_len(raw)) return -1;
	*offset = raw;
	*bytes = frame->body + used;
	*len = frame->body_len - used;
	return 0;
}

int bssh_wire_read_attach(const bssh_wire_frame_t *frame, uint64_t *offset, uint16_t *columns,
                          uint16_t *rows) {
	situ_msg_t msg;
	situ_view_t view;

	if (body_message(frame, &msg)) return -1;
	if (situ_bssh_frame_attach_view(&msg, 0, frame->body_len, &view) != SITU_OK) return -1;
	if (situ_bssh_frame_attach_validate(view) != SITU_OK) return -1;
	if (situ_bssh_frame_attach_offset_get(view, offset) != SITU_OK) return -1;
	*columns = situ_bssh_frame_attach_columns_get(view);
	*rows = situ_bssh_frame_attach_rows_get(view);
	return 0;
}

int bssh_wire_read_resize(const bssh_wire_frame_t *frame, uint16_t *columns, uint16_t *rows) {
	situ_msg_t msg;
	situ_view_t view;

	if (body_message(frame, &msg)) return -1;
	if (situ_bssh_frame_resize_view(&msg, 0, &view) != SITU_OK) return -1;
	*columns = situ_bssh_frame_resize_columns_get(view);
	*rows = situ_bssh_frame_resize_rows_get(view);
	return 0;
}

int bssh_wire_read_exited(const bssh_wire_frame_t *frame, int32_t *status) {
	situ_msg_t msg;
	situ_view_t view;

	if (body_message(frame, &msg)) return -1;
	if (situ_bssh_frame_exited_view(&msg, 0, &view) != SITU_OK) return -1;
	*status = situ_bssh_frame_exited_status_get(view);
	return 0;
}

int bssh_wire_read_mode(const bssh_wire_frame_t *frame, uint8_t *mode) {
	situ_msg_t msg;
	situ_view_t view;

	if (body_message(frame, &msg)) return -1;
	if (situ_bssh_frame_mode_view(&msg, 0, &view) != SITU_OK) return -1;
	if (situ_bssh_frame_mode_validate(view) != SITU_OK) return -1;
	*mode = situ_bssh_frame_mode_mode_get(view);
	return 0;
}
