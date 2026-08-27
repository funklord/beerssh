#include "term/emulator.h"

#include <QHash>
#include <QTimer>

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

	/*
	 * Kept so DECRQM can answer for them (sec 7.1). A bool is enough to
	 * decide whether to send mouse reports; it is not enough to say
	 * WHICH mode is set, and a terminal that answers 1000 while 1003 is
	 * what is running has told the remote something worse than nothing.
	 */
	int mouse_mode;
	bool alt_screen;

	/*
	 * What TERM the app told the remote, so XTGETTCAP's TN answers with
	 * the same string rather than a plausible guess. A terminal that
	 * names itself one thing in the environment and another when asked
	 * is worse than one that will not say.
	 */
	QString term_name;
	bool sync_output;
	bool sync_damage_pending;
	QTimer *sync_timeout;

	bssh_image_store images;
	bssh_scrollback scrollback;
	bssh_graphics_transmission_t graphics;   /* chunked kitty transmissions */
	QByteArray apc_buffer;        /* an APC split across reads */
	QByteArray osc_buffer;

	/* Accumulates a selection payload across fragments (OSC 52). */
	QByteArray selection_buffer;
	bool selection_oversized = false;

	/*
	 * libvterm decodes the base64 into a buffer WE own and it does not
	 * grow: it is a cap as much as a buffer. 64 KB is more than any
	 * plausible yank and far less than a problem, and the payload is
	 * attacker-controlled -- without a bound a host could spend the
	 * terminal's memory for the price of writing to a socket.
	 */
	char selection_storage[64 * 1024];
	QByteArray dcs_buffer;

	/* OSC 8 hyperlinks (sec 7.1). */
	struct link_range_t {
		int row;
		int from;
		int to;
		quint16 id;
	};

	QHash<quint16, QString> links;
	QList<link_range_t> pending_links;
	quint16 next_link_id;
	quint16 open_link_id;
	int link_start_row;
	int link_start_col;
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

	/*
	 * The single point every repaint request goes through, which is what
	 * makes the synchronised-output hold possible at all: a hold spread
	 * across the six places that emit damage would leak one of them.
	 */
	void notify_damaged();
	void begin_sync();
	void end_sync();

	void begin_link(const QString &uri);
	void end_link();
	void apply_pending_links();
	VTermPos cursor_position() const;

	bssh_emulator_private(int cols, int rows, bssh_emulator *owner)
	    : vt(nullptr), vts(nullptr), screen(cols, rows), owner(owner),
	      modify_other_keys(0), mouse_reporting(false), mouse_mode(0), alt_screen(false),
	    term_name(QStringLiteral("xterm-256color")), sync_output(false),
	      sync_damage_pending(false), sync_timeout(nullptr),
	      next_link_id(0), open_link_id(0), link_start_row(0), link_start_col(0), cell_width(8), cell_height(16),
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
 * h,U+00E9,l,l,o. Since SSH reads split wherever the network happened to
 * put a packet boundary, holding the tail back until it is complete is
 * the caller's job, and this is that caller.
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

/*
 * The far half of a double-width glyph, built from the half that carries
 * the character (sec 9.3).
 *
 * There is one of these rather than two because the screen and the
 * scrollback have to agree: the renderer skips a continuation on
 * `width == 0` and draws everything else, so a path that spells the
 * continuation differently gets its cell drawn -- and libvterm's own
 * marker for one is chars[0] == (uint32_t)-1, which is not a character at
 * all. The colours come from the base cell because a background spans the
 * whole glyph, and because libvterm leaves the marker cell's pen at
 * whatever happened to be there before.
 */
bssh_cell_t continuation_of(const bssh_cell_t &base) {
	bssh_cell_t out;
	out.ch = U'\0';
	out.width = 0;
	out.attrs = base.attrs;
	out.fg = base.fg;
	out.bg = base.bg;
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
	self->notify_damaged();
	return 1;
}

