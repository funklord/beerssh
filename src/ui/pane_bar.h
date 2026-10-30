#ifndef BSSH_PANE_BAR_H
#define BSSH_PANE_BAR_H

#include <QList>
#include <QPointer>
#include <QWidget>

class QTabBar;
class QToolButton;
class bssh_long_press;
class bssh_session;

/*
 * The side of an area nearest `at`, for the tab bar's handle (sec 8.7a):
 * the edge a bar dragged there would move to. Measured as a share of the
 * area, as the pane's own drop zones are, so a wide area and a tall one are
 * divided alike; there is no middle -- every point is nearest some side.
 */
Qt::Edge bssh_bar_side_at(const QPoint &at, const QSize &area);

/*
 * bssh_pane_bar -- one pane's tab bar and its "+" (project.md sec 8.3,
 * the bar per pane agreed 2026-10-10).
 *
 * It shows the tabs it is given and reports what a hand does to them;
 * it decides nothing. Which sessions those are comes from the layout
 * manager, and what picking, closing, moving or tearing one out MEANS is
 * the window's. Rebuilt with its pane on every layout change, so it holds
 * no state worth keeping.
 *
 * Every tab carries its session, so an index is read back as a session at
 * the moment of the event rather than through a list kept beside the bar
 * that can fall out of step with it.
 */
class bssh_pane_bar : public QWidget {
	Q_OBJECT

public:
	/* `front` is the pane's own session; `tabs` what its bar lists. */
	bssh_pane_bar(bssh_session *front, const QList<bssh_session *> &tabs,
	              Qt::Edge edge, bool pointer, QWidget *parent = nullptr);

	bssh_session *front() const { return m_front; }
	QList<bssh_session *> sessions() const;
	QTabBar *tab_bar() const { return m_bar; }

	/* The session behind tab `index`, or null. */
	bssh_session *session_at(int index) const;

	/* Titles, tooltips and status dots again; the tabs are the same. */
	void refresh();

signals:
	void tab_picked(bssh_session *session);
	void close_requested(bssh_session *session);

	/* Moved along the bar to `slot`, in the bar's order after the move. */
	void tab_moved(bssh_session *session, int slot);

	void menu_requested(bssh_session *session, const QPoint &global);

	/* Dragged clear of the bar: the session drag takes over (sec 8.4). */
	void drag_requested(bssh_session *session);

	/* The "+": another session, in this pane. */
	void new_tab_requested();

	/*
	 * The bar's own handle dragged (sec 8.7a): where the pointer is, in
	 * global coordinates, as it moves, and where it was let go.
	 */
	void side_drag_moved(const QPoint &global);
	void side_drag_finished(const QPoint &global);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	bssh_session *m_front;
	QTabBar *m_bar;

	/*
	 * A tab picked under a held button, delivered when it comes up: picking
	 * rebuilds the panes and this bar with them, and doing it on the press
	 * destroyed the bar under the drag that press may be starting -- so a
	 * tab not already in front could never be dragged out.
	 */
	bool m_pressed = false;
	QPointer<bssh_session> m_pending;

	/* And a reorder made under it, applied the same way. */
	QPointer<bssh_session> m_moved;
	int m_moved_to = 0;
	QToolButton *m_add;
	QWidget *m_handle;
	bssh_long_press *m_long_press;
};

#endif /* BSSH_PANE_BAR_H */
