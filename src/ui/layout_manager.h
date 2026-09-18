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
	static const int BSSH_COLUMN_PANE_LIMIT = 64;

	explicit bssh_layout_manager(QObject *parent = nullptr);

	static size_class_t size_class_for(int width_dp);

	/*
	 * How many panes may be shown at this width. Capped at 4 with a
	 * single nesting level for v1 (sec 8.1): it covers the realistic
	 * cases and avoids a whole class of layout, focus and drag-target
	 * bugs that an arbitrary tmux-style tree brings.
	 *
	 * This is the TILED answer, keyed on width because side-by-side panes
	 * are what a narrow window cannot hold. A column stacks instead, so it
	 * is not width that limits it -- see max_panes().
	 */
	static int max_panes_for(size_class_t size_class);

	/*
	 * The effective cap right now, arrangement and width together. A
	 * column shows at most two (sec 8.7: one full-screen or two
	 * half-screen), regardless of width, because stacking is not what a
	 * narrow window runs out of room for. Tiled defers to max_panes_for.
	 *
	 * The "is there room for another pane" question every split and drop
	 * asks goes through here rather than max_panes_for, or a phone in
	 * column arrangement -- COMPACT, so max_panes_for is 1 -- would refuse
	 * the second terminal the column exists to show.
	 */
	int max_panes() const;

	void set_width(int width_dp);
	int width() const { return m_width_dp; }
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

	void forget(bssh_session *session);

signals:
	/* The visible arrangement changed; the pane container rebuilds. */
	void layout_changed();

private:
	int m_width_dp;
	arrangement_t m_arrangement;
	QList<bssh_session *> m_intent;
};

#endif /* BSSH_LAYOUT_MANAGER_H */