int on_move_cursor(VTermPos pos, VTermPos oldpos, int visible, void *user) {
	Q_UNUSED(oldpos);
	Q_UNUSED(visible);
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);
	self->screen.set_cursor(pos.row, pos.col);
	self->notify_damaged();
	return 1;
}

int on_move_rect(VTermRect dest, VTermRect src, void *user) {
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);
	/*
	 * A scroll, done by moving the mirror's own rows.
	 *
	 * Re-copying the destination out of libvterm was correct and cost a
	 * full-screen read per scrolled line: a 200x50 grid scrolling one
	 * line re-read ten thousand cells and converted every one. Plain
	 * scrolling output ran at 0.2 MB/s while a full-screen TUI repaint --
	 * more bytes, no scrolling -- managed 21 MB/s, and the gap scaled
	 * with the grid rather than with the output.
	 *
	 * The mirror already holds these cells; moving them is a memmove per
	 * row. libvterm damages whatever the scroll exposed, so the newly
	 * blank line arrives through on_damage as usual.
	 */
	self->screen.move_rect(dest.start_row, dest.start_col,
		                      src.start_row, src.start_col,
		                      qMin(dest.end_row - dest.start_row, src.end_row - src.start_row),
		                      qMin(dest.end_col - dest.start_col, src.end_col - src.start_col));

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

	self->notify_damaged();
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
		self->mouse_mode = int(value->number);
		return 1;
	case VTERM_PROP_ALTSCREEN:
		/*
		 * libvterm owns the screens themselves; returning 0 here would
		 * leave it to its own default handling, and this returns 1
		 * having only recorded which one is up. That answers DECRQM for
		 * 1049, and it tells the view whose history the scrollback
		 * currently holds (sec 6.4).
		 */
		if (value->boolean != self->alt_screen) {
			self->alt_screen = value->boolean;
			emit self->owner->alternate_screen_changed(self->alt_screen);
		}
		return 1;
	case VTERM_PROP_CURSORVISIBLE:
		self->screen.set_cursor_visible(value->boolean);
		self->notify_damaged();
		return 1;
	case VTERM_PROP_RESET:
		/*
		 * The terminal was reset -- RIS, DECSTR, or our own
		 * reset_modes(). libvterm has already put back everything it
		 * models and told us about the two we mirror; what is left is
		 * the modes it does not model at all, which are ours and which
		 * nothing else can clear (vterm-patch/0005).
		 *
		 * Both specifications say a reset clears them, and the cost of
		 * not doing it is not cosmetic: kitty flags left set encode
		 * Ctrl+C as CSI 99;5u, so no SIGINT is delivered and the escape
		 * lands on the shell's command line instead.
		 */
		self->modify_other_keys = 0;
		self->kitty_stack.clear();
		self->kitty_stack.append(0);
		self->end_sync();
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
/*
 * DECRQM's answer for one mode (sec 7.1), for the modes THIS terminal
 * implements on top of libvterm.
 *
 * Almost nothing belongs here. libvterm answers DECRQM itself for every
 * mode it models -- the cursor, autowrap, each mouse mode, bracketed
 * paste -- and it knows their state better than this side does; a second
 * opinion here would be the "two places computing one thing" that this
 * project keeps having to unpick. What it cannot answer for is a mode it
 * does not model, which is exactly the set an application implements on
 * top of it. Those queries only arrive here at all because of patch 0004.
 *
 * Returns -1 for "not ours", which leaves libvterm to send its own "not
 * recognised".
 */
int decrqm_state(const bssh_emulator_private *self, int mode) {
	switch (mode) {
	case 2026:
		/*
		 * Synchronised output, which this terminal implements and
		 * libvterm does not model. It is the mode clients actually
		 * probe, and answering it is the whole point: "2" means
		 * understood and currently off, which is how a client learns
		 * the feature is there before using it. Without patch 0004 the
		 * answer was "0", telling every client the terminal lacked a
		 * feature it has.
		 */
		return self->sync_output ? 1 : 2;
	default:
		return -1;
	}
}

