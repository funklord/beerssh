#include "session/screen_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vterm.h>

struct bssh_screen_model {
	VTerm *vt;
	VTermScreen *screen;
	int cursor_visible;
	int alternate;
	/* The end of the last read, held until the next completes it. */
	uint8_t held[4];
	size_t held_length;
};

static int utf8_sequence_length(uint8_t byte) {
	if ((byte & 0xE0) == 0xC0) return 2;
	if ((byte & 0xF0) == 0xE0) return 3;
	if ((byte & 0xF8) == 0xF0) return 4;
	return 0;
}

/*
 * How much of the end of `bytes` to hold back for the next read: a UTF-8
 * sequence it does not complete, or a lone ESC that may be the first half of
 * an ST. The client's rule, in its feed (src/term/emulator_vterm.cpp),
 * because libvterm decodes a sequence split across two writes as U+FFFD --
 * and a pty read splits wherever it likes. Kept the same at both ends by
 * emulator_test::the_servers_screen_agrees_with_the_clients, which found
 * the server writing U+FFFD for a CJK character the client drew.
 */
static size_t tail_to_hold(const uint8_t *bytes, size_t length) {
	const size_t limit = length < 4 ? length : 4;
	for (size_t back = 1; back <= limit; ++back) {
		const uint8_t byte = bytes[length - back];
		const int needed = utf8_sequence_length(byte);
		if (needed > 0) return (size_t)needed > back ? back : 0;
		/* A continuation byte: keep walking back towards its lead. */
		if ((byte & 0xC0) != 0x80) break;
	}
	return length > 0 && bytes[length - 1] == 0x1b ? 1 : 0;
}

static int on_settermprop(VTermProp prop, VTermValue *value, void *user) {
	bssh_screen_model_t *model = user;
	if (prop == VTERM_PROP_CURSORVISIBLE) model->cursor_visible = value->boolean;
	else if (prop == VTERM_PROP_ALTSCREEN) model->alternate = value->boolean;
	return 1;
}

static const VTermScreenCallbacks callbacks = {
	.settermprop = on_settermprop,
};

/*
 * What the terminal says back -- the answer to a cursor-position query, say.
 * Dropped for now: while a client is attached its own emulator answers, as
 * it does over plain SSH. Answering from here, during a replay, is sec 5.6's
 * and comes with the modes that need it. Taken rather than left unset, since
 * libvterm otherwise keeps it in a buffer of its own and complains on stderr
 * when that fills.
 */
static void on_output(const char *bytes, size_t length, void *user) {
	(void)bytes;
	(void)length;
	(void)user;
}

bssh_screen_model_t *bssh_screen_model_new(int rows, int columns) {
	bssh_screen_model_t *model = calloc(1, sizeof *model);
	if (!model) return NULL;
	model->vt = vterm_new(rows > 0 ? rows : 1, columns > 0 ? columns : 1);
	if (!model->vt) {
		free(model);
		return NULL;
	}
	model->cursor_visible = 1;

	/* As the client's emulator sets it up (src/term/emulator_vterm.cpp),
	 * so a byte means the same at both ends. */
	vterm_set_utf8(model->vt, 1);
	model->screen = vterm_obtain_screen(model->vt);
	vterm_screen_set_callbacks(model->screen, &callbacks, model);
	vterm_screen_enable_reflow(model->screen, true);
	vterm_screen_set_damage_merge(model->screen, VTERM_DAMAGE_ROW);
	vterm_screen_enable_altscreen(model->screen, 1);
	vterm_screen_reset(model->screen, 1);
	vterm_output_set_callback(model->vt, on_output, model);
	return model;
}

void bssh_screen_model_free(bssh_screen_model_t *model) {
	if (!model) return;
	vterm_free(model->vt);
	free(model);
}

void bssh_screen_model_feed(bssh_screen_model_t *model, const uint8_t *bytes, size_t length) {
	if (!model || !bytes || length == 0) return;

	/* What was held, completed from the front of this read. */
	while (model->held_length > 0 && length > 0) {
		model->held[model->held_length++] = *bytes++;
		--length;
		const size_t hold = tail_to_hold(model->held, model->held_length);
		if (hold == 0 || model->held_length == sizeof model->held) {
			vterm_input_write(model->vt, (const char *)model->held, model->held_length);
			model->held_length = 0;
		}
	}
	if (length == 0) return;

	const size_t hold = tail_to_hold(bytes, length);
	if (length > hold) vterm_input_write(model->vt, (const char *)bytes, length - hold);
	memcpy(model->held, bytes + length - hold, hold);
	model->held_length = hold;
}

void bssh_screen_model_resize(bssh_screen_model_t *model, int rows, int columns) {
	if (!model || rows < 1 || columns < 1) return;
	vterm_set_size(model->vt, rows, columns);
}

