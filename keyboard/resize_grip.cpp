#include "keyboard/resize_grip.h"

#include <QMouseEvent>
#include <QPainter>

bssh_resize_grip::bssh_resize_grip(Qt::Orientation drags, QWidget *parent)
    : QWidget(parent),
      m_drags(drags) {
	setObjectName(QStringLiteral("keyboard-grip"));
	setAccessibleName(QStringLiteral("Resize the keyboard"));
	setToolTip(QStringLiteral("Drag to resize the keyboard"));
	setCursor(drags == Qt::Horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor);

	if (drags == Qt::Horizontal) {
		setFixedWidth(THICKNESS_DP);
		setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
	} else {
		setFixedHeight(THICKNESS_DP);
		setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	}
}

QSize bssh_resize_grip::sizeHint() const {
	return QSize(THICKNESS_DP, THICKNESS_DP);
}

void bssh_resize_grip::set_lit(bool lit) {
	if (lit == m_lit) return;
	m_lit = lit;
	update();
}

int bssh_resize_grip::along(const QPointF &global) const {
	return int(m_drags == Qt::Horizontal ? global.x() : global.y());
}

/*
 * A line a third of the bar's thickness, centred, in the text colour at
 * low alpha: visible on either theme without being one more thing on a
 * keyboard that is already all edges. Brighter while held, so the finger
 * knows it has the bar rather than the key beside it.
 */
void bssh_resize_grip::paintEvent(QPaintEvent *) {
	QPainter painter(this);
	QColor ink = palette().color(QPalette::WindowText);
	ink.setAlphaF(m_down || m_lit ? 0.8 : 0.35);

	const int line = qMax(2, THICKNESS_DP / 3);
	QRect bar = rect();
	if (m_drags == Qt::Horizontal) {
		bar = QRect((width() - line) / 2, height() / 4, line, height() / 2);
	} else {
		bar = QRect(width() / 4, (height() - line) / 2, width() / 2, line);
	}
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(ink);
	painter.drawRoundedRect(bar, line / 2.0, line / 2.0);
}

void bssh_resize_grip::mousePressEvent(QMouseEvent *event) {
	if (event->button() != Qt::LeftButton) return;
	m_down = true;
	m_from = along(event->globalPosition());
	update();
	emit pressed();
}

void bssh_resize_grip::mouseMoveEvent(QMouseEvent *event) {
	if (!m_down) return;
	emit dragged(along(event->globalPosition()) - m_from);
}

void bssh_resize_grip::mouseReleaseEvent(QMouseEvent *event) {
	if (!m_down || event->button() != Qt::LeftButton) return;
	m_down = false;
	update();
	emit released();
}
