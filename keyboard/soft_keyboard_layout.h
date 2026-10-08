#ifndef SK_KEYBOARD_LAYOUT_H
#define SK_KEYBOARD_LAYOUT_H

#include <QList>
#include <QMap>
#include <QString>

#include "keyboard/key_row_layout.h"

/*
 * soft_keyboard_layout -- what the built-in keyboard holds
 * (project.md sec 6.2).
 *
 * sec 6.2 asked whether to ship a keyboard of our own and answered
 * "probably no for v1, rely on the key row plus external keyboards". The
 * holder has since answered it the other way, and the reason is the one
 * the section could not have known when it was written: on a phone with
 * no physical keyboard the key row is a strip of modifiers above a system
 * keyboard that is not there, which is a terminal you cannot type into at
 * all.
 *
 * The KEYS are sk_key_t, deliberately, and not a second struct with the
 * same fields. Both on-screen surfaces are answering one question -- what
 * does this button do when a thumb lands on it -- and a terminal that
 * grew two vocabularies for that would get two answers to "what does Ctrl
 * do", which is exactly the drift bssh_input_router exists to prevent one
 * layer down (sec 6.3).
 *
 * What is added here is the two things a row does not have: keys arranged
 * in ROWS rather than one strip, and a way to reach the other groups.
 */

/*
 * One page of the keyboard: letters, digits and symbols, or the terminal
 * keys a shell needs.
 *
 * Groups rather than one enormous grid, because a phone has room for
 * about forty keys at a size a thumb can hit and a terminal wants well
 * over a hundred. Which forty is the only real decision, and it is made
 * per group: the letters group is what you type prose and commands with,
 * and the others are one tap away.
 */
/*
 * Every row of every group is exactly this wide, in half-key columns.
 *
 * Half-columns rather than whole keys, because a QWERTY row of ten and a
 * bottom row of six cannot share a column grid otherwise -- and the
 * alternative, a separate layout per row, gives up the one thing a grid
 * is for: a key in the same place from one row to the next. Ten letters
 * at span 2 fill it, and so do six wider keys at 3, 3, 3, 5, 3, 3.
 */
const int SK_KEYBOARD_COLUMNS = 22;

/*
 * The widest any group is, and the only thing that needs to know both
 * numbers: clearing the stretch on columns a narrower group does not use
 * has to walk past its own width. Asserted against the catalog, so a
 * group wider than this fails a test rather than being drawn clipped.
 */
const int SK_KEYBOARD_MAX_COLUMNS = 60;

struct sk_group_t {
	QString id;

	/*
	 * How wide every row of THIS group is, in grid columns.
	 *
	 * Per group rather than one constant for the tree, because the two
	 * keyboards do not share a unit. The compact one counts half-keys and
	 * is 20 wide; the full-size one counts QUARTER-keys and is 60, which
	 * is 15u -- the width of a real keyboard's main block -- because that
	 * is the granularity 1.25u, 1.75u, 2.25u and 6.25u need to come out
	 * exact. A single grid could not express both without one of them
	 * rounding, and a keyboard whose Shift is a quarter-key wrong is one
	 * a touch-typist's hand notices immediately.
	 */
	int columns;

	/*
	 * What the side button reads while THIS group is the next one. Short,
	 * because it is drawn in a column one key wide -- "123", not
	 * "Numbers and symbols".
	 */
	QString button_label;

	/* Shown wherever the group is named in full, such as a test failure. */
	QString display_name;

	/*
	 * Always the same NUMBER of rows in every group, and the catalog is
	 * checked for it.
	 *
	 * Not tidiness: the keyboard is docked under the terminal, so a group
	 * one row taller than another shrinks the terminal when the side
	 * button is pressed. That is a resize on the wire (sec 6.3) and a
	 * reflow of whatever the remote is drawing, on every tap of a button
	 * whose whole job is to show different keys.
	 */
	QList<QList<sk_key_t> > rows;

