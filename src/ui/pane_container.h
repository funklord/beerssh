#ifndef BSSH_PANE_CONTAINER_H
#define BSSH_PANE_CONTAINER_H

#include <QList>
#include <QWidget>

class QSplitter;
class bssh_layout_manager;
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

	void focus_session(bssh_session *session);

	/*
	 * Restores every pane to the same width, for sec 8.3's double-click or
	 * double-tap on a divider. Public because it is a plain command --
	 * a menu or a chord could reasonably want it too.
	 */
	void equalize_panes();

	/*
	 * Re-measures every visible pane against the display it is on now
	 * (sec 9.5's multi-monitor case). Each view snaps its cell to whole
	 * device pixels, so a window dragged to a differently-scaled monitor
	 * needs the cell recomputed rather than the old one stretched.
	 */
	void remeasure_panes();

private:
	/* Marks the panes on screen, or away if this widget is not showing. */
	void update_on_screen();

public:

	/*
	 * Moves focus one pane left or right. Stops at the ends rather than
	 * wrapping: with two panes, wrapping makes "left" and "right" do the
	 * same thing, and the user loses track of where focus is.
	 */
	void focus_adjacent(int direction);

	/* Rounds pane widths down to whole cells (sec 8.3). */
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

	/* A view was built or destroyed; the window rewires what it needs. */
	void panes_rebuilt();

private:
	void apply_focus_styling();

	bssh_layout_manager *m_layout;
	QSplitter *m_splitter;
	QList<bssh_terminal_view *> m_views;
	QList<bssh_session *> m_sessions;
	bssh_session *m_focused;
};

#endif /* BSSH_PANE_CONTAINER_H */
