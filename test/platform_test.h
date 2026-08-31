#ifndef BSSH_PLATFORM_TEST_H
#define BSSH_PLATFORM_TEST_H

#include <QObject>

/*
 * The platform seam (project.md sec 3, core principle 3) and the
 * close-with-live-sessions policy (sec 5.5).
 *
 * What is testable here is the desktop half and the policy; the Android
 * half is JNI into a foreground service and is honestly not implemented,
 * which platform_android.cpp says at the call site rather than pretending
 * with a stub.
 */
class platform_test : public QObject {
	Q_OBJECT

private slots:
	void desktop_platform_is_selected_and_named();
	void keep_alive_controls_quit_on_last_window();
	void going_out_of_sight_is_the_platforms_to_answer_for();
	void session_count_and_notify_are_safe_without_a_tray();
	void close_policy_asks_only_when_something_is_live();
	void the_window_opens_on_the_front_door();
	void the_key_row_setting_survives_a_restart();
	void the_caps_setting_survives_a_restart();
	void hiding_from_screenshots_is_not_promised_where_it_cannot_work();
	void the_focus_follows_mouse_setting_survives_a_restart();

	/* sec 8.3, sec 8.4, sec 6.1: the settings this app grew for its
	 * desktop furniture, each stored as words a person can hand-edit. */
	void the_scrollbar_setting_survives_a_restart();
	void the_status_line_setting_survives_a_restart();
	void the_window_title_format_survives_a_restart();
	void a_chord_can_be_rebound_and_given_back();

	/* sec 6.4's multi-line paste warning: the rule, and that it persists. */
	void a_paste_is_questioned_only_when_it_carries_more_than_a_line();

	/*
	 * A failure reason is a sentence, and a QLabel asks for room for all
	 * of it. Left unchecked that pushed the window wider than the screen
	 * and everything in it -- including the terminal -- off the right
	 * edge, so a connection error blanked the app on a phone.
	 */
	void a_long_failure_does_not_widen_the_window();
	void the_system_scheme_is_never_unknown();
	void the_palettes_are_actually_light_and_dark();
	void an_explicit_mode_ignores_the_system();
	void the_theme_setting_survives_a_restart();

	/*
	 * Tier 4 of the shared dark-desktop rule: the scheme a TDE or KDE 3
	 * desktop writes to kdeglobals, which is the only place it says so.
	 * The parser takes its sources as an argument precisely so this can
	 * run without one of those desktops.
	 */
	void the_desktops_own_colours_are_read_when_nothing_else_says();
	void a_light_scheme_in_the_same_format_reads_light();
	void the_colours_decide_and_the_scheme_name_does_not();
	void the_keys_are_taken_from_general_and_not_another_section();
	void a_file_that_cannot_be_read_abstains_rather_than_guessing();
	void a_half_written_scheme_abstains_and_the_next_file_answers();
};

#endif /* BSSH_PLATFORM_TEST_H */
