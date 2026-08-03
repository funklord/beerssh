#include "term/image_store.h"

/*
 * 32 MB of decoded image, which is roughly a dozen full-screen phone
 * screenshots or a very great many plots. Chosen to be generous enough
 * that normal use never meets it and small enough that a session left
 * running all day cannot quietly become the reason the app is killed
 * (sec 5.1). It is a per-session budget: the count that matters to
 * Android is the process total, and sessions are what multiply.
 */
static const qint64 BSSH_DEFAULT_IMAGE_BUDGET = 32 * 1024 * 1024;

bssh_image_store::bssh_image_store() : m_next_id(1), m_budget(BSSH_DEFAULT_IMAGE_BUDGET) {
}

int bssh_image_store::place(const bssh_image_placement_t &placement) {
	if (placement.image.isNull()) return 0;

	bssh_image_placement_t stored = placement;
	if (stored.id == 0) stored.id = m_next_id++;

	/*
	 * A protocol may re-transmit an existing id to replace the image
	 * rather than to add a second copy of it.
	 */
	for (int i = 0; i < m_placements.size(); ++i) {
		if (m_placements.at(i).id == stored.id) {
			m_placements[i] = stored;
			return stored.id;
		}
	}

	m_placements.append(stored);
	return stored.id;
}

void bssh_image_store::remove(int id) {
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

void bssh_image_store::trim_to(int rows) {
	for (int i = m_placements.size() - 1; i >= 0; --i) {
		if (m_placements.at(i).row >= rows) m_placements.removeAt(i);
	}
}
