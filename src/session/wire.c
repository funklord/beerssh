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

size_t bssh_wire_put_lost(uint8_t *out, size_t capacity, uint64_t offset, uint64_t lines) {
	uint8_t body[20];
	const uint32_t first = situ_varint_put(body, 10, offset);
	const uint32_t second = situ_varint_put(body + first, 10, lines);

	return bssh_wire_put(out, capacity, BSSH_WIRE_LOST, body, first + second);
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

int bssh_wire_read_lost(const bssh_wire_frame_t *frame, uint64_t *offset, uint64_t *lines) {
	situ_msg_t msg;
	situ_view_t view;

	if (body_message(frame, &msg)) return -1;
	if (situ_bssh_frame_lost_view(&msg, 0, frame->body_len, &view) != SITU_OK) return -1;
	if (situ_bssh_frame_lost_offset_get(view, offset) != SITU_OK) return -1;
	return situ_bssh_frame_lost_lines_get(view, lines) == SITU_OK ? 0 : -1;
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

size_t bssh_wire_put_keyframe(uint8_t *out, size_t capacity, uint64_t from, int lost) {
	uint8_t body[11];
	const uint32_t len = situ_varint_put(body, 10, from);

	body[len] = lost ? 1 : 0;
	return bssh_wire_put(out, capacity, BSSH_WIRE_KEYFRAME, body, len + 1);
}

/* A body being written: 0 once anything has not fitted. */
typedef struct writer {
	uint8_t *at;
	size_t left;
	int full;
} writer_t;

static void write_bytes(writer_t *writer, const void *bytes, size_t len) {
	if (writer->full || len > writer->left) {
		writer->full = 1;
		return;
	}
	memcpy(writer->at, bytes, len);
	writer->at += len;
	writer->left -= len;
}

static void write_varint(writer_t *writer, uint64_t value) {
	uint8_t bytes[10];
	write_bytes(writer, bytes, situ_varint_put(bytes, sizeof(bytes), value));
}

static void write_color(writer_t *writer, const bssh_screen_color_t *color) {
	uint8_t bytes[4] = { color->kind, 0, 0, 0 };
	size_t len = 1;

	if (color->kind == BSSH_SCREEN_COLOR_INDEXED) {
		bytes[1] = color->index;
		len = 2;
	} else if (color->kind == BSSH_SCREEN_COLOR_RGB) {
		bytes[1] = color->red;
		bytes[2] = color->green;
		bytes[3] = color->blue;
		len = 4;
	} else {
		bytes[0] = BSSH_SCREEN_COLOR_DEFAULT;
	}
	write_bytes(writer, bytes, len);
}

static int same_style(const bssh_screen_cell_t *a, const bssh_screen_cell_t *b) {
	return a->attrs == b->attrs && !memcmp(&a->fg, &b->fg, sizeof(a->fg))
	       && !memcmp(&a->bg, &b->bg, sizeof(a->bg));
}

static int far_half(const bssh_screen_cell_t *cell) {
	return cell->chars[0] == 0xFFFFFFFFu;
}

static uint64_t cell_code(const bssh_screen_cell_t *cell) {
	const uint32_t ch = cell->chars[0] > 0x10FFFF ? 0xFFFD : cell->chars[0];
	return ((uint64_t)ch << 1) | (cell->width == 2);
}

size_t bssh_wire_put_history(uint8_t *out, size_t capacity, uint64_t number, int continuation,
                             const bssh_screen_cell_t *cells, int count) {
	/* Room for a kind and the longest length, and the body is built in place. */
	if (capacity < 4) return 0;
	uint8_t *body = out + 4;
	const size_t room = capacity - 4 < BSSH_WIRE_BODY_MAX ? capacity - 4 : BSSH_WIRE_BODY_MAX;
	writer_t writer = { body, room, 0 };

	write_varint(&writer, number);
	const uint8_t flag = continuation ? 1 : 0;
	write_bytes(&writer, &flag, 1);
	if (writer.full) return 0;

	for (int i = 0; i < count;) {
		if (far_half(&cells[i])) {
			++i;
			continue;
		}
		/* A run: every cell in this style, the far halves between them skipped. */
		uint64_t run = 0;
		int end = i;
		while (end < count && (far_half(&cells[end]) || same_style(&cells[end], &cells[i]))) {
			run += !far_half(&cells[end]);
			++end;
		}

		writer_t trial = writer;
		const uint8_t attrs = cells[i].attrs & 0x7F;
		write_bytes(&trial, &attrs, 1);
		write_color(&trial, &cells[i].fg);
		write_color(&trial, &cells[i].bg);
		write_varint(&trial, run);
		for (int j = i; j < end; ++j)
			if (!far_half(&cells[j])) write_varint(&trial, cell_code(&cells[j]));
		/* What does not fit is left off whole, so a line never ends mid-run. */
		if (trial.full) break;
		writer = trial;
		i = end;
	}

	const size_t body_len = (size_t)(writer.at - body);
	uint8_t length[4];
	const uint32_t length_len = situ_varint_put(length, sizeof(length), body_len);
	out[0] = BSSH_WIRE_HISTORY;
	memcpy(out + 1, length, length_len);
	memmove(out + 1 + length_len, body, body_len);
	return 1 + length_len + body_len;
}

size_t bssh_wire_put_repaint(uint8_t *out, size_t capacity, uint64_t offset, uint64_t lines, int last,
                             const uint8_t *bytes, size_t len, size_t *taken) {
	uint8_t head[21];
	uint32_t head_len = situ_varint_put(head, 10, offset);
	head_len += situ_varint_put(head + head_len, 10, lines);

	*taken = 0;
	if (capacity < 4 + head_len + 1) return 0;
	size_t room = capacity - 4 - head_len - 1;
	if (room > BSSH_WIRE_BODY_MAX - head_len - 1) room = BSSH_WIRE_BODY_MAX - head_len - 1;
	const size_t piece = len < room ? len : room;

	/* The last part only if it is all of what is left. */
	head[head_len] = last && piece == len ? 1 : 0;
	const size_t body_len = head_len + 1 + piece;
	const uint32_t length_len = situ_varint_put(out + 1, 3, body_len);
	out[0] = BSSH_WIRE_REPAINT;
	memcpy(out + 1 + length_len, head, head_len + 1);
	if (piece) memcpy(out + 1 + length_len + head_len + 1, bytes, piece);
	*taken = piece;
	return 1 + length_len + body_len;
}

int bssh_wire_read_keyframe(const bssh_wire_frame_t *frame, uint64_t *from, int *lost) {
	situ_msg_t msg;
	situ_view_t view;

	if (body_message(frame, &msg)) return -1;
	if (situ_bssh_frame_keyframe_view(&msg, 0, frame->body_len, &view) != SITU_OK) return -1;
	if (situ_bssh_frame_keyframe_validate(view) != SITU_OK) return -1;
	if (situ_bssh_frame_keyframe_from_get(view, from) != SITU_OK) return -1;
	*lost = situ_bssh_frame_keyframe_lost_get(view);
	return 0;
}

int bssh_wire_read_repaint(const bssh_wire_frame_t *frame, uint64_t *offset, uint64_t *lines, int *last,
                           const uint8_t **bytes, uint32_t *len) {
	situ_msg_t msg;
	situ_view_t view;

	if (body_message(frame, &msg)) return -1;
	if (situ_bssh_frame_repaint_view(&msg, 0, frame->body_len, &view) != SITU_OK) return -1;
	if (situ_bssh_frame_repaint_validate(view) != SITU_OK) return -1;
	if (situ_bssh_frame_repaint_offset_get(view, offset) != SITU_OK) return -1;
	if (situ_bssh_frame_repaint_lines_get(view, lines) != SITU_OK) return -1;
	*last = situ_bssh_frame_repaint_last_get(view);

	const uint32_t head = situ_bssh_frame_repaint_last_offset(view) + 1;
	if (head > frame->body_len) return -1;
	*bytes = frame->body + head;
	*len = frame->body_len - head;
	return 0;
}

int bssh_wire_read_history(const bssh_wire_frame_t *frame, uint64_t *number, int *continuation,
                           bssh_wire_cells_t *cells) {
	situ_msg_t msg;
	situ_view_t view;

	if (body_message(frame, &msg)) return -1;
	if (situ_bssh_frame_history_view(&msg, 0, frame->body_len, &view) != SITU_OK) return -1;
	if (situ_bssh_frame_history_validate(view) != SITU_OK) return -1;
	if (situ_bssh_frame_history_number_get(view, number) != SITU_OK) return -1;
	*continuation = situ_bssh_frame_history_continuation_get(view);

	const uint32_t head = situ_bssh_frame_history_continuation_offset(view) + 1;
	if (head > frame->body_len) return -1;
	memset(cells, 0, sizeof(*cells));
	cells->at = frame->body + head;
	cells->end = frame->body + frame->body_len;
	return 0;
}

static int read_varint(bssh_wire_cells_t *cells, uint64_t *value) {
	const uint32_t used = situ_varint_get(cells->at, (uint32_t)(cells->end - cells->at), 10u, value);

	if (!used || used != situ_varint_len(*value)) return -1;
	cells->at += used;
	return 0;
}

static int read_color(bssh_wire_cells_t *cells, bssh_screen_color_t *color) {
	memset(color, 0, sizeof(*color));
	if (cells->at == cells->end) return -1;
	color->kind = *cells->at++;
	if (color->kind == BSSH_SCREEN_COLOR_DEFAULT) return 0;
	if (color->kind == BSSH_SCREEN_COLOR_INDEXED) {
		if (cells->end - cells->at < 1) return -1;
		color->index = *cells->at++;
		return 0;
	}
	if (color->kind != BSSH_SCREEN_COLOR_RGB || cells->end - cells->at < 3) return -1;
	color->red = cells->at[0];
	color->green = cells->at[1];
	color->blue = cells->at[2];
	cells->at += 3;
	return 0;
}

int bssh_wire_next_cell(bssh_wire_cells_t *cells, bssh_screen_cell_t *cell) {
	if (!cells->left) {
		if (cells->at == cells->end) return 0;

		bssh_screen_cell_t style;
		memset(&style, 0, sizeof(style));
		style.attrs = *cells->at++;
		if (style.attrs & 0x80) return -1;
		if (read_color(cells, &style.fg) || read_color(cells, &style.bg)) return -1;
		if (read_varint(cells, &cells->left) || !cells->left) return -1;
		cells->style = style;
	}

	uint64_t code = 0;
	if (read_varint(cells, &code)) return -1;
	if ((code >> 1) > 0x10FFFF) return -1;

	*cell = cells->style;
	cell->chars[0] = (uint32_t)(code >> 1);
	cell->width = (code & 1) ? 2 : 1;
	--cells->left;
	return 1;
}
