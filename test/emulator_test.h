#ifndef BSSH_EMULATOR_TEST_H
#define BSSH_EMULATOR_TEST_H

#include <QObject>

/*
 * Golden-file style tests for the emulator (project.md sec 12.3): byte
 * stream in, expected screen out. Cheap to write, and the only practical
 * defence against a parser fix quietly breaking three other sequences.
 *
 * These assert on bssh_screen, not on libvterm's own state, which is what
 * makes them survive a change of parser -- they describe what a terminal
 * must do, not what this particular library does.
 */
class emulator_test : public QObject {
	Q_OBJECT

private slots:
	void a_terminal_moves_whole_into_another_emulator();
	void a_moved_terminal_refuses_what_it_cannot_take();
	void the_kitty_query_reports_only_the_flags_in_force();
	void a_remote_cannot_grow_the_title_without_limit();
	void a_switched_off_image_protocol_costs_no_memory();
	void the_kitty_graphics_query_is_answered();
	void the_terminal_answers_for_itself_not_for_libvterm();
	void the_terminal_answers_the_questions_a_compositing_client_asks();
	void the_palette_query_reports_the_hosts_own_sixteen();
	void decrqm_answers_for_the_alternate_screen();
	void a_switched_off_feature_is_not_advertised_either();
	void a_mode_libvterm_models_can_still_be_declined();
	void a_feature_spec_reads_left_to_right();
	void an_unknown_feature_name_is_refused();
	void focus_is_reported_only_when_the_remote_asked();
	void plain_text_lands_on_the_first_row();
	void a_line_longer_than_the_grid_wraps_without_debris();
	void wrapped_lines_scrolling_off_leave_no_debris();
	void a_wrapped_line_redrawn_shorter_leaves_no_debris();
	void a_reconnect_keeps_the_history_and_drops_the_modes();
	void a_reset_from_the_remote_clears_the_modes_it_did_not_set();
	void decscusr_sets_the_cursor_shape_and_blink();
	void decscnm_toggles_reverse_video();
	void newline_and_carriage_return_move_as_expected();
	void cursor_addressing_places_text();
	void erase_display_clears_everything();
	void scrolling_drops_the_top_line();
	void sgr_sets_colour_and_attributes();
	void wide_characters_take_two_cells();
	void utf8_split_across_chunks_still_composes();
	void device_status_report_produces_a_reply();
	void title_is_reported_from_osc();
	void the_last_column_survives_the_canonical_erase();
	void resize_keeps_existing_text();
	void a_rotation_and_back_keeps_the_prompt();
	void osc8_marks_cells_with_a_link();
	void osc8_survives_a_wrap_and_ends_where_told();
	void an_enormous_link_is_dropped_whole_and_shown_short();
	void erase_saved_lines_actually_empties_the_scrollback();
	void the_kitty_flags_stack_is_bounded();
	void the_hyperlink_table_is_bounded_and_a_reset_empties_it();
	void synchronised_output_holds_the_repaint();
	void synchronised_output_releases_itself_if_abandoned();
	void scrolled_lines_enter_the_scrollback();
	void a_row_scrolled_off_before_its_damage_is_flushed_is_still_intact();
	void scrollback_is_capped();
	void scrollback_keeps_colour();
	void alternate_screen_does_not_pollute_the_scrollback();
	void the_remote_may_ask_for_the_clipboard_but_never_read_it();
	void scrolling_moves_the_grid_without_losing_anything();
	void a_write_is_on_the_screen_when_it_returns();
	void damage_is_reported_per_row_not_per_cell();
	void damage_names_the_rows_it_touched();
	void scrolling_output_is_one_scroll_and_the_rows_it_exposed();
	void growing_the_terminal_brings_back_scrollback_as_it_was();
	void a_terminal_resized_several_ways_keeps_its_lines_in_order();
	void a_terminal_made_very_small_and_back_keeps_every_line_data();
	void a_terminal_made_very_small_and_back_keeps_every_line();
	void a_narrowed_terminal_rewraps_and_a_widened_one_joins_again();
	void scrollback_drops_trailing_blanks_but_not_blank_colour();

	/*
	 * A line written and scrolled away in the same breath.
	 *
	 * The interesting case for where the scrollback's copy comes FROM.
	 * Damage is row-merged (sec 9.5), so a row's contents reach the
	 * mirror when writing moves off it -- and a program that addresses
	 * row 0, writes, and immediately scrolls has not moved off anything.
	 * If the scrollback took the mirror's row without that having landed,
	 * it would file the PREVIOUS occupant of the row.
	 */
	void a_line_written_then_immediately_scrolled_lands_intact();

	/*
	 * The screen mirror is a ring (sec 9.5): scrolling the whole screen
	 * rotates an offset instead of moving every row. These are the cases
	 * that offset gets wrong -- scrolling far enough to wrap it round
	 * more than once, scrolling BACKWARDS, and scrolling inside margins,
	 * where rotating the whole thing would take rows with it that must
	 * not move.
	 */
	void scrolling_past_a_full_screen_keeps_the_rows_in_order();
	void scrolling_backwards_keeps_the_rows_in_order();
	void a_margin_scroll_leaves_the_rows_outside_it_alone();

	/*
	 * sec 7.1 tier 3: letting a remote ASK what this terminal supports
	 * rather than guess it from TERM. The sibling fuzzypickles TUI probes
	 * exactly these from the other side.
	 */
	void decrqm_answers_for_modes_it_knows();
	void decrqm_says_nothing_it_cannot_stand_behind();
	void xtgettcap_reports_what_this_terminal_is();
	void a_wide_glyph_scrolls_back_the_way_it_was_on_screen();
};

#endif /* BSSH_EMULATOR_TEST_H */