	/*
	 * Whether rows[0] is the digit row every compact page carries (sec
	 * 6.2). The split keyboard leaves it out: asked for in portrait, and
	 * in landscape five rows of 48dp keys did not fit the Fold's cover
	 * screen -- the status line and the mode buttons went off the bottom.
	 */
	bool digit_row_first = false;

	sk_group_t() : columns(SK_KEYBOARD_COLUMNS) {}
};

/*
 * The groups, in the order the side button walks them.
 *
 * Curated and compiled in, the same shape as the key row's layouts, the
 * fonts (sec 9.2) and the colour schemes (sec 10.2): addressed by a
 * stable id, identical on both platforms, so what a user learns on one
 * device is what they find on another.
 */
/*
 * A keyboard layout: what the alphanumeric slots carry (project.md sec 6.2).
 *
 * The geometry names XKB positions -- AD01, AE12, TLDE -- and a layout
 * says what those positions send. So a second layout is a second table
 * and not a second keyboard, which is the whole reason the two were
 * split apart.
 *
 * It governs the LETTER BLOCK only. The compact keyboard's symbol pages
 * are a palette rather than a layout: they name ASCII characters
 * directly and go on sending them whatever the letters become, which is
 * what keeps a terminal typeable under a layout that has no live tilde.
 *
 * The soft keyboard only, as well. sec 6.1 settled that without naming
 * it: a physical key arrives as a QKeyEvent with the OS layout already
 * applied, so translating again would translate twice.
 */
struct sk_keyboard_layout_t {
	QString id;
	QString display_name;
};

/*
 * Every layout, in the order a chooser should list them. The first is
 * the default, and is US International.
 */
QList<sk_keyboard_layout_t> sk_keyboard_layouts();

/*
 * A user's own layout (sec 6.3), as rows of an XKB slot and its four
 * levels -- plain, Shift, AltGr, Shift+AltGr -- empty where the slot has
 * none.
 *
 * The shipped tables are `const char *` literals with static lifetime;
 * a user's is text that arrives at run time, so it needs owning. This
 * carries the owned copy and `sk_set_custom_layout` builds the C table
 * the existing slot lookup already reads, rather than teaching that
 * lookup about a second kind of layout.
 */
struct sk_custom_slot_t {
	QString slot;
	QString level[4];
};

/*
 * Parse the app.ini form: one entry per slot, four levels separated by
 * `|', `\|' for a literal one -- needed because `|' is a real legend on
 * BKSL. Unknown or malformed rows are dropped rather than refusing the
 * whole layout: one mistyped row should cost that slot, not the other
 * forty-six, and what decides whether the remainder is usable is the
 * contract below rather than the parse.
 */
QList<sk_custom_slot_t> sk_parse_layout_slots(const QMap<QString, QString> &rows);

/*
 * A shipped layout's slots, as the same rows a user would write.
 *
 * The transcription this whole format is for starts somewhere, and
 * starting from a table that already satisfies the contract is the only
 * starting point that does. Empty for an id the catalogue does not hold.
 */
QList<sk_custom_slot_t> sk_shipped_layout_slots(const QString &id);

/*
 * One slot as the text app.ini holds, escaped so it parses back.
 *
 * Beside the parser rather than left to callers, because the round trip
 * is the whole point of the format and a caller that joins the four
 * levels itself will forget the escapes. That is not hypothetical: the
 * first version of this had no writer, the test joined them by hand, and
 * a transcription of US International came back unable to type a
 * backslash -- a level that IS a backslash, written raw, puts one
 * immediately before the separator and the parser reads the pair as an
 * escaped pipe.
 */
QString sk_layout_slot_spec(const sk_custom_slot_t &row);

/*
 * Install those as the reserved `custom' entry, or refuse and say why.
 *
 * sec 6.3 asks that the catalogue's checks "become the contract every
 * layout must satisfy rather than tests about the two boards we happen
 * to have", and the key row showed what that costs if it is left in the
 * suite: a rule living only in a test reports the breach after it has
 * happened, and reports it against the layouts that are correct. So the
 * two checks run HERE -- every printable ASCII reachable, and no level
 * shown that no modifier offers -- and a layout failing either is not
 * installed.
 */