void bssh_screen_model_size(const bssh_screen_model_t *model, int *rows, int *columns) {
	int r = 0, c = 0;
	if (model) vterm_get_size(model->vt, &r, &c);
	if (rows) *rows = r;
	if (columns) *columns = c;
}

static bssh_screen_color_t color_of(const VTermColor *color) {
	bssh_screen_color_t out;
	memset(&out, 0, sizeof out);
	/* The theme's default stays the theme's: not an index, not a value. */
	if (VTERM_COLOR_IS_DEFAULT_FG(color) || VTERM_COLOR_IS_DEFAULT_BG(color)) return out;
	if (VTERM_COLOR_IS_INDEXED(color)) {
		out.kind = BSSH_SCREEN_COLOR_INDEXED;
		out.index = color->indexed.idx;
	} else if (VTERM_COLOR_IS_RGB(color)) {
		out.kind = BSSH_SCREEN_COLOR_RGB;
		out.red = color->rgb.red;
		out.green = color->rgb.green;
		out.blue = color->rgb.blue;
	}
	return out;
}

int bssh_screen_model_cell(const bssh_screen_model_t *model, int row, int column, bssh_screen_cell_t *out) {
	if (!out) return 0;
	memset(out, 0, sizeof *out);
	out->width = 1;
	if (!model) return 0;
	int rows = 0, columns = 0;
	vterm_get_size(model->vt, &rows, &columns);
	if (row < 0 || row >= rows || column < 0 || column >= columns) return 0;

	VTermPos pos = { .row = row, .col = column };
	VTermScreenCell cell;
	if (!vterm_screen_get_cell(model->screen, pos, &cell)) return 0;

	for (int i = 0; i < BSSH_SCREEN_CELL_CHARS && i < VTERM_MAX_CHARS_PER_CELL; ++i) {
		out->chars[i] = cell.chars[i];
		if (!cell.chars[i]) break;
	}
	out->width = (uint8_t)cell.width;
	uint8_t attrs = 0;
	if (cell.attrs.bold) attrs |= BSSH_SCREEN_BOLD;
	if (cell.attrs.italic) attrs |= BSSH_SCREEN_ITALIC;
	if (cell.attrs.underline) attrs |= BSSH_SCREEN_UNDERLINE;
	if (cell.attrs.reverse) attrs |= BSSH_SCREEN_REVERSE;
	if (cell.attrs.blink) attrs |= BSSH_SCREEN_BLINK;
	if (cell.attrs.strike) attrs |= BSSH_SCREEN_STRIKE;
	if (cell.attrs.dim) attrs |= BSSH_SCREEN_DIM;
	out->attrs = attrs;
	out->fg = color_of(&cell.fg);
	out->bg = color_of(&cell.bg);
	return 1;
}

void bssh_screen_model_cursor(const bssh_screen_model_t *model, int *row, int *column, int *visible) {
	VTermPos pos = { 0, 0 };
	if (model) vterm_state_get_cursorpos(vterm_obtain_state(model->vt), &pos);
	if (row) *row = pos.row;
	if (column) *column = pos.col;
	if (visible) *visible = model ? model->cursor_visible : 0;
}

int bssh_screen_model_alternate(const bssh_screen_model_t *model) {
	return model ? model->alternate : 0;
}

/* A repaint being written: past `capacity` it only counts. */
typedef struct sink {
	uint8_t *out;
	size_t capacity;
	size_t length;
} sink_t;

static void put(sink_t *sink, const char *bytes, size_t length) {
	for (size_t i = 0; i < length; ++i) {
		if (sink->length < sink->capacity) sink->out[sink->length] = (uint8_t)bytes[i];
		++sink->length;
	}
}

static void put_text(sink_t *sink, const char *text) {
	put(sink, text, strlen(text));
}

static void put_utf8(sink_t *sink, uint32_t code) {
	char bytes[4];
	size_t length;
	if (code < 0x80) {
		bytes[0] = (char)code;
		length = 1;
	} else if (code < 0x800) {
		bytes[0] = (char)(0xC0 | (code >> 6));
		bytes[1] = (char)(0x80 | (code & 0x3F));
		length = 2;
	} else if (code < 0x10000) {
		bytes[0] = (char)(0xE0 | (code >> 12));
		bytes[1] = (char)(0x80 | ((code >> 6) & 0x3F));
		bytes[2] = (char)(0x80 | (code & 0x3F));
		length = 3;
	} else {
		bytes[0] = (char)(0xF0 | (code >> 18));
		bytes[1] = (char)(0x80 | ((code >> 12) & 0x3F));
		bytes[2] = (char)(0x80 | ((code >> 6) & 0x3F));
		bytes[3] = (char)(0x80 | (code & 0x3F));
		length = 4;
	}
	put(sink, bytes, length);
}