int on_unrecognised_csi(const char *leader, const long args[], int argcount,
	                       const char *intermed, char command, void *user) {
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);

	const QByteArray lead = QByteArray(leader ? leader : "");
	const QByteArray inter = QByteArray(intermed ? intermed : "");

	/*
	 * DECRQM, for a DEC private mode libvterm does not model. Answered
	 * with DECRPM, which echoes the mode back so a client with several
	 * queries in flight can tell the answers apart.
	 *
	 * Declining leaves libvterm to answer, which for every mode it models
	 * it already does correctly.
	 */
	if (command == 'p' && inter == "$" && lead == "?") {
		const int mode = argcount > 0 ? int(args[0]) : 0;
		const int state = decrqm_state(self, mode);
		if (state < 0) return 0;

		self->reply(QByteArrayLiteral("\x1b[?")
			           + QByteArray::number(mode)
			           + QByteArrayLiteral(";")
			           + QByteArray::number(state)
			           + QByteArrayLiteral("$y"));
		return 1;
	}

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

	if (command == 'h' || command == 'l') {
		/* DEC 2026: synchronised output (sec 7.1). */
		if (lead == "?" && argcount > 0 && args[0] == 2026) {
			if (command == 'h') {
				self->begin_sync();
			} else {
				self->end_sync();
			}
			return 1;
		}
		return 0;
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

/*
 * OSC 8 hyperlinks: "OSC 8 ; params ; URI ST" opens one, "OSC 8 ; ; ST"
 * closes it, and everything printed between belongs to it.
 */
int on_osc_link(const QByteArray &payload, bssh_emulator_private *self) {
	/* payload is "params;URI", params ignored (id= is the only defined one). */
	const int semicolon = payload.indexOf(';');
	if (semicolon < 0) return 0;

	const QString uri = QString::fromUtf8(payload.mid(semicolon + 1));
	if (uri.isEmpty()) {
		self->end_link();
	} else {
		self->begin_link(uri);
	}
	return 1;
}

/* iTerm2 inline images: OSC 1337. */
int on_osc(int command, VTermStringFragment frag, void *user) {
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);
	if (command != 1337 && command != 8) return 0;

	if (frag.initial) self->osc_buffer.clear();
	self->osc_buffer.append(frag.str, int(frag.len));
	if (!frag.final) return 1;

	/*
	 * OSC 8 and OSC 1337 arrive through the same callback and are
	 * completely different things. Without this split the hyperlink
	 * payload was handed to the image parser, which rejected it in
	 * silence -- a whole feature doing nothing, with no error anywhere.
	 */
	if (command == 8) return on_osc_link(self->osc_buffer, self);

	self->apply_graphics(bssh_parse_iterm2(self->osc_buffer,
		                                      self->cell_width, self->cell_height));
	return 1;
}

/*
 * OSC 52 -- the remote asking to put something on the user's clipboard, or
 * to read what is already there (project.md sec 7.1).
 *
 * Through libvterm's selection callbacks rather than the OSC fallback,
 * because libvterm handles OSC 52 itself and returns before the fallback
 * is consulted: the first version of this parsed the sequence by hand and
 * was never called at all. The library also does the base64 and the
 * chunking, which is work not worth repeating badly.
 *
 * THE READ IS NOT IMPLEMENTED AND WILL NOT BE. A query asks the terminal
 * to send the local clipboard back down the wire, which turns every
 * connection into an exfiltration channel for whatever the user last
 * copied -- a password, a token, someone else's message. No setting makes
 * that acceptable, because the risk is not the user's to accept on behalf
 * of something they copied ten minutes ago. The query is answered with
 * silence, which is what a terminal without the feature does.
 *
 * The write IS offered, per host and off by default (sec 7.1: "off by
 * default, if offered at all"). Yanking in vim over ssh is why the
 * sequence exists; the hazard is that a compromised host can replace what
 * the user thinks they copied and wait for them to paste it. So it is a
 * per-host trust decision -- which is what a preset is -- and the window
 * says so on the status line rather than letting it happen silently.
 */
