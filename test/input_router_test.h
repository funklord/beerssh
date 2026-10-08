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
	void kitty_event_types_report_repeats_and_releases();
	void kitty_all_keys_as_escapes_with_text_and_alternates();
	void kitty_reaches_the_on_screen_keyboard_too();
	void kitty_release_answers_its_own_press();
	void kitty_modes_are_per_screen_and_an_emptied_stack_resets();
	void super_is_a_kitty_modifier_and_nothing_else_hears_it();
	void the_keypad_is_its_own_keys_under_the_kitty_protocol();
	void a_paste_cannot_close_its_own_bracket();
	/* sec 6.1: the stolen set is the user's to change, and to refuse. */
	void a_rebound_chord_is_the_one_that_is_stolen();
	void a_chord_given_back_reaches_the_remote();

	/*
	 * General key remapping (sec 6.1): a remapped key sends its target's
	 * bytes, an unmapped key is untouched, and a chord the app keeps beats
	 * a remap on the same key -- the precedence handle_key promises.
	 */
	void a_key_remap_sends_the_targets_bytes();
	void a_stolen_chord_beats_a_key_remap();

	/*
	 * Press-and-hold (sec 6.4, sec 8.3). Qt does not provide it and does
	 * not synthesise a context-menu event from it on Android, so this is
	 * the gesture both the terminal and the host list depend on.
	 */
	void a_hold_fires_once_it_has_been_held();
	void a_hold_that_moves_is_a_drag();
	void a_hold_released_early_is_a_click();
	void a_hold_does_nothing_until_it_is_enabled();
	void a_matured_hold_swallows_its_own_release();
	void plain_characters_pass_through();
	void ctrl_c_is_the_interrupt_byte();
	void a_reset_gives_ctrl_c_back_after_a_program_dies_holding_it();
	void ctrl_letters_are_case_insensitive();
	void arrows_follow_application_cursor_mode();
	void function_keys_and_navigation_produce_sequences();
	void alt_prefixes_with_escape();
	void sticky_modifier_applies_once_then_clears();
	void a_modifier_locks_on_the_second_press_and_releases_on_the_third();
	void a_held_modifier_carries_every_key_and_a_tap_still_cycles();
	void two_fingers_chord_on_the_key_row();
	void two_fingers_chord_on_the_built_in_keyboard();
	void armed_locked_and_off_draw_differently();
	void ctrl_shift_c_is_kept_by_the_app();
	void paste_is_bracketed_only_when_the_remote_asked();
	void kitty_protocol_disambiguates_ctrl_and_escape();
	void kitty_protocol_pushes_and_pops();
	void kitty_protocol_answers_a_query();
	void modify_other_keys_encodes_and_reports_its_level();
	void pane_chords_are_kept_by_the_app();
	void key_row_goes_through_the_router();

	/*
	 * The on-screen keyboard reaches the app chords the hardware keyboard
	 * does -- notably scroll, the only way a phone can reach its
	 * scrollback until body-drag lands (sec 8.7). send_key walked past
	 * stolen_chords() and paged the remote instead.
	 */
	void the_on_screen_keyboard_reaches_stolen_chords();

	/*
	 * And its LETTER chords -- Ctrl+Shift+C and the like -- which arrive as
	 * a character through send_text, not a named key, so Shift+PgUp's fix
	 * does not cover them. Before this a soft-keyboard Ctrl+Shift+C sent
	 * the shell an interrupt instead of copying (sec 6.1, sec 8.7).
	 */
	void the_on_screen_keyboard_reaches_letter_chords();

	/*
	 * That a HELD key actually sends more than once, end to end through
	 * the row and the router into the emulator's output. The property
	 * check in layout_test says the flag is set on the right buttons;
	 * only this says the flag does anything, and Qt's auto-repeat is a
	 * timer on a mouse press rather than something a click() can reach.
	 */
	void a_held_key_repeats();
	void a_modifier_button_stops_claiming_a_session_that_has_gone();
	void key_row_visibility_follows_the_input_device();
	void a_key_row_layout_changes_which_keys_exist();
	void a_custom_key_row_is_composed_from_words();
	void a_custom_row_that_cannot_drive_a_terminal_is_refused();
	void the_custom_row_note_names_only_words_that_parse();
	void the_fn_key_reaches_the_function_keys();
	void caps_lock_can_be_made_to_send_ctrl();
	void a_chord_key_sends_one_press_not_a_sticky_modifier();
	void every_stolen_chord_is_the_table_and_nothing_else();
	void ctrl_punctuation_reaches_a_legacy_shell_as_a_vt100_would();
};

#endif /* BSSH_INPUT_ROUTER_TEST_H */
