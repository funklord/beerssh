#include "ui/layout_manager.h"

#include <functional>

#include <QHash>
#include <QStringList>

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

/* Every leaf's tabs, leaf by leaf. */
void collect_all(const bssh_pane_node &node, QList<bssh_session *> &out) {
	if (node.is_leaf()) {
		out.append(node.pane_tabs());
		return;
	}
	for (const bssh_pane_node &child : node.children) collect_all(child, out);
}

/* Each leaf's tabs, keyed by its front, so a front can be moved with them. */
void collect_stacks(const bssh_pane_node &node,
                    QHash<bssh_session *, QList<bssh_session *>> &out) {
	if (node.is_leaf()) {
		out.insert(node.session, node.tabs);
		return;
	}
	for (const bssh_pane_node &child : node.children) collect_stacks(child, out);
}

/*
 * Hands `order` out to the leaves in depth-first order, so a list that
 * is a permutation of the leaves reorders the sessions and keeps the
 * shape: what the list callers mean by "move this pane along". A front
 * takes its tabs with it (sec 8.3: the pane moves, not one session of
 * it); one new to the tree arrives alone.
 */
void assign_leaves(bssh_pane_node &node, const QList<bssh_session *> &order, int &next,
                   const QHash<bssh_session *, QList<bssh_session *>> &stacks) {
	if (node.is_leaf()) {
		if (next < order.size()) {
			node.session = order.at(next);
			node.tabs = stacks.value(node.session);
		}
		++next;
		return;
	}
	for (bssh_pane_node &child : node.children) assign_leaves(child, order, next, stacks);
}

void assign_leaves(bssh_pane_node &root, const QList<bssh_session *> &order) {
	QHash<bssh_session *, QList<bssh_session *>> stacks;
	collect_stacks(root, stacks);
	int next = 0;
	assign_leaves(root, order, next, stacks);
}

/* The leaf whose tabs hold `session`, front or not. */
bssh_pane_node *find_pane(bssh_pane_node &node, bssh_session *session) {
	if (node.is_leaf()) return node.pane_tabs().contains(session) ? &node : nullptr;
	for (bssh_pane_node &child : node.children) {
		if (bssh_pane_node *found = find_pane(child, session)) return found;
	}
	return nullptr;
}