const int BSSH_MAX_SELECTION_BYTES = 64 * 1024;

int on_selection_set(VTermSelectionMask mask, VTermStringFragment frag, void *user) {
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);
	Q_UNUSED(mask);

	if (frag.initial) {
		self->selection_buffer.clear();
		self->selection_oversized = false;
	}

	/*
	 * libvterm decodes into a fixed buffer and hands it over in pieces as
	 * that buffer fills, so a payload larger than the cap arrives as
	 * several fragments rather than being refused. Accumulating them
	 * without a bound of our own would put the cap back where it started:
	 * the remote decides how much memory this costs.
	 *
	 * An oversized payload is dropped WHOLE rather than truncated. Half a
	 * clipboard is not a smaller version of the thing the user asked for
	 * -- it is a different string, and pasting it somewhere is worse than
	 * pasting nothing.
	 */
	if (self->selection_buffer.size() + int(frag.len) > BSSH_MAX_SELECTION_BYTES) {
		self->selection_oversized = true;
		self->selection_buffer.clear();
	}

	if (!self->selection_oversized) {
		self->selection_buffer.append(frag.str, int(frag.len));
	}

	if (!frag.final) return 1;

	/*
	 * An empty final fragment is how libvterm reports input it could not
	 * decode. Nothing to put on a clipboard, and saying "the host set your
	 * clipboard to nothing" would be a notification about a malformed
	 * escape sequence.
	 */
	if (!self->selection_oversized && !self->selection_buffer.isEmpty()) {
		emit self->owner->clipboard_write_requested(
			      QString::fromUtf8(self->selection_buffer));
	}

	self->selection_buffer.clear();
	self->selection_oversized = false;
	return 1;
}

int on_selection_query(VTermSelectionMask mask, void *user) {
	Q_UNUSED(mask);
	Q_UNUSED(user);

	/* Answered with silence. See above -- this one is not a setting. */
	return 1;
}

const VTermSelectionCallbacks BSSH_SELECTION_CALLBACKS = {
	on_selection_set,
	on_selection_query
};

/*
 * What this terminal will tell a remote about itself (sec 7.1).
 *
 * Only things that are true of it whatever it is running on, and
 * everything else is answered as unknown rather than guessed. A
 * capability database is a promise: a client that is told "bce" will use
 * it, and a terminal that answered by pattern-matching what terminals
 * usually support has broken the program that believed it.
 */
QByteArray terminfo_capability(const QByteArray &name, const bssh_emulator_private *self) {
	if (name == "TN") return self->term_name.toUtf8();
	if (name == "Co" || name == "colors") return QByteArrayLiteral("256");
	if (name == "RGB") return QByteArrayLiteral("8/8/8");
	return QByteArray();
}

QByteArray to_hex(const QByteArray &raw) {
	return raw.toHex().toUpper();
}

/*
 * XTGETTCAP: DCS + q <hex name> ; <hex name> ... ST, answered per name
 * with DCS 1 + r <hex>=<hex> ST, or DCS 0 + r <hex> ST for one this
 * terminal will not vouch for.
 *
 * Answered one reply per name rather than one reply for all of them: a
 * single reply carrying a mix of known and unknown names has to pick one
 * status byte for the lot, and clients disagree about what that means.
 */
