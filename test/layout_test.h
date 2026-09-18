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
	 * The arrangement dimension (sec 8.7): a manager defaults to tiled,
	 * takes a column, and its effective pane cap follows the arrangement
	 * rather than the width -- so a phone-width window in a column holds
	 * the second terminal the same width tiled would push to a tab. The
	 * relationship is the point: max_panes() and visible_panes() must
	 * DIFFER between the two arrangements at exactly the width the column
	 * exists for (COMPACT), or the column has changed nothing.
	 */
	void a_column_caps_where_a_tile_cannot();
	void a_column_holds_the_terminal_a_tile_would_tab();

	/*
	 * And the container renders it stacked: a column runs its splitter
	 * vertically, so the two panes share the width and sit at different
	 * heights, where a tile shares the height at different x. Asserted
	 * through the geometry rather than the private splitter, because that
	 * is what a person sees.
	 */
	void a_column_stacks_its_panes();

	/*
	 * The magnetic divider (sec 8.7): pure math, so the detent is testable
	 * without the gesture. Near the halfway line it snaps to it; away from
	 * it, to a whole row; and never onto the neighbour's last row.
	 */
	void a_column_divider_is_magnetic_to_the_halfway_line();

	/*
	 * The input selector (sec 8.7): the always-visible control defaults to
	 * scroll, reports a user's change and reflects one made elsewhere
	 * without emitting; and the window pushes its mode to every pane,
	 * including a pane built after the mode was chosen.
	 */
	void the_input_selector_defaults_to_scroll_and_reports_changes();
	void the_selector_pushes_its_mode_to_every_pane();

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

	/*
	 * The other half of fitting, and the fit assertion cannot see it: it
	 * asks whether a MINIMUM fits, and a control willing to truncate its
	 * own text has a small minimum -- so a clipping widget passes more
	 * easily than an honest one. Measured: the settings page went green
	 * at 320dp while drawing "trl+Shift+C" and "BeerSSH's own, compac".
	 */
	void no_section_page_shows_a_widget_from_another_section();
	void every_setting_is_on_exactly_one_section();
	void no_control_is_narrower_than_the_text_it_shows();

	/*
	 * The editors stack their labels above their fields because they are
	 * NARROW, not because a finger is doing the pointing.
	 *
	 * Both asked is_touch_primary() until 2026-09-11, which is a question
	 * about the input device standing in for one about the room: an
	 * unfolded Galaxy Z Fold got a 320dp form across 840dp, and a desktop
	 * window dragged narrow kept two columns and starved the fields the
	 * phone was stacked to protect. The assertion is the relationship --
	 * one widget, two widths, two answers -- which no version keyed on the
	 * platform can satisfy, since the platform does not change under it.
	 */
	void a_form_stacks_on_width_not_on_touch();

	/*
	 * ONE CONFIG MENU, and every config page on it (sec 8.7).
	 *
	 * Reported as "there should be exactly one config menu" and "the
	 * presets should be in the config menu, editable". The front door's
	 * overflow offered Presets, "This device" and Licences as siblings,
	 * and "This device" opened an index offering Licences AGAIN -- so
	 * one entry duplicated another and two were config pages outside
	 * config.
	 *
	 * Two halves, because the fault had two. The menu and the index must
	 * name nothing in common, which is the duplicate the report was
	 * about; and every row of the index must land on the page it names,
	 * which is the failure a list mapped to pages BY POSITION invites --
	 * the same shape sec 8.7 records the tab bar's off-by-one in, and
	 * this change prepends a row to both halves of that mapping.
	 */
	void every_config_page_sits_on_the_one_index();

	/*
	 * The front door's overflow menu reaches sec 6.2's 48dp floor under
	 * touch, and keeps pointer metrics otherwise.
	 *
	 * bssh_apply_touch_menu() has three callers and had no test, while
	 * touch_metrics.h's own comments record touch heights regressing in
	 * exactly this shape more than once -- 23dp rows on a Note 9, a tab
	 * menu left at pointer height until it was pulled out. A QMenu's rows
	 * are QActions, so no widget-height walk can see them and no
	 * every_touch_surface_fits check covers them; this is the one guard
	 * that a menu keeps its floor. The device visual check sec 8.7 owes
	 * is separate and still owed -- this pins the reachable half.
	 */
	void the_front_door_overflow_menu_reaches_the_touch_floor();

	/*
	 * A scroll chord from the SESSION's router scrolls the window's
	 * terminal -- the composition send_key -> steal_chord ->
	 * shortcut_pressed -> handle_shortcut -> scroll_by, which nothing
	 * tested end to end for either keyboard. The on-screen keyboard is
	 * now a producer of that shortcut (sec 8.7), so the wiring it depends
	 * on is worth a test rather than three tested pieces and a hope.
	 */
	void a_scroll_chord_scrolls_the_focused_terminal();

	/*
	 * The soft keyboard's own letter key, pressed with Ctrl+Shift armed,
	 * reaches the copy chord rather than sending the shell an interrupt --
	 * the wiring from the button through steal_char_chord, which the
	 * router-level test cannot see (sec 6.1, sec 8.7).
	 */
	void a_soft_keyboard_letter_chord_is_kept_by_the_app();
	void the_full_keyboard_answers_about_itself();
	void a_narrow_screen_falls_back_to_the_compact_keyboard();
	void both_keyboards_read_one_layout();
	void every_slot_the_geometry_names_exists();
	void a_keypad_key_sends_only_its_digit();
	void a_keyboard_can_reach_every_level_it_shows();
	void both_keyboards_reach_every_printable_character();
	void an_unknown_layout_is_refused_and_changes_nothing();
	void a_small_screen_loses_no_letter();
	void a_fourth_page_appears_only_where_a_letter_needs_it();
	void a_non_latin_layout_opens_in_latin_and_reaches_its_script();
	void the_script_toggle_is_offered_only_where_it_does_something();
	void the_detach_action_makes_a_window();
	void the_last_tab_does_not_tear_out();
	void a_pane_can_be_moved_along_the_arrangement();
	void a_tab_can_be_moved_along_the_bar();
	void touch_gets_no_pane_grip();
	void the_keyboard_side_icons_are_still_named();
	void a_pane_dropped_back_changes_nothing();
	void the_tab_bar_row_takes_a_drop_beyond_the_tabs();
	void a_window_that_loses_its_last_session_closes();
	void the_pane_grip_arms_a_drag();
	void the_grip_asks_for_a_drag_once_the_pointer_moves();
	void the_brightness_slider_cannot_reach_unreadable();
	void the_layout_setting_reaches_the_keyboard();
	void choosing_a_layout_changes_what_the_keys_send();
	void an_ampersand_cap_is_not_left_to_the_style();
	void altgr_reaches_the_us_international_level();
	void shift_reaches_every_character_a_real_keyboard_has();
	void the_hide_key_is_remembered_and_a_tap_undoes_it();
	void the_keyboard_config_key_opens_the_settings_page();
	void the_back_key_leaves_the_page_not_the_app();
	void the_altgr_legend_is_shown_and_then_stands_aside();
	void every_keyboard_group_is_the_same_shape();
	void the_built_in_keyboard_fits_a_phone();
	void only_one_on_screen_keyboard_is_ever_shown();
	void the_key_row_fits_a_phone();

	/*
	 * Auto-repeat, on both on-screen keyboards at once (sec 6.2).
	 *
	 * One test rather than two because there is one rule, and the two
	 * boards build their buttons in different files: a rule enforced in
	 * two places is a rule that drifts, and the way it would drift is a
	 * key added to one board and not given the call.
	 */
	void every_key_that_sends_something_repeats();
	void the_desktop_opens_a_shell_and_the_phone_opens_its_config();
	void the_config_is_reachable_from_a_terminal();
	void the_host_list_offers_the_local_shell();
	void two_windows_are_two_views_onto_one_registry();
	void focus_follows_the_pointer_only_when_asked();
	void a_window_shows_only_its_own_tabs();
	void cancelling_the_settings_keeps_the_session();
	void closing_the_settings_window_means_cancel();
	void every_config_category_shows_its_own_page();
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
	void a_long_title_cannot_push_the_other_tabs_off_the_bar();
	void a_pane_drag_is_not_raised_from_inside_the_views_own_handler();
	void a_session_adopted_from_another_window_is_wired_to_this_one();
	void a_shortcut_acts_only_on_the_window_it_was_typed_in();
	void a_failure_is_reported_against_the_session_it_came_from();
	void two_windows_write_to_one_host_list();
	void a_moved_session_asks_the_window_it_is_in();
	void closing_a_window_leaves_the_other_windows_sessions_alone();
	void hovering_a_tiled_pane_moves_focus_to_it();
	void a_tiled_session_leaves_the_tab_bar();
	void a_pane_dropped_on_the_tab_bar_becomes_a_tab();
	void narrowing_a_window_turns_its_extra_panes_into_tabs();
	void what_the_terminal_reports_is_what_it_paints();
	void a_keystroke_in_a_pane_reaches_the_wire();
	void a_mouse_report_reaches_the_wire();
	void a_resized_pane_tells_the_remote_its_size();
	void closing_the_visible_tab_leaves_no_view_on_a_dead_session();
	void split_beside_current_makes_a_second_pane();
	void a_tab_dropped_on_a_pane_reaches_the_window();
	void a_reorder_along_the_strip_is_not_a_tear_out();
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
	void a_resize_that_keeps_the_size_class_keeps_the_views();
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
	void the_wheel_scrolls_the_settings_page_and_changes_nothing();
	void the_screenshot_setting_is_absent_where_it_cannot_work();
	void the_biometric_setting_is_absent_where_it_cannot_work();
	void every_surface_names_the_copyright_holder();
	void focus_reporting_follows_the_focused_pane();
	void backing_out_of_a_session_reaches_the_host_list();
};

#endif /* BSSH_LAYOUT_TEST_H */
