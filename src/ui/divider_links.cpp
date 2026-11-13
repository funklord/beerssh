#include "ui/divider_links.h"

#include <algorithm>
#include <cstdlib>

namespace {

/* Whether two dividers' runs meet or nearly: the end of one within `gap`
 * of the other's start, either way round, or overlapping. */
bool continues(const bssh_divider_t &a, const bssh_divider_t &b, int gap) {
	return a.from <= b.to + gap && b.from <= a.to + gap;
}

} /* namespace */

QList<int> bssh_linked_dividers(const QList<bssh_divider_t> &all, int dragged, int tolerance, int gap) {
	if (dragged < 0 || dragged >= all.size()) return {};
	const bssh_divider_t &start = all.at(dragged);
	QList<int> linked{ dragged };
	/*
	 * Grown until nothing more joins: a third row's divider is reached
	 * through the second's, though it does not touch the first's.
	 */
	for (int added = 1; added > 0;) {
		added = 0;
		for (int i = 0; i < all.size(); ++i) {
			if (linked.contains(i)) continue;
			const bssh_divider_t &each = all.at(i);
			if (each.vertical != start.vertical || std::abs(each.at - start.at) > tolerance) continue;
			for (const int j : linked) {
				if (!continues(each, all.at(j), gap)) continue;
				linked.append(i);
				++added;
				break;
			}
		}
	}
	return linked;
}

int bssh_snap_divider(const QList<bssh_divider_t> &all, const QList<int> &linked, int at,
                      int radius, int gap) {
	if (linked.isEmpty()) return -1;
	const bool vertical = all.at(linked.first()).vertical;
	int best = -1;
	int nearest = radius + 1;
	for (int i = 0; i < all.size(); ++i) {
		if (linked.contains(i)) continue;
		const bssh_divider_t &each = all.at(i);
		if (each.vertical != vertical) continue;
		const int off = std::abs(each.at - at);
		if (off > radius || off >= nearest) continue;
		bool beside = false;
		for (const int j : linked) beside = beside || continues(each, all.at(j), gap);
		if (!beside) continue;
		best = i;
		nearest = off;
	}
	return best;
}

namespace {

/*
 * The panes split into runs by straight lines across `orientation`'s
 * direction that cut none of them: side by side (Horizontal) groups by x.
 * A pane starts a new run when it begins past every earlier one's end --
 * a divider lies between them, so strictly past.
 */
std::vector<std::vector<bssh_placed_pane_t>> runs_of(std::vector<bssh_placed_pane_t> panes,
                                                    Qt::Orientation orientation) {
	const bool across = orientation == Qt::Horizontal;
	const auto start = [across](const bssh_placed_pane_t &p) { return across ? p.rect.left() : p.rect.top(); };
	const auto end = [across](const bssh_placed_pane_t &p) { return across ? p.rect.right() : p.rect.bottom(); };
	std::sort(panes.begin(), panes.end(), [&start](const bssh_placed_pane_t &a, const bssh_placed_pane_t &b) {
		return start(a) < start(b);
	});
	std::vector<std::vector<bssh_placed_pane_t>> runs;
	int reach = 0;
	for (const bssh_placed_pane_t &pane : panes) {
		if (runs.empty() || start(pane) > reach) {
			runs.emplace_back();
			reach = end(pane);
		}
		runs.back().push_back(pane);
		reach = std::max(reach, end(pane));
	}
	return runs;
}

bool cut(const std::vector<bssh_placed_pane_t> &panes, Qt::Orientation first, bssh_pane_node *out) {
	if (panes.size() == 1) {
		*out = panes.front().leaf;
		return true;
	}
	const Qt::Orientation other = first == Qt::Horizontal ? Qt::Vertical : Qt::Horizontal;
	for (const Qt::Orientation orientation : { first, other }) {
		const std::vector<std::vector<bssh_placed_pane_t>> runs = runs_of(panes, orientation);
		if (runs.size() < 2) continue;
		bssh_pane_node node;
		node.orientation = orientation;
		for (const std::vector<bssh_placed_pane_t> &run : runs) {
			bssh_pane_node child;
			if (!cut(run, first, &child)) return false;
			/* A run cut the same way as this node is part of it. */
			if (!child.is_leaf() && child.orientation == orientation) {
				for (const bssh_pane_node &grand : child.children) node.children.push_back(grand);
			} else {
				node.children.push_back(child);
			}
		}
		*out = node;
		return true;
	}
	return false;
}

} /* namespace */

bool bssh_tree_from_rects(const std::vector<bssh_placed_pane_t> &panes, Qt::Orientation first,
                          bssh_pane_node *out) {
	if (panes.empty() || !out) return false;
	return cut(panes, first, out);
}