int on_xtgettcap(const QByteArray &payload, bssh_emulator_private *self) {
	const QList<QByteArray> names = payload.split(';');

	for (const QByteArray &hex : names) {
		const QByteArray name = QByteArray::fromHex(hex);
		const QByteArray value = terminfo_capability(name, self);

		if (value.isEmpty()) {
			self->reply(QByteArrayLiteral("\x1bP0+r") + to_hex(name)
				           + QByteArrayLiteral("\x1b\\"));
			continue;
		}

		self->reply(QByteArrayLiteral("\x1bP1+r") + to_hex(name)
			           + QByteArrayLiteral("=") + to_hex(value)
			           + QByteArrayLiteral("\x1b\\"));
	}
	return 1;
}

/* Sixel: DCS <params> q <data> ST. XTGETTCAP: DCS + q <names> ST. */
int on_dcs(const char *command, size_t commandlen, VTermStringFragment frag, void *user) {
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);

	if (commandlen < 1 || command[commandlen - 1] != 'q') return 0;

	/*
	 * Both of these end in 'q', which is why the introducer has to be
	 * looked at rather than just its last byte: an XTGETTCAP query was
	 * reaching the sixel parser, where it decoded to nothing and was
	 * answered with silence.
	 */
	if (commandlen >= 2 && command[commandlen - 2] == '+') {
		if (frag.initial) self->dcs_buffer.clear();
		self->dcs_buffer.append(frag.str, int(frag.len));
		if (!frag.final) return 1;

		const int handled = on_xtgettcap(self->dcs_buffer, self);
		self->dcs_buffer.clear();
		return handled;
	}

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
 * Whether a cell would draw as nothing whatsoever, so that a run of them
 * at the end of a line can be left out of the scrollback entirely.
 *
 * Deliberately strict. A space carrying a background colour is a visible
 * thing -- half the TUIs in existence draw a bar that way -- and so is one
 * under a hyperlink, so only a cell that is blank in EVERY respect
 * qualifies. Note what the width test excludes: the continuation half of a
 * double-width glyph, which the mirror records with width 0 rather than
 * with libvterm's chars[0] == (uint32_t)-1 marker, so a trim stops there
 * and cannot cut a wide character in half.
 */
bool is_invisible_cell(const bssh_cell_t &cell) {
	if (cell.ch != 0 && cell.ch != U' ') return false;
	if (cell.width != 1) return false;
	if (cell.attrs != bssh_cell_t::NONE) return false;
	if (cell.fg.kind != bssh_color_t::DEFAULT) return false;
	if (cell.bg.kind != bssh_color_t::DEFAULT) return false;
	if (cell.link != 0) return false;
	return true;
}

/*
 * A line has scrolled off the top. libvterm hands over the cells rather
 * than the text, so colours and attributes survive being scrolled back
 * to -- which is the whole reason the scrollback stores cells.
 *
 * Trailing blanks are dropped rather than stored. libvterm hands over a
 * full grid row every time, so a 58-character line of build output on a
 * 200-column grid arrived as 142 cells of nothing -- and it is the GRID
 * that decides that cost, not the output: the same log scrolling past a
 * wider window converted and kept proportionally more padding. Measured
 * on 20,000 lines at 200x50, 2.82 of 3.99 million converted cells were
 * trailing blanks, and at the default 5000-line cap the buffer they
 * filled was tens of megabytes, which on a phone is the whole reason
 * sec 7 caps it at all.
 *
 * Every reader already treats a short line as blank past its end (the
 * renderer, the selection, and the popline handler below all bound the
 * column against size()), so a trimmed line reads back identically.
 */