bool sk_set_custom_layout(const QString &display_name,
                             const QList<sk_custom_slot_t> &rows,
                             QString *why);

/*
 * Read the catalog through this layout from now on.
 *
 * Returns true if the id was known. An unknown one leaves the current
 * layout alone and says so, rather than falling back silently to a
 * keyboard whose letters are not the ones the user asked for -- a
 * setting written by a newer version, or by hand, should not quietly
 * become something else.
 *
 * The catalog is rebuilt, so any sk_keyboard on screen has to be
 * rebuilt too; bssh_main_window does that where it applies the setting.
 */
bool sk_set_keyboard_layout(const QString &id);

QString sk_keyboard_layout();

/*
 * The Latin toggle, for a layout whose letter block is not Latin.
 *
 * sk_layout_has_latin() is false for Russian, Greek and Ukrainian and
 * true for every Latin layout, so it is also the test for whether the
 * toggle should be offered at all -- a Swedish keyboard has no second
 * script to reach and must not be given a button that does nothing.
 *
 * Latin mode is ON by default and reset to ON whenever the layout
 * changes: a terminal is written in Latin, so that is what a non-Latin
 * layout opens in, and its own script is one tap away.
 */
bool sk_layout_has_latin();
bool sk_latin_mode();
void sk_set_latin_mode(bool on);

class sk_catalog {
public:
	/*
	 * Two keyboards, not two settings.
	 *
	 * COMPACT is the thumb keyboard: three groups of four rows, letters
	 * where a phone user expects them. FULL is a real keyboard's shape
	 * and key count, for a tablet, a foldable's inner screen, or anyone
	 * who would rather have Tab, Ctrl and the function row where their
	 * hands already know they are.
	 */
	enum style_t {
		STYLE_COMPACT,
		STYLE_FULL,

		/*
		 * The compact keyboard cut down the middle, its halves beside
		 * the terminal rather than under it: for a screen wider than it
		 * is tall, where a keyboard under the terminal leaves it one
		 * row (sec 6.2). The SAME groups as compact, divided at draw
		 * time, so the layout, the pages, AltGr and everything else a
		 * compact keyboard has come with it by construction rather than
		 * as a second copy to keep in step.
		 */
		STYLE_SPLIT
	};

	/*
	 * Every lookup takes the style, and NONE of them defaults it.
	 *
	 * They defaulted to STYLE_COMPACT for about an hour, and in that hour
	 * two callers in sk_keyboard silently asked the wrong
	 * catalogue: group_id() named a compact group while the full keyboard
	 * was on screen, and set_group() clamped an index against the compact
	 * list's three groups when the full one has two -- so set_group(2)
	 * was accepted, rebuild() found it out of range, and the keyboard
	 * came up EMPTY.
	 *
	 * A default argument is a way of not asking the question. Requiring
	 * it makes the compiler find every consumer, which is the only form
	 * of "every consumer has it" that cannot rot -- sec 14's M10 records the
	 * same lesson for preset fields, where counting them by hand was what
	 * failed.
	 */

	static sk_catalog &instance();

	sk_catalog();

	const QList<sk_group_t> &all(style_t style) const {
		return style == STYLE_FULL ? m_full : m_groups;
	}

	/*
	 * Which half of a split keyboard a key goes in, from where it sits
	 * in its row: left when its middle is left of the group's middle.
	 * By the middle rather than the start, so a key straddling the cut
	 * goes where most of it is.
	 */
	static bool in_left_half(int column, int span, int columns) {
		return 2 * column + span < columns;
	}

	/*
	 * The group after this one, wrapping. The side button cycles rather
	 * than opening a chooser: with three groups the furthest is two taps
	 * away, and a chooser would be a menu over a keyboard that exists
	 * because the screen is already too small.
	 */
	int next_index(int index, style_t style) const;

