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
};

#endif /* BSSH_LAYOUT_TEST_H */
