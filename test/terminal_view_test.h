#ifndef BSSH_TERMINAL_VIEW_TEST_H
#define BSSH_TERMINAL_VIEW_TEST_H

#include <QObject>

/*
 * Host unit tests -- no device, no server, no display (project.md sec
 * 12.3). Everything here runs under the offscreen QPA plugin in CI.
 *
 * Two things are under test: the grid geometry the whole resize path
 * depends on, and the byte path from an SSH session, through the
 * emulator, to what the widget actually paints. Both are what sec 12.3
 * predicts will regress quietly if nothing watches them.
 */
class terminal_view_test : public QObject {
	Q_OBJECT

private slots:
	void the_cursor_style_reaches_the_pixels();
	void a_reattached_screen_is_not_read_from_the_old_one();
	void an_unfocused_cursor_is_hollow();
	void only_the_focused_pane_blinks();
	void alt_drag_asks_for_the_pane_to_be_moved();
	/*
	 * The scrollbar's default position is process-wide, and a window
	 * built by a test sets it from the settings. Put it back after
	 * every test, or whichever test runs next inherits a bar it never
	 * asked for and measures the wrong grid.
	 */
	void cleanup();

	/* sec 6.4's drops: a file's path, shell-safe, or the text itself. */
	void a_dropped_file_arrives_as_a_path_a_shell_can_read();
	void a_drop_asks_the_window_to_paste_rather_than_pasting_itself();

	/* sec 8.3's scrollbar: the grid gives up the width it takes. */
	void the_scrollbar_takes_its_width_from_the_grid();
	void the_scrollbar_follows_the_view_and_drives_it();

	void grid_scales_with_widget_size();
	void grid_resized_fires_only_on_real_change();
	void renders_without_a_screen_attached();
	void renders_cell_colours_from_the_screen();
	void session_bytes_reach_the_screen_through_the_emulator();
	void ime_commit_text_becomes_input();
	void drag_selects_text();
	void selection_spans_the_scrollback();
	void a_selection_stays_on_its_text_when_the_scrollback_evicts();
	void shrinking_the_scrollback_brings_the_view_back_with_it();
	void double_click_selects_a_word();
	void triple_click_selects_the_line();
	void mouse_goes_to_the_remote_when_it_asks_for_it();
	void shift_overrides_mouse_reporting_to_select();

	/*
	 * The touch input selector routes a body drag (sec 8.7): scroll moves
	 * the scrollback -- the route a phone did not have -- select is the
	 * Shift override a phone cannot press, and click leaves the gesture to
	 * a remote that grabbed the mouse.
	 */
	void a_touch_body_drag_scrolls_in_scroll_mode();
	void a_touch_body_drag_reorders_in_move_mode();
	void a_touch_body_drag_pans_the_column_in_pan_mode();
	void find_locates_matches_navigates_and_scrolls();
	void find_matches_across_a_wrapped_line();
	void select_mode_is_the_touch_shift_override();
	void click_mode_lets_the_remote_have_the_touch();

	/*
	 * The system font-scale feature (sec 9.5): the pure scaling helper
	 * grows chrome and no-ops on a 1.0 or broken reading, and -- the
	 * property it all rests on -- the terminal grid ignores the
	 * application font entirely, so scaling chrome never moves the columns.
	 */
	void the_interface_font_scales_and_no_ops_at_one();
	void the_terminal_grid_ignores_the_application_font();

	/*
	 * The ligature setting reaches the grid: a view builds its font with
	 * the device default (sec 9.2), which is the wiring make_font's own
	 * test cannot see.
	 */
	void a_view_builds_its_font_with_the_ligature_default();
	void a_sticky_shift_overrides_mouse_reporting_on_touch();
	void a_long_press_reaches_the_menu_even_when_the_remote_has_the_mouse();
	void the_wheel_goes_to_the_remote_when_it_asked_for_the_mouse();
	void the_alternate_screen_has_no_history_to_scroll_into();
	void session_records_what_the_client_wrote();
	void session_reports_every_state_on_the_way_up();
	void a_hosts_resolved_font_reaches_its_view();
	void an_unset_font_means_the_default_not_the_fallback_chain();
	void changing_a_hosts_font_changes_its_grid();
	void the_cell_lands_on_whole_device_pixels();
	void the_cursor_lands_on_the_cell_it_marks();
	void the_block_cursor_does_not_cover_its_character();
	void a_long_run_of_text_stays_on_the_grid();
	void the_terminal_reads_itself_to_assistive_technology();
	void every_control_announces_a_name();
	void the_contrast_floor_can_be_turned_off();
	void the_floor_lifts_glyphs_and_leaves_backgrounds_faithful();
	void dimming_takes_the_text_and_the_blocks_but_never_the_background();
	void a_reader_can_reach_the_scrollback_and_the_selection();
	void trimming_the_history_reaches_the_reader_too();
	void a_selection_change_is_announced_not_only_answerable();
	void a_readers_rectangles_follow_the_grid_and_the_scroll();
	void a_reader_counts_characters_where_the_grid_counts_cells();

	/*
	 * sec 6.4's terminal menu, and the two mouse conventions around it.
	 * Right-click had no effect at all before this: the menus existed for
	 * the tab bar and the host list and not for the thing the app is for.
	 */
	void select_all_reaches_the_scrollback_and_the_screen();
	void select_all_still_reaches_the_screen_once_the_ring_evicts();
	void copying_a_wrapped_line_gives_back_one_line();
	void a_real_newline_still_separates_two_lines();
	void a_wrapped_line_stays_one_line_after_it_scrolls_away();
	void middle_click_pastes_the_primary_selection();
	void the_remote_keeps_the_right_button_when_it_asked_for_the_mouse();
	void a_release_names_the_button_the_press_reported();
	void a_click_while_scrolled_back_reports_the_screen_row();
	void the_menu_layout_follows_the_device_until_told_otherwise();
	void the_terminal_menu_offers_the_three_verbs_in_order();
	void the_menu_shows_the_chord_the_router_would_actually_steal();
	void both_menus_agree_on_when_paste_is_offered();
	void the_touch_layout_grows_the_rows_and_drops_the_chords();
	void a_bare_url_is_clickable_and_stops_where_it_should();
	void a_long_press_selects_and_handles_adjust_it();

	/*
	 * Zoom (sec 9.5). The first three are about the view's arithmetic;
	 * the last two are the ones that can catch a feature wired to
	 * nothing -- the chord reaching the session, and the menu offering
	 * the entries at all.
	 */
	void zoom_moves_the_size_the_grid_is_drawn_at();
	void a_resolve_moves_the_base_under_the_zoom();
	void the_zoom_stops_where_the_settings_layer_does();
	void ctrl_wheel_zooms_while_the_remote_owns_the_wheel();
	void the_zoom_chords_reach_the_focused_session();
	void the_terminal_menu_offers_zoom();
	void the_scrollback_search_is_reachable_without_a_keyboard();
	void a_zoom_past_the_limit_still_steps_back();
	void a_cursor_under_the_floor_is_lifted_like_text_is();
};

#endif /* BSSH_TERMINAL_VIEW_TEST_H */
