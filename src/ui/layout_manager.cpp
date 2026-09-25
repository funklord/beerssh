#include "ui/layout_manager.h"

#include <functional>

namespace {

void collect_leaves(const bssh_pane_node &node, QList<bssh_session *> &out) {
	if (node.is_leaf()) {
		out.append(node.session);
		return;
	}
	for (const bssh_pane_node &child : node.children) collect_leaves(child, out);
}

/*
 * The one shape each arrangement has: no empty nodes, no split of one
 * child, and no split directly inside another of the same orientation.
 * The last is what makes "is the parent already this way round" a
 * complete answer when placing a pane -- two nested side-by-side splits
 * look like one to the user, so they are one here.
 */
void normalize(bssh_pane_node &node) {
	if (node.is_leaf()) return;

	std::vector<bssh_pane_node> kept;
	for (bssh_pane_node &child : node.children) {
		normalize(child);
		if (child.is_empty()) continue;

		if (!child.is_leaf() && child.orientation == node.orientation) {
			for (bssh_pane_node &grandchild : child.children)
				kept.push_back(std::move(grandchild));
		} else {
			kept.push_back(std::move(child));
		}
	}
	node.children = std::move(kept);

	if (node.children.size() == 1) {
		bssh_pane_node only = std::move(node.children.front());
		node = std::move(only);
	}
}

/* Leaves whose session `keep` refuses go; the shape around them closes up. */
void prune(bssh_pane_node &node, const std::function<bool(bssh_session *)> &keep) {
	if (node.is_leaf()) {
		if (!keep(node.session)) node = bssh_pane_node();
		return;
	}
	for (bssh_pane_node &child : node.children) prune(child, keep);
	normalize(node);
}

/*
 * Hands `order` out to the leaves in depth-first order, so a list that
 * is a permutation of the leaves reorders the sessions and keeps the
 * shape: what the list callers mean by "move this pane along".
 */
void assign_leaves(bssh_pane_node &node, const QList<bssh_session *> &order, int &next) {
	if (node.is_leaf()) {
		if (next < order.size()) node.session = order.at(next);
		++next;
		return;
	}
	for (bssh_pane_node &child : node.children) assign_leaves(child, order, next);
}

bssh_pane_node *find_leaf(bssh_pane_node &node, bssh_session *session,
                          bssh_pane_node **parent) {
	if (node.is_leaf()) return node.session == session ? &node : nullptr;

	for (bssh_pane_node &child : node.children) {
		if (child.is_leaf() && child.session == session) {
			*parent = &node;
			return &child;
		}
		if (bssh_pane_node *found = find_leaf(child, session, parent)) return found;
	}
	return nullptr;
}

bssh_pane_node leaf(bssh_session *session) {
	bssh_pane_node node;
	node.session = session;
	return node;
}

/*
 * A session the tree has never held joins it BESIDE everything else --
 * what "Show beside current" and a split have always meant -- so the
 * common case, a row of panes, is exactly the flat list it always was.
 */
void append_beside(bssh_pane_node &root, bssh_session *session) {
	if (root.is_empty()) {
		root = leaf(session);
		return;
	}
	if (!root.is_leaf() && root.orientation == Qt::Horizontal) {
		root.children.push_back(leaf(session));
		return;
	}

	bssh_pane_node split;
	split.orientation = Qt::Horizontal;
	split.children.push_back(std::move(root));
	split.children.push_back(leaf(session));
	root = std::move(split);
}

} /* namespace */

bssh_layout_manager::bssh_layout_manager(QObject *parent)
    : QObject(parent),
      m_width_dp(0),
      m_arrangement(TILED) {
}

bssh_layout_manager::size_class_t bssh_layout_manager::size_class_for(int width_dp) {
	if (width_dp < 600) return COMPACT;
	if (width_dp < 840) return MEDIUM;
	return EXPANDED;
}

int bssh_layout_manager::max_panes_for(size_class_t size_class) {
	switch (size_class) {
	case COMPACT:
		/*
		 * Tabs only, and this is the call the whole section exists to
		 * make: an 80-column shell needs horizontal room, and two
		 * 40-column panes on a phone are worse than useless.
		 */
		return 1;
	case MEDIUM:
		return 2;
	case EXPANDED:
		return 4;
	}
	return 1;
}

int bssh_layout_manager::max_panes() const {
	/*
	 * A column holds every session the window shows (sec 8.7): it stacks
	 * them and scrolls, so there is nothing to truncate to a tab and the
	 * tab bar's off-by-one goes with the concept. What keeps that cheap is
	 * not a pane cap but that a resize moves one neighbour, and a phone
	 * rarely stacks more than two -- so this is a sanity ceiling against a
	 * runaway, not the "one full or two half" of the common case. Tiled is
	 * width's question, and stays capped by what fits side by side.
	 */
	if (m_arrangement == COLUMN) return BSSH_COLUMN_PANE_LIMIT;
	return max_panes_for(size_class());
}

