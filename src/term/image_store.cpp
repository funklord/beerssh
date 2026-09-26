#include "term/image_store.h"

/*
 * 32 MB of decoded image, which is roughly a dozen full-screen phone
 * screenshots or a very great many plots. Chosen to be generous enough
 * that normal use never meets it and small enough that a session left
 * running all day cannot quietly become the reason the app is killed
 * (sec 5.1). It is a per-session budget: the count that matters to
 * Android is the process total, and sessions are what multiply.
 *
 * Only the starting value now: the user sets it per terminal on the
 * settings page ("Picture history", the holder's answer of 2026-09-26),
 * whose own default is this same 32.
 */
static const qint64 BSSH_DEFAULT_IMAGE_BUDGET = 32 * 1024 * 1024;

bssh_image_store::bssh_image_store() : m_budget(BSSH_DEFAULT_IMAGE_BUDGET) {
}

int bssh_image_store::place(const bssh_image_placement_t &placement) {
	if (placement.image.isNull()) return 0;

	/*
	 * An id is the REMOTE's id, and an anonymous placement stays
	 * anonymous -- which is what the field's own comment in the header
	 * has always said: "0 means the protocol did not give one, which is
	 * the normal case for sixel and iTerm2".
	 *
	 * This handed those an id anyway, out of a counter of its own, into
	 * the one namespace kitty's ids live in. The counter started at 1 and
	 * kitty's numbering starts at 1, so the two collided from the first
	 * image each side drew: in a session where both protocols are used --
	 * a plot from one tool, a picture from another -- `a=T,i=1` replaced
	 * a sixel frame the terminal had numbered 1, and `a=d,i=1` deleted
	 * it. Neither program had ever heard of the other, and neither has
	 * any way to find out: nothing in either protocol reports the number
	 * a terminal invented for somebody else's image.
	 *
	 * Nothing needed the invented number. The covering test, the budget,
	 * the scroll and the trim all work on geometry, and place()'s return
	 * value has no caller at all.
	 */
	if (placement.id != 0) {
		/*
		 * A protocol may re-transmit an existing id to replace the image
		 * rather than to add a second copy of it.
		 */
		for (int i = 0; i < m_placements.size(); ++i) {
			if (m_placements.at(i).id == placement.id) {
				m_placements[i] = placement;
				drop_covered_by(i);
				return placement.id;
			}
		}
	}

	m_placements.append(placement);
	drop_covered_by(m_placements.size() - 1);
	return placement.id;
}

/*
 * A placement drawn entirely over by a later one is dead: the renderer
 * walks this list in order and paints the later one on top, so not one
 * of its pixels can reach the screen again.
 *
 * Without this, sixel ANIMATION grows without limit. Sixel carries no
 * image id, so no frame can replace an earlier one and `place` simply
 * appends each -- and a program that redraws in place, which is what an
 * animation is, never scrolls. enforce_budget() cannot help: it drops only what is
 * above the fold, and each frame is on screen by construction, so the
 * whole stack of them is exactly the case it declines to touch. The
 * budget's own comment says it exists so "a session left running all day
 * cannot quietly become the reason the app is killed", and this is the
 * way that happened anyway.
 *
 * Dropping these is not the thing enforce_budget() refuses to do. That
 * refusal is about not deleting an image the user is reading; this one is
 * invisible by definition, and if it were not, the covering test would be
 * wrong rather than the policy.
 *
 * z is compared by SIDE, not by value: below-text images are painted in a
 * separate pass, so one of those is not covered by an image above the
 * text however exactly it overlaps.
 */
void bssh_image_store::drop_covered_by(int index) {
	const bssh_image_placement_t cover = m_placements.at(index);

	/* Nothing covers anything without area of its own. */
	if (cover.rows <= 0 || cover.cols <= 0) return;

	for (int i = index - 1; i >= 0; --i) {
		const bssh_image_placement_t &under = m_placements.at(i);

		if ((under.z < 0) != (cover.z < 0)) continue;

		if (under.row < cover.row) continue;
		if (under.col < cover.col) continue;
		if (under.row + under.rows > cover.row + cover.rows) continue;
		if (under.col + under.cols > cover.col + cover.cols) continue;

		m_placements.removeAt(i);
	}
}

void bssh_image_store::remove(int id) {
	/*
	 * Anonymous placements answer to no id; see place(). Without this,
	 * "delete image 0" is a request every sixel frame in the buffer
	 * matches at once.
	 */
	if (id == 0) return;

	for (int i = m_placements.size() - 1; i >= 0; --i) {
		if (m_placements.at(i).id == id) m_placements.removeAt(i);
	}
}

void bssh_image_store::clear() {
	m_placements.clear();
}

void bssh_image_store::scrolled(int top_row, int bottom_row, int lines) {
	if (lines == 0) return;

	for (int i = m_placements.size() - 1; i >= 0; --i) {
		bssh_image_placement_t &placement = m_placements[i];

		/* Only what is inside the scrolling region moves. */
		if (placement.row + placement.rows <= top_row) continue;
		if (placement.row >= bottom_row) continue;

		placement.row -= lines;
	}

	/*
	 * Nothing is dropped for having scrolled off the top -- that is the
	 * scrollback. What drops images is the budget, applied here because a
	 * scroll is the only moment new ones arrive above the fold.
	 */
	enforce_budget();
}

void bssh_image_store::set_memory_budget(qint64 bytes) {
	m_budget = qMax(qint64(0), bytes);
	enforce_budget();
}

qint64 bssh_image_store::memory_used() const {
	qint64 total = 0;
	for (const bssh_image_placement_t &placement : m_placements) {
		total += placement.image.sizeInBytes();
	}
	return total;
}

void bssh_image_store::drop_above_the_fold() {
	for (int i = m_placements.size() - 1; i >= 0; --i) {
		const bssh_image_placement_t &placement = m_placements.at(i);
		if (placement.row + placement.rows > 0) continue;
		m_placements.removeAt(i);
	}
}

void bssh_image_store::enforce_budget() {
	if (m_budget <= 0) return;

	while (memory_used() > m_budget) {
		/*
		 * The one furthest above the screen goes first: it is the oldest
		 * and the one a user would have to scroll longest to reach.
		 */
		int oldest = -1;
		int oldest_row = 0;

		for (int i = 0; i < m_placements.size(); ++i) {
			const bssh_image_placement_t &placement = m_placements.at(i);

			/* Never the one being looked at, whatever it weighs. */
			if (placement.row + placement.rows > 0) continue;

			if (oldest < 0 || placement.row < oldest_row) {
				oldest = i;
				oldest_row = placement.row;
			}
		}

		/*
		 * Everything left is on screen. Over budget and unable to help
		 * it is the honest state: dropping a visible image to satisfy a
		 * number would be a terminal deleting what the user is reading.
		 */
		if (oldest < 0) return;

		m_placements.removeAt(oldest);
	}
}

void bssh_image_store::shift_rows(int lines) {
	if (lines == 0) return;

	for (int i = 0; i < m_placements.size(); ++i) {
		m_placements[i].row -= lines;
	}

	/* Some of them are above the fold now that were not. */
	enforce_budget();
}

void bssh_image_store::trim_to(int rows) {
	for (int i = m_placements.size() - 1; i >= 0; --i) {
		if (m_placements.at(i).row >= rows) m_placements.removeAt(i);
	}
}
