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
	void plain_text_lands_on_the_first_row();
	void newline_and_carriage_return_move_as_expected();
	void cursor_addressing_places_text();
	void erase_display_clears_everything();
	void scrolling_drops_the_top_line();
	void sgr_sets_colour_and_attributes();
	void wide_characters_take_two_cells();
	void utf8_split_across_chunks_still_composes();
	void device_status_report_produces_a_reply();
	void title_is_reported_from_osc();
	void resize_keeps_existing_text();
	void scrolled_lines_enter_the_scrollback();
	void scrollback_is_capped();
	void scrollback_keeps_colour();
	void alternate_screen_does_not_pollute_the_scrollback();
};

#endif /* BSSH_EMULATOR_TEST_H */
