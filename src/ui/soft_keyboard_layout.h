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

struct bssh_soft_key_group_t {
	QString id;

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
	static bssh_soft_keyboard_catalog &instance();

	bssh_soft_keyboard_catalog();

	const QList<bssh_soft_key_group_t> &all() const { return m_groups; }

	/*
	 * The group after this one, wrapping. The side button cycles rather
	 * than opening a chooser: with three groups the furthest is two taps
	 * away, and a chooser would be a menu over a keyboard that exists
	 * because the screen is already too small.
	 */
	int next_index(int index) const;

	int index_of(const QString &id) const;

private:
	QList<bssh_soft_key_group_t> m_groups;
};

#endif /* BSSH_SOFT_KEYBOARD_LAYOUT_H */