const bssh_pane_node *find_pane(const bssh_pane_node &node, bssh_session *session) {
	return find_pane(const_cast<bssh_pane_node &>(node), session);
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
 * `session` out of whichever pane holds it. Another tab takes the front
 * if it was there -- the one after it, or before it at the end, as a
 * browser does -- and a pane left with nothing closes, its split closing
 * around it. Nothing happens to a session the tree does not hold.
 */
void take_out(bssh_pane_node &root, bssh_session *session) {
	bssh_pane_node *pane = find_pane(root, session);
	if (!pane) return;

	QList<bssh_session *> rest = pane->pane_tabs();
	const int at = rest.indexOf(session);
	rest.removeAt(at);
	if (rest.isEmpty()) {
		prune(root, [session](bssh_session *each) { return each != session; });
		return;
	}
	if (pane->session == session) pane->session = rest.at(qMin(at, int(rest.size()) - 1));
	pane->tabs = rest.size() > 1 ? rest : QList<bssh_session *>();
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

bssh_layout_manager::arrangement_choice_t
bssh_layout_manager::choose_arrangement(preference_t wanted, bool touch, const QSize &screen) {
	if (wanted == PREFER_TILED) return {TILED, 0};

	arrangement_t arrangement = COLUMN;
	if (wanted == PREFER_AUTO) {
		if (!touch) arrangement = TILED;
		else if (screen.isValid()
		         && qMin(screen.width(), screen.height()) >= 600) arrangement = TILED;
	}
	if (arrangement == TILED) return {TILED, 0};

	/*
	 * One on screen held sideways, two held upright. Only where there is
	 * a device to hold: a desktop's column shows what fits.
	 */
	if (!screen.isValid()) return {COLUMN, 0};
	return {COLUMN, screen.width() > screen.height() ? 1 : 2};
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
	if (fit_applies()) return BSSH_TILED_PANE_LIMIT;
	return max_panes_for(size_class());
}

bool bssh_layout_manager::fit_applies() const {
	return m_arrangement == TILED && size_class() == EXPANDED
	       && m_room_area.isValid() && !m_room_area.isEmpty() && m_room_least.isValid();
}

QSize bssh_layout_manager::least_size(const bssh_pane_node &node, const QSize &least_pane,
                                      int divider) {
	if (node.is_leaf()) return least_pane;
	if (node.children.empty()) return QSize(0, 0);
	const bool across = node.orientation == Qt::Horizontal;
	int along = divider * int(node.children.size() - 1);
	int thick = 0;
	for (const bssh_pane_node &child : node.children) {
		const QSize least = least_size(child, least_pane, divider);
		along += across ? least.width() : least.height();
		thick = qMax(thick, across ? least.height() : least.width());
	}
	return across ? QSize(along, thick) : QSize(thick, along);
}

bool bssh_layout_manager::fits(const bssh_pane_node &tree) const {
	const QSize least = least_size(tree, m_room_least, m_room_divider);
	return least.width() <= m_room_area.width() && least.height() <= m_room_area.height();
}

void bssh_layout_manager::set_room(const QSize &area, const QSize &least_pane, int divider) {
	if (area == m_room_area && least_pane == m_room_least && divider == m_room_divider) return;
	const QList<bssh_session *> before = visible_panes();
	m_room_area = area;
	m_room_least = least_pane;
	m_room_divider = divider;

	/* A rebuild only when what is shown changes: a drag of the window's
	 * edge is a resize per pixel, and most of them fit as before. */
	if (visible_panes() != before) emit layout_changed();
}

bool bssh_layout_manager::room_to_split(bssh_session *target, int edge) const {
	if (m_intent.size() >= max_panes()) return false;
	if (!fit_applies()) return true;

	/* The tree as shown, with the target's pane halved at that edge. */
	bssh_pane_node tree = visible_tree();
	bssh_pane_node *leaf = find_pane(tree, target);
	if (!leaf || !leaf->is_leaf()) return true;
	bssh_pane_node half = *leaf;
	bssh_pane_node split;
	split.orientation = edge == EDGE_TOP || edge == EDGE_BOTTOM ? Qt::Vertical : Qt::Horizontal;
	split.children.push_back(half);
	split.children.push_back(half);
	*leaf = split;
	return fits(tree);
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
		if (present.contains(session)) continue;

		/* A tab named as a pane leaves its stack for a pane of its own. */
		take_out(m_tree, session);
		append_beside(m_tree, session);
	}

	assign_leaves(m_tree, sessions);

	m_intent = sessions;
	emit layout_changed();
}

bool bssh_layout_manager::place(bssh_session *session, bssh_session *target,
                                 drop_edge_t edge) {
	if (!session || session == target || !m_intent.contains(target)) return false;

	if (edge == EDGE_MIDDLE) {
		bssh_pane_node *there = find_pane(m_tree, target);
		bssh_pane_node *here = find_pane(m_tree, session);

		if (m_intent.contains(session) || !here) {
			/*
			 * Two fronts: the panes swap whole, their tabs going with
			 * them. An arrival takes the target's place and the target
			 * leaves the tree -- still shown, for the caller to keep,
			 * because leaving is never closing (sec 8.2).
			 */
			QList<bssh_session *> order = m_intent;
			const int at = order.indexOf(target);
			const int from = order.indexOf(session);
			if (from >= 0) {
				order.swapItemsAt(from, at);
				assign_leaves(m_tree, order);
			} else {
				QList<bssh_session *> tabs = there->pane_tabs();
				tabs[tabs.indexOf(target)] = session;
				there->session = session;
				there->tabs = tabs.size() > 1 ? tabs : QList<bssh_session *>();
			}
		} else if (here == there) {
			/* A tab of the target's own pane: it simply comes to the front. */
			there->session = session;
		} else {
			/*
			 * A tab of another pane trades places with the target: it
			 * fronts the target's pane in the target's slot, and the
			 * target goes into the bar it came from, in its slot.
			 */
			QList<bssh_session *> to = there->pane_tabs();
			QList<bssh_session *> from = here->pane_tabs();
			to[to.indexOf(target)] = session;
			from[from.indexOf(session)] = target;
			there->session = session;
			there->tabs = to.size() > 1 ? to : QList<bssh_session *>();
			here->tabs = from.size() > 1 ? from : QList<bssh_session *>();
		}

		m_intent.clear();
		collect_leaves(m_tree, m_intent);
		emit layout_changed();
		return true;
	}

	/* Whatever it was, it leaves its old place first, and that space closes. */
	take_out(m_tree, session);

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
		/*
		 * Across the target's run: the target itself is split in two --
		 * the target's node as it is, tabs and all, not a new leaf of it
		 * alone, which dropped every tab behind it.
		 */
		bssh_pane_node kept = *found;
		bssh_pane_node split;
		split.orientation = along;
		split.children.push_back(before ? leaf(session) : kept);
		split.children.push_back(before ? kept : leaf(session));
		*found = std::move(split);
	}

	normalize(m_tree);

	m_intent.clear();
	collect_leaves(m_tree, m_intent);
	emit layout_changed();
	return true;
}

