#ifndef BSSH_DIVIDER_LINKS_H
#define BSSH_DIVIDER_LINKS_H

#include <QList>
#include <QRect>

#include <vector>

#include "ui/layout_manager.h"

/*
 * Which pane dividers move together, and which one a dragged divider
 * snaps to (project.md sec 8.3). Pure geometry, no widgets, so a test
 * asserts the relationship directly.
 *
 * A split layout is a tree, so a grid's columns are separate dividers in
 * each row's own split: dragging one moved that row alone and pulled the
 * grid out of line (the holder, 2026-11-13). Dividers that sit in line,
 * end to end, are taken as one: dragging one moves them all. That
 * decides itself from what is on screen -- a fresh grid is all in line,
 * a row of two over a row of three has nothing to link -- so nothing has
 * to remember that a layout "is a grid".
 */
struct bssh_divider_t {
	/* A vertical line, between panes side by side; else horizontal. */
	bool vertical = true;
	/* Where the line is across: x of a vertical one, y of a horizontal. */
	int at = 0;
	/* Where it runs along: its first and last pixel. */
	int from = 0;
	int to = 0;
};

/*
 * The dividers moved with `dragged`: those of its direction in line with
 * it (within `tolerance` pixels across) and reached from it end to end,
 * a gap of at most `gap` between one's end and the next's start -- a
 * crossing divider sits in that gap. `dragged` itself is included.
 */
QList<int> bssh_linked_dividers(const QList<bssh_divider_t> &all, int dragged, int tolerance, int gap);

/*
 * The divider a dragged one, now at `at`, should snap into line with:
 * one of its direction, not already moving with it (`linked`), that
 * continues any of the linked ones end to end and is within `radius`
 * across. -1 for none; the nearest when several are.
 */
int bssh_snap_divider(const QList<bssh_divider_t> &all, const QList<int> &linked, int at,
                      int radius, int gap);

/*
 * The other half of it: a divider running the whole way across -- a 3x3's
 * rows, each one divider through every column -- has no piece to move
 * alone, because the tree puts rows first. Moved alone, the tree is
 * rebuilt from where the panes are, cut the other way first: found by
 * looking for straight lines across the whole area that cut no pane, and
 * again inside each part. `first` is the direction tried first at every
 * level (Qt::Horizontal: side by side, cut by vertical lines). False when
 * some part has no such line either way -- four panes wound round a fifth,
 * which no split tree can hold -- and the layout is left as it was.
 *
 * Each pane's node comes with it, tabs and bar side and all; only the
 * nesting around them is new.
 */
struct bssh_placed_pane_t {
	bssh_pane_node leaf;
	QRect rect;
};
bool bssh_tree_from_rects(const std::vector<bssh_placed_pane_t> &panes, Qt::Orientation first,
                          bssh_pane_node *out);

#endif
