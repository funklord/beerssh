#include "term/emulator.h"

#include <vterm.h>

#include "term/graphics_internal.h"

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

	/*
	 * Kitty keyboard protocol flags, as a stack: the protocol is defined
	 * as push/pop precisely so a full-screen program can enable what it
	 * needs and restore whatever the shell had without knowing what that
	 * was. Bottom entry is 0 -- legacy -- and is never popped.
	 */
	QList<int> kitty_stack;
	bssh_emulator *owner;
	int modify_other_keys;
	bool mouse_reporting;

	bssh_image_store images;
	bssh_scrollback scrollback;
	bssh_graphics_transmission_t graphics;   /* chunked kitty transmissions */
	QByteArray apc_buffer;        /* an APC split across reads */
	QByteArray osc_buffer;
	QByteArray dcs_buffer;
	int cell_width;
	int cell_height;

	/*
	 * Rows the cursor owes an image. It cannot be applied inside the APC
	 * callback: moving our own mirror's cursor there is invisible to
	 * libvterm, which then writes the next text over the image it just
	 * placed. libvterm exposes no cursor setter, so the movement is fed
	 * back to it as a real CUD once the current write has finished.
	 */
	int pending_cursor_advance;

	void apply_graphics(const bssh_graphics_result_t &result);

	bssh_emulator_private(int cols, int rows, bssh_emulator *owner)
	    : vt(nullptr), vts(nullptr), screen(cols, rows), owner(owner),
	      modify_other_keys(0), mouse_reporting(false), cell_width(8), cell_height(16),
	      pending_cursor_advance(0) {
		kitty_stack.append(0);
	}

	int kitty_flags() const { return kitty_stack.last(); }
	bool encode_modern_key(char32_t ch, quint8 modifiers);
	void reply(const QByteArray &bytes);

	int copy_cell(int row, int col);
	void copy_rect(const VTermRect &rect);
	bssh_cell_t convert_cell(const VTermScreenCell &source) const;
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

	/*
	 * Images move with the text they were drawn beside. An image pinned
	 * to its original row while everything around it scrolls looks like
	 * the terminal has come apart.
	 */
	if (dest.start_col == src.start_col && dest.start_row != src.start_row) {
		self->images.scrolled(qMin(dest.start_row, src.start_row),
			                     qMax(dest.end_row, src.end_row),
			                     src.start_row - dest.start_row);
		emit self->owner->images_changed();
	}

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
	case VTERM_PROP_MOUSE:
		/*
		 * The remote turned mouse tracking on or off. Worth mirroring
		 * rather than querying: the view has to decide on every click
		 * whether it is selecting text or sending a report, and asking
		 * libvterm per event would be a lookup for something that
		 * changes twice a session.
		 */
		self->mouse_reporting = value->number != VTERM_PROP_MOUSE_NONE;
		return 1;
	case VTERM_PROP_CURSORVISIBLE:
		self->screen.set_cursor_visible(value->boolean);
		emit self->owner->damaged();
		return 1;
	default:
		return 0;
	}
}

/*
 * Everything libvterm does not recognise arrives here (sec 7.1). This is
 * its documented extension hook, so the vendored copy stays unpatched --
 * which matters, because a patched dependency is one that has to be
 * re-patched on every update.
 */
