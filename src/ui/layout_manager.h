#ifndef BSSH_LAYOUT_MANAGER_H
#define BSSH_LAYOUT_MANAGER_H

#include <QList>
#include <QObject>
#include <QSize>

#include <functional>
#include <vector>

class bssh_session;

/*
 * The shape of a tiled window: a tree of splits whose leaves are sessions
 * (sec 8.1). A leaf has a session; a split has none, an orientation and
 * two or more children laid out along it -- Qt::Horizontal is side by
 * side, Qt::Vertical is one above the other.
 *
 * std::vector rather than QList because a node holds a list of itself,
 * which the standard guarantees for vector since C++17 and Qt does not
 * promise for QList.
 */
struct bssh_pane_node {
	/* A leaf's FRONT session: the one its pane shows. */
	bssh_session *session = nullptr;

	/*
	 * A leaf's tabs, in their order, the front among them (sec 8.3's bar
	 * per pane). Empty means the front alone, so a leaf built with just a
	 * session -- every leaf before tabs were per pane -- is a pane of one.
	 */
	QList<bssh_session *> tabs;

	Qt::Orientation orientation = Qt::Horizontal;
	std::vector<bssh_pane_node> children;

	bool is_leaf() const { return session != nullptr; }
	bool is_empty() const { return !session && children.empty(); }
	QList<bssh_session *> pane_tabs() const {
		if (!session) return {};
		return tabs.isEmpty() ? QList<bssh_session *>{ session } : tabs;
	}
};

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

	/*
	 * How the visible terminals occupy the window (sec 8.7). This is the
	 * `arrangement` dimension the three-way naming bought, introduced with
	 * its first consumer rather than as a standalone refactor: `column` is
	 * the portrait scrollable stack, `tiled` is everything sec 8.4 built.
	 *
	 * It is set here, defaulting from the idiom (mobile -> column, desktop
	 * -> tiled) and then free to be anything, because `tiled` has to be
	 * runnable on a phone and `column` on a desktop or neither can be
	 * tested without two devices. The manager is TOLD the arrangement and
	 * does not infer it, which keeps it as platform-free as the width it
	 * already takes.
	 *
	 * `row`, the landscape mirror, is deliberately absent: it has no
	 * decision and no consumer (sec 8.7 leaves it open), so an enum value
	 * nothing produces would be the scaffolding this split exists to avoid.
	 */
	enum arrangement_t {
		TILED,   /* side by side, dividers vertical (sec 8.4) */
		COLUMN   /* stacked, holds every session, scrolls (sec 8.7) */
	};

	/*
	 * A column's sanity ceiling, not a design cap: it holds every session
	 * (sec 8.7), and this only stops a runaway from stacking an absurd
	 * number. The common case is one or two.
	 */
	static constexpr int BSSH_COLUMN_PANE_LIMIT = 64;

	/*
	 * A desktop's tiled ceiling, once it has told the layout its room
	 * (set_room): what limits it below this is whether the panes fit, not
	 * a count -- the holder, 2026-10-30, of the old four: "there seems to
	 * be a limit of 4 terminals". Like the column's, a ceiling against a
	 * runaway rather than the answer.
	 */
	static constexpr int BSSH_TILED_PANE_LIMIT = 16;

	/*
	 * Which arrangement a window takes, and how many terminals a column
	 * shows at once (sec 8.7) -- the holder's rule, answered 2026-09-25:
	 * a screen at least 600dp both ways (a Fold unfolded, either way up)
	 * is side by side; anything smaller stacks, one terminal on screen in
	 * landscape and two in portrait, the rest scrolled to. The phone's
	 * cover screen turned sideways is landscape and gets one.
	 *
	 * `screen` is the device's screen in dp, invalid where there is none
	 * to follow (a desktop), which keeps the older answer: touch stacks,
	 * a pointer tiles, and a column shows what fits. `wanted` is the
	 * setting; only AUTOMATIC reads the screen, but a column chosen by
	 * hand on a phone still follows the orientation for its count.
	 *
	 * `visible` is 0 for "no count": the panes share the space and the
	 * column scrolls only when they stop fitting.
	 */
	enum preference_t { PREFER_AUTO, PREFER_TILED, PREFER_COLUMN };
	struct arrangement_choice_t {
		arrangement_t arrangement;
		int visible;
	};
	static arrangement_choice_t choose_arrangement(preference_t wanted, bool touch,
	                                               const QSize &screen);

	explicit bssh_layout_manager(QObject *parent = nullptr);

	static size_class_t size_class_for(int width_dp);

	/*
	 * How many panes may be shown at this width, however they are nested.
	 * Capped at 4 (sec 8.1). This once also held splits to a single level
	 * for v1; the holder asked for panes to be dropped above and below as
	 * well as beside each other (2026-09-25), which is a tree, and the cap
	 * on COUNT is what still keeps a window from being carved into panes
	 * too small to hold a shell.
	 *
	 * This is the TILED answer, keyed on width because side-by-side panes
	 * are what a narrow window cannot hold. A column stacks instead, so it
	 * is not width that limits it -- see max_panes().
	 */
	static int max_panes_for(size_class_t size_class);

	/*
	 * The effective cap right now, arrangement and width together. A
	 * column ignores width entirely, because stacking is not what a
	 * narrow window runs out of room for; tiled defers to max_panes_for.
	 *
	 * The "is there room for another pane" question every split and drop
	 * asks goes through here rather than max_panes_for, or a phone in
	 * column arrangement -- COMPACT, so max_panes_for is 1 -- would refuse
	 * the second terminal the column exists to show.
	 *
	 * This used to say a column shows AT MOST TWO, citing sec 8.7's "one
	 * full-screen or two half-screen", and it was true of the first
	 * increment and of nothing since: the column learned to hold every
	 * session, and BSSH_COLUMN_PANE_LIMIT became the sanity ceiling its
	 * own comment describes. The two claims sat eight lines apart in this
	 * header, and this is the one a reader meets first -- somebody
	 * following it would conclude that Split must be refused at two
	 * panes, which is the opposite of what a column is for. "One
	 * full-screen or two half-screen" remains true of the DIVIDER's
	 * detent, which is where pane_snap.h still says it.
	 */
	int max_panes() const;

	void set_width(int width_dp);
	int width() const { return m_width_dp; }

	/*
	 * The room a desktop's tiled panes have, in pixels: the panes' area,
	 * the least one pane can be (a terminal's 20x4 and its bar), and a
	 * divider. Once set, a wide tiled window shows as many panes as FIT
	 * rather than four: the panes past what fits become tabs, as a narrow
	 * window's do (sec 8.2), and come back when the window grows. Touch
	 * never sets it -- a keyboard opening resizes the window on every
	 * frame, and a rebuild per frame is what made the app untypable once
	 * (see resizeEvent) -- so a tablet keeps the count it had.
	 */
	void set_room(const QSize &area, const QSize &least_pane, int divider);

	/* The least a tree can be laid out in: side by side adds up across,
	 * stacked adds up down, each pane at least `least_pane`. */
	static QSize least_size(const bssh_pane_node &node, const QSize &least_pane, int divider);

	/*
	 * Whether `target`'s pane can be split at `edge` for a new pane: under
	 * the ceiling, and where room is known, the tree with the new pane
	 * still fits. Every split and every drop that makes a pane asks this.
	 *
	 * `moving` is a pane that leaves its place to go there. Moved out of
	 * a stack into a full row it needs a column the window may not have,
	 * and the fold that keeps a tree in its room would then make the LAST
	 * pane a tab -- a drop on one terminal making another disappear.
	 */
	bool room_to_split(bssh_session *target, int edge, bssh_session *moving = nullptr) const;
	size_class_t size_class() const { return size_class_for(m_width_dp); }

	void set_arrangement(arrangement_t arrangement);
	arrangement_t arrangement() const { return m_arrangement; }

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

	/* Out of the tree, wherever it is: a pane left empty closes. */
	void forget(bssh_session *session);

	/*
	 * Each pane is a stack of sessions with a bar of its own (sec 8.3,
	 * agreed 2026-10-10). The intent stays the panes' FRONT sessions, so
	 * everything reading it as a list goes on meaning the panes.
	 */

	/* Every session in the tree, pane by pane, each pane's tabs in order. */
	QList<bssh_session *> sessions() const;

	/* The tabs of the pane holding `member`, in order; empty if none does. */
	QList<bssh_session *> tabs(bssh_session *member) const;

	/* The front of the pane holding `member`, or null. */
	bssh_session *front_of(bssh_session *member) const;

	/*
	 * What the bar of the pane showing `front` lists: its own tabs, and
	 * on the last pane a narrow window shows, the tabs of every pane it
	 * cannot (sec 8.2) -- never closed, reachable from there.
	 */
	QList<bssh_session *> bar_tabs(bssh_session *front) const;

	/*
	 * `session` joins the pane holding `member` as a tab at `slot` (past
	 * the end appends) and comes to the front, leaving wherever it was --
	 * its old pane closes if it was that pane's last tab. False, changing
	 * nothing, when no pane holds `member`.
	 */
	bool add_tab(bssh_session *session, bssh_session *member, int slot);

	/*
	 * The tab picked: `session` becomes the front of its pane. One whose
	 * pane a narrow window does not show trades places with the last pane
	 * it does, so picking it from that pane's bar shows it. False when the
	 * tree does not hold it.
	 */
	bool bring_to_front(bssh_session *session);

	/*
	 * Where a dropped pane goes, relative to the pane it was dropped on
	 * (sec 8.3). An edge splits the target that way -- left and right
	 * side by side, top and bottom one above the other -- and the middle
	 * swaps the two.
	 */
	enum drop_edge_t { EDGE_LEFT, EDGE_RIGHT, EDGE_TOP, EDGE_BOTTOM, EDGE_MIDDLE };

	/*
	 * Puts `session` at `edge` of the pane showing `target`, which must be
	 * in the intent. `session` may already be a pane here (it moves), a
	 * tab in some pane's stack (it leaves that stack), or not in the tree
	 * (it arrives); an edge gives it a pane of its own. The middle swaps:
	 * two fronts swap their panes whole, tabs and all; a tab trades places
	 * with the target, which goes into the bar the tab came from; and an
	 * arrival takes the target's place, the target leaving the tree for
	 * the caller to keep (sec 8.2). The caller asks max_panes() first when
	 * a session arrives; this places what it is told to.
	 *
	 * False, changing nothing, for a target that is not a pane or a
	 * session dropped on itself.
	 */
	bool place(bssh_session *session, bssh_session *target, drop_edge_t edge);

	/*
	 * The whole shape, and the shape as shown: the latter is pruned to
	 * visible_panes(), so a window too narrow for all of it shows the
	 * first panes in their places and the rest as tabs -- the same
	 * truncation the flat list has always had, applied to a tree.
	 *
	 * The leaves in depth-first order ARE the intent, always; everything
	 * that reads the intent as a list goes on working unchanged.
	 */
	const bssh_pane_node &tree() const { return m_tree; }
	bssh_pane_node visible_tree() const;

	/*
	 * Replaces the whole shape -- a restored one (sec 8.4's layout.ini).
	 * Normalized, and the intent becomes its leaves in order.
	 */
	void set_tree(const bssh_pane_node &tree);

	/*
	 * The shape as text and back, for saving it: "H(a,V(b,c))", with each
	 * leaf named by `name` -- a profile id when it is written to disk,
	 * since sessions do not survive a restart and hosts do. A leaf that
	 * `name` gives nothing for is left out, and one that `find` cannot
	 * resolve on the way back is dropped, the shape closing around it:
	 * a host deleted since the save does not come back as a hole.
	 * Unparseable text reads as an empty tree, which callers treat as
	 * "nothing saved".
	 */
	static QString shape_text(const bssh_pane_node &node,
	                          const std::function<QString(bssh_session *)> &name);
	static bssh_pane_node parse_shape(const QString &text,
	                                  const std::function<bssh_session *(const QString &)> &find);

signals:
	/* The visible arrangement changed; the pane container rebuilds. */
	void layout_changed();

private:
	/* Room is known and applies: a desktop's wide, tiled window. */
	bool fit_applies() const;
	bool fits(const bssh_pane_node &tree) const;

	int m_width_dp;
	QSize m_room_area;
	QSize m_room_least;
	int m_room_divider = 0;
	arrangement_t m_arrangement;
	QList<bssh_session *> m_intent;
	bssh_pane_node m_tree;
};

#endif /* BSSH_LAYOUT_MANAGER_H */
