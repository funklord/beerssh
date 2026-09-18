#ifndef BSSH_INPUT_MODE_H
#define BSSH_INPUT_MODE_H

#include <QMetaType>

/*
 * What a touch body-drag does (project.md sec 8.7's input selector). One
 * mode for the whole window, chosen at the always-visible selector and
 * read by every terminal view -- a phone has no Shift to hold, so the mode
 * is the visible, switchable override that Shift is on a desktop.
 *
 * Three of sec 8.7's five states live here: they route a gesture WITHIN a
 * terminal. The other two -- scroll the column, reorder the column -- act
 * on the arrangement rather than on a terminal, and wait for the scrollable
 * column they need (recorded in sec 8.7).
 *
 * SCROLL is the default, and that is the decision rather than the order:
 * scrollback is the commonest non-typing gesture and selection is
 * occasional, so the constant action is the one that costs no mode change.
 */
enum bssh_input_mode_t {
	BSSH_INPUT_SCROLL,   /* body drag moves this terminal's scrollback */
	BSSH_INPUT_SELECT,   /* body drag highlights */
	BSSH_INPUT_CLICK     /* the gesture is forwarded to the remote */
};

/*
 * So the mode can ride the mode_changed signal and be read back by a
 * QSignalSpy -- an enum in a signal is not a metatype until it is said to
 * be one.
 */
Q_DECLARE_METATYPE(bssh_input_mode_t)

#endif /* BSSH_INPUT_MODE_H */
