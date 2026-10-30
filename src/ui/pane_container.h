#ifndef BSSH_PANE_CONTAINER_H
#define BSSH_PANE_CONTAINER_H

#include <QList>
#include <QWidget>

#include "ui/input_mode.h"

#include <functional>

class QMimeData;
class QScrollArea;
class QSplitter;
class QVBoxLayout;
class bssh_layout_manager;
struct bssh_pane_node;
class bssh_session;
class bssh_terminal_view;

/*
 * bssh_pane_container -- the terminal views, one per visible pane
 * (project.md sec 8).
 *
 * It asks bssh_layout_manager what to show and builds that; it decides
 * nothing itself (sec 8.4). Rebuilding is cheap and therefore the whole
 * strategy: views are destroyed and recreated on every layout change,
 * which is only safe because sessions live in the registry and a view
 * owns nothing (sec 3, core principle 2).
 *
 * Exactly one pane holds focus and receives every keystroke (sec 6.3),
 * and it says so visibly -- two identical shells side by side is a
 * genuine footgun when the user cannot tell which is about to receive
 * `rm -rf`.
 */
class bssh_pane_container : public QWidget {
	Q_OBJECT

public:
	explicit bssh_pane_container(bssh_layout_manager *layout, QWidget *parent = nullptr);

	/* Rebuilds the panes from the layout manager's current answer. */
	void rebuild();

	bssh_session *focused_session() const { return m_focused; }
	bssh_terminal_view *focused_view() const;

	/* Every visible pane's view, for settings that apply to all of them
	 * rather than to the focused one (sec 6.2). */
	const QList<bssh_terminal_view *> &views() const { return m_views; }

	int pane_count() const { return m_views.size(); }
	bssh_terminal_view *view_at(int index) const;

	/*
	 * The view showing this session, or nullptr.
	 *
	 * m_sessions and m_views are built together and stay in step, so the
	 * index of one is the index of the other. Needed so the window can
	 * mark the pane a drag picked up.
	 */
	bssh_terminal_view *view_for(bssh_session *session) const;

	void focus_session(bssh_session *session);

	/*
	 * Restores every pane to the same width, for sec 8.3's double-click or
	 * double-tap on a divider. Public because it is a plain command --
	 * a menu or a chord could reasonably want it too. Every splitter; a
	 * double-click equalizes only the one whose divider it was on.
	 */
	void equalize_panes();

	/*
	 * How many terminals a column shows at once (sec 8.7): 1 held
	 * sideways, 2 upright, 0 for "whatever fits". Applied as the column's
	 * TOTAL height -- the viewport times panes over this count -- rather
	 * than a minimum per pane, so the divider between two stacked panes
	 * stays draggable. Ignored outside a column.
	 */
	void set_column_visible(int count);
	int column_visible() const { return m_column_visible; }

	/*
	 * Re-measures every visible pane against the display it is on now
	 * (sec 9.5's multi-monitor case). Each view snaps its cell to whole
	 * device pixels, so a window dragged to a differently-scaled monitor
	 * needs the cell recomputed rather than the old one stretched.
	 */
	void remeasure_panes();

	/*
	 * Scrolls the column's viewport by a delta (sec 8.7's Pan state): the
	 * content follows the finger, so a downward drag reveals the panes
	 * above. A no-op outside the column arrangement, where the splitter is
	 * not in the scroll area and there is nothing to scroll.
	 */
	void scroll_column_by(int delta);

	/*
	 * Settles that scroll on a pane top when the gesture ends (sec 8.7's
	 * "a terminal starts at the top of the viewport rather than half off
	 * it"). Separate from scroll_column_by because a snap applied to
	 * every step would fight the finger: the drag is free and the
	 * release is what lands it. The math is bssh_settle_column_scroll,
	 * in ui/pane_snap.h, so it can be asserted without a widget.
	 */
	void settle_column_scroll();

	/*
	 * Who decides what a session dropped on a pane does (sec 8.4): given
	 * the payload, the pane's own session and the zone, a drop_edge_t,
	 * BSSH_DROP_CANCEL or BSSH_DROP_REFUSED. Handed to every pane built
	 * from now on, and to the ones already up.
	 */
	void set_drop_planner(std::function<int(const QMimeData *, bssh_session *, int)> planner);

	/*
	 * A tab bar per pane (sec 8.3): each pane is a frame holding its bar
	 * and its view, and the bar is the WINDOW's to build -- what a tab
	 * does when picked, closed, dragged or right-clicked is the window's
	 * business, not this container's. Called for every pane on every
	 * rebuild with the pane's front session; null for no bar.
	 */
	void set_bar_factory(std::function<QWidget *(bssh_session *front, QWidget *parent)> factory);

	/* Which side of each pane its bar sits on; the tab position setting. */
	void set_bar_edge(Qt::Edge edge);
	Qt::Edge bar_edge() const { return m_bar_edge; }

	/* The frame a pane is drawn in -- its bar and its view -- or null. */
	QWidget *frame_for(bssh_session *front) const;

	/* The bar the factory built for that pane, or null. */
	QWidget *bar_for(bssh_session *front) const;

private:
	std::function<int(const QMimeData *, bssh_session *, int)> m_drop_planner;
	void apply_drop_planner(bssh_terminal_view *view, bssh_session *session);
	/* Marks the panes on screen, or away if this widget is not showing. */
	void update_on_screen();

public:

	/*
	 * Moves focus one pane left or right. Stops at the ends rather than
	 * wrapping: with two panes, wrapping makes "left" and "right" do the
	 * same thing, and the user loses track of where focus is.
	 */
	void focus_adjacent(int direction);

	/* Rounds pane widths down to whole cells (sec 8.3), in every splitter. */
	void snap_divider_to_cells();

protected:
	/*
	 * Watches the splitter's own handles for sec 8.3's double-click or
	 * double-tap to equalize. QSplitter raises no signal for it, and the
	 * handles are made and unmade with the panes, so the filter is
	 * installed on each rebuild rather than once.
	 */
	bool eventFilter(QObject *watched, QEvent *event) override;

	/*
	 * The window going and coming back is the other half of "on screen"
	 * (sec 8.3): hidden to the tray or moved to the back, the panes are
	 * still laid out and nobody is looking at them.
	 */
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

signals:
	/* The focused pane changed -- the window retargets its shortcuts. */
	void focus_changed(bssh_session *session);

	/*
	 * Alt+drag on a pane (sec 8.4). Named here rather than in the view,
	 * which has no idea which session it is showing -- the container is
	 * the one object that holds both halves of that pair.
	 *
	 * Wired for EVERY view at rebuild time, not for the focused one:
	 * dragging the pane you are not typing in is the ordinary case, and
	 * a signal connected only on focus would make it work the second
	 * time you tried.
	 */
	/*
	 * A tap in some pane asked for a keyboard (sec 6.2). Which pane does
	 * not matter: there is one on-screen keyboard for the window.
	 */
	void input_panel_requested();

	void pane_drag_requested(bssh_session *session);

	/*
	 * A pane's corner chose an input mode (sec 8.7a). The mode is the
	 * window's, one for every pane, so the window applies it to all.
	 */
	void input_mode_chosen(bssh_input_mode_t mode);

	/*
	 * A pane's corner asked for the tabs of the pane showing `front`, or
	 * for the menus (sec 8.7a). The window builds both.
	 */
	void corner_tabs_requested(bssh_session *front, const QPoint &global);
	void corner_menu_requested(const QPoint &global);

	/*
	 * A pane or tab was dropped on the pane showing `target`, at `edge`
	 * (a bssh_layout_manager::drop_edge_t). Direct, not queued: the drop's
	 * data is valid only while the drop is being delivered, so whoever
	 * reads the session out of it has to do so now -- and must then defer
	 * anything that rebuilds the panes, since the view delivering this is
	 * one of them.
	 */
	void session_dropped(const QMimeData *mime, bssh_session *target, int edge);

	/*
	 * A body drag in Move mode reordered this pane (sec 8.7): move the
	 * session one place in the shown list, negative toward the top.
	 * Queued for the same reason as pane_drag_requested -- the move
	 * rebuilds the panes and destroys the view that asked for it.
	 */
	void pane_move_requested(bssh_session *session, int direction);

	/* A view was built or destroyed; the window rewires what it needs. */
	void panes_rebuilt();

	/*
	 * A pane's find matches changed without a search being asked for
	 * (sec 6.4) -- the history under them was dropped or trimmed. The
	 * window owns the find bar and pulls the count, so it has to be told
	 * or the bar keeps a number that was true a moment ago.
	 */
	void find_matches_changed();

private:
	void apply_focus_styling();

	/*
	 * The panes as QSplitters: one flat splitter for a column, and for a
	 * tiled window one splitter per split in the layout's tree (sec 8.1),
	 * nested as the tree is. Views are created in the tree's depth-first
	 * order, which is the order of m_sessions, so view_at(i) still names
	 * the i-th session.
	 */
	void build_panes(const QList<bssh_session *> &panes);
	void add_split_children(const bssh_pane_node &node, QSplitter *into);
	bssh_terminal_view *add_pane_view(bssh_session *session, QSplitter *parent);

	/* Handle width, collapsing and the snap, the same for every splitter. */
	void configure_splitter(QSplitter *splitter);
	QList<QSplitter *> all_splitters() const;
	void snap_splitter(QSplitter *splitter);
	void equalize_splitter(QSplitter *splitter);

	/* Sizes the column for m_column_visible; again whenever the viewport moves. */
	void apply_column_visible();
	int m_column_visible = 0;

	bssh_layout_manager *m_layout;
	QVBoxLayout *m_outer;
	QSplitter *m_splitter;

	/*
	 * A column can hold more sessions than fit the screen (sec 8.7), so
	 * its splitter lives in a scroll area that scrolls when they overflow
	 * -- built once, and the splitter is moved into it only in the column
	 * arrangement, so a tiled window is byte-for-byte what it was. The
	 * scroll's FEEL (kinetic flick, snap to a pane top) waits for a device
	 * pass; this is reachability, not yet the gesture.
	 */
	QScrollArea *m_scroll;

	/* One per pane, in step with m_views: what holds its bar and its view. */
	QList<QWidget *> m_frames;
	QList<QWidget *> m_bars;
	std::function<QWidget *(bssh_session *, QWidget *)> m_bar_factory;
	Qt::Edge m_bar_edge = Qt::TopEdge;
	bool m_rebuilding = false;

	QList<bssh_terminal_view *> m_views;
	QList<bssh_session *> m_sessions;
	bssh_session *m_focused;
};

#endif /* BSSH_PANE_CONTAINER_H */
