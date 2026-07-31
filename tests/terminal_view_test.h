#ifndef BSSH_TERMINAL_VIEW_TEST_H
#define BSSH_TERMINAL_VIEW_TEST_H

#include <QObject>

/*
 * Host unit tests -- no device, no server, no display (project.md sec
 * 12.3). Everything here runs under the offscreen QPA plugin in CI.
 *
 * These cover the two things M0/M1 actually claim: the grid geometry the
 * whole resize path depends on, and the byte path from an SSH session to
 * the widget. Both are pure logic, and both are what sec 12.3 predicts
 * will regress quietly if nothing watches them.
 */
class terminal_view_test : public QObject {
	Q_OBJECT

private slots:
	void grid_scales_with_widget_size();
	void grid_resized_fires_only_on_real_change();
	void feed_appends_and_splits_lines();
	void feed_drops_control_bytes();
	void session_bytes_reach_the_view();
	void session_records_what_the_client_wrote();
	void session_reports_every_state_on_the_way_up();
};

#endif /* BSSH_TERMINAL_VIEW_TEST_H */