static void put_color(sink_t *sink, const bssh_screen_color_t *color, int foreground) {
	char text[32];
	if (color->kind == BSSH_SCREEN_COLOR_INDEXED)
		snprintf(text, sizeof text, ";%d;5;%u", foreground ? 38 : 48, color->index);
	else if (color->kind == BSSH_SCREEN_COLOR_RGB)
		snprintf(text, sizeof text, ";%d;2;%u;%u;%u", foreground ? 38 : 48, color->red, color->green,
		         color->blue);
	else
		return; /* the 0 that opens every SGR here has already said default */
	put_text(sink, text);
}

static int same_style(const bssh_screen_cell_t *a, const bssh_screen_cell_t *b) {
	return a->attrs == b->attrs && !memcmp(&a->fg, &b->fg, sizeof a->fg) && !memcmp(&a->bg, &b->bg, sizeof a->bg);
}

/* From a reset, so nothing set for an earlier cell can leak into this one. */
static void put_style(sink_t *sink, const bssh_screen_cell_t *cell) {
	put_text(sink, "\x1b[0");
	if (cell->attrs & BSSH_SCREEN_BOLD) put_text(sink, ";1");
	if (cell->attrs & BSSH_SCREEN_DIM) put_text(sink, ";2");
	if (cell->attrs & BSSH_SCREEN_ITALIC) put_text(sink, ";3");
	if (cell->attrs & BSSH_SCREEN_UNDERLINE) put_text(sink, ";4");
	if (cell->attrs & BSSH_SCREEN_BLINK) put_text(sink, ";5");
	if (cell->attrs & BSSH_SCREEN_REVERSE) put_text(sink, ";7");
	if (cell->attrs & BSSH_SCREEN_STRIKE) put_text(sink, ";9");
	put_color(sink, &cell->fg, 1);
	put_color(sink, &cell->bg, 0);
	put_text(sink, "m");
}

size_t bssh_screen_model_repaint(const bssh_screen_model_t *model, uint8_t *out, size_t capacity) {
	sink_t sink = { out, out ? capacity : 0, 0 };
	if (!model) return 0;
	char text[48];

	/*
	 * Into the alternate screen first when it is up, so what follows lands
	 * on it; then a clean slate. An erased cell is an unwritten one at both
	 * ends, which is what most of a screen is.
	 */
	put_text(&sink, model->alternate ? "\x1b[?1049h" : "\x1b[?1049l");
	put_text(&sink, "\x1b[0m\x1b[H\x1b[2J");

	int rows = 0, columns = 0;
	vterm_get_size(model->vt, &rows, &columns);
	bssh_screen_cell_t style;
	memset(&style, 0, sizeof style);
	for (int row = 0; row < rows; ++row) {
		int at = -1; /* where the terminal's cursor is in this row, if known */
		for (int col = 0; col < columns; ++col) {
			bssh_screen_cell_t cell;
			if (!bssh_screen_model_cell(model, row, col, &cell)) continue;
			if (cell.chars[0] == 0xFFFFFFFFu) continue; /* a wide glyph's far half */
			/* Never written and nothing to show: the slate already says so. */
			if (cell.chars[0] == 0 && cell.attrs == 0 && cell.bg.kind == BSSH_SCREEN_COLOR_DEFAULT
			    && cell.fg.kind == BSSH_SCREEN_COLOR_DEFAULT)
				continue;
			if (at != col) {
				snprintf(text, sizeof text, "\x1b[%d;%dH", row + 1, col + 1);
				put_text(&sink, text);
			}
			if (!same_style(&cell, &style)) {
				put_style(&sink, &cell);
				style = cell;
			}
			if (cell.chars[0] == 0) {
				/* Erased in a colour: erased again, in the same pen. */
				put_text(&sink, "\x1b[X");
				at = -1;
				continue;
			}
			for (int i = 0; i < BSSH_SCREEN_CELL_CHARS && cell.chars[i]; ++i) put_utf8(&sink, cell.chars[i]);
			at = col + (cell.width > 0 ? cell.width : 1);
			/* The last column leaves the cursor waiting to wrap; say where
			 * the next cell is rather than trust it. */
			if (at >= columns) at = -1;
		}
	}

	int row = 0, col = 0, visible = 1;
	bssh_screen_model_cursor(model, &row, &col, &visible);
	put_text(&sink, "\x1b[0m");
	snprintf(text, sizeof text, "\x1b[%d;%dH", row + 1, col + 1);
	put_text(&sink, text);
	put_text(&sink, visible ? "\x1b[?25h" : "\x1b[?25l");
	return sink.length;
}
