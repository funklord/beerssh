#ifndef BSSH_LAYOUT_TEST_H
#define BSSH_LAYOUT_TEST_H

#include <QObject>

/*
 * Adaptive layout (project.md sec 8).
 *
 * The layout manager takes a width and knows nothing else, which is what
 * makes it testable by handing it numbers -- and what makes a desktop
 * window dragged narrow behave exactly like a folded foldable.
 */
class layout_test : public QObject {
	Q_OBJECT

private slots:
	void size_classes_follow_the_window_not_the_platform();
	void pane_limits_match_the_size_class();
	void shrinking_collapses_panes_to_tabs_without_closing_them();
	void widening_restores_the_arrangement();
	void layout_only_changes_on_a_class_boundary();
	void panes_get_their_own_geometry();
	void focus_is_visible_only_when_there_is_a_choice();
	void pane_focus_moves_with_the_keyboard();
	void dividers_snap_to_whole_cells();
	void panes_collapse_to_tabs_and_come_back();

	/*
	 * Every surface a thumb can reach, at the narrowest width sec 8.2
	 * designs to.
	 *
	 * Written because the same bug kept turning up one screen at a time,
	 * always found by looking at a device rather than by any test: the
	 * status line pushed the terminal off the right edge, the password
	 * prompt demanded 508px on a 360px screen, and the backend name in
	 * settings was clipped mid-sentence. Each was fixed alone. This asks
	 * the question of all of them at once, so the next one is caught
	 * before it ships rather than after.
	 */
	void every_touch_surface_fits_a_phone();
	void the_key_row_fits_a_phone();
	void the_desktop_opens_a_shell_and_the_phone_opens_its_config();
	void the_config_is_reachable_from_a_terminal();
	void the_host_list_offers_the_local_shell();
	void two_windows_are_two_views_onto_one_registry();
	void focus_follows_the_pointer_only_when_asked();
	void a_window_shows_only_its_own_tabs();
	void cancelling_the_settings_keeps_the_session();
	void a_torn_out_tab_becomes_a_window_without_moving_the_session();
	void a_dropped_session_leaves_the_window_it_came_from();
	void a_drop_on_the_panes_tiles_where_there_is_room();
	void a_dragged_tab_lands_where_it_is_dropped();
	void a_drop_naming_no_session_is_declined();
	void a_window_watches_for_a_screen_change();

	/*
	 * sec 8.6's --geometry, which is a claim about CELLS and can only be
	 * checked by asking the grid what it became. The arithmetic behind it
	 * -- cell metrics plus whatever chrome is showing -- is exactly the
	 * kind that looks right and lands a row short.
	 */
	void a_window_can_be_sized_in_cells();

	/*
	 * sec 7.1's OSC 52, end to end through the window that honours it.
	 * The wiring was written with Qt::UniqueConnection and a lambda,
	 * which Qt refuses outright -- so the feature had never once run.
	 */
	void a_trusted_host_can_actually_set_the_clipboard();
	void a_live_tab_is_not_offered_connect();
	/*
	 * And the desktop front door, at the small window sec 8.2 already
	 * contemplates when it talks about tiling one into a half-screen
	 * column.
	 */
	void every_desktop_surface_fits_a_small_window();
	void the_desktop_front_door_lines_its_fields_up();
	/*
	 * Which ARRANGEMENT the front door embeds, which is a different
	 * question from whether it fits: the desktop dialog was building the
	 * touch editor, and both of them fit.
	 */
	void the_desktop_front_door_uses_the_desktop_preset_editor();

	/*
	 * sec 8.3's tab menu is reachable by BOTH gestures. The right-click
	 * path moved out of a lambda into a method when press-and-hold was
	 * added, and a signal left unconnected by that move would take the
	 * desktop's only route to the menu with it.
	 */
	void the_tab_menu_is_reachable_by_both_gestures();
	void reordering_tabs_reorders_the_sessions_behind_them();
	void a_dragged_tab_bar_is_a_pointers_affordance();
	void middle_click_closes_the_tab_under_it();
	void the_tab_menu_offers_sec_8_3s_three_verbs();
	void clicking_a_pane_focuses_it();
	void a_divider_equalizes_on_a_double_click();
	void a_divider_is_thumb_sized_only_where_a_thumb_uses_it();
	void a_bell_in_a_visible_pane_is_not_a_missed_one();
	void a_bell_with_the_window_away_is_a_missed_one();
	void the_caps_setting_reaches_a_sessions_router();
	void the_screenshot_setting_is_absent_where_it_cannot_work();
	void focus_reporting_follows_the_focused_pane();
	void backing_out_of_a_session_reaches_the_host_list();
};

#endif /* BSSH_LAYOUT_TEST_H */