int on_unrecognised_csi(const char *leader, const long args[], int argcount,
	                       const char *intermed, char command, void *user) {
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);
	Q_UNUSED(intermed);

	const QByteArray lead = QByteArray(leader ? leader : "");

	if (command == 'u') {
		/* Kitty keyboard protocol: push, pop, set, query. */
		if (lead == ">") {
			const int flags = argcount > 0 ? int(args[0]) : 0;
			self->kitty_stack.append(flags);
			return 1;
		}
		if (lead == "<") {
			const int count = argcount > 0 && args[0] > 0 ? int(args[0]) : 1;
			for (int i = 0; i < count && self->kitty_stack.size() > 1; ++i) {
				self->kitty_stack.removeLast();
			}
			return 1;
		}
		if (lead == "=") {
			const int flags = argcount > 0 ? int(args[0]) : 0;
			const int mode = argcount > 1 ? int(args[1]) : 1;
			int current = self->kitty_stack.last();
			if (mode == 2) {
				current |= flags;
			} else if (mode == 3) {
				current &= ~flags;
			} else {
				current = flags;
			}
			self->kitty_stack.last() = current;
			return 1;
		}
		if (lead == "?") {
			self->reply(QByteArrayLiteral("\x1b[?")
				           + QByteArray::number(self->kitty_flags())
				           + QByteArrayLiteral("u"));
			return 1;
		}
	}

	if (command == 'm') {
		/* xterm modifyOtherKeys: CSI > 4 ; n m sets, CSI ? 4 m queries. */
		if (lead == ">" && argcount > 0 && args[0] == 4) {
			int level = argcount > 1 ? int(args[1]) : 0;
			/*
			 * Level 3 is clamped to 2 rather than accepted (sec 7.1):
			 * XTQMODKEYS reports only the current value, so a client
			 * that asked for 3 and got 2 silently could not tell. It
			 * asks, it is told 2, it can decide.
			 */
			if (level > 2) level = 2;
			if (level < 0) level = 0;
			self->modify_other_keys = level;
			return 1;
		}
		if (lead == "?" && argcount > 0 && args[0] == 4) {
			self->reply(QByteArrayLiteral("\x1b[>4;")
				           + QByteArray::number(self->modify_other_keys)
				           + QByteArrayLiteral("m"));
			return 1;
		}
	}

	return 0;
}


/*
 * Kitty's graphics protocol arrives as APC. libvterm passes these through
 * untouched, which is all this needs -- unlike the private CSIs in sec
 * 7.1, no patch was required here.
 */
int on_apc(VTermStringFragment frag, void *user) {
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);

	if (frag.initial) self->apc_buffer.clear();
	self->apc_buffer.append(frag.str, int(frag.len));
	if (!frag.final) return 1;

	if (!self->apc_buffer.startsWith('G')) return 0;

	self->apply_graphics(bssh_parse_kitty(self->apc_buffer, self->graphics,
		                                     self->cell_width, self->cell_height));
	return 1;
}

/* iTerm2 inline images: OSC 1337. */
int on_osc(int command, VTermStringFragment frag, void *user) {
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);
	if (command != 1337) return 0;

	if (frag.initial) self->osc_buffer.clear();
	self->osc_buffer.append(frag.str, int(frag.len));
	if (!frag.final) return 1;

	self->apply_graphics(bssh_parse_iterm2(self->osc_buffer,
		                                      self->cell_width, self->cell_height));
	return 1;
}

/* Sixel: DCS <params> q <data> ST. */
int on_dcs(const char *command, size_t commandlen, VTermStringFragment frag, void *user) {
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);

	if (commandlen < 1 || command[commandlen - 1] != 'q') return 0;

	/*
	 * Reassembled with its introducer, because libsixel needs it: given
	 * only the body it reports success and returns a 1x1 image, which is
	 * the kind of failure that reaches a screenshot rather than a log.
	 */
	if (frag.initial) {
		self->dcs_buffer = QByteArrayLiteral("\x1bP") + QByteArray(command, int(commandlen));
	}
	self->dcs_buffer.append(frag.str, int(frag.len));
	if (!frag.final) return 1;

	self->apply_graphics(bssh_parse_sixel(self->dcs_buffer,
		                                     self->cell_width, self->cell_height));
	self->dcs_buffer.clear();
	return 1;
}

/*
 * A line has scrolled off the top. libvterm hands over the cells rather
 * than the text, so colours and attributes survive being scrolled back
 * to -- which is the whole reason the scrollback stores cells.
 */
int on_sb_pushline(int cols, const VTermScreenCell *cells, void *user) {
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);

	QList<bssh_cell_t> line;
	line.reserve(cols);
	for (int col = 0; col < cols; ++col) {
		line.append(self->convert_cell(cells[col]));
	}

	self->scrollback.push(line);
	emit self->owner->scrollback_changed();
	return 1;
}

/*
 * libvterm wants a line back, because the screen is scrolling down again
 * -- leaving the alternate screen, or a resize pulling content back onto
 * the grid. Returning 0 tells it there is nothing, which it handles.
 */
