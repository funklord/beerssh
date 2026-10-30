#ifndef BSSH_INPUT_SELECTOR_H
#define BSSH_INPUT_SELECTOR_H

#include <QList>
#include <QWidget>

#include "ui/input_mode.h"

class QAbstractButton;
class QButtonGroup;

/*
 * bssh_input_selector -- the control that says what a body drag or a click
 * does (project.md sec 8.7), shown in the focused pane's corner (sec 8.7a).
 *
 * Three rows tall. The holder: "we can scroll between the modes easily as
 * that is a very common need, so that part needs to be at least the height
 * of 3 options." Three modes are all shown, in order, the current one
 * marked; with the column's five it is a drum, the current mode in the
 * middle and wrapping, so any of them is two steps away. A wheel notch or
 * a drag of one row steps it; a tap on a row chooses that one.
 *
 * It replaces the Shift override a phone cannot produce, and on the desktop
 * it says what the pointer will do -- Click shows a mark while the program
 * is listening to the mouse.
 *
 * Move and Pan, which act on the column, are offered only when the window
 * is in the column arrangement -- there is no column to reorder otherwise.
 */
class bssh_input_selector : public QWidget {
	Q_OBJECT

public:
	explicit bssh_input_selector(QWidget *parent = nullptr);

	bssh_input_mode_t mode() const { return m_mode; }

	/*
	 * Sets the shown state without emitting, for keeping the control in
	 * step with the window's mode -- every pane's corner shows it, and a
	 * control that only reflects its own clicks silently lies the first
	 * time something else moves the mode.
	 */
	void set_mode(bssh_input_mode_t mode);

	/*
	 * Shows or hides the column-only states (Move, Pan) as the arrangement
	 * gains or loses a column. Losing it while one is chosen would strand
	 * the mode on a state nobody can see, so this falls back to the
	 * platform's default and emits -- the one path where a change of
	 * arrangement, not a gesture, moves the mode.
	 */
	void set_column_available(bool available);
	bool column_available() const { return m_column_available; }

	/* The remote program has asked for the mouse: Click is marked. */
	void set_program_listening(bool listening);
	bool program_listening() const { return m_program_listening; }

	/* The modes on offer, in drum order. */
	QList<bssh_input_mode_t> available() const;

	/* What is shown in the three rows, top to bottom. */
	bssh_input_mode_t shown(int row) const;

	/*
	 * Whether the drum turns: only with more modes than rows. Three are
	 * shown in their order, the current one marked.
	 */
	bool rotates() const;

	/* One row's height; the drum is three of them. */
	int row_height() const;

	/*
	 * Three rows, or one where three do not fit -- a phone held sideways
	 * with its keyboard up leaves a pane shorter than the drum, and a
	 * corner too tall for its pane is hidden, which took the only mode
	 * control a phone has with it. One row shows the current mode; a tap
	 * moves to the next, wrapping, and a swipe or the wheel steps.
	 */
	void set_rows(int rows);
	int rows() const { return m_rows; }

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override { return sizeHint(); }

signals:
	void mode_changed(bssh_input_mode_t mode);

protected:
	void resizeEvent(QResizeEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void changeEvent(QEvent *event) override;

private:
	void choose(bssh_input_mode_t mode);
	void step(int direction);
	void relayout();
	QString label_for(bssh_input_mode_t mode) const;

	QButtonGroup *m_group;
	bssh_input_mode_t m_mode;
	bool m_column_available;
	bool m_program_listening;

	bool m_pressed = false;
	bool m_dragged = false;
	int m_drag_y = 0;
	int m_wheel = 0;
	int m_rows = 3;
};

#endif /* BSSH_INPUT_SELECTOR_H */
