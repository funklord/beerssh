#ifndef BSSH_LONG_PRESS_H
#define BSSH_LONG_PRESS_H

#include <QObject>
#include <QPoint>

class QTimer;
class QWidget;

/*
 * Press-and-hold, for the widgets that open a menu with it (project.md
 * sec 6.4, sec 8.3).
 *
 * It exists because Qt does not do this for us and the project needs it
 * twice. **Qt does NOT turn a long press into a context-menu event on
 * Android** -- checked on a device, where a held press on the host list
 * arrived as an ordinary tap and opened the host. Every widget here that
 * says "long-press (touch) or right-click (desktop)" was therefore
 * reachable by exactly one of those two.
 *
 * An event filter rather than a base class or a copy in each widget: the
 * widgets involved are a QWidget and a QListWidget's viewport with
 * nothing in common, and the alternative -- the same timer logic written
 * out twice -- is the shape this project keeps having to unpick.
 *
 * Half a second is Android's own long-press threshold. Matching it is
 * what makes the gesture feel like the rest of the system rather than
 * like this app's idea of a hold.
 */
class bssh_long_press : public QObject {
	Q_OBJECT

public:
	/*
	 * Watches `target`, which is usually a widget's viewport rather than
	 * the widget: a scrolling view delivers its mouse events there.
	 */
	explicit bssh_long_press(QWidget *target, QObject *parent = nullptr);

	/*
	 * Off unless something turns it on. A pointer keeps press-and-hold
	 * meaning "start a selection and think about it", so the widgets that
	 * want this enable it for the layout that has no right button.
	 */
	void set_enabled(bool enabled);
	bool is_enabled() const { return m_enabled; }

signals:
	/*
	 * Global coordinates, because what opens is a popup.
	 *
	 * Emitted when the finger LIFTS after a hold, not when the hold
	 * matures: a popup opened under a finger that is still down is
	 * dismissed by that finger's own release.
	 */
	void triggered(const QPoint &at);

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	QWidget *m_target;
	QTimer *m_timer;
	QPoint m_pressed_at;
	bool m_enabled;
	bool m_matured;
};

#endif /* BSSH_LONG_PRESS_H */
