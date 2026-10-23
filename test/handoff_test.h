#ifndef BSSH_HANDOFF_TEST_H
#define BSSH_HANDOFF_TEST_H

#include <QObject>

/*
 * Moving a live session to ANOTHER PROCESS (project.md sec 8.4): this test
 * binary, run a second time with --handoff-take, is the taker.
 */
class handoff_test : public QObject {
	Q_OBJECT

private slots:
	void a_live_session_moves_to_another_process();
	void a_session_nobody_has_is_refused_and_nothing_moves();
	void a_taker_that_vanishes_leaves_the_session_running();
};

/*
 * The taker's side, for --handoff-take <path> <handoff id>: takes the
 * session, runs a command in it, prints what its screen shows, and exits
 * 0 if the command answered. Run by handoff_test in a child process.
 */
int bssh_handoff_take_mode(const QString &path, const QString &handoff_id);

#endif /* BSSH_HANDOFF_TEST_H */
