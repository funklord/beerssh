#ifndef BSSH_INPUT_MODE_H
#define BSSH_INPUT_MODE_H

#include <QMetaType>

/*
 * What a touch body-drag does (project.md sec 8.7's input selector). One
 * mode for the whole window, chosen at the always-visible selector and
 * read by every terminal view -- a phone has no Shift to hold, so the mode
 * is the visible, switchable override that Shift is on a desktop.
 *
 * Three of these route a gesture WITHIN a terminal (scroll its scrollback,
 * highlight, forward to the remote). MOVE acts on the ARRANGEMENT instead:
 * it reorders the panes in the portrait column. It waited on the scrollable
 * column that now exists (sec 8.7), and the selector offers it only in the
 * column arrangement, where there is a column to reorder. Reordering the
 * column is what replaces the touch tab-reorder the portrait column
 * dissolved, so a phone can put its terminals back in the order it wants.
 *
 * SCROLL is the default, and that is the decision rather than the order:
 * scrollback is the commonest non-typing gesture and selection is
 * occasional, so the constant action is the one that costs no mode change.
 */
enum bssh_input_mode_t {
	BSSH_INPUT_SCROLL,   /* body drag moves this terminal's scrollback */
	BSSH_INPUT_SELECT,   /* body drag highlights */
	BSSH_INPUT_CLICK,    /* the gesture is forwarded to the remote */
	BSSH_INPUT_MOVE      /* body drag reorders the panes in the column */
};

/*
 * A mode that acts on the arrangement rather than within a terminal, so a
 * body drag reorders the column instead of scrolling one terminal. The
 * selector offers these only in a column, and a view routes their gesture
 * to the window rather than handling it itself.
 */
inline bool is_column_mode(bssh_input_mode_t mode) {
	return mode == BSSH_INPUT_MOVE;
}

/*
 * So the mode can ride the mode_changed signal and be read back by a
 * QSignalSpy -- an enum in a signal is not a metatype until it is said to
 * be one.
 */
Q_DECLARE_METATYPE(bssh_input_mode_t)

#endif /* BSSH_INPUT_MODE_H */
