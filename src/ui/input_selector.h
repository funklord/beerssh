#ifndef BSSH_INPUT_SELECTOR_H
#define BSSH_INPUT_SELECTOR_H

#include <QWidget>

#include "ui/input_mode.h"

class QButtonGroup;

/*
 * bssh_input_selector -- the always-visible control that says what a touch
 * body-drag does (project.md sec 8.7).
 *
 * One per window and visible at all times, because the thing it replaces --
 * the Shift override that lets a user select or scroll inside a full-screen
 * program -- is unproducible on a phone, and an invisible mode is exactly
 * the defect sec 8.7 exists to answer. So it is a plain segmented control a
 * thumb can always reach, not a menu the user has to remember to open.
 *
 * It always shows the three states that route a gesture within a terminal.
 * The Move state, which reorders the column, is shown only when the window
 * is in the column arrangement -- there is no column to reorder otherwise,
 * so offering it would be a control that does nothing.
 */
class bssh_input_selector : public QWidget {
	Q_OBJECT

public:
	explicit bssh_input_selector(QWidget *parent = nullptr);

	bssh_input_mode_t mode() const { return m_mode; }

	/*
	 * Sets the shown state without emitting, for keeping the control in
	 * step with a mode changed elsewhere -- there is nothing elsewhere
	 * today, but a control that only reflects its own clicks is one that
	 * silently lies the first time something else moves the mode.
	 */
	void set_mode(bssh_input_mode_t mode);

	/*
	 * Shows or hides the column-only states (Move) as the arrangement
	 * gains or loses a column. Hiding the button a user had selected
	 * would strand the mode on a control they can no longer see, so this
	 * falls the mode back to SCROLL and emits -- the one path where a
	 * change of arrangement, not a click, moves the mode.
	 */
	void set_column_available(bool available);

signals:
	void mode_changed(bssh_input_mode_t mode);

private:
	void choose(bssh_input_mode_t mode);

	QButtonGroup *m_group;
	bssh_input_mode_t m_mode;
	bool m_column_available;
};

#endif /* BSSH_INPUT_SELECTOR_H */
