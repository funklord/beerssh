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
	void mouse_goes_to_the_remote_when_it_asks_for_it();
	void shift_overrides_mouse_reporting_to_select();
	void session_records_what_the_client_wrote();
	void session_reports_every_state_on_the_way_up();
};

#endif /* BSSH_TERMINAL_VIEW_TEST_H */
