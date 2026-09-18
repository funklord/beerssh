#include "ui/layout_manager.h"

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
	 * A column stacks vertically, so a narrow window does not run out of
	 * room the way side-by-side panes do: it shows one full-screen or two
	 * half-screen (sec 8.7) at any width. Tiled is width's question.
	 */
	if (m_arrangement == COLUMN) return 2;
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
	m_intent = sessions;
	emit layout_changed();
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

	m_intent.removeAll(session);
	emit layout_changed();
}
