#ifndef BSSH_MULTIPLEXER_MENU_H
#define BSSH_MULTIPLEXER_MENU_H

#include <QDialog>
#include <QList>

#include "model/multiplexer.h"

class QListWidget;
class QPushButton;

/*
 * Which tmux or screen session to open (sec 10.2a): the sessions a shell
 * found running when it connected, each with a tick. One ticked opens in
 * this tab; more open one tab each on the same host. "Shell only" leaves
 * the shell as it is and does not ask that session again.
 */
class bssh_multiplexer_menu : public QDialog {
	Q_OBJECT

public:
	bssh_multiplexer_menu(const QString &host, const QList<bssh_mux_session_t> &found,
	                      QWidget *parent = nullptr);

	/* The ticked ones, in the order listed. */
	QList<bssh_mux_session_t> chosen() const;

private:
	void sync_open();

	QList<bssh_mux_session_t> m_found;
	QListWidget *m_list;
	QPushButton *m_open;
};

#endif /* BSSH_MULTIPLEXER_MENU_H */
