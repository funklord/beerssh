#ifndef BSSH_CONPTY_TEST_H
#define BSSH_CONPTY_TEST_H

#include <QObject>

/*
 * The local shell on Windows, through ConPTY (ssh/conpty_session.cpp).
 * Built and run on Windows only; on POSIX pty_session_test covers the
 * same interface.
 */
class conpty_test : public QObject {
	Q_OBJECT

private slots:
	void a_command_prints_and_its_exit_status_arrives();
	void the_shell_reads_what_is_typed_and_exits_when_told();
	void a_resize_reaches_the_console();
	void closing_a_running_shell_ends_it();
	void a_program_that_does_not_exist_fails_with_a_reason();
};

#endif
