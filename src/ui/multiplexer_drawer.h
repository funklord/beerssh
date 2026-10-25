#ifndef BSSH_MULTIPLEXER_DRAWER_H
#define BSSH_MULTIPLEXER_DRAWER_H

#include <QList>
#include <QWidget>

#include "model/multiplexer.h"

class QAbstractButton;
class QFrame;
class QListWidget;
class QPushButton;

/*
 * The tmux and screen sessions a shell found running when it connected
 * (sec 10.2a), offered from the terminal's edge rather than in a dialog:
 * the holder's "an on-screen side drawer ... if there are screens, and it
 * could expand/close into the terminal until a choice is made or it is
 * closed".
 *
 * Closed, it is a handle on the right edge naming how many there are;
 * open, a panel over the terminal with a tick per session. Typing goes on
 * reaching the terminal either way. It stays until a choice is made --
 * Open, one ticked attaching here and more a tab each -- or it is
 * dismissed, which is "Shell only". Placed by the terminal view it is a
 * child of (bssh_terminal_view::place_multiplexer_drawer).
 */
class bssh_multiplexer_drawer : public QWidget {
	Q_OBJECT

public:
	explicit bssh_multiplexer_drawer(QWidget *parent = nullptr);

	void set_sessions(const QList<bssh_mux_session_t> &found);

	bool is_open() const { return m_open; }
	void set_open(bool open);

	/* The ticked ones, in the order listed. */
	QList<bssh_mux_session_t> chosen() const;

	/* What the handle says: "tmux . 2", "screen . 1", "tmux/screen . 3". */
	QString handle_text() const;

	QSize sizeHint() const override;

signals:
	void sessions_chosen(const QList<bssh_mux_session_t> &chosen);
	void declined();

	/* Opened or closed: the view places it again. */
	void open_changed(bool open);

private:
	void sync_open_button();

	QList<bssh_mux_session_t> m_found;
	bool m_open = false;
	QAbstractButton *m_handle;
	QFrame *m_panel;
	QListWidget *m_list;
	QPushButton *m_choose;
};

#endif /* BSSH_MULTIPLEXER_DRAWER_H */
