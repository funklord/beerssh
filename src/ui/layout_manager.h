#ifndef BSSH_LAYOUT_MANAGER_H
#define BSSH_LAYOUT_MANAGER_H

#include <QList>
#include <QObject>

class bssh_session;

/*
 * bssh_layout_manager -- the only thing that decides tabs versus splits
 * (project.md sec 8.4).
 *
 * Widgets ask; they never decide. The reason that rule is worth enforcing
 * is that the decision has three inputs -- window width, how many panes
 * the user asked for, and what the layout was before it last collapsed --
 * and any widget that answers it locally will get a different answer from
 * the widget next to it.
 *
 * It takes a WIDTH, in logical pixels, and knows nothing about which
 * platform supplied it (sec 8.1). That is what makes a desktop window
 * dragged narrow behave exactly like a folded foldable, and what makes
 * this testable by handing it numbers.
 */
class bssh_layout_manager : public QObject {
	Q_OBJECT

public:
	/*
	 * Android's window size classes, applied everywhere. The thresholds
	 * are theirs; the point is that they describe the WINDOW, so
	 * multi-window, freeform and tiling window managers all land in the
	 * right class without special cases.
	 */
	enum size_class_t {
		COMPACT,  /* < 600dp  -- phone portrait, narrow desktop window */
		MEDIUM,   /* 600-840  -- tablet portrait, unfolded, half-screen */
		EXPANDED  /* >= 840   -- tablet landscape, maximised desktop */
	};

	explicit bssh_layout_manager(QObject *parent = nullptr);

	static size_class_t size_class_for(int width_dp);

	/*
	 * How many panes may be shown at this width. Capped at 4 with a
	 * single nesting level for v1 (sec 8.1): it covers the realistic
	 * cases and avoids a whole class of layout, focus and drag-target
	 * bugs that an arbitrary tmux-style tree brings.
	 */
	static int max_panes_for(size_class_t size_class);

	void set_width(int width_dp);
	int width() const { return m_width_dp; }
	size_class_t size_class() const { return size_class_for(m_width_dp); }

	/* Sessions the user asked to see side by side, in order. */
	void set_intent(const QList<bssh_session *> &sessions);
	const QList<bssh_session *> &intent() const { return m_intent; }

	/*
	 * What can actually be shown right now: the intent, truncated to what
	 * the current width allows. Everything else is reachable as a tab --
	 * never closed, never disconnected (sec 8.2).
	 */
	QList<bssh_session *> visible_panes() const;

	/* True when the intent does not fit and the rest became tabs. */
	bool is_collapsed() const;

	void forget(bssh_session *session);

signals:
	/* The visible arrangement changed; the pane container rebuilds. */
	void layout_changed();

private:
	int m_width_dp;
	QList<bssh_session *> m_intent;
};

#endif /* BSSH_LAYOUT_MANAGER_H */
