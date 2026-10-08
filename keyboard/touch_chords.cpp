#include "touch_chords.h"

#include <QAbstractButton>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QTouchEvent>
#include <QWidget>

namespace {

/*
 * One mouse event straight to the button, not through the window: each
 * finger has its own button, and the window's single mouse grab would
 * hand the second finger's press to the first one's button.
 */
void send_mouse(QAbstractButton *button, QEvent::Type type, const QPointF &global) {
	const QPointF local = button->mapFromGlobal(global);
	const Qt::MouseButton changed = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
	const Qt::MouseButtons down = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
	QMouseEvent event(type, local, global, changed, down, Qt::NoModifier);
	QCoreApplication::sendEvent(button, &event);
}

} /* namespace */

bssh_touch_chords::bssh_touch_chords(QWidget *board)
    : QObject(board),
      m_board(board) {
	board->setAttribute(Qt::WA_AcceptTouchEvents);
	board->installEventFilter(this);
}

bool bssh_touch_chords::eventFilter(QObject *watched, QEvent *event) {
	if (watched != m_board) return false;

	switch (event->type()) {
	case QEvent::TouchBegin:
	case QEvent::TouchUpdate:
	case QEvent::TouchEnd:
	case QEvent::TouchCancel:
		return touch(static_cast<QTouchEvent *>(event));
	default:
		return false;
	}
}

bool bssh_touch_chords::touch(QTouchEvent *event) {
	if (event->type() == QEvent::TouchCancel) {
		cancel();
		event->accept();
		return true;
	}

	/*
	 * Ours only once a chord is under way (see the header). Ignoring the
	 * rest is what hands them back to Qt, which then makes the mouse
	 * events a single tap and the key row's scroller have always had.
	 */
	bool chord = !m_fingers.isEmpty();
	for (const QEventPoint &point : event->points()) {
		if (point.state() == QEventPoint::State::Pressed
		     && modifier_of(button_at(point.globalPosition())) != 0) {
			chord = true;
		}
	}
	if (!chord) {
		event->ignore();
		return false;
	}

	for (const QEventPoint &point : event->points()) {
		const QPointF global = point.globalPosition();

		switch (point.state()) {
		case QEventPoint::State::Pressed: {
			QAbstractButton *button = button_at(global);
			const finger_t finger = { button, modifier_of(button) };
			m_fingers.insert(point.id(), finger);
			if (!button) break;

			if (finger.modifier != 0) {
				button->setDown(true);
				emit modifier_held(finger.modifier);
			} else {
				send_mouse(button, QEvent::MouseButtonPress, global);
			}
			break;
		}
		case QEventPoint::State::Updated: {
			auto found = m_fingers.find(point.id());
			if (found != m_fingers.end() && found->button && found->modifier == 0) {
				send_mouse(found->button, QEvent::MouseMove, global);
			}
			break;
		}
		case QEventPoint::State::Released: {
			auto found = m_fingers.find(point.id());
			if (found == m_fingers.end()) break;
			finger_t finger = *found;
			m_fingers.erase(found);
			release(finger, global, false);
			break;
		}
		default:
			break;
		}
	}

	event->accept();
	return true;
}

void bssh_touch_chords::cancel() {
	/* Taken first: a release can rebuild the board and call back in. */
	const QHash<int, finger_t> fingers = m_fingers;
	m_fingers.clear();

	for (finger_t finger : fingers) {
		/*
		 * Released OUTSIDE its button, which is how a button is let go
		 * without being clicked: it draws up again and sends nothing.
		 */
		const QPointF away = finger.button
		        ? finger.button->mapToGlobal(QPointF(-1, -1))
		        : QPointF();
		release(finger, away, true);
	}
}

void bssh_touch_chords::release(finger_t &finger, const QPointF &global, bool cancelled) {
	if (finger.modifier != 0) {
		if (finger.button) finger.button->setDown(false);
		emit modifier_released(finger.modifier, cancelled);
		return;
	}
	if (!finger.button) return;

	const QPointF at = cancelled ? finger.button->mapToGlobal(QPointF(-1, -1)) : global;
	send_mouse(finger.button, QEvent::MouseButtonRelease, at);
}

QAbstractButton *bssh_touch_chords::button_at(const QPointF &global) const {
	QWidget *widget = m_board->childAt(m_board->mapFromGlobal(global).toPoint());
	while (widget && widget != m_board) {
		if (QAbstractButton *button = qobject_cast<QAbstractButton *>(widget)) return button;
		widget = widget->parentWidget();
	}
	return nullptr;
}

quint8 bssh_touch_chords::modifier_of(const QAbstractButton *button) const {
	if (!button) return 0;
	return quint8(button->property("bssh_hold_modifier").toUInt());
}