	int index_of(const QString &id, style_t style) const;

	/*
	 * Build the groups again, because the layout changed.
	 *
	 * The groups hold characters rather than a reference to a layout, so
	 * a new layout means new groups. In place, so pointers into the
	 * catalog survive.
	 */
	void rebuild();

private:
	QList<sk_group_t> m_groups;
	QList<sk_group_t> m_full;
};

/*
 * The narrowest window the full-size keyboard is drawn in, in dp.
 *
 * sec 8.1's MEDIUM boundary, reused rather than a second number invented
 * beside it: a tree with two width thresholds has two things to keep in
 * step, and this one is asking the same question sec 8.1 asks -- is there
 * room to do the roomy thing.
 *
 * It is a compromise and the measurement says so. 15u of keyboard plus the
 * page button shares the width, so a 1u key gets width/16.5:
 *
 *     317dp  Fold cover screen      19dp   no
 *     360dp  ordinary phone         22dp   no
 *     600dp  this threshold         36dp   usable, under sec 6.2's 48dp
 *     680dp  Fold inner screen      41dp   usable
 *     840dp  sec 8.1's EXPANDED     51dp   meets 48dp
 *
 * Nothing below 600 is defensible. Above it, the honest 48dp answer is
 * about 770dp -- which would exclude a folding phone's inner screen, the
 * device this keyboard was asked for. So the threshold buys the Fold at
 * the price of keys a little under target, and the alternative was to
 * refuse the full keyboard on the hardware it exists for.
 */
const int SK_FULL_KEYBOARD_MIN_DP = 600;

/*
 * Which keyboard to DRAW, given which one was chosen and how much room
 * there is.
 *
 * A pure function of the two, so it can be asked without a widget: the
 * fallback is a display decision, and the stored setting is never
 * rewritten by it. A phone that is folded shows the compact keyboard and
 * unfolds to the full one, the way sec 8.2 collapses splits to tabs and
 * restores them, with the intent remembered rather than overwritten.
 */
/*
 * What a key sends, or shows, given Shift.
 *
 * One function for both, because the button's LABEL and the character it
 * SENDS are the same question asked twice, and a keyboard whose keycap
 * disagrees with its output is worse than one that does not relabel at
 * all -- the user learns the wrong thing and keeps using it.
 *
 * `twin` is the key's second legend, zero where it has none. Unshifted, or
 * with no twin and no capital, a key sends what it shows.
 */
QChar sk_shifted_char(char32_t plain, char32_t twin, bool shifted);

/*
 * The same question over four levels: what does this key send, given
 * Shift and AltGr.
 *
 * Falls back down the levels rather than sending nothing: a key with no
 * level 4 but a level 3 gives its level 3 under Shift+AltGr, and a key
 * with no AltGr legend at all behaves exactly as it did before. That is
 * what keeps AltGr harmless on the 21 keys of a US board that have no
 * third legend.
 */
QChar sk_keyboard_char(const sk_key_t &key, bool shifted, bool altgr);

/*
 * The tallest a window may be and still get the split keyboard on its
 * own, in dp: sec 8.1's MEDIUM boundary again, as SK_FULL_KEYBOARD_MIN_DP
 * is. Below it a keyboard under the terminal takes most of the height --
 * measured on the Fold's cover screen in landscape, where the full board
 * left one usable row -- and above it there is room for both.
 */
const int SK_SPLIT_KEYBOARD_MAX_HEIGHT_DP = 600;

/*
 * Height as well as width, and required: a landscape phone is the case a
 * width-only answer got wrong. SPLIT when asked for, or when the window
 * is wider than tall and short enough that a keyboard underneath would
 * crowd the terminal out -- whichever of compact and full was chosen,
 * since both fail there the same way. The stored choice is never
 * rewritten; rotating back gives back what was chosen.
 */
sk_catalog::style_t sk_keyboard_style_for(
        sk_catalog::style_t wanted, int width_dp, int height_dp);

#endif /* SK_KEYBOARD_LAYOUT_H */
