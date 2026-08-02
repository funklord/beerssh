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

	int pane_count() const { return m_views.size(); }
	bssh_terminal_view *view_at(int index) const;

	void focus_session(bssh_session *session);

	/*
	 * Moves focus one pane left or right. Stops at the ends rather than
	 * wrapping: with two panes, wrapping makes "left" and "right" do the
	 * same thing, and the user loses track of where focus is.
	 */
	void focus_adjacent(int direction);

	/* Rounds pane widths down to whole cells (sec 8.3). */
	void snap_divider_to_cells();

signals:
	/* The focused pane changed -- the window retargets its shortcuts. */
	void focus_changed(bssh_session *session);

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
