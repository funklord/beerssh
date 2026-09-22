#ifndef BSSH_PANE_SNAP_H
#define BSSH_PANE_SNAP_H

/*
 * Pure layout math for the column divider (project.md sec 8.7). No Qt, so
 * a headless test reaches it without building a widget -- the snap is the
 * relationship worth asserting, and the gesture that drives it is the part
 * that needs a device.
 */

/*
 * Magnetic reach of the halfway detent, in pixels. Sec 8.7 asks for 5-10
 * and "configurable"; the config plumbing is not built yet, so this is the
 * default a setting would later override.
 */
static const int BSSH_COLUMN_SNAP_RADIUS = 8;

/*
 * The most a stored radius may be. A magnet wider than the panes would
 * hold the divider on the halfway line whatever the finger did, which
 * reads as a divider that cannot be moved rather than as a setting
 * somebody typed wrong -- so the value is clamped rather than trusted.
 * Twenty is comfortably past the 5-10 sec 8.7 suggests and comfortably
 * short of a phone's half-screen.
 */
static const int BSSH_MAX_COLUMN_SNAP_RADIUS = 20;

/* Round a pane's extent down to whole character cells (sec 8.3), never
 * below one cell. */
inline int bssh_snap_pane_extent(int extent, int cell) {
	if (cell <= 1) return extent;
	const int snapped = (extent / cell) * cell;
	return snapped < cell ? cell : snapped;
}

/*
 * Where a two-pane column divider settles: whole rows, with a magnetic
 * pull to the halfway line (sec 8.7's "two half-screen"). `first` is the
 * top pane's height, `total` the pair's, `cell` one row, `radius` the
 * magnetic reach. Returns the top pane's snapped height, always leaving
 * the other pane at least one row so neither collapses.
 */
inline int bssh_snap_column_divider(int first, int total, int cell, int radius) {
	if (total <= 0) return first;

	int delta = first - (total / 2);
	if (delta < 0) delta = -delta;

	int target = (delta <= radius) ? (total / 2) : first;
	target = bssh_snap_pane_extent(target, cell);

	/*
	 * Keep a whole row for the neighbour. Only where the pair is tall
	 * enough to hold two -- below that a phone-width sliver has no honest
	 * answer and the snap just leaves it be.
	 */
	if (cell > 1 && total >= 2 * cell && target > total - cell)
		target = bssh_snap_pane_extent(total - cell, cell);

	return target;
}

/*
 * Where the column's scroll settles when a pan gesture ends (sec 8.7):
 * "the column's SCROLLING snaps too, so a terminal starts at the top of
 * the viewport rather than half off it". `offset` is the scroll position
 * the finger left behind, `tops` the panes' y positions in the column in
 * ascending order, `max_offset` the furthest the viewport can scroll.
 *
 * Nearest pane top rather than a magnetic radius, which the divider uses
 * and this deliberately does not. A radius leaves every position outside
 * it reachable, and the position this exists to remove -- a terminal half
 * off the top -- is exactly one of those. The end of the range is a
 * candidate alongside the tops, so a last pane shorter than the viewport
 * still settles showing its bottom instead of being dragged back up to a
 * top the scrollbar cannot reach.
 *
 * Ties go to the smaller offset, so a drag that stops exactly between two
 * panes reveals the earlier one rather than depending on rounding.
 */
inline int bssh_settle_column_scroll(int offset, const int *tops, int count,
                                      int max_offset) {
	if (max_offset < 0) max_offset = 0;
	if (offset < 0) offset = 0;
	if (offset > max_offset) offset = max_offset;

	/*
	 * The range's end, always a candidate. Seeded before the tops so a
	 * top at the same distance wins the tie by being smaller.
	 */
	int best = max_offset;
	int best_distance = max_offset - offset;
	if (best_distance < 0) best_distance = -best_distance;

	for (int i = 0; i < count; ++i) {
		const int top = tops[i];
		if (top < 0 || top > max_offset) continue;

		int distance = top - offset;
		if (distance < 0) distance = -distance;

		if (distance < best_distance || (distance == best_distance && top < best)) {
			best = top;
			best_distance = distance;
		}
	}

	return best;
}

#endif /* BSSH_PANE_SNAP_H */
