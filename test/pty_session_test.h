#ifndef BSSH_PTY_SESSION_TEST_H
#define BSSH_PTY_SESSION_TEST_H

#include <QObject>

/*
 * The local shell transport (project.md sec 2, sec 3).
 *
 * This is the one transport that can be tested for real without a server,
 * because the far end is a process on this machine. The SSH one needs
 * either a fake or tools/check-ssh.sh's throwaway sshd; a pty needs
 * /bin/sh, which is present wherever this suite runs at all.
 */
class pty_session_test : public QObject {
	Q_OBJECT

private slots:
	void a_shell_starts_and_echoes_what_it_is_given();
	void exiting_the_shell_is_not_a_dropped_connection();
	void a_resize_reaches_the_shell();
	void a_command_runs_instead_of_the_shell();
	void a_command_starts_where_it_was_told_to();
	void a_command_that_cannot_be_run_says_so();
	void a_local_profile_is_valid_without_a_host();
};

#endif /* BSSH_PTY_SESSION_TEST_H */
