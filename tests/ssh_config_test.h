#ifndef BSSH_SSH_CONFIG_TEST_H
#define BSSH_SSH_CONFIG_TEST_H

#include <QObject>

/*
 * Reading OpenSSH's client config (project.md sec 10.5).
 *
 * Pure text in, host structs out, so all of this runs on the host with no
 * file, no server and nobody's real ~/.ssh/config -- which matters more
 * here than usual: the one input a developer would reach for to try this
 * by hand is the file describing their employer's network.
 */
class ssh_config_test : public QObject {
	Q_OBJECT

private slots:
	void a_plain_host_block_becomes_a_profile();
	void an_alias_without_a_hostname_is_its_own_hostname();
	void the_first_value_wins_not_the_last();
	void wildcard_blocks_configure_hosts_without_becoming_one();
	void negated_patterns_veto_a_block();
	void match_blocks_are_skipped_rather_than_guessed_at();
	void separators_and_comments_and_quotes();
	void unsupported_directives_are_reported_not_dropped();
	void junk_does_not_produce_junk_hosts();
	void only_the_ticked_hosts_are_imported();
	void an_import_never_overwrites_an_existing_host();

	/*
	 * The jump-host spec, which is a string OpenSSH defines and this app
	 * has to take apart before it can dial anything (sec 10.5). Getting
	 * it wrong is not a parse error anyone sees -- it is a connection to
	 * the wrong machine, or to the right one on the wrong port.
	 */
	void a_jump_host_spec_is_taken_apart();
	void a_chain_of_jump_hosts_is_refused_not_truncated();
};

#endif /* BSSH_SSH_CONFIG_TEST_H */
