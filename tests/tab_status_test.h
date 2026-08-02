#ifndef BSSH_TAB_STATUS_TEST_H
#define BSSH_TAB_STATUS_TEST_H

#include <QObject>

/*
 * The per-tab status dot (project.md sec 8.3), tested as a rule rather
 * than as pixels: which state wins when a session is both disconnected
 * and has an unheard bell is a decision, and one that would otherwise
 * only ever be checked by squinting at a tab bar.
 */
class tab_status_test : public QObject {
	Q_OBJECT

private slots:
	void reports_the_connection_state();
	void a_bell_is_remembered_only_while_you_are_elsewhere();
	void a_dead_session_outranks_its_own_bell();
};

#endif /* BSSH_TAB_STATUS_TEST_H */