void bssh_layout_manager::set_arrangement(arrangement_t arrangement) {
	if (arrangement == m_arrangement) return;

	m_arrangement = arrangement;

	/*
	 * A changed arrangement is a changed layout even at a fixed width:
	 * the same two sessions are side by side or stacked, and the pane
	 * container rebuilds to the new shape. So it emits like set_intent
	 * rather than gating on a class change the way set_width does.
	 */
	emit layout_changed();
}

void bssh_layout_manager::set_width(int width_dp) {
	if (width_dp == m_width_dp) return;

	const size_class_t before = size_class_for(m_width_dp);
	m_width_dp = width_dp;
	const size_class_t after = size_class();

	/*
	 * Only a change of CLASS changes the layout. A desktop window drag
	 * produces a continuous stream of widths, and rebuilding the pane
	 * tree on each one would be a rebuild per pixel -- the same reason
	 * the grid only reports real size changes (sec 8.1).
	 */
	if (before != after) emit layout_changed();
}

void bssh_layout_manager::set_intent(const QList<bssh_session *> &sessions) {
	/*
	 * The list callers go on saying what they always said -- these
	 * sessions, in this order -- and the tree keeps whatever shape it can
	 * around that: leaves no longer listed go and their space closes up,
	 * new ones arrive beside the rest, and the order is handed back out
	 * to the leaves. So reordering the list moves sessions between the
	 * places a user built, rather than flattening the places.
	 */
	prune(m_tree, [&sessions](bssh_session *session) { return sessions.contains(session); });

	QList<bssh_session *> present;
	collect_leaves(m_tree, present);
	for (bssh_session *session : sessions) {
		if (!present.contains(session)) append_beside(m_tree, session);
	}

	int next = 0;
	assign_leaves(m_tree, sessions, next);

	m_intent = sessions;
	emit layout_changed();
}

bool bssh_layout_manager::place(bssh_session *session, bssh_session *target,
                                 drop_edge_t edge) {
	if (!session || session == target || !m_intent.contains(target)) return false;

	if (edge == EDGE_MIDDLE) {
		/*
		 * A swap when both are panes; otherwise the arrival takes the
		 * target's place and the target leaves the panes -- still shown,
		 * as a tab, because leaving the intent is never closing (sec 8.2).
		 */
		QList<bssh_session *> order = m_intent;
		const int at = order.indexOf(target);
		const int from = order.indexOf(session);
		if (from >= 0) order.swapItemsAt(from, at);
		else order[at] = session;

		int next = 0;
		assign_leaves(m_tree, order, next);
		m_intent = order;
		emit layout_changed();
		return true;
	}

	/* A pane that is moving leaves its old place first, and that space closes. */
	if (m_intent.contains(session)) {
		prune(m_tree, [session](bssh_session *each) { return each != session; });
	}

	const Qt::Orientation along =
	        (edge == EDGE_LEFT || edge == EDGE_RIGHT) ? Qt::Horizontal : Qt::Vertical;
	const bool before = edge == EDGE_LEFT || edge == EDGE_TOP;

	bssh_pane_node *parent = nullptr;
	bssh_pane_node *found = find_leaf(m_tree, target, &parent);
	if (!found) return false;

	if (parent && parent->orientation == along) {
		/* Already running that way: one more beside the target. */
		std::size_t index = 0;
		while (&parent->children[index] != found) ++index;
		parent->children.insert(parent->children.begin() + long(before ? index : index + 1),
		                        leaf(session));
	} else {
		/* Across the target's run: the target itself is split in two. */
		bssh_pane_node split;
		split.orientation = along;
		split.children.push_back(before ? leaf(session) : leaf(target));
		split.children.push_back(before ? leaf(target) : leaf(session));
		*found = std::move(split);
	}

	normalize(m_tree);

	m_intent.clear();
	collect_leaves(m_tree, m_intent);
	emit layout_changed();
	return true;
}

bssh_pane_node bssh_layout_manager::visible_tree() const {
	const QList<bssh_session *> visible = visible_panes();

	bssh_pane_node shown = m_tree;
	prune(shown, [&visible](bssh_session *session) { return visible.contains(session); });
	return shown;
}

QList<bssh_session *> bssh_layout_manager::visible_panes() const {
	const int allowed = max_panes();

	QList<bssh_session *> visible = m_intent;
	while (visible.size() > allowed) {
		/*
		 * Truncated from the end, so the first pane -- the one the user
		 * started from -- is the one that survives a shrink. The rest
		 * are NOT closed: they stay in the registry and become tabs,
		 * which is the whole of sec 8.2.
		 */
		visible.removeLast();
	}
	return visible;
}

bool bssh_layout_manager::is_collapsed() const {
	return m_intent.size() > visible_panes().size();
}

void bssh_layout_manager::forget(bssh_session *session) {
	if (!m_intent.contains(session)) return;

	QList<bssh_session *> rest = m_intent;
	rest.removeAll(session);
	set_intent(rest);
}
