#ifndef BSSH_SCREEN_MODEL_H
#define BSSH_SCREEN_MODEL_H

/*
 * The server's copy of the screen (project.md sec 5.6, the live screen).
 *
 * The session host feeds it every byte the pty writes, attached or not, so
 * that a client coming back over a bad link can be sent the screen as it is
 * NOW -- a keyframe -- before the bytes that led to it. It is the same patched
 * libvterm the client runs, set up the same way (UTF-8, reflow, the alternate
 * screen), so the two ends cannot disagree about what a byte meant.
 *
 * A cell carries what libvterm's does and nothing the client adds later: the
 * characters, the width, the attributes the renderer draws, and the two
 * colours with "the theme's default" kept apart from an explicit palette
 * entry (bssh_color_t says why). The client converts one with the rule it
 * already uses for its own cells.
 *
 * Not modelled yet: history. Lines scrolled off the top are dropped here, so
 * a resize that would pull lines back down from the scrollback reflows
 * differently from the client's screen. The history stream is the next part.
 *
 * Plain C with no Qt, like the rest of src/session.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* libvterm's VTERM_MAX_CHARS_PER_CELL. */
#define BSSH_SCREEN_CELL_CHARS 6

enum {
	BSSH_SCREEN_COLOR_DEFAULT = 0,
	BSSH_SCREEN_COLOR_INDEXED = 1,
	BSSH_SCREEN_COLOR_RGB = 2
};

typedef struct bssh_screen_color {
	uint8_t kind;
	uint8_t index;
	uint8_t red, green, blue;
} bssh_screen_color_t;

enum {
	BSSH_SCREEN_BOLD = 1 << 0,
	BSSH_SCREEN_ITALIC = 1 << 1,
	BSSH_SCREEN_UNDERLINE = 1 << 2,
	BSSH_SCREEN_REVERSE = 1 << 3,
	BSSH_SCREEN_BLINK = 1 << 4,
	BSSH_SCREEN_STRIKE = 1 << 5,
	BSSH_SCREEN_DIM = 1 << 6
};

typedef struct bssh_screen_cell {
	/* Zero-terminated when shorter; chars[0] == 0 is a cell never written,
	 * and 0xFFFFFFFF the far half of a double-width glyph. */
	uint32_t chars[BSSH_SCREEN_CELL_CHARS];
	uint8_t width;
	uint8_t attrs;
	bssh_screen_color_t fg;
	bssh_screen_color_t bg;
} bssh_screen_cell_t;

typedef struct bssh_screen_model bssh_screen_model_t;

/* NULL when out of memory. */
bssh_screen_model_t *bssh_screen_model_new(int rows, int columns);
void bssh_screen_model_free(bssh_screen_model_t *model);

void bssh_screen_model_feed(bssh_screen_model_t *model, const uint8_t *bytes, size_t length);
void bssh_screen_model_resize(bssh_screen_model_t *model, int rows, int columns);

void bssh_screen_model_size(const bssh_screen_model_t *model, int *rows, int *columns);
/* 0 when the position is off the screen, and the cell is left blank. */
int bssh_screen_model_cell(const bssh_screen_model_t *model, int row, int column, bssh_screen_cell_t *out);
void bssh_screen_model_cursor(const bssh_screen_model_t *model, int *row, int *column, int *visible);
int bssh_screen_model_alternate(const bssh_screen_model_t *model);

#ifdef __cplusplus
}
#endif

#endif
