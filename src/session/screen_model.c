#include "session/screen_model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vterm.h>
/*
 * The state's modes are kept where libvterm keeps them, in a structure no
 * API reads; the same vendored, patched library as this file's, built with
 * it, so the two cannot disagree about its layout.
 */
#include "../../vterm/src/vterm_internal.h"

/* A line gone off the top, trailing blanks left off. */
typedef struct history_line {
	bssh_screen_cell_t *cells;
	int width;
	int continuation;
	/* Fed no later than this offset: what a client at it already has. */
	uint64_t at;
} history_line_t;

struct bssh_screen_model {
	VTerm *vt;
	VTermScreen *screen;
	/* A ring of `limit` slots; `kept` of them used, the oldest at `oldest`. */
	history_line_t *history;
	size_t limit;
	size_t oldest;
	size_t kept;
	uint64_t gone;   /* lines dropped off the front: the oldest kept's number */
	uint64_t gone_at;   /* `at` of the newest line dropped */
	uint64_t fed;       /* bytes given to feed, held ones included */
	uint64_t pushing_at;   /* `at` for a line pushed now */
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

static void drop_oldest(bssh_screen_model_t *model);
static int on_pushline4(int cols, const VTermScreenCell *cells, bool continuation, void *user);
static int on_pushline(int cols, const VTermScreenCell *cells, void *user);
static int on_popline4(int cols, VTermScreenCell *cells, bool *continuation, void *user);
static int on_popline(int cols, VTermScreenCell *cells, void *user);

static const VTermScreenCallbacks callbacks = {
	.settermprop = on_settermprop,
	.sb_pushline = on_pushline,
	.sb_popline = on_popline,
	.sb_pushline4 = on_pushline4,
	.sb_popline4 = on_popline4,
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
	bssh_screen_model_set_history_limit(model, BSSH_SCREEN_HISTORY_LINES);

	/* As the client's emulator sets it up (src/term/emulator_vterm.cpp),
	 * so a byte means the same at both ends. */
	vterm_set_utf8(model->vt, 1);
	model->screen = vterm_obtain_screen(model->vt);
	vterm_screen_set_callbacks(model->screen, &callbacks, model);
	vterm_screen_callbacks_has_pushline4(model->screen);
	vterm_screen_enable_reflow(model->screen, true);
	vterm_screen_set_damage_merge(model->screen, VTERM_DAMAGE_ROW);
	vterm_screen_enable_altscreen(model->screen, 1);
	vterm_screen_reset(model->screen, 1);
	vterm_output_set_callback(model->vt, on_output, model);
	return model;
}

void bssh_screen_model_free(bssh_screen_model_t *model) {
	if (!model) return;
	while (model->kept) drop_oldest(model);
	free(model->history);
	vterm_free(model->vt);
	free(model);
}

static void feed_piece(bssh_screen_model_t *model, const uint8_t *bytes, size_t length) {
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

/*
 * A line at a time, so each line pushed off the top knows how far the bytes
 * had got: up to the end of the line being written, which a client at that
 * offset has seen. Never earlier than the truth -- a line pushed by a wrap
 * part-way along is marked at the line's end -- so a client coming back is
 * sent a line it had sooner than missing one it did not.
 */
void bssh_screen_model_feed(bssh_screen_model_t *model, const uint8_t *bytes, size_t length) {
	if (!model || !bytes || length == 0) return;

	while (length > 0) {
		const uint8_t *newline = memchr(bytes, '\n', length);
		const size_t piece = newline ? (size_t)(newline - bytes) + 1 : length;

		model->pushing_at = model->fed + piece;
		feed_piece(model, bytes, piece);
		model->fed += piece;
		bytes += piece;
		length -= piece;
	}
}

uint64_t bssh_screen_model_offset(const bssh_screen_model_t *model) {
	return model ? model->fed - model->held_length : 0;
}

int bssh_screen_model_history_after(const bssh_screen_model_t *model, uint64_t offset, uint64_t *from) {
	if (!model) {
		if (from) *from = 0;
		return 1;
	}
	/* The marks only grow, so the first line past `offset` is found halving. */
	size_t low = 0, high = model->kept;
	while (low < high) {
		const size_t middle = low + (high - low) / 2;
		if (model->history[(model->oldest + middle) % model->limit].at <= offset) low = middle + 1;
		else high = middle;
	}
	if (from) *from = model->gone + low;
	return low > 0 || model->gone == 0 || model->gone_at <= offset;
}

void bssh_screen_model_resize(bssh_screen_model_t *model, int rows, int columns) {
	if (!model || rows < 1 || columns < 1) return;
	/*
	 * What a resize pushes, a client at the bytes so far has not seen: it is
	 * marked past them. A client that resized with the host while attached
	 * pushed the same lines itself, and one coming back puts its history
	 * back as it stood (bssh_emulator::replace_history) -- so a line twice
	 * at worst, never one missing.
	 */
	model->pushing_at = model->fed + 1;
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

static void convert(const VTermScreenCell *source, bssh_screen_cell_t *out) {
	const VTermScreenCell cell = *source;
	memset(out, 0, sizeof *out);
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
	convert(&cell, out);
	return 1;
}

/* Blank as the client's scrollback trims (bssh_scrollback::is_blank): a
 * space or nothing, one cell wide, no attribute, both colours the theme's. */
static int blank(const bssh_screen_cell_t *cell) {
	return (cell->chars[0] == 0 || cell->chars[0] == ' ') && cell->width == 1 && cell->attrs == 0
	       && cell->fg.kind == BSSH_SCREEN_COLOR_DEFAULT && cell->bg.kind == BSSH_SCREEN_COLOR_DEFAULT;
}

static void trim(history_line_t *line) {
	while (line->width > 0 && blank(&line->cells[line->width - 1])) --line->width;
}

static void drop_oldest(bssh_screen_model_t *model) {
	model->gone_at = model->history[model->oldest].at;
	free(model->history[model->oldest].cells);
	model->history[model->oldest].cells = NULL;
	model->oldest = (model->oldest + 1) % model->limit;
	--model->kept;
	++model->gone;
}

static int on_pushline4(int cols, const VTermScreenCell *cells, bool continuation, void *user) {
	bssh_screen_model_t *model = user;
	if (model->limit == 0) {
		model->gone_at = model->pushing_at;
		++model->gone;
		return 1;
	}
	/*
	 * The line before is trimmed once this one shows it ended there, as the
	 * client's scrollback does: blanks at the end of a line that wrapped
	 * are text ("ls -l " wrapped is not "ls -l"), so a line is trimmed only
	 * when the next does not continue it, and the newest not yet.
	 */
	if (!continuation && model->kept)
		trim(&model->history[(model->oldest + model->kept - 1) % model->limit]);
	if (model->kept == model->limit) drop_oldest(model);

	history_line_t line = { NULL, 0, continuation ? 1 : 0, model->pushing_at };
	if (cols > 0) line.cells = malloc((size_t)cols * sizeof *line.cells);
	if (line.cells) {
		for (int col = 0; col < cols; ++col) convert(&cells[col], &line.cells[col]);
		line.width = cols;
	}
	/* Out of memory: the line is kept empty, so the numbering still holds. */
	model->history[(model->oldest + model->kept) % model->limit] = line;
	++model->kept;
	return 1;
}

static int on_pushline(int cols, const VTermScreenCell *cells, void *user) {
	return on_pushline4(cols, cells, false, user);
}

/*
 * A resize that grows the screen takes the newest line back down, as the
 * client's does (its pop_line): the base character only, since that is all
 * the client keeps of a line it scrolled away, and the two screens have to
 * agree after the pop as before it.
 */
static int on_popline4(int cols, VTermScreenCell *cells, bool *continuation, void *user) {
	bssh_screen_model_t *model = user;
	if (model->kept == 0) return 0;
	const size_t slot = (model->oldest + model->kept - 1) % model->limit;
	history_line_t line = model->history[slot];
	--model->kept;

	VTermColor default_fg, default_bg;
	vterm_state_get_default_colors(vterm_obtain_state(model->vt), &default_fg, &default_bg);
	for (int col = 0; col < cols; ++col) {
		VTermScreenCell *to = &cells[col];
		memset(to, 0, sizeof *to);
		to->width = 1;
		to->fg = default_fg;
		to->bg = default_bg;
		if (col >= line.width || !line.cells) continue;
		const bssh_screen_cell_t *from = &line.cells[col];
		to->chars[0] = from->chars[0];
		to->width = from->width ? from->width : 1;
		if (from->fg.kind == BSSH_SCREEN_COLOR_INDEXED) vterm_color_indexed(&to->fg, from->fg.index);
		else if (from->fg.kind == BSSH_SCREEN_COLOR_RGB)
			vterm_color_rgb(&to->fg, from->fg.red, from->fg.green, from->fg.blue);
		if (from->bg.kind == BSSH_SCREEN_COLOR_INDEXED) vterm_color_indexed(&to->bg, from->bg.index);
		else if (from->bg.kind == BSSH_SCREEN_COLOR_RGB)
			vterm_color_rgb(&to->bg, from->bg.red, from->bg.green, from->bg.blue);
		to->attrs.bold = !!(from->attrs & BSSH_SCREEN_BOLD);
		to->attrs.italic = !!(from->attrs & BSSH_SCREEN_ITALIC);
		to->attrs.underline = !!(from->attrs & BSSH_SCREEN_UNDERLINE);
		to->attrs.reverse = !!(from->attrs & BSSH_SCREEN_REVERSE);
		to->attrs.blink = !!(from->attrs & BSSH_SCREEN_BLINK);
		to->attrs.strike = !!(from->attrs & BSSH_SCREEN_STRIKE);
		to->attrs.dim = !!(from->attrs & BSSH_SCREEN_DIM);
	}
	if (continuation) *continuation = line.continuation != 0;
	free(line.cells);
	return 1;
}

static int on_popline(int cols, VTermScreenCell *cells, void *user) {
	return on_popline4(cols, cells, NULL, user);
}

void bssh_screen_model_set_history_limit(bssh_screen_model_t *model, size_t lines) {
	if (!model) return;
	while (model->kept > lines) drop_oldest(model);
	history_line_t *ring = lines ? calloc(lines, sizeof *ring) : NULL;
	if (lines && !ring) return; /* keep the old ring rather than none */
	for (size_t i = 0; i < model->kept; ++i) ring[i] = model->history[(model->oldest + i) % model->limit];
	free(model->history);
	model->history = ring;
	model->limit = lines;
	model->oldest = 0;
}

void bssh_screen_model_history(const bssh_screen_model_t *model, uint64_t *first, uint64_t *count) {
	if (first) *first = model ? model->gone : 0;
	if (count) *count = model ? model->kept : 0;
}

int bssh_screen_model_history_line(const bssh_screen_model_t *model, uint64_t number,
                                   bssh_screen_cell_t *out, int capacity, int *continuation) {
	if (!model || number < model->gone || number - model->gone >= model->kept) return -1;
	const history_line_t *line = &model->history[(model->oldest + (size_t)(number - model->gone)) % model->limit];
	const int copy = line->width < capacity ? line->width : capacity;
	if (out && copy > 0) memcpy(out, line->cells, (size_t)copy * sizeof *out);
	if (continuation) *continuation = line->continuation;
	return line->width;
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

/*
 * A cell of the screen showing, or of the primary one underneath an alternate
 * screen (vterm-patch/0026): what the repaint draws first when a full-screen
 * program is up, so leaving it shows the shell as it was.
 */
static int read_cell(const bssh_screen_model_t *model, int primary, int row, int column, bssh_screen_cell_t *out) {
	if (!primary || !model->alternate) return bssh_screen_model_cell(model, row, column, out);
	memset(out, 0, sizeof *out);
	out->width = 1;
	int rows = 0, columns = 0;
	vterm_get_size(model->vt, &rows, &columns);
	if (row < 0 || row >= rows || column < 0 || column >= columns) return 0;
	VTermPos pos = { .row = row, .col = column };
	VTermScreenCell cell;
	if (!vterm_screen_get_primary_cell(model->screen, pos, &cell)) return 0;
	convert(&cell, out);
	return 1;
}

/* Whether a row of that screen continues the row above. */
static int row_continues(const bssh_screen_model_t *model, int primary, int row) {
	if (row <= 0) return 0;
	const VTermState *state = vterm_obtain_state(model->vt);
	if (primary && model->alternate)
		return state->lineinfos[BUFIDX_PRIMARY] && state->lineinfos[BUFIDX_PRIMARY][row].continuation;
	const VTermLineInfo *info = vterm_state_get_lineinfo(state, row);
	return info && info->continuation;
}

/*
 * Row `row`'s last column written again, so the cursor waits at the edge and
 * the next character wraps. A wide glyph there is written whole; a cell never
 * written goes as a space, since a wrap needs a character: returns 2 then, for
 * the caller to erase it again once the wrap is made, and 1 otherwise.
 */
static int put_last_column(sink_t *sink, const bssh_screen_model_t *model, int primary, int row, int columns,
                           bssh_screen_cell_t *style) {
	char text[48];
	int col = columns - 1;
	bssh_screen_cell_t cell;
	read_cell(model, primary, row, col, &cell);
	if (cell.chars[0] == 0xFFFFFFFFu && col > 0) read_cell(model, primary, row, --col, &cell);
	snprintf(text, sizeof text, "\x1b[%d;%dH", row + 1, col + 1);
	put_text(sink, text);
	if (!same_style(&cell, style)) {
		put_style(sink, &cell);
		*style = cell;
	}
	if (cell.chars[0] == 0) put_utf8(sink, ' ');
	for (int i = 0; i < BSSH_SCREEN_CELL_CHARS && cell.chars[i]; ++i) put_utf8(sink, cell.chars[i]);
	return cell.chars[0] == 0 ? 2 : 1;
}

/*
 * A stand-in space erased again, in the pen it had: the cell was never
 * written. At the last column by deleting it rather than erasing it, since
 * libvterm takes an erase that reaches the edge as the line ending there and
 * drops the next row's mark -- the one the wrap was written to make.
 */
static void erase_stand_in(sink_t *sink, const bssh_screen_model_t *model, int primary, int row, int col,
                           int columns, bssh_screen_cell_t *style) {
	char text[48];
	bssh_screen_cell_t cell;
	read_cell(model, primary, row, col, &cell);
	snprintf(text, sizeof text, "\x1b[%d;%dH", row + 1, col + 1);
	put_text(sink, text);
	if (!same_style(&cell, style)) {
		put_style(sink, &cell);
		*style = cell;
	}
	put_text(sink, col == columns - 1 ? "\x1b[P" : "\x1b[X");
}

int bssh_screen_model_row_continues(const bssh_screen_model_t *model, int row) {
	int rows = 0, columns = 0;
	if (!model) return 0;
	vterm_get_size(model->vt, &rows, &columns);
	if (row <= 0 || row >= rows) return 0;
	const VTermLineInfo *info = vterm_state_get_lineinfo(vterm_obtain_state(model->vt), row);
	return info && info->continuation;
}

/* One screen's cells, onto a clean slate the caller has made. */
static void put_screen(sink_t *sink, const bssh_screen_model_t *model, int primary, int rows, int columns) {
	char text[48];
	bssh_screen_cell_t style;
	memset(&style, 0, sizeof style);
	/*
	 * A row that continues the one above is drawn by writing on past the
	 * edge, as the program's text did: the terminal then marks the row
	 * itself, and it is that mark a copy joins by and a narrower window
	 * rewraps by. Placed with a cursor move, the row would be a line of
	 * its own at the client however it looked.
	 */
	int wrapping = 0; /* the last column was just written */
	for (int row = 0; row < rows; ++row) {
		const int continues = row_continues(model, primary, row);
		int stand_in_above = 0;
		if (continues && !wrapping) {
			wrapping = 1;
			stand_in_above = put_last_column(sink, model, primary, row - 1, columns, &style) == 2;
		}

		int at = continues && wrapping ? 0 : -1; /* where the cursor is in this row, if known */
		wrapping = 0;
		for (int col = 0; col < columns; ++col) {
			bssh_screen_cell_t cell;
			if (!read_cell(model, primary, row, col, &cell)) continue;
			if (cell.chars[0] == 0xFFFFFFFFu) continue; /* a wide glyph's far half */
			const int wrap_here = continues && col == 0 && at == 0;
			/* Never written and nothing to show: the slate already says so. */
			if (!wrap_here && cell.chars[0] == 0 && cell.attrs == 0
			    && cell.bg.kind == BSSH_SCREEN_COLOR_DEFAULT && cell.fg.kind == BSSH_SCREEN_COLOR_DEFAULT)
				continue;
			if (at != col) {
				snprintf(text, sizeof text, "\x1b[%d;%dH", row + 1, col + 1);
				put_text(sink, text);
			}
			if (!same_style(&cell, &style)) {
				put_style(sink, &cell);
				style = cell;
			}
			if (cell.chars[0] == 0 && !wrap_here) {
				/* Erased in a colour: erased again, in the same pen. */
				put_text(sink, "\x1b[X");
				at = -1;
				continue;
			}
			/* The wrap needs a character; a space stands for one never written. */
			if (cell.chars[0] == 0) put_utf8(sink, ' ');
			for (int i = 0; i < BSSH_SCREEN_CELL_CHARS && cell.chars[i]; ++i) put_utf8(sink, cell.chars[i]);
			at = col + (cell.width > 0 ? cell.width : 1);
			if (wrap_here) {
				/* The wrap is made: what stood in for unwritten cells goes. */
				if (stand_in_above) erase_stand_in(sink, model, primary, row - 1, columns - 1, columns, &style);
				if (cell.chars[0] == 0) erase_stand_in(sink, model, primary, row, 0, columns, &style);
				if (stand_in_above || cell.chars[0] == 0) at = -1;
			}
			/* The last column leaves the cursor waiting to wrap; say where
			 * the next cell is rather than trust it, unless the next row
			 * is where it wraps to. */
			if (at >= columns) {
				at = -1;
				wrapping = 1;
			}
		}
	}
}

size_t bssh_screen_model_repaint(const bssh_screen_model_t *model, uint8_t *out, size_t capacity) {
	sink_t sink = { out, out ? capacity : 0, 0 };
	if (!model) return 0;
	char text[48];

	int rows = 0, columns = 0;
	vterm_get_size(model->vt, &rows, &columns);
	const VTermState *state_now = vterm_obtain_state(model->vt);

	/*
	 * The primary screen first, then into the alternate one when it is up,
	 * as the program went: leaving it then shows the shell as it was, at
	 * the place its cursor was saved, and not whatever the client had.
	 * Each onto a clean slate; an erased cell is an unwritten one at both
	 * ends, which is what most of a screen is. Nothing the client had set
	 * may bend the drawing: origin mode would move every placement, insert
	 * mode push cells along, and a wrap needs autowrap.
	 */
	put_text(&sink, "\x1b[?1049l\x1b[?6l\x1b[4l\x1b[r\x1b[?7h\x1b[0m\x1b[H\x1b[2J");
	if (model->alternate) {
		put_screen(&sink, model, 1, rows, columns);
		snprintf(text, sizeof text, "\x1b[0m\x1b[%d;%dH", state_now->saved_primary.pos.row + 1,
		         state_now->saved_primary.pos.col + 1);
		put_text(&sink, text);
		put_text(&sink, "\x1b[?1049h\x1b[0m\x1b[H\x1b[2J");
	}
	put_screen(&sink, model, 0, rows, columns);

	/*
	 * The modes, each said both ways: the client fed this was somewhere
	 * else, so what it has set is no guide. These are the ones a key, a
	 * paste or a click is sent by, and the ones that move later output.
	 */
	VTermState *state = vterm_obtain_state(model->vt);
	put_text(&sink, state->mode.cursor ? "\x1b[?1h" : "\x1b[?1l");
	put_text(&sink, state->mode.keypad ? "\x1b=" : "\x1b>");
	put_text(&sink, state->mode.bracketpaste ? "\x1b[?2004h" : "\x1b[?2004l");
	put_text(&sink, state->mode.report_focus ? "\x1b[?1004h" : "\x1b[?1004l");
	put_text(&sink, "\x1b[?1000l\x1b[?1002l\x1b[?1003l\x1b[?1005l\x1b[?1006l\x1b[?1015l");
	if (state->mouse_flags & MOUSE_WANT_MOVE) put_text(&sink, "\x1b[?1003h");
	else if (state->mouse_flags & MOUSE_WANT_DRAG) put_text(&sink, "\x1b[?1002h");
	else if (state->mouse_flags & MOUSE_WANT_CLICK) put_text(&sink, "\x1b[?1000h");
	if (state->mouse_protocol == MOUSE_UTF8) put_text(&sink, "\x1b[?1005h");
	else if (state->mouse_protocol == MOUSE_SGR) put_text(&sink, "\x1b[?1006h");
	else if (state->mouse_protocol == MOUSE_RXVT) put_text(&sink, "\x1b[?1015h");
	put_text(&sink, state->mode.autowrap ? "\x1b[?7h" : "\x1b[?7l");
	put_text(&sink, state->mode.insert ? "\x1b[4h" : "\x1b[4l");
	put_text(&sink, state->mode.newline ? "\x1b[20h" : "\x1b[20l");

	/* The region and origin mode both send the cursor home, so before it. */
	const int top = state->scrollregion_top;
	const int bottom = SCROLLREGION_BOTTOM(state);
	if (top == 0 && bottom == rows) {
		put_text(&sink, "\x1b[r");
	} else {
		snprintf(text, sizeof text, "\x1b[%d;%dr", top + 1, bottom);
		put_text(&sink, text);
	}
	if (state->mode.origin) put_text(&sink, "\x1b[?6h");

	int row = 0, col = 0, visible = 1;
	bssh_screen_model_cursor(model, &row, &col, &visible);
	put_text(&sink, "\x1b[0m");
	/* Under origin mode a position is counted from the region's top. */
	snprintf(text, sizeof text, "\x1b[%d;%dH", (state->mode.origin ? row - top : row) + 1, col + 1);
	put_text(&sink, text);
	put_text(&sink, visible ? "\x1b[?25h" : "\x1b[?25l");
	return sink.length;
}
