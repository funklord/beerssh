#ifndef BSSH_TERMINALS_PANEL_H
#define BSSH_TERMINALS_PANEL_H

#include <QList>
#include <QPointer>
#include <QString>
#include <QWidget>

#include "model/multiplexer.h"

class QListWidget;
class bssh_session;

/*
 * One terminal the Terminals sidebar lists: in this window, in another
 * window of this process, or in another BeerSSH -- every desktop window
 * is its own process (sec 8.4), reached by its handoff socket.
 */
struct bssh_terminal_entry_t {
	/*
	 * In the order listed. The screen and tmux sessions running where the
	 * focused terminal is come last, under the terminals (the holder,
	 * 2026-11-21: one list, "screens below and then tmuxes").
	 */
	enum place_t { HERE, OTHER_WINDOW, OTHER_PROCESS, SCREEN, TMUX };
	place_t place = HERE;
	QString title;
	/* HERE and OTHER_WINDOW: the session itself. */
	QPointer<bssh_session> session;
	/* OTHER_PROCESS: where to ask, and what for. */
	QString path;
	QString id;
	/* SCREEN and TMUX: the one to switch the focused terminal to. */
	bssh_mux_session_t mux;
	/* The one this window has focused, or the one it is attached to. */
	bool current = false;
};

/*
 * bssh_terminals_panel -- a list of every terminal this user has open, to
 * open one here from (the holder, 2026-11-20: "list terminals, then open
 * the terminal from a sidebar"; closing a window still closes its
 * terminals, so this is how one is handed off before that), and under
 * them the screen and tmux sessions of the terminal it is over. A panel
 * over that terminal since 2026-11-23, no longer a sidebar: the holder
 * found two lists, "one is global for the process, while the other
 * loads in a terminal", and chose the one in the terminal.
 *
 * Only the list: what an entry is and what opening it does are the
 * window's, so the panel can be reshaped -- the holder expects to, as a
 * workflow settles -- without touching how a terminal moves.
 */
class bssh_terminals_panel : public QWidget {
	Q_OBJECT

public:
	explicit bssh_terminals_panel(QWidget *parent = nullptr);

	void set_entries(const QList<bssh_terminal_entry_t> &entries);
	const QList<bssh_terminal_entry_t> &entries() const { return m_entries; }

	/* The list, for a test to find a row by. */
	QListWidget *list() const { return m_list; }

	/* Scrolled to the screen and tmux rows, where there are any. */
	void show_multiplexers();

	QSize sizeHint() const override;

signals:
	/* An entry chosen: clicked, or Return on it. */
	void picked(const bssh_terminal_entry_t &entry);

	/* Hidden: by its x, by focus leaving its terminal, or by the window. */
	void closed();

protected:
	void hideEvent(QHideEvent *event) override;

private:
	QListWidget *m_list;
	QList<bssh_terminal_entry_t> m_entries;
	/* One pick at a time: a click can also be an activation. */
	bool m_pick_queued = false;
};

#endif
