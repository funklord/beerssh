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

#endif /* BSSH_PANE_SNAP_H */
