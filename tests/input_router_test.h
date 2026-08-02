#ifndef BSSH_INPUT_ROUTER_TEST_H
#define BSSH_INPUT_ROUTER_TEST_H

#include <QObject>

/*
 * Tests for key -> bytes (project.md sec 6.3).
 *
 * These are the cheapest tests in the project and among the most
 * valuable: the bytes a key produces are invisible until a remote program
 * behaves oddly, and by then the cause looks like the program's fault
 * rather than the terminal's. Asserting them directly is what stops
 * "arrow keys do nothing in vim" being discovered by a user.
 *
 * They also pin the mode-dependence: the same key produces different
 * bytes depending on terminal state, which is exactly why sec 6.3 puts
 * the translation behind the emulator instead of in the widget.
 */
class input_router_test : public QObject {
	Q_OBJECT

private slots:
	void plain_characters_pass_through();
	void ctrl_c_is_the_interrupt_byte();
	void ctrl_letters_are_case_insensitive();
	void arrows_follow_application_cursor_mode();
	void function_keys_and_navigation_produce_sequences();
	void alt_prefixes_with_escape();
	void sticky_modifier_applies_once_then_clears();
	void ctrl_shift_c_is_kept_by_the_app();
	void paste_is_bracketed_only_when_the_remote_asked();
	void kitty_protocol_disambiguates_ctrl_and_escape();
	void kitty_protocol_pushes_and_pops();
	void kitty_protocol_answers_a_query();
	void modify_other_keys_encodes_and_reports_its_level();
	void pane_chords_are_kept_by_the_app();
	void key_row_goes_through_the_router();
	void key_row_visibility_follows_the_input_device();
	void a_key_row_layout_changes_which_keys_exist();
	void a_chord_key_sends_one_press_not_a_sticky_modifier();
};

#endif /* BSSH_INPUT_ROUTER_TEST_H */
