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
 * History is kept too: the lines scrolled off the top, numbered from the
 * start of the session and each marked if it continues the one above, up to
 * a limit -- what a client that missed them is to be sent, and what a resize
 * that grows the screen pulls back down, as the client's does.
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
/* Whether a row is the tail of a line that ran past the right edge of the row above. */
int bssh_screen_model_row_continues(const bssh_screen_model_t *model, int row);

/*
 * The screen as bytes a terminal of the same size draws it from: the
 * keyframe, written in the terminal's own language rather than a format of
 * ours, so a client applies it by feeding it to the emulator it already
 * has. Every cell with its attributes and colours, the cursor where it is
 * and whether it shows, and the alternate screen if that is up.
 *
 * Writes at most `capacity` bytes to `out` and returns the length of the
 * whole repaint, as snprintf does, so a caller can size a buffer and ask
 * again.
 *
 * Which rows continue the row above is carried by drawing them through the
 * wrap, and the modes a key, a paste, a click or later output depend on are
 * said both ways. Not carried yet: the primary screen underneath an
 * alternate one.
 */
size_t bssh_screen_model_repaint(const bssh_screen_model_t *model, uint8_t *out, size_t capacity);

/*
 * The history: lines `first` up to `first + count`, numbered from the start of
 * the session, so a line keeps its number however many have gone since. The
 * oldest go first past the limit (default BSSH_SCREEN_HISTORY_LINES); 0 keeps
 * none, though the numbering still counts every line that went past.
 */
#define BSSH_SCREEN_HISTORY_LINES 2000
void bssh_screen_model_set_history_limit(bssh_screen_model_t *model, size_t lines);
void bssh_screen_model_history(const bssh_screen_model_t *model, uint64_t *first, uint64_t *count);
/*
 * Line `number`'s cells into `out`, at most `capacity` of them, and whether it
 * continues the line above. Returns how many cells it has -- trailing blanks
 * left off once the next line shows it ended, as the client keeps them -- or
 * -1 when the line is not held.
 */
int bssh_screen_model_history_line(const bssh_screen_model_t *model, uint64_t number,
                                   bssh_screen_cell_t *out, int capacity, int *continuation);

/*
 * How many bytes of the program's output the screen shows: every byte fed but
 * the few held back for the next read (an unfinished UTF-8 sequence). Bytes
 * from here on are what a client given this screen still needs.
 */
uint64_t bssh_screen_model_offset(const bssh_screen_model_t *model);

/*
 * The first line a client that has seen output up to `offset` lacks: the first
 * pushed after it, or the end of the history when it lacks none. Each line is
 * marked when it goes, at the end of the line of output being written, so a
 * line wrapped off part-way may be counted as lacked when it was not -- sent
 * twice rather than missed. Returns 0 when lines it lacked may have gone past
 * the limit before `*from` as well, how many being unknown.
 */
int bssh_screen_model_history_after(const bssh_screen_model_t *model, uint64_t offset, uint64_t *from);

#ifdef __cplusplus
}
#endif

#endif
