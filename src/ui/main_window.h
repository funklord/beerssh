#ifndef BSSH_MAIN_WINDOW_H
#define BSSH_MAIN_WINDOW_H

#include <QWidget>

class QLabel;
class bssh_terminal_view;

/*
 * bssh_main_window -- the top-level window.
 *
 * A plain QWidget, deliberately, not a QMainWindow: the menubar/toolbar/
 * statusbar/dock scaffolding QMainWindow brings is desktop furniture that
 * has to be fought on a phone, and this window has to be the same widget
 * on both (project.md sec 1). The sibling fuzzypickles GUI made the same
 * call for the same reason.
 *
 * At M0 it holds one terminal view and a status line. The tab bar and the
 * pane tree arrive at M7/M8, at which point the decision about how many
 * panes to show belongs to bssh_layout_manager and not to this class
 * (sec 8.4) -- this window will ask, not decide.
 */
class bssh_main_window : public QWidget {
	Q_OBJECT

public:
	explicit bssh_main_window(QWidget *parent = nullptr);

private slots:
	void on_grid_resized(int cols, int rows);

private:
	void update_status();

	bssh_terminal_view *m_terminal_view;
	QLabel *m_status_label;
	int m_cols;
	int m_rows;
};

#endif /* BSSH_MAIN_WINDOW_H */
