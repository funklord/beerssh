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
	void grid_scales_with_widget_size();
	void grid_resized_fires_only_on_real_change();
	void renders_without_a_screen_attached();
	void renders_cell_colours_from_the_screen();
	void session_bytes_reach_the_screen_through_the_emulator();
	void ime_commit_text_becomes_input();
	void drag_selects_text();
	void selection_spans_the_scrollback();
	void double_click_selects_a_word();
	void triple_click_selects_the_line();
	void mouse_goes_to_the_remote_when_it_asks_for_it();
	void shift_overrides_mouse_reporting_to_select();
	void session_records_what_the_client_wrote();
	void session_reports_every_state_on_the_way_up();
	void a_hosts_resolved_font_reaches_its_view();
	void an_unset_font_means_the_default_not_the_fallback_chain();
	void changing_a_hosts_font_changes_its_grid();
	void the_cell_lands_on_whole_device_pixels();
	void a_long_run_of_text_stays_on_the_grid();
	void the_terminal_reads_itself_to_assistive_technology();
	void every_control_announces_a_name();
	void the_contrast_floor_can_be_turned_off();
	void the_floor_lifts_glyphs_and_leaves_backgrounds_faithful();
	void a_reader_can_reach_the_scrollback_and_the_selection();

	/*
	 * sec 6.4's terminal menu, and the two mouse conventions around it.
	 * Right-click had no effect at all before this: the menus existed for
	 * the tab bar and the host list and not for the thing the app is for.
	 */
	void select_all_reaches_the_scrollback_and_the_screen();
	void middle_click_pastes_the_primary_selection();
	void the_remote_keeps_the_right_button_when_it_asked_for_the_mouse();
	void the_menu_layout_follows_the_device_until_told_otherwise();
	void the_terminal_menu_offers_the_three_verbs_in_order();
	void the_touch_layout_grows_the_rows_and_drops_the_chords();
};

#endif /* BSSH_TERMINAL_VIEW_TEST_H */
