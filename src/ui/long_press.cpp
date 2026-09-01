#include "ui/long_press.h"

#include <QMouseEvent>
#include <QTimer>
#include <QWidget>

namespace {

/* Android's own long-press threshold; see the header. */
const int BSSH_HOLD_MS = 500;

/*
 * How far a finger may wander and still count as holding still. A thumb
 * never holds perfectly still, and cancelling on one pixel would make the
 * gesture impossible to perform; letting it wander further would swallow
 * the start of a drag-select.
 */
const int BSSH_HOLD_SLOP_PX = 12;

} /* namespace */

bssh_long_press::bssh_long_press(QWidget *target, QObject *parent)
    : QObject(parent),
      m_target(target),
      m_timer(new QTimer(this)),
      m_enabled(false),
      m_matured(false) {
	m_timer->setSingleShot(true);
	m_timer->setInterval(BSSH_HOLD_MS);

	/*
	 * The timer marks the hold as matured; it does not raise the menu.
	 *
	 * Raising it here was the first version, and it did not work on a
	 * device: the popup opens while the finger is still down, and the
	 * release that follows is delivered to the popup and dismisses it
	 * again. The signal fired, the menu appeared and vanished inside the
	 * same gesture, and every screenshot showed nothing.
	 */
	connect(m_timer, &QTimer::timeout, this, [this] { m_matured = true; });

	if (m_target) m_target->installEventFilter(this);
}

void bssh_long_press::set_enabled(bool enabled) {
	m_enabled = enabled;
	if (!enabled) m_timer->stop();
}

bool bssh_long_press::eventFilter(QObject *watched, QEvent *event) {
	if (watched != m_target || !m_enabled) return QObject::eventFilter(watched, event);

	switch (event->type()) {
	case QEvent::MouseButtonPress: {
		const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
		if (mouse->button() == Qt::LeftButton) {
			m_pressed_at = mouse->pos();
			m_matured = false;
			m_timer->start();
		}
		break;
	}

	case QEvent::MouseMove: {
		/*
		 * Movement means a drag, not a hold -- a selection in the
		 * terminal, a flick-scroll in a list.
		 */
		const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
		if ((m_timer->isActive() || m_matured)
		     && (mouse->pos() - m_pressed_at).manhattanLength() > BSSH_HOLD_SLOP_PX) {
			m_timer->stop();
			m_matured = false;
		}
		break;
	}

	case QEvent::MouseButtonRelease: {
		m_timer->stop();
		if (!m_matured) break;

		/*
		 * The hold matured and the finger has lifted: now the menu can
		 * open, with no pending press to dismiss it.
		 *
		 * And the release is CONSUMED, which is the other half. Without
		 * it the widget behind this filter also treats the gesture as a
		 * click -- which is how holding a row in the host list opened
		 * that host, and how holding in the terminal left a selection
		 * behind the menu.
		 */
		m_matured = false;
		emit triggered(m_target->mapToGlobal(m_pressed_at));
		return true;
	}

	case QEvent::Leave:
		m_timer->stop();
		m_matured = false;
		break;

	default:
		break;
	}

	return QObject::eventFilter(watched, event);
}
