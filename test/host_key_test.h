#ifndef BSSH_HOST_KEY_TEST_H
#define BSSH_HOST_KEY_TEST_H

#include <QObject>

/*
 * What the known-hosts check said, as a rule rather than as a connection
 * (project.md sec 11).
 *
 * The whole of src/ssh/ssh_session_libssh.cpp needs a real server to
 * exercise, so `make check-ssh` is the only thing that has ever reached
 * it -- and that is a gate somebody has to remember to run, against a
 * host they have to have. The most consequential decision in the file
 * therefore had no test at all, which is how two of libssh's six verdicts
 * came to be handled as a third.
 *
 * bssh_host_key_verdict is pulled out so this can be asked without a
 * socket. The same move bssh_tab_status::state_for makes, for the same
 * reason and with more at stake.
 */
class host_key_test : public QObject {
	Q_OBJECT

private slots:
	void a_recorded_and_unchanged_key_is_trusted();
	void an_unrecorded_host_is_a_first_contact();
	void a_key_that_does_not_match_is_a_mismatch_whichever_way();
	void a_check_that_could_not_run_is_never_a_first_contact();
	void an_unrecognised_verdict_refuses_rather_than_trusts();
};

#endif /* BSSH_HOST_KEY_TEST_H */
