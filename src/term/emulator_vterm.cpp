#include "term/emulator.h"

#include <vterm.h>

/*
 * The libvterm binding. This file is the only one in the tree that
 * includes vterm.h.
 *
 * libvterm owns the parsing and its own copy of the screen; this code
 * mirrors the damaged parts of it into bssh_screen, which is what the
 * renderer and the tests see. Mirroring rather than reading libvterm's
 * cells directly at paint time is deliberate: it keeps the parser off the
 * UI thread's critical path later, and it means a golden-file test asserts
 * on OUR grid, so replacing the parser would not invalidate every test.
 */

struct bssh_emulator_private {
	VTerm *vt;
	VTermScreen *vts;
	bssh_screen screen;
	QByteArray pending_output;
	QByteArray pending_input;
	QString title;
	bssh_emulator *owner;

	bssh_emulator_private(int cols, int rows, bssh_emulator *owner)
	    : vt(nullptr), vts(nullptr), screen(cols, rows), owner(owner) {}

	int copy_cell(int row, int col);
	void copy_rect(const VTermRect &rect);
};

namespace {

/*
 * How many bytes the UTF-8 sequence starting with this lead byte needs,
 * or 0 if it is not a lead byte.
 */
int utf8_sequence_length(unsigned char byte) {
	if ((byte & 0xE0) == 0xC0) return 2;
	if ((byte & 0xF0) == 0xE0) return 3;
	if ((byte & 0xF8) == 0xF0) return 4;
	return 0;
}

/*
 * Length of the incomplete UTF-8 sequence at the end of a buffer, or 0 if
 * it ends cleanly.
 *
 * libvterm does NOT tolerate a sequence split across two
 * vterm_input_write() calls -- verified against the library directly: the
 * bytes "68 c3" then "a9 6c 6c 6f" decode to h,U+FFFD,l,l,o rather than
 * "héllo". Since SSH reads split wherever the network happened to put a
 * packet boundary, holding the tail back until it is complete is the
 * caller's job, and this is that caller.
 */
int incomplete_tail_length(const QByteArray &bytes) {
	const int max_sequence = 4;
	const int limit = qMin(max_sequence, bytes.size());

	for (int back = 1; back <= limit; ++back) {
		const unsigned char byte = static_cast<unsigned char>(bytes.at(bytes.size() - back));
		const int needed = utf8_sequence_length(byte);

		if (needed > 0) return needed > back ? back : 0;

		/* A continuation byte: keep walking back towards its lead. */
		if ((byte & 0xC0) != 0x80) return 0;
	}

	/*
	 * Four continuation bytes with no lead in sight is malformed input,
	 * not a split sequence. Pass it through and let libvterm render the
	 * replacement character -- stalling the stream waiting for a lead
	 * byte that is never coming would hang the terminal on one bad byte.
	 */
	return 0;
}

bssh_color_t to_bssh_color(const VTermColor &color, bool foreground) {
	bssh_color_t out;

	if (VTERM_COLOR_IS_DEFAULT_FG(&color) || VTERM_COLOR_IS_DEFAULT_BG(&color)) {
		/*
		 * "Whatever the theme says" is preserved as DEFAULT rather than
		 * resolved to a concrete value here -- resolving it at parse
		 * time would bake today's colour scheme into every cell already
		 * on screen (see bssh_color_t).
		 */
		Q_UNUSED(foreground);
		return out;
	}
	if (VTERM_COLOR_IS_INDEXED(&color)) {
		out.kind = bssh_color_t::INDEXED;
		out.index = color.indexed.idx;
		return out;
	}
	if (VTERM_COLOR_IS_RGB(&color)) {
		out.kind = bssh_color_t::RGB;
		out.rgb = qRgb(color.rgb.red, color.rgb.green, color.rgb.blue);
	}
	return out;
}

quint8 to_bssh_attrs(const VTermScreenCellAttrs &attrs) {
	quint8 out = bssh_cell_t::NONE;

	if (attrs.bold) out |= bssh_cell_t::BOLD;
	if (attrs.italic) out |= bssh_cell_t::ITALIC;
	if (attrs.underline) out |= bssh_cell_t::UNDERLINE;
	if (attrs.reverse) out |= bssh_cell_t::INVERSE;
	if (attrs.blink) out |= bssh_cell_t::BLINK;
	if (attrs.strike) out |= bssh_cell_t::STRIKE;
	return out;
}

int on_damage(VTermRect rect, void *user) {
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);
	self->copy_rect(rect);
	emit self->owner->damaged();
	return 1;
}