int on_sb_pushline(int cols, const VTermScreenCell *cells, void *user) {
	Q_UNUSED(cells);
	bssh_emulator_private *self = static_cast<bssh_emulator_private *>(user);

	/*
	 * Taken from the MIRROR's top row, not converted from libvterm's
	 * cells a second time.
	 *
	 * This callback arrives before moverect -- checked, not assumed -- so
	 * the row leaving the top of the screen is still logical row 0 here,
	 * and the mirror already holds it converted. Converting it again cost
	 * a second pass over every cell of every scrolled line: with damage
	 * conversion it was the larger half of what plain scrolling spends on
	 * a wide grid.
	 *
	 * It also removes a duplicate. The wide-glyph continuation used to be
	 * synthesised here "exactly as the screen synthesises it" -- two
	 * places computing one thing, which is precisely how an undrawable
	 * character reached the scrollback while the live screen showed the
	 * glyph correctly. There is one synthesis now, and the scrollback
	 * gets whatever the screen decided.
	 *
	 * Reading the mirror means reading something only as current as the
	 * last damage flush, and damage is merged per ROW: a row stays
	 * pending until a different row is touched. So a row written and
	 * then scrolled away before anything else happened is still pending
	 * HERE, and the mirror has never been told about it -- measured, and
	 * the scrollback line came back EMPTY, not merely stale.
	 *
	 * This callback runs from the state layer's premove, which is before
	 * the screen layer's scrollrect and therefore before its flush. So
	 * ask for the flush now. The buffer has not moved yet, so the
	 * pending rectangle still describes what is in it -- the same
	 * ordering argument as vterm-patch/0003, one callback earlier.
	 */
	vterm_screen_flush_damage(self->vts);

	const int width_limit = qMin(cols, self->screen.cols());

	int width = width_limit;
	while (width > 0 && is_invisible_cell(self->screen.cell(0, width - 1))) --width;

	QList<bssh_cell_t> line;
	line.reserve(width);
	for (int col = 0; col < width; ++col) {
		line.append(self->screen.cell(0, col));
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
	/*
	 * A continuation marker is not a character and must never reach a
	 * cell: it is not valid Unicode, so the renderer asks the font
	 * catalog which family covers it and draws the answer. Callers skip
	 * these -- the two that build cells both advance past the far half of
	 * a wide glyph -- but converting one has to fail safe rather than
	 * produce U+FFFFFFFF, because what it costs is a visible box.
	 */
	if (source.chars[0] == static_cast<uint32_t>(-1)) return continuation_of(cell);

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
		screen.set_cell(row, col + 1, continuation_of(cell));
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
	 * Damage merged per row rather than per cell (sec 9.5).
	 *
	 * libvterm's default is VTERM_DAMAGE_CELL, which reports every printed
	 * cell as its own rect: 20000 lines of ordinary build output through a
	 * 200x50 grid arrived as 1.19 MILLION damage callbacks, each of them
	 * paying for a copy_rect and a damaged() signal to report one
	 * character. Merging to the row coalesces a printed line into one
	 * rect, which is the granularity the mirror copies at anyway.
	 *
	 * The cost is that damage is no longer delivered synchronously -- a
	 * row is held until something flushes it -- so feed() must flush
	 * (below) or the last line of a write sits invisible until the next
	 * one. libvterm flushes on its own before moverect and erase, which is
	 * what keeps the mirror's rows correct when a scroll moves them.
	 */
	vterm_screen_set_damage_merge(m_private->vts, VTERM_DAMAGE_ROW);

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

	/*
	 * OSC 52 goes through here rather than through the OSC fallback:
	 * libvterm handles that command itself and returns before any
	 * fallback is consulted (sec 7.1).
	 */
	vterm_state_set_selection_callbacks(vterm_obtain_state(m_private->vt),
	                                     &BSSH_SELECTION_CALLBACKS, m_private,
	                                     m_private->selection_storage,
	                                     sizeof(m_private->selection_storage));
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
	 * Row-merged damage is owed, not delivered (see the constructor), and
	 * vterm_input_write does not settle it. Flushing here is what makes a
	 * write's last row reach the mirror, and it has to happen BEFORE the
	 * links below: copy_rect rewrites a damaged cell from libvterm's
	 * screen, which knows nothing about links, so a row flushed after the
	 * marking would quietly erase it.
	 */
	vterm_screen_flush_damage(m_private->vts);

	/*
	 * An image placed during that write leaves the cursor sitting on top
	 * of it. Move it down past the image the only way libvterm accepts --
	 * by parsing a cursor-down -- and do it here, after the write, rather
	 * than re-entering the parser from inside its own callback.
	 */
	m_private->apply_pending_links();

	if (m_private->pending_cursor_advance > 0) {
		const QByteArray advance = QByteArrayLiteral("\x1b[")
		                            + QByteArray::number(m_private->pending_cursor_advance)
		                            + QByteArrayLiteral("B");
		m_private->pending_cursor_advance = 0;
		vterm_input_write(m_private->vt, advance.constData(), advance.size());
		vterm_screen_flush_damage(m_private->vts);
	}
}

void bssh_emulator::resize(int cols, int rows) {
	cols = qMax(1, cols);
	rows = qMax(1, rows);

	/*
	 * Settle what the old geometry owes before the coordinates it was
	 * measured in stop being true (see the constructor).
	 */
	vterm_screen_flush_damage(m_private->vts);

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

	m_private->notify_damaged();
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

void bssh_emulator_private::notify_damaged() {
	if (sync_output) {
		/*
		 * Held, not dropped: the repaint happens when the remote says
		 * the frame is complete. One flag rather than a queue -- what is
		 * owed is "repaint", and owing it twice is the same as owing it
		 * once.
		 */
		sync_damage_pending = true;
		return;
	}

	emit owner->damaged();
}

void bssh_emulator_private::begin_sync() {
	sync_output = true;

	/*
	 * A program that sets 2026 and then dies, or a connection that drops
	 * mid-frame, would otherwise freeze the display permanently. Every
	 * terminal that implements this carries a timeout for exactly that;
	 * ours releases the hold after 150ms, which is far longer than a
	 * frame and far shorter than a user noticing a stuck screen.
	 */
	if (!sync_timeout) {
		sync_timeout = new QTimer(owner);
		sync_timeout->setSingleShot(true);
		sync_timeout->setInterval(150);
		QObject::connect(sync_timeout, &QTimer::timeout, owner, [this] { end_sync(); });
	}
	sync_timeout->start();
}

void bssh_emulator_private::end_sync() {
	if (!sync_output) return;

	sync_output = false;
	if (sync_timeout) sync_timeout->stop();

	if (sync_damage_pending) {
		sync_damage_pending = false;
		emit owner->damaged();
	}
}

void bssh_emulator_private::apply_pending_links() {
	if (pending_links.isEmpty()) return;

	for (const link_range_t &range : pending_links) {
		for (int col = range.from; col <= range.to; ++col) {
			bssh_cell_t cell = screen.cell(range.row, col);
			cell.link = range.id;
			screen.set_cell(range.row, col, cell);
		}
	}

	pending_links.clear();
	notify_damaged();
}

VTermPos bssh_emulator_private::cursor_position() const {
	VTermPos pos;
	pos.row = 0;
	pos.col = 0;

	if (vt) vterm_state_get_cursorpos(vterm_obtain_state(vt), &pos);
	return pos;
}

void bssh_emulator_private::begin_link(const QString &uri) {
	/*
	 * Ids wrap rather than grow without bound: a session left running on
	 * output full of links would otherwise allocate for ever. Wrapping
	 * can only ever mislabel a link that has already scrolled out of the
	 * bounded scrollback.
	 */
	if (++next_link_id == 0) next_link_id = 1;

	open_link_id = next_link_id;
	links.insert(open_link_id, uri);

	/*
	 * libvterm's own cursor, not the mirror's. Damage and cursor
	 * callbacks are flushed in batches, so at the moment an OSC is
	 * parsed the mirror can still be showing where the cursor WAS -- and
	 * a link anchored there covers the wrong cells, or none at all,
	 * which is exactly how the first version of this failed.
	 */
	VTermPos pos = cursor_position();
	link_start_row = pos.row;
	link_start_col = pos.col;
}

void bssh_emulator_private::end_link() {
	if (open_link_id == 0) return;

	/*
	 * The link covers everything printed since it opened, which may have
	 * wrapped across rows -- so this walks cells rather than a single
	 * span, and stops at the current cursor.
	 */
	const VTermPos pos = cursor_position();
	const int end_row = pos.row;
	const int end_col = pos.col;

	for (int row = link_start_row; row <= end_row && row < screen.rows(); ++row) {
		const int from = row == link_start_row ? link_start_col : 0;
		const int to = row == end_row ? end_col - 1 : screen.cols() - 1;

		/*
		 * Queued, not applied. libvterm flushes damage in a batch at the
		 * end of the write, and copy_rect rewrites every damaged cell
		 * from libvterm's own screen -- which knows nothing about links,
		 * so marking cells now means marking cells that are about to be
		 * overwritten. That is exactly how the first version of this
		 * silently did nothing.
		 *
		 * Applying afterwards is also self-correcting: if the remote
		 * later redraws those cells, the damage copy clears the link,
		 * which is right, because the text it decorated is gone.
		 */
		link_range_t range;
		range.row = row;
		range.from = from;
		range.to = qMin(to, screen.cols() - 1);
		range.id = open_link_id;
		if (range.to >= range.from) pending_links.append(range);
	}

	open_link_id = 0;
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
		notify_damaged();
		break;
	}
	case bssh_graphics_result_t::DELETE_IMAGES:
		if (result.id > 0) {
			images.remove(result.id);
		} else {
			images.clear();
		}
		emit owner->images_changed();
		notify_damaged();
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

void bssh_emulator::set_term_name(const QString &term) {
	if (!term.isEmpty()) m_private->term_name = term;
}

void bssh_emulator::set_cell_pixels(int width, int height) {
	m_private->cell_width = qMax(1, width);
	m_private->cell_height = qMax(1, height);
}

QString bssh_emulator::link_uri(quint16 id) const {
	return m_private->links.value(id);
}

bool bssh_emulator::synchronised_output() const {
	return m_private->sync_output;
}

bool bssh_emulator::mouse_reporting() const {
	return m_private->mouse_reporting;
}

void bssh_emulator::reset_modes() {
	/*
	 * A soft reset, not a hard one: the hard branch homes the cursor and
	 * erases the screen, which is exactly the history a reconnect exists
	 * to keep (sec 5.3).
	 *
	 * Everything else is libvterm's now. This used to feed CSI ? 1049 l
	 * first, because vterm_state_reset() cleared mode.alt_screen by
	 * assignment and the screen layer was never told -- and it used to
	 * clear our own mouse, kitty and modifyOtherKeys shadows by hand,
	 * because nothing announced the reset. vterm-patch/0005 makes the
	 * reset announce what it clears, so both halves arrive through
	 * on_set_term_prop and the remote's own RIS or DECSTR is handled by
	 * the same code as this call. One path, and the workaround is gone.
	 */
	vterm_screen_reset(m_private->vts, 0);

	emit damaged();
}

bool bssh_emulator::alternate_screen() const {
	return m_private->alt_screen;
}

void bssh_emulator::send_mouse_move(int row, int col, quint8 modifiers) {
	vterm_mouse_move(m_private->vt, row, col, static_cast<VTermModifier>(modifiers));
}

void bssh_emulator::send_mouse_button(int button, bool pressed, quint8 modifiers) {
	vterm_mouse_button(m_private->vt, button, pressed, static_cast<VTermModifier>(modifiers));
}

void bssh_emulator::set_focused(bool focused) {
	VTermState *state = vterm_obtain_state(m_private->vt);
	if (!state) return;

	if (focused) {
		vterm_state_focus_in(state);
	} else {
		vterm_state_focus_out(state);
	}
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
