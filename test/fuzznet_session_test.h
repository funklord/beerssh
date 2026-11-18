#ifndef BSSH_FUZZNET_SESSION_TEST_H
#define BSSH_FUZZNET_SESSION_TEST_H

#include <QObject>

/*
 * BeerSSH's transport for a host reached over fuzznet
 * (src/ssh/fuzznet_session.h, project.md sec 5.6), against a real
 * `beersshd --fuzznet --sessions` the Makefile builds: paired by beersshd's
 * own commands, then the session helper asked its version through the
 * transport, as a kept session's attach is. Skipped where fuzznet is not
 * built, or BSSH_TEST_SSHD_BIN and BSSH_TEST_SESSION_BIN are not given.
 */
class fuzznet_session_test : public QObject {
	Q_OBJECT

private slots:
	void the_helper_answers_through_the_transport();
	void a_shell_not_kept_is_refused();
	void a_fuzznet_host_keeps_its_shell_through_a_drop();
	void a_fuzznet_host_without_its_device_says_so();
	void the_editor_takes_a_device_image_and_keeps_it_out_of_the_profile();
	void a_host_is_paired_over_its_ssh_connection();
};

#endif