int on_move_cursor(VTermPos pos, VTermPos oldpos, int visible, void *user) {
	Q_UNUSED(oldpos);
	Q_UNUSED(visible);
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);
	self->screen.set_cursor(pos.row, pos.col);
	emit self->owner->damaged();
	return 1;
}

int on_move_rect(VTermRect dest, VTermRect src, void *user) {
	Q_UNUSED(src);
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);
	/*
	 * A scroll. libvterm has already moved its own cells, so re-copying
	 * the destination is enough -- there is no need to reimplement the
	 * move, and doing so would be a second chance to get it wrong.
	 */
	self->copy_rect(dest);
	emit self->owner->damaged();
	return 1;
}

int on_set_term_prop(VTermProp prop, VTermValue *value, void *user) {
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);

	switch (prop) {
	case VTERM_PROP_TITLE:
		/*
		 * Delivered as fragments, not as a whole string: a long title
		 * split across two reads arrives as two callbacks, and treating
		 * either one as complete is how a tab ends up labelled with half
		 * a path.
		 */
		if (value->string.initial) self->title.clear();
		self->title.append(QString::fromUtf8(value->string.str, value->string.len));
		if (value->string.final) emit self->owner->title_changed(self->title);
		return 1;
	case VTERM_PROP_CURSORVISIBLE:
		self->screen.set_cursor_visible(value->boolean);
		emit self->owner->damaged();
		return 1;
	default:
		return 0;
	}
}

int on_bell(void *user) {
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);
	emit self->owner->bell();
	return 1;
}

void on_output(const char *bytes, size_t len, void *user) {
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);
	self->pending_output.append(bytes, static_cast<int>(len));
	emit self->owner->output_ready();
}

const VTermScreenCallbacks screen_callbacks = {
	.damage      = on_damage,
	.moverect    = on_move_rect,
	.movecursor  = on_move_cursor,
	.settermprop = on_set_term_prop,
	.bell        = on_bell,
	.resize      = nullptr, /* our own resize() drives the mirror */
	.sb_pushline = nullptr, /* scrollback is its own milestone */
	.sb_popline  = nullptr,
	.sb_clear    = nullptr,
	/* Opt-in ABI extension; only used if the has_pushline4 call is made. */
	.sb_pushline4 = nullptr
	/*
	 * Designated initialisers on purpose: libvterm has added callbacks
	 * before (sb_pushline4 is newer than this list), and a positional
	 * initialiser would silently shift every function pointer down one
	 * slot the next time that happens.
	 */
};

} /* namespace */

int bssh_emulator_private::copy_cell(int row, int col) {
	VTermPos pos;
	pos.row = row;
	pos.col = col;

	VTermScreenCell source;
	if (!vterm_screen_get_cell(vts, pos, &source)) return 1;

	bssh_cell_t cell;
	/*
	 * chars[0] only: libvterm hands back the base character plus any
	 * combining marks that follow it. Composing them properly is a
	 * rendering question (sec 9.3 -- they belong in the base cell, not a
	 * new one), and taking just the base is the honest subset until the
	 * glyph pipeline at M9 can draw the rest.
	 */
	cell.ch = source.chars[0] ? source.chars[0] : U' ';
	cell.width = static_cast<quint8>(source.width);
	cell.attrs = to_bssh_attrs(source.attrs);
	cell.fg = to_bssh_color(source.fg, true);
	cell.bg = to_bssh_color(source.bg, false);

	screen.set_cell(row, col, cell);

	/*
	 * The second half of a double-width glyph. libvterm reports width 2
	 * on the first cell and leaves the next one as filler; the renderer
	 * needs it marked so it draws nothing there rather than a stray
	 * space (sec 9.3).
	 */
	if (source.width == 2) {
		bssh_cell_t continuation;
		continuation.ch = U'\0';
		continuation.width = 0;
		continuation.attrs = cell.attrs;
		continuation.fg = cell.fg;
		continuation.bg = cell.bg;
		screen.set_cell(row, col + 1, continuation);
		return 2;
	}

	return 1;
}

void bssh_emulator_private::copy_rect(const VTermRect &rect) {
	/*
	 * Advancing by the glyph's width, rather than one column at a time,
	 * is what keeps a double-width character's continuation cell intact:
	 * libvterm reports the second half as an ordinary width-1 blank, so
	 * copying it would overwrite the width-0 marker the renderer needs
	 * (sec 9.3) and the glyph would end up painted over by a space.
	 *
	 * Starting one column early when the rect begins mid-glyph handles
	 * the other half of the same problem -- a damage rect is free to
	 * start on a continuation column.
	 */
	int start_col = rect.start_col;
	if (start_col > 0 && screen.cell(rect.start_row, start_col).width == 0) --start_col;

	for (int row = rect.start_row; row < rect.end_row; ++row) {
		int col = start_col;
		while (col < rect.end_col) {
			col += copy_cell(row, col);
		}
	}
}