void bssh_layout_manager::set_tree(const bssh_pane_node &tree) {
	m_tree = tree;
	normalize(m_tree);

	m_intent.clear();
	collect_leaves(m_tree, m_intent);
	emit layout_changed();
}

QString bssh_layout_manager::shape_text(const bssh_pane_node &node,
                                        const std::function<QString(bssh_session *)> &name) {
	if (node.is_leaf()) {
		/*
		 * A pane of tabs as "b|*a|c": in their order, the front marked.
		 * Those with no name are left out; the front's going, the first
		 * left takes its mark.
		 */
		const QList<bssh_session *> tabs = node.pane_tabs();
		QStringList named;
		int front = -1;
		for (bssh_session *each : tabs) {
			const QString text = name(each);
			if (text.isEmpty()) continue;
			if (each == node.session) front = int(named.size());
			named.append(text);
		}
		if (named.size() < 2) return named.value(0);
		named[qMax(0, front)].prepend(QLatin1Char('*'));
		return named.join(QLatin1Char('|'));
	}
	if (node.is_empty()) return QString();

	QStringList parts;
	for (const bssh_pane_node &child : node.children) {
		const QString part = shape_text(child, name);
		if (!part.isEmpty()) parts.append(part);
	}
	if (parts.isEmpty()) return QString();
	if (parts.size() == 1) return parts.first();

	return (node.orientation == Qt::Horizontal ? QStringLiteral("H(") : QStringLiteral("V("))
	       + parts.join(QLatin1Char(',')) + QLatin1Char(')');
}

namespace {

/*
 * One node from `text` at `at`: a split "H(" or "V(", its children and
 * ")", or a leaf running to the next ',' or ')'. False on anything else,
 * and the caller then keeps nothing -- a half-read shape is not one the
 * user made.
 */
bool parse_node(const QString &text, int &at, bssh_pane_node &out,
                const std::function<bssh_session *(const QString &)> &find) {
	if (at + 1 < text.size() && text.at(at + 1) == QLatin1Char('(')
	    && (text.at(at) == QLatin1Char('H') || text.at(at) == QLatin1Char('V'))) {
		out.orientation = text.at(at) == QLatin1Char('H') ? Qt::Horizontal : Qt::Vertical;
		at += 2;

		for (;;) {
			bssh_pane_node child;
			if (!parse_node(text, at, child, find)) return false;
			if (!child.is_empty()) out.children.push_back(std::move(child));

			if (at >= text.size()) return false;
			if (text.at(at) == QLatin1Char(')')) { ++at; return true; }
			if (text.at(at) != QLatin1Char(',')) return false;
			++at;
		}
	}

	const int start = at;
	while (at < text.size() && text.at(at) != QLatin1Char(',') && text.at(at) != QLatin1Char(')'))
		++at;
	if (at == start) return false;

	/* "b|*a|c": tabs, the front marked; hosts not found are dropped. */
	QList<bssh_session *> tabs;
	bssh_session *front = nullptr;
	for (QString part : text.mid(start, at - start).split(QLatin1Char('|'))) {
		const bool marked = part.startsWith(QLatin1Char('*'));
		if (marked) part.remove(0, 1);
		bssh_session *found = part.isEmpty() ? nullptr : find(part);
		if (!found || tabs.contains(found)) continue;
		tabs.append(found);
		if (marked) front = found;
	}
	if (tabs.isEmpty()) return true;
	out.session = front ? front : tabs.first();
	if (tabs.size() > 1) out.tabs = tabs;
	return true;
}

} /* namespace */

