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
	void osc8_marks_cells_with_a_link();
	void osc8_survives_a_wrap_and_ends_where_told();
	void synchronised_output_holds_the_repaint();
	void synchronised_output_releases_itself_if_abandoned();
	void scrolled_lines_enter_the_scrollback();
	void scrollback_is_capped();
	void scrollback_keeps_colour();
	void alternate_screen_does_not_pollute_the_scrollback();
	void the_remote_may_ask_for_the_clipboard_but_never_read_it();
	void scrolling_moves_the_grid_without_losing_anything();
	void a_write_is_on_the_screen_when_it_returns();
	void damage_is_reported_per_row_not_per_cell();
};

#endif /* BSSH_EMULATOR_TEST_H */
