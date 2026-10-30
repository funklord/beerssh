#ifndef BSSH_MULTIPLEXER_DRAWER_H
#define BSSH_MULTIPLEXER_DRAWER_H

#include <QList>
#include <QWidget>

#include "model/multiplexer.h"

class QFrame;
class QListWidget;

/*
 * The tmux and screen sessions a shell found running when it connected
 * (sec 10.2a), offered from the terminal's edge rather than in a dialog:
 * the holder's "an on-screen side drawer ... if there are screens, and it
 * could expand/close into the terminal until a choice is made or it is
 * closed".
 *
 * Closed, it is a button in the pane's corner naming how many there are
 * (bssh_pane_corner, sec 8.7a); open, a panel over the terminal listing
 * them, the one this tab shows marked. A click switches to one and closes
 * the drawer, so another can be chosen after; x closes it.
 * Typing goes on reaching the terminal throughout. Placed by the terminal
 * view it is a child of (bssh_terminal_view::place_multiplexer_drawer).
 */
class bssh_multiplexer_drawer : public QWidget {
	Q_OBJECT

public:
	explicit bssh_multiplexer_drawer(QWidget *parent = nullptr);

	void set_sessions(const QList<bssh_mux_session_t> &found);

	bool is_open() const { return m_open; }
	void set_open(bool open);

	/* What the corner's button says: "tmux . 2", "screen . 1", "tmux/screen . 3". */
	QString handle_text() const;

	/*
	 * The same, short enough for the corner, whose width is the mode
	 * labels': "tmux 2", "scr 17", "mux 3". The tooltip says the rest.
	 */
	QString button_text() const;

	QSize sizeHint() const override;

signals:
	void session_chosen(const bssh_mux_session_t &chosen);

	/* Opened or closed: the view places it again. */
	void open_changed(bool open);

private:
	QList<bssh_mux_session_t> m_found;
	bool m_open = false;
	QFrame *m_panel;
	QListWidget *m_list;
};

#endif /* BSSH_MULTIPLEXER_DRAWER_H */
