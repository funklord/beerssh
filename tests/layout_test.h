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
};

#endif /* BSSH_LAYOUT_TEST_H */
