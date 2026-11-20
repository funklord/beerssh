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
	void a_hover_says_where_and_then_that_it_left();
	void only_sockets_nobody_listens_on_are_swept();
	void a_session_nobody_has_is_refused_and_nothing_moves();
	void a_taker_that_vanishes_leaves_the_session_running();
	void a_drag_payload_names_where_to_ask();
	void a_dropped_session_lands_in_this_process();
	void a_new_version_takes_every_session();
	void another_process_lists_its_terminals_by_title();
	void an_arrangement_reads_back_what_it_wrote();
	void a_new_version_takes_the_windows_as_they_were();

	/*
	 * A terminal moved -- within a window, as a tab, to another window and
	 * back -- rebuilds each window's panes once, and each terminal goes
	 * from its old size straight to its new one: the holder's "huge update
	 * party when moving terminals around" (2026-11-20).
	 */
	void a_move_rebuilds_each_window_once();
	void a_torn_out_tab_and_a_new_window_start_processes_of_their_own();
	void a_start_brings_back_every_window_not_open_elsewhere();
	void a_torn_out_tab_arrives_in_a_process_of_its_own();
};

/*
 * The taker's side, for --handoff-take <path> <handoff id>: takes the
 * session, runs a command in it, prints what its screen shows, and exits
 * 0 if the command answered. Run by handoff_test in a child process.
 */
int bssh_handoff_take_mode(const QString &path, const QString &handoff_id);

/*
 * For --handoff-terminals <path>: asks the process there for its terminals,
 * prints "id|title" a line each and "peers: N", and exits 0.
 */
int bssh_handoff_terminals_mode(const QString &path);

/*
 * The giving side, for --handoff-give: opens a local shell, listens, prints
 * "<path> <handoff id>" on one line, and serves until the session has been
 * taken -- then exits 0 -- or for at most 60 seconds.
 */
int bssh_handoff_give_mode();

/*
 * The old version, for --handoff-arranged: two windows of local shells,
 * the first split H(a,V(b,c)) with c current, the second holding d, each
 * shell having printed "pane-<letter>". Prints its handoff path, serves
 * until nothing is left in it or for at most 60 seconds, and exits 0 if
 * everything was taken.
 */
int bssh_handoff_arranged_mode();

#endif /* BSSH_HANDOFF_TEST_H */
