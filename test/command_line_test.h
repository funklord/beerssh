#ifndef BSSH_COMMAND_LINE_TEST_H
#define BSSH_COMMAND_LINE_TEST_H

#include <QObject>

/*
 * The command line (project.md sec 8.6).
 *
 * Every one of these runs against bssh_parse_command_line rather than
 * against the program, which is the point of that function being pure:
 * "what does `beerssh --workdir /srv user@host` mean" is answerable
 * without starting a window, forking a shell or dialling anything.
 */
class command_line_test : public QObject {
	Q_OBJECT

private slots:
	void a_target_with_colons_is_a_host_not_a_port();
	void a_bare_destination_is_the_target();
	void a_destination_becomes_a_profile();
	void execute_takes_the_rest_of_the_line();
	void execute_with_an_equals_sign_is_one_word();
	void execute_with_nothing_after_it_is_refused();
	void geometry_is_counted_in_cells();
	void geometry_that_is_not_a_grid_is_refused();
	void an_ini_path_is_a_document_and_not_a_host();
	void two_destinations_are_refused();
	void load_and_a_destination_are_refused();
	void workdir_over_ssh_is_refused();
	void hold_and_title_and_self_test_are_read();
	void an_unknown_option_is_refused();
	void help_and_version_ask_for_nothing_else();
	void every_option_is_documented_and_every_documented_option_parses();
	void the_manual_page_describes_the_options_the_parser_takes();
};

#endif /* BSSH_COMMAND_LINE_TEST_H */