int on_sb_popline(int cols, VTermScreenCell *cells, void *user) {
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);

	QList<bssh_cell_t> line;
	if (!self->scrollback.pop(line)) return 0;

	for (int col = 0; col < cols; ++col) {
		const bssh_cell_t source = col < line.size() ? line.at(col) : bssh_cell_t();

		/*
		 * Only what libvterm needs to lay the line out again: the full
		 * attribute round-trip would mean rebuilding VTermColor by hand,
		 * and a wrong colour handed back to the parser is worse than a
		 * default one. The cells the VIEW draws come from our own copy,
		 * which kept everything.
		 */
		cells[col] = VTermScreenCell();
		cells[col].chars[0] = source.ch;
		cells[col].width = source.width ? source.width : 1;
	}

	emit self->owner->scrollback_changed();
	return 1;
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

const VTermStateFallbacks unrecognised_fallbacks = {
	.control = nullptr,
	.csi     = on_unrecognised_csi,
	.osc     = on_osc,
	.dcs     = on_dcs,
	.apc     = on_apc,
	.pm      = nullptr,
	.sos     = nullptr
};

const VTermScreenCallbacks screen_callbacks = {
	.damage      = on_damage,
	.moverect    = on_move_rect,
	.movecursor  = on_move_cursor,
	.settermprop = on_set_term_prop,
	.bell        = on_bell,
	.resize      = nullptr, /* our own resize() drives the mirror */
	.sb_pushline = on_sb_pushline,
	.sb_popline  = on_sb_popline,
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

bssh_cell_t bssh_emulator_private::convert_cell(const VTermScreenCell &source) const {
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
	return cell;
}

int bssh_emulator_private::copy_cell(int row, int col) {
	VTermPos pos;
	pos.row = row;
	pos.col = col;

	VTermScreenCell source;
	if (!vterm_screen_get_cell(vts, pos, &source)) return 1;

	const bssh_cell_t cell = convert_cell(source);
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

	/*
	 * The alternate screen has to be asked for: libvterm does not
	 * allocate its buffer unless told, and without it DECSET 1049 does
	 * nothing at all. That is not merely a scrollback nicety -- it means
	 * vim and less would redraw over the primary buffer and destroy the
	 * shell output underneath them, with nothing to restore on exit.
	 * Found by a scrollback test noticing an editor's redraws in history.
	 */
	vterm_screen_enable_altscreen(m_private->vts, 1);
	vterm_screen_reset(m_private->vts, 1);

	vterm_screen_set_unrecognised_fallbacks(m_private->vts, &unrecognised_fallbacks, m_private);
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

	/*
	 * An image placed during that write leaves the cursor sitting on top
	 * of it. Move it down past the image the only way libvterm accepts --
	 * by parsing a cursor-down -- and do it here, after the write, rather
	 * than re-entering the parser from inside its own callback.
	 */
	if (m_private->pending_cursor_advance > 0) {
		const QByteArray advance = QByteArrayLiteral("\x1b[")
		                            + QByteArray::number(m_private->pending_cursor_advance)
		                            + QByteArrayLiteral("B");
		m_private->pending_cursor_advance = 0;
		vterm_input_write(m_private->vt, advance.constData(), advance.size());
	}
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
	m_private->images.trim_to(rows);

	emit damaged();
}

const bssh_screen &bssh_emulator::screen() const {
	return m_private->screen;
}

void bssh_emulator_private::reply(const QByteArray &bytes) {
	pending_output.append(bytes);
	emit owner->output_ready();
}

/*
 * Encodes one key under whichever modern protocol is active, or returns
 * false to let libvterm's legacy encoder handle it.
 *
 * Only the cases legacy cannot express are taken: a plain character with
 * no modifiers still sends itself, because a protocol that re-encodes
 * ordinary typing would break every program that has not opted in. That
 * is the whole design of kitty's "disambiguate" tier.
 */
bool bssh_emulator_private::encode_modern_key(char32_t ch, quint8 modifiers) {
	const bool kitty = kitty_flags() != 0;
	if (!kitty && modify_other_keys < 2) return false;

	/* Nothing to disambiguate without ctrl or alt. */
	const bool interesting = modifiers & (bssh_emulator::MOD_CTRL | bssh_emulator::MOD_ALT);
	if (!interesting) return false;

	/* Both protocols number modifiers the same way: 1 + the bitmask. */
	const int encoded_mod = 1 + int(modifiers);

	if (kitty) {
		reply(QByteArrayLiteral("\x1b[")
		       + QByteArray::number(uint(ch))
		       + QByteArrayLiteral(";")
		       + QByteArray::number(encoded_mod)
		       + QByteArrayLiteral("u"));
		return true;
	}

	reply(QByteArrayLiteral("\x1b[27;")
	       + QByteArray::number(encoded_mod)
	       + QByteArrayLiteral(";")
	       + QByteArray::number(uint(ch))
	       + QByteArrayLiteral("~"));
	return true;
}

void bssh_emulator_private::apply_graphics(const bssh_graphics_result_t &result) {
	switch (result.action) {
	case bssh_graphics_result_t::PLACE: {
		/*
		 * Placed at the cursor, which is where every one of the three
		 * protocols puts an image, and the cursor then advances past it
		 * the way a wide glyph advances past its cells.
		 */
		bssh_image_placement_t placement = result.placement;
		placement.row = screen.cursor_row();
		placement.col = screen.cursor_col();
		images.place(placement);
		pending_cursor_advance = qMax(pending_cursor_advance, placement.rows);

		emit owner->images_changed();
		emit owner->damaged();
		break;
	}
	case bssh_graphics_result_t::DELETE_IMAGES:
		if (result.id > 0) {
			images.remove(result.id);
		} else {
			images.clear();
		}
		emit owner->images_changed();
		emit owner->damaged();
		break;
	case bssh_graphics_result_t::CONTINUE:
	case bssh_graphics_result_t::NOTHING:
		break;
	}
}

const bssh_scrollback &bssh_emulator::scrollback() const {
	return m_private->scrollback;
}

void bssh_emulator::set_scrollback_lines(int lines) {
	m_private->scrollback.set_max_lines(lines);
	emit scrollback_changed();
}

const bssh_image_store &bssh_emulator::images() const {
	return m_private->images;
}

void bssh_emulator::set_cell_pixels(int width, int height) {
	m_private->cell_width = qMax(1, width);
	m_private->cell_height = qMax(1, height);
}

bool bssh_emulator::mouse_reporting() const {
	return m_private->mouse_reporting;
}

void bssh_emulator::send_mouse_move(int row, int col, quint8 modifiers) {
	vterm_mouse_move(m_private->vt, row, col, static_cast<VTermModifier>(modifiers));
}

void bssh_emulator::send_mouse_button(int button, bool pressed, quint8 modifiers) {
	vterm_mouse_button(m_private->vt, button, pressed, static_cast<VTermModifier>(modifiers));
}

int bssh_emulator::kitty_keyboard_flags() const {
	return m_private->kitty_flags();
}

int bssh_emulator::modify_other_keys_level() const {
	return m_private->modify_other_keys;
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

	/*
	 * A bare Escape is THE ambiguous key: legacy cannot distinguish it
	 * from the start of any escape sequence, which is why terminal apps
	 * carry an ESC timeout at all. Under kitty's protocol it becomes
	 * unambiguous, and the timeout stops being needed.
	 */
	if (key == KEY_ESCAPE && m_private->kitty_flags() != 0) {
		m_private->reply(QByteArrayLiteral("\x1b[27")
		                  + (modifiers ? QByteArrayLiteral(";") + QByteArray::number(1 + int(modifiers))
		                               : QByteArray())
		                  + QByteArrayLiteral("u"));
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
	if (m_private->encode_modern_key(ch, modifiers)) return;

	/*
	 * Legacy Ctrl+letter is the classic control byte, and this is ours to
	 * decide rather than libvterm's to impose: libvterm emits CSI u
	 * unconditionally for the ambiguous ones (Ctrl+I, Ctrl+M, Ctrl+[),
	 * which is right for an editor that always speaks the modern
	 * protocol and wrong for a terminal talking to whatever is out there.
	 * A remote that has not enabled a keyboard protocol must get exactly
	 * what a VT100 would have sent, or it sees a sequence it cannot
	 * parse.
	 */
	if ((modifiers & MOD_CTRL) && !(modifiers & MOD_ALT) && ch < 0x80) {
		const char32_t upper = (ch >= U'a' && ch <= U'z') ? ch - 32 : ch;
		if (upper >= U'@' && upper <= U'_') {
			const char control = char(upper & 0x1f);
			m_private->reply(QByteArray(1, control));
			return;
		}
	}

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
