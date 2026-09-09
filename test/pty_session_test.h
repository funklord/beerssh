#ifndef BSSH_PTY_SESSION_TEST_H
#define BSSH_PTY_SESSION_TEST_H

#include <QObject>

/*
 * The local shell transport (project.md sec 2, sec 3).
 *
 * This is the one transport that can be tested for real without a server,
 * because the far end is a process on this machine. The SSH one needs
 * either a fake or tool/check-ssh.sh's throwaway sshd; a pty needs
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
	void a_big_send_does_not_block_the_ui_thread();
	void a_local_profile_is_valid_without_a_host();

	/*
	 * The user's report: typing past the right edge and then holding
	 * Backspace deleted back to the start of the wrapped row and stopped.
	 *
	 * Driven through a REAL shell, because the interesting half is what
	 * the shell emits. Measured with a plain pty first: bash crosses the
	 * wrap with cursor-up and a redraw, and dash emits nothing but
	 * `\b \b` and never moves up -- so the same keystrokes are correct
	 * on one shell and visually stuck on the other, on every terminal
	 * including xterm. This asserts the half that is ours: that the
	 * emulator follows a line editor that does it properly.
	 */
	void a_shipped_terminfo_reaches_the_shell();
	void backspace_crosses_a_wrapped_line();
};

#endif /* BSSH_PTY_SESSION_TEST_H */
