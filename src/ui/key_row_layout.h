#ifndef BSSH_KEY_ROW_LAYOUT_H
#define BSSH_KEY_ROW_LAYOUT_H

#include <QList>
#include <QString>

/*
 * bssh_key_row_layout -- what the on-screen key row holds (project.md
 * sec 6.2, sec 10.2).
 *
 * sec 6.2 says the row's layout is a preset-owned setting "so different
 * server types can get different key rows", and the starter presets are
 * named for exactly those types: a network switch, an OpenWrt box, a
 * tmux-first workflow. Until now the row was one hardcoded strip and the
 * preset field selecting between layouts had nothing to select from.
 *
 * The keys that matter genuinely differ by what is on the other end. A
 * Cisco-style console is driven by `?` and Tab far more than by arrows; a
 * tmux user needs one chord more than anything else, and it is a chord no
 * soft keyboard can type; a BusyBox box has no readline, so history keys
 * are wasted space on it.
 *
 * Same shape as the font and colour-scheme sets (sec 9.2, sec 10.2):
 * curated, compiled in, addressed by a stable id, identical on both
 * platforms so a preset means the same thing wherever it is opened.
 */

struct bssh_key_t {
	enum kind_t {
		/* A named key: Esc, Tab, an arrow. Goes through the router so a
		 * sticky modifier applies to it. */
		KEY,

		/* Ctrl or Alt: sticky, applying to the NEXT key pressed. */
		MODIFIER,

		/* A character the soft keyboard buries two layers deep. */
		TEXT,

		/*
		 * A modifier and a key in one press -- tmux's Ctrl-B, screen's
		 * Ctrl-A. This exists because those are the chords a touch user
		 * cannot type at all: two sticky presses to reach one prefix,
		 * dozens of times an hour, is the difference between usable and
		 * not.
		 */
		CHORD,

		/*
		 * Occupies its columns and draws nothing.
		 *
		 * The full-size keyboard (sec 6.2) is laid out in a real
		 * keyboard's proportions, and a real keyboard has gaps: between
		 * the function banks, between the main block and the navigation
		 * cluster. It also has keys a terminal cannot send -- Super,
		 * Menu, PrtSc, ScrLk, Pause, NumLock have no encoding in
		 * libvterm and no meaning down the wire. Both are the same
		 * problem: the geometry has to hold the space or every key after
		 * it shifts, and drawing a button that does nothing is the
		 * "absent, not faked" rule sec 11 applies to capabilities.
		 */
		SPACER,

		/*
		 * Fn: swaps the row for F1..F12 and back (sec 6.2, which lists
		 * it among the keys the row must carry).
		 *
		 * A toggle rather than a sticky modifier, because there is
		 * nothing on this row for it to modify -- the digits it would
		 * combine with live two layers down a soft keyboard, which is
		 * the whole reason the row exists. The emulator has understood
		 * KEY_FUNCTION_BASE + n since M3 and the router has translated
		 * physical F1..F35 for as long; a touch user simply had no way
		 * to reach any of them, so htop, mc and vim's F-keys were off
		 * the table on a phone.
		 */
		FUNCTION_TOGGLE
	};

	QString label;
	kind_t kind;

	/* A bssh_emulator key code, a modifier mask, or a codepoint,
	 * depending on kind. CHORD uses both value and modifier. */
	int value;

	/*
	 * What this key sends WITH SHIFT, when that is not simply the capital
	 * of what it sends without.
	 *
	 * A keycap carries two legends and Shift picks the other one. Modelling
	 * it as QChar::toUpper() instead is right for letters and silently
	 * wrong for everything else -- toUpper('1') is '1', so on a keyboard
	 * with a real number row that left 21 characters unreachable:
	 * ~ ! @ # $ % ^ & * ( ) _ + { } | : " < > ? -- which in a terminal is
	 * the pipe, the home directory and most of a shell's punctuation.
	 * Reported from a device with "the full keyb isn't full", which was
	 * exact.
	 *
	 * Zero means "no second legend": a letter takes toUpper, and a key
	 * with nothing different to send is left alone.
	 */
	int shifted_value;
	quint8 modifier;

	/*
	 * How many key widths this one occupies, for the built-in keyboard's
	 * grid. A row ignores it: its keys are laid out end to end and scroll
	 * (sec 6.2), so there are no columns for a key to span.
	 *
	 * Here rather than in a keyboard-only struct because a space bar is
	 * still a key and not a different kind of thing. Splitting the type
	 * to carry one integer is how two vocabularies for one concept start.
	 */
	int span;

	bssh_key_t() : kind(KEY), value(0), shifted_value(0), modifier(0), span(1) {}
};

struct bssh_key_row_layout_t {
	QString id;
	QString display_name;

	/* Shown under the picker: which end of a connection this suits. */
	QString description;

	QList<bssh_key_t> keys;
};

/*
 * F1..F12, which is what the Fn key swaps the row for.
 *
 * Prefixed and declared here because it crosses a translation unit: the
 * layouts are built in one file and the row that shows them lives in
 * another, and both need the same twelve keys. Twelve is what a keyboard
 * has and what the programs asking for them expect -- htop labels F1
 * through F10, mc the same, and vim's help is F1.
 */
QList<bssh_key_t> bssh_function_keys();

class bssh_key_row_catalog {
public:
	static bssh_key_row_catalog &instance();

	bssh_key_row_catalog();

	const QList<bssh_key_row_layout_t> &all() const { return m_layouts; }
	const bssh_key_row_layout_t *by_id(const QString &id) const;

	/* Empty means the default; an unknown id passes through unchanged, so
	 * a preset from a newer build still shows what it asked for. */
	QString effective_id(const QString &stored) const;
	QString default_id() const;

private:
	QList<bssh_key_row_layout_t> m_layouts;
};

#endif /* BSSH_KEY_ROW_LAYOUT_H */
