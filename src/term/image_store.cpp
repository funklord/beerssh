#include "term/image_store.h"

bssh_image_store::bssh_image_store() : m_next_id(1) {
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

		if (placement.row + placement.rows <= 0) m_placements.removeAt(i);
	}
}

void bssh_image_store::trim_to(int rows) {
	for (int i = m_placements.size() - 1; i >= 0; --i) {
		if (m_placements.at(i).row >= rows) m_placements.removeAt(i);
	}
}
