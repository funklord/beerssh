#ifndef BSSH_SOFT_KEYBOARD_LAYOUT_H
#define BSSH_SOFT_KEYBOARD_LAYOUT_H

#include <QList>
#include <QString>

#include "ui/key_row_layout.h"

/*
 * bssh_soft_keyboard_layout -- what the built-in keyboard holds
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
 * The KEYS are bssh_key_t, deliberately, and not a second struct with the
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
const int BSSH_KEYBOARD_COLUMNS = 20;

/*
 * The widest any group is, and the only thing that needs to know both
 * numbers: clearing the stretch on columns a narrower group does not use
 * has to walk past its own width. Asserted against the catalog, so a
 * group wider than this fails a test rather than being drawn clipped.
 */
const int BSSH_KEYBOARD_MAX_COLUMNS = 60;

struct bssh_soft_key_group_t {
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
	QList<QList<bssh_key_t> > rows;

	bssh_soft_key_group_t() : columns(BSSH_KEYBOARD_COLUMNS) {}
};

/*
 * The groups, in the order the side button walks them.
 *
 * Curated and compiled in, the same shape as the key row's layouts, the
 * fonts (sec 9.2) and the colour schemes (sec 10.2): addressed by a
 * stable id, identical on both platforms, so what a user learns on one
 * device is what they find on another.
 */
class bssh_soft_keyboard_catalog {
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
		STYLE_FULL
	};

	/*
	 * Every lookup takes the style, and NONE of them defaults it.
	 *
	 * They defaulted to STYLE_COMPACT for about an hour, and in that hour
	 * two callers in bssh_soft_keyboard silently asked the wrong
	 * catalogue: group_id() named a compact group while the full keyboard
	 * was on screen, and set_group() clamped an index against the compact
	 * list's three groups when the full one has two -- so set_group(2)
	 * was accepted, rebuild() found it out of range, and the keyboard
	 * came up EMPTY.
	 *
	 * A default argument is a way of not asking the question. Requiring
	 * it makes the compiler find every consumer, which is the only form
	 * of "every consumer has it" that cannot rot -- sec 10 records the
	 * same lesson for preset fields, where counting them by hand was what
	 * failed.
	 */

	static bssh_soft_keyboard_catalog &instance();

	bssh_soft_keyboard_catalog();

	const QList<bssh_soft_key_group_t> &all(style_t style) const {
		return style == STYLE_FULL ? m_full : m_groups;
	}

	/*
	 * The group after this one, wrapping. The side button cycles rather
	 * than opening a chooser: with three groups the furthest is two taps
	 * away, and a chooser would be a menu over a keyboard that exists
	 * because the screen is already too small.
	 */
	int next_index(int index, style_t style) const;

	int index_of(const QString &id, style_t style) const;

private:
	QList<bssh_soft_key_group_t> m_groups;
	QList<bssh_soft_key_group_t> m_full;
};

/*
 * The narrowest window the full-size keyboard is drawn in, in dp.
 *
 * sec 8.2's MEDIUM boundary, reused rather than a second number invented
 * beside it: a tree with two width thresholds has two things to keep in
 * step, and this one is asking the same question sec 8.2 asks -- is there
 * room to do the roomy thing.
 *
 * It is a compromise and the measurement says so. 15u of keyboard plus the
 * page button shares the width, so a 1u key gets width/16.5:
 *
 *     317dp  Fold cover screen      19dp   no
 *     360dp  ordinary phone         22dp   no
 *     600dp  this threshold         36dp   usable, under sec 6.2's 48dp
 *     680dp  Fold inner screen      41dp   usable
 *     840dp  sec 8.2's EXPANDED     51dp   meets 48dp
 *
 * Nothing below 600 is defensible. Above it, the honest 48dp answer is
 * about 770dp -- which would exclude a folding phone's inner screen, the
 * device this keyboard was asked for. So the threshold buys the Fold at
 * the price of keys a little under target, and the alternative was to
 * refuse the full keyboard on the hardware it exists for.
 */
const int BSSH_FULL_KEYBOARD_MIN_DP = 600;

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
bssh_soft_keyboard_catalog::style_t bssh_keyboard_style_for(
        bssh_soft_keyboard_catalog::style_t wanted, int width_dp);

#endif /* BSSH_SOFT_KEYBOARD_LAYOUT_H */
