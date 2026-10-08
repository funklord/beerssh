#ifndef SK_KEY_ROW_LAYOUT_H
#define SK_KEY_ROW_LAYOUT_H

#include <QList>
#include <QString>

/*
 * key_row_layout -- what the on-screen key row holds (project.md
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

struct sk_key_t {
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

	/*
	 * Levels 3 and 4: what this key sends with AltGr, and with
	 * Shift+AltGr. Zero for neither.
	 *
	 * The names are XKB's, because the table they come from is: US
	 * International puts a, a and o on q, w and p at level 3, and a
	 * layout is exactly a choice of what sits at each level. Four levels
	 * is the number every European layout needs, so it is the number to
	 * settle on before layouts exist rather than after -- adding a level
	 * later is a migration of every table written in the meantime.
	 *
	 * AltGr is NOT sent. It picks a character, the way Shift does; by the
	 * time anything reaches the wire its whole effect is in the text.
	 * See SK_MOD_ALTGR.
	 */
	int altgr_value;
	int shift_altgr_value;
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

	sk_key_t()
	    : kind(KEY), value(0), shifted_value(0), altgr_value(0),
	      shift_altgr_value(0), modifier(0), span(1) {}
};

struct sk_key_row_layout_t {
	QString id;
	QString display_name;

	/* Shown under the picker: which end of a connection this suits. */
	QString description;

	QList<sk_key_t> keys;
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
QList<sk_key_t> sk_function_keys();

/*
 * Whether a row can drive a terminal at all: it needs Escape and at least
 * one modifier. Without those it is a strip of keys with nothing to escape
 * out of and nothing to modify the rest with, which is sec 6.2's floor.
 *
 * This lived as an assertion inside a test that walked the curated
 * layouts, and a user-composed row turns that inside out: an invalid row
 * reaching the catalogue would make the CONTRACT test fail rather than
 * being refused at the door, so the failure would land on the five rows
 * that are correct. The rule is here now, the test calls it over the
 * curated set, and set_custom() refuses anything failing it -- sec 6.3's
 * "they become the contract every layout must satisfy rather than tests
 * about the two boards we happen to have".
 */
bool sk_key_row_usable(const QList<sk_key_t> &keys);

/*
 * A row as words, because app.ini is meant to be opened and edited
 * (sec 10.6) and a row stored as numbers is one nobody can fix by hand.
 *
 * Tokens are separated by spaces: `esc tab ctrl alt left down up right`.
 * A reserved word names a key (esc, tab, enter, bksp, ins, del, home,
 * end, pgup, pgdn, up, down, left, right, f1..f12), a modifier (ctrl,
 * alt, shift), the function bank's toggle (fn), or a blank column (gap).
 * `ctrl-b` is a chord, one press sending what two would. Any single
 * character is itself -- `|`, `/`, `~` -- which is why every reserved
 * word is at least two characters and none of them can collide with one.
 *
 * sk_parse_key_row skips a token it does not know rather than refusing
 * the row: a spec typed by hand with one word wrong should lose that key
 * and not the other eleven, and sk_key_row_usable is what decides
 * whether what survived is still a row.
 */
QList<sk_key_t> sk_parse_key_row(const QString &spec);
QString sk_key_row_spec(const QList<sk_key_t> &keys);

class sk_key_row_catalog {
public:
	static sk_key_row_catalog &instance();

	sk_key_row_catalog();

	const QList<sk_key_row_layout_t> &all() const { return m_layouts; }
	const sk_key_row_layout_t *by_id(const QString &id) const;

	/* Empty means the default; an unknown id passes through unchanged, so
	 * a preset from a newer build still shows what it asked for. */
	QString effective_id(const QString &stored) const;
	QString default_id() const;

	/*
	 * Replaces the "custom" entry with the user's own row -- the same
	 * push-in the colour catalogue uses, and for the same reason: this
	 * lives in keyboard/ and the setting lives in model/, so a catalogue
	 * that read the settings store would be a dependency running the
	 * wrong way. Whoever owns the settings calls this.
	 *
	 * The keys only. The id, the display name and the description are
	 * this catalogue's -- a custom row that could rename itself would be
	 * one the picker cannot find.
	 *
	 * Returns false and changes nothing where the row could not drive a
	 * terminal, so a spec that has been edited into uselessness leaves
	 * the last good one in place rather than emptying the strip.
	 */
	bool set_custom(const QList<sk_key_t> &keys);

private:
	QList<sk_key_row_layout_t> m_layouts;
};

#endif /* SK_KEY_ROW_LAYOUT_H */