bssh_emulator::bssh_emulator(int cols, int rows, QObject *parent)
    : QObject(parent),
      m_private(new bssh_emulator_private(cols, rows, this)) {
	m_private->vt = vterm_new(qMax(1, rows), qMax(1, cols));
	vterm_set_utf8(m_private->vt, 1);

	m_private->vts = vterm_obtain_screen(m_private->vt);
	vterm_screen_set_callbacks(m_private->vts, &screen_callbacks, m_private);
	vterm_screen_reset(m_private->vts, 1);

	vterm_output_set_callback(m_private->vt, on_output, m_private);
}

bssh_emulator::~bssh_emulator() {
	if (m_private->vt) vterm_free(m_private->vt);
	delete m_private;
}

void bssh_emulator::feed(const QByteArray &bytes) {
	if (bytes.isEmpty()) return;

	QByteArray buffer = m_private->pending_input;
	buffer.append(bytes);
	m_private->pending_input.clear();

	const int tail = incomplete_tail_length(buffer);
	if (tail > 0) {
		m_private->pending_input = buffer.right(tail);
		buffer.chop(tail);
	}

	if (buffer.isEmpty()) return;
	vterm_input_write(m_private->vt, buffer.constData(), buffer.size());
}

void bssh_emulator::resize(int cols, int rows) {
	cols = qMax(1, cols);
	rows = qMax(1, rows);

	vterm_set_size(m_private->vt, rows, cols);
	m_private->screen.resize(cols, rows);

	/*
	 * Re-mirror everything: libvterm reflows on resize and does not
	 * damage every cell it moved, so copying only what it reports would
	 * leave the tail of a shrunk screen holding stale text.
	 */
	VTermRect all;
	all.start_row = 0;
	all.end_row = rows;
	all.start_col = 0;
	all.end_col = cols;
	m_private->copy_rect(all);

	emit damaged();
}

const bssh_screen &bssh_emulator::screen() const {
	return m_private->screen;
}

void bssh_emulator::send_key(int key, quint8 modifiers) {
	const VTermModifier mod = static_cast<VTermModifier>(modifiers);

	if (key >= KEY_FUNCTION_BASE) {
		/* The macro yields an int; C++ will not narrow that to the enum. */
		vterm_keyboard_key(m_private->vt,
		                    static_cast<VTermKey>(VTERM_KEY_FUNCTION(key - KEY_FUNCTION_BASE)),
		                    mod);
		return;
	}

	static const VTermKey mapping[] = {
		VTERM_KEY_NONE,
		VTERM_KEY_ENTER,
		VTERM_KEY_TAB,
		VTERM_KEY_BACKSPACE,
		VTERM_KEY_ESCAPE,
		VTERM_KEY_UP,
		VTERM_KEY_DOWN,
		VTERM_KEY_LEFT,
		VTERM_KEY_RIGHT,
		VTERM_KEY_INS,
		VTERM_KEY_DEL,
		VTERM_KEY_HOME,
		VTERM_KEY_END,
		VTERM_KEY_PAGEUP,
		VTERM_KEY_PAGEDOWN
	};

	if (key <= KEY_NONE || key >= int(sizeof(mapping) / sizeof(mapping[0]))) return;
	vterm_keyboard_key(m_private->vt, mapping[key], mod);
}

void bssh_emulator::send_char(char32_t ch, quint8 modifiers) {
	vterm_keyboard_unichar(m_private->vt, ch, static_cast<VTermModifier>(modifiers));
}

void bssh_emulator::send_paste(const QString &text) {
	if (text.isEmpty()) return;

	/*
	 * libvterm emits the bracketing markers itself, and only when the
	 * remote enabled the mode -- which is why this goes through the
	 * library rather than prepending the escape sequences by hand.
	 */
	vterm_keyboard_start_paste(m_private->vt);

	const QList<uint> codepoints = text.toUcs4();
	for (uint codepoint : codepoints) {
		if (codepoint == u'\r' || codepoint == u'\n') {
			vterm_keyboard_key(m_private->vt, VTERM_KEY_ENTER, VTERM_MOD_NONE);
			continue;
		}
		vterm_keyboard_unichar(m_private->vt, codepoint, VTERM_MOD_NONE);
	}

	vterm_keyboard_end_paste(m_private->vt);
}

QByteArray bssh_emulator::take_output() {
	QByteArray out;
	out.swap(m_private->pending_output);
	return out;
}
