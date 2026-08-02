#ifndef BSSH_MAIN_WINDOW_H
#define BSSH_MAIN_WINDOW_H

#include <QString>
#include <QWidget>

#include "input/input_router.h"
#include "ssh/ssh_session.h"
#include "term/emulator.h"

class QLabel;
class bssh_key_row;
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
 * At M1 it holds one terminal view, one session and a status line. The
 * tab bar and the pane tree arrive at M7/M8, at which point sessions move
 * into bssh_session_registry and this window stops owning one -- the
 * pointer below is M1 scaffolding, not the design (sec 3, core principle
 * 2).
 */
class bssh_main_window : public QWidget {
	Q_OBJECT

public:
	/*
	 * target is an optional [user@]host[:port] from the command line.
	 * Empty means no connection: the app comes up showing an empty grid,
	 * which is all it could do before M1 anyway. Host profiles replace
	 * this at M6 (sec 10) -- a command-line target is a bring-up
	 * affordance, not the intended way in.
	 */
	explicit bssh_main_window(const QString &target = QString(), QWidget *parent = nullptr);

private slots:
	void on_grid_resized(int cols, int rows);
	void on_state_changed(bssh_ssh_session::state_t state);
	void on_failed(const QString &reason);
	void on_emulator_output();

private:
	void connect_to(const QString &target);
	void update_status();

	bssh_terminal_view *m_terminal_view;
	QLabel *m_status_label;
	bssh_ssh_session *m_session;
	bssh_emulator *m_emulator;
	bssh_input_router *m_input_router;
	bssh_key_row *m_key_row;
	QString m_password;
	QString m_target;
	QString m_error;
	int m_cols;
	int m_rows;
};

#endif /* BSSH_MAIN_WINDOW_H */