bssh_pane_node bssh_layout_manager::parse_shape(
        const QString &text, const std::function<bssh_session *(const QString &)> &find) {
	bssh_pane_node tree;
	int at = 0;
	if (text.isEmpty() || !parse_node(text, at, tree, find) || at != text.size())
		return bssh_pane_node();

	normalize(tree);
	return tree;
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

	/* And on a desktop, the same from the end until what is left fits. */
	if (fit_applies()) {
		while (visible.size() > 1) {
			bssh_pane_node shown = m_tree;
			prune(shown, [&visible](bssh_session *session) { return visible.contains(session); });
			if (fits(shown)) break;
			visible.removeLast();
		}
	}
	return visible;
}

bool bssh_layout_manager::is_collapsed() const {
	return m_intent.size() > visible_panes().size();
}

void bssh_layout_manager::forget(bssh_session *session) {
	if (!find_pane(m_tree, session)) return;

	take_out(m_tree, session);
	normalize(m_tree);
	m_intent.clear();
	collect_leaves(m_tree, m_intent);
	emit layout_changed();
}

QList<bssh_session *> bssh_layout_manager::sessions() const {
	QList<bssh_session *> all;
	collect_all(m_tree, all);
	return all;
}

QList<bssh_session *> bssh_layout_manager::tabs(bssh_session *member) const {
	const bssh_pane_node *pane = find_pane(m_tree, member);
	return pane ? pane->pane_tabs() : QList<bssh_session *>();
}

bssh_session *bssh_layout_manager::front_of(bssh_session *member) const {
	const bssh_pane_node *pane = find_pane(m_tree, member);
	return pane ? pane->session : nullptr;
}

QList<bssh_session *> bssh_layout_manager::bar_tabs(bssh_session *front) const {
	QList<bssh_session *> bar = tabs(front);
	const QList<bssh_session *> visible = visible_panes();
	if (visible.isEmpty() || front != visible.last()) return bar;

	for (bssh_session *hidden : m_intent) {
		if (!visible.contains(hidden)) bar.append(tabs(hidden));
	}
	return bar;
}

bool bssh_layout_manager::add_tab(bssh_session *session, bssh_session *member, int slot) {
	bssh_pane_node *pane = find_pane(m_tree, member);
	if (!session || !pane) return false;

	QList<bssh_session *> list = pane->pane_tabs();
	const int from = list.indexOf(session);
	if (from >= 0) {
		/* Along its own bar: the slot counts the tabs as they are now. */
		list.removeAt(from);
		if (slot > from) --slot;
	} else {
		/* From another pane, which closes if this was its last tab. */
		take_out(m_tree, session);
		pane = find_pane(m_tree, member);
		list = pane->pane_tabs();
	}

	list.insert(qBound(0, slot, int(list.size())), session);
	pane->session = session;
	pane->tabs = list.size() > 1 ? list : QList<bssh_session *>();

	normalize(m_tree);
	m_intent.clear();
	collect_leaves(m_tree, m_intent);
	emit layout_changed();
	return true;
}

bool bssh_layout_manager::bring_to_front(bssh_session *session) {
	bssh_pane_node *pane = find_pane(m_tree, session);
	if (!pane) return false;

	pane->session = session;
	m_intent.clear();
	collect_leaves(m_tree, m_intent);

	/* Its pane not shown: it trades places with the last that is. */
	const QList<bssh_session *> visible = visible_panes();
	if (!visible.isEmpty() && !visible.contains(session)) {
		QList<bssh_session *> order = m_intent;
		order.swapItemsAt(order.indexOf(session), order.indexOf(visible.last()));
		assign_leaves(m_tree, order);
		m_intent = order;
	}

	emit layout_changed();
	return true;
}
