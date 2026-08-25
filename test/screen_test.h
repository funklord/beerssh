#ifndef BSSH_SCREEN_TEST_H
#define BSSH_SCREEN_TEST_H

#include <QObject>

/*
 * Tests for the grid itself, with no parser involved (project.md sec
 * 12.3). These are the invariants both candidate emulators depend on, so
 * they are worth having before either one is chosen.
 */
class screen_test : public QObject {
	Q_OBJECT

private slots:
	void starts_blank_at_the_requested_size();
	void resize_preserves_the_top_left();
	void resize_keeps_the_cursor_on_the_grid();
	void out_of_bounds_access_is_harmless();
	void to_text_trims_trailing_blanks();
	void to_text_skips_double_width_continuations();
};

#endif /* BSSH_SCREEN_TEST_H */
