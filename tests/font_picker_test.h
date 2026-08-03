#ifndef BSSH_FONT_PICKER_TEST_H
#define BSSH_FONT_PICKER_TEST_H

#include <QObject>

/*
 * Tests for choosing a font and for the choice surviving the trip into a
 * profile (project.md sec 9.4, sec 10.1).
 *
 * The picker is the only place a user can act on everything the font
 * catalog does, so these check the two things a picker can silently get
 * wrong: offering the wrong set, and storing something other than what it
 * offered.
 */
class font_picker_test : public QObject {
	Q_OBJECT

private slots:
	void offers_the_selectable_set_and_nothing_else();
	void an_empty_choice_shows_the_platform_default();
	void a_font_this_build_does_not_have_is_shown_not_swallowed();
	void choosing_a_font_reports_its_id_rather_than_its_label();
	void the_session_dialog_keeps_what_its_form_does_not_show();
	void the_session_dialog_round_trips_the_key_file();
	void agreeing_with_the_preset_clears_the_override();
	void the_desktop_form_never_saves_a_nameless_host();
	void a_font_matching_the_preset_is_not_recorded_as_an_override();
	void reverting_drops_the_override_rather_than_copying_the_value();
};

#endif /* BSSH_FONT_PICKER_TEST_H */
