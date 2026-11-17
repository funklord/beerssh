#ifndef BSSH_WIRE_H
#define BSSH_WIRE_H

/*
 * Frames of the session protocol (project.md sec 5.6), whose bytes are
 * defined by frame.situ. Reading goes through situ's generated code, so its
 * bounds checks are not written by hand here. situ generates no writer for a
 * varint, so the writers are these few lines over its situ_varint_put.
 */

#include "session/screen_model.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Numbered as in frame.situ. */
enum {
	BSSH_WIRE_ATTACH = 1,
	BSSH_WIRE_OUTPUT = 2,
	BSSH_WIRE_INPUT = 3,
	BSSH_WIRE_ACK = 4,
	BSSH_WIRE_RESIZE = 5,
	BSSH_WIRE_EXITED = 6,
	BSSH_WIRE_LOST = 7,
	BSSH_WIRE_MODE = 8,
	BSSH_WIRE_HELLO = 9,
	BSSH_WIRE_RESTARTED = 10,
	BSSH_WIRE_HANGUP = 11,
	BSSH_WIRE_CAUGHT_UP = 12,
	BSSH_WIRE_KEYFRAMES = 13,
	BSSH_WIRE_KEYFRAME = 14,
	BSSH_WIRE_HISTORY = 15,
	BSSH_WIRE_REPAINT = 16,
};

/*
 * The host's first bytes on every link. A length alone cannot tell this
 * protocol from text: a banner from a chatty .bashrc parses as a frame of an
 * unknown kind with a long length, and a reader would wait for the rest of it.
 * So a fixed hello comes first, and anything else means something other than
 * this protocol is on the far end, at the first byte that differs.
 */
#define BSSH_WIRE_HELLO_TEXT "beerssh-session 1"

enum {
	BSSH_WIRE_MODE_LIVE = 0,
	BSSH_WIRE_MODE_EVENTUAL = 1,
	BSSH_WIRE_MODE_SERIAL = 2,
};

#define BSSH_WIRE_BODY_MAX 65536u
/* A kind, a length of at most three bytes, and the largest body. */
#define BSSH_WIRE_FRAME_MAX (BSSH_WIRE_BODY_MAX + 4u)

typedef struct bssh_wire_frame {
	uint8_t kind;
	const uint8_t *body;
	uint32_t body_len;
} bssh_wire_frame_t;

/*
 * The first frame in `data`. Returns 1 with *frame and *used set, 0 when more
 * bytes are needed, -1 when no amount of bytes would make this a frame. The
 * body points into `data`. A kind nobody knows is a frame like any other, for
 * the caller to skip.
 */
int bssh_wire_take(uint8_t *data, uint32_t have, bssh_wire_frame_t *frame, uint32_t *used);

/* Each returns the bytes written, or 0 if `capacity` is too small. */
size_t bssh_wire_put(uint8_t *out, size_t capacity, uint8_t kind, const void *body, size_t len);
size_t bssh_wire_put_offset(uint8_t *out, size_t capacity, uint8_t kind, uint64_t offset);
size_t bssh_wire_put_lost(uint8_t *out, size_t capacity, uint64_t offset, uint64_t lines);
size_t bssh_wire_put_output(uint8_t *out, size_t capacity, uint64_t offset, const uint8_t *bytes,
                            size_t len);
size_t bssh_wire_put_attach(uint8_t *out, size_t capacity, uint64_t offset, uint16_t columns,
                            uint16_t rows);
size_t bssh_wire_put_resize(uint8_t *out, size_t capacity, uint16_t columns, uint16_t rows);
size_t bssh_wire_put_exited(uint8_t *out, size_t capacity, int32_t status);
size_t bssh_wire_put_hello(uint8_t *out, size_t capacity);
size_t bssh_wire_put_keyframe(uint8_t *out, size_t capacity, uint64_t from, int lost);
/* As many of the cells as fit; the far half of a wide character is skipped. */
size_t bssh_wire_put_history(uint8_t *out, size_t capacity, uint64_t number, int continuation,
                             const bssh_screen_cell_t *cells, int count);
/* At most what fits of `len`; *taken says how much. */
size_t bssh_wire_put_repaint(uint8_t *out, size_t capacity, uint64_t offset, uint64_t lines, int last,
                             const uint8_t *bytes, size_t len, size_t *taken);

/* Reading a body. Each returns 0, or -1 when the body is not that message. */
int bssh_wire_read_offset(const bssh_wire_frame_t *frame, uint64_t *offset);
int bssh_wire_read_lost(const bssh_wire_frame_t *frame, uint64_t *offset, uint64_t *lines);
int bssh_wire_read_output(const bssh_wire_frame_t *frame, uint64_t *offset, const uint8_t **bytes,
                          uint32_t *len);
int bssh_wire_read_attach(const bssh_wire_frame_t *frame, uint64_t *offset, uint16_t *columns,
                          uint16_t *rows);
int bssh_wire_read_resize(const bssh_wire_frame_t *frame, uint16_t *columns, uint16_t *rows);
int bssh_wire_read_exited(const bssh_wire_frame_t *frame, int32_t *status);
int bssh_wire_read_mode(const bssh_wire_frame_t *frame, uint8_t *mode);
int bssh_wire_read_keyframe(const bssh_wire_frame_t *frame, uint64_t *from, int *lost);
int bssh_wire_read_repaint(const bssh_wire_frame_t *frame, uint64_t *offset, uint64_t *lines, int *last,
                           const uint8_t **bytes, uint32_t *len);

/* A history line's cells, read one at a time; the bytes stay the frame's. */
typedef struct bssh_wire_cells {
	const uint8_t *at;
	const uint8_t *end;
	bssh_screen_cell_t style;
	uint64_t left;   /* cells still to come in this run */
} bssh_wire_cells_t;

int bssh_wire_read_history(const bssh_wire_frame_t *frame, uint64_t *number, int *continuation,
                           bssh_wire_cells_t *cells);
/* 1 with the next cell, 0 at the end of the line, -1 for a malformed one. */
int bssh_wire_next_cell(bssh_wire_cells_t *cells, bssh_screen_cell_t *cell);

#ifdef __cplusplus
}
#endif

#endif
