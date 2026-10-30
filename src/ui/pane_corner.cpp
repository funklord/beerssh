#include "ui/pane_corner.h"

#include <QApplication>
#include <QEnterEvent>
#include <QGraphicsOpacityEffect>
#include <QMouseEvent>
#include <QPainter>
#include <QToolButton>

#include "ui/input_selector.h"

namespace {

const int PAD = 2;

/*
 * How much of the corner shows while nothing points at it: enough to see
 * that it is there and which mode is lit, little enough that the text
 * under it reads through.
 */
const qreal IDLE_OPACITY = 0.3;

/*
 * The grip. It starts nothing on a press: the drag begins once the pointer
 * has moved as far as the platform says a drag must, so a click on it is
 * not a gesture -- the same rule the view's Alt+drag keeps (sec 8.4).
 */
class corner_grip : public QWidget {
public:
	explicit corner_grip(bssh_pane_corner *corner) : QWidget(corner), m_corner(corner) {
		setObjectName(QStringLiteral("pane_grip"));
		setAccessibleName(QStringLiteral("Drag to move this pane"));
		setToolTip(QStringLiteral("Drag to move this pane"));
		setCursor(Qt::OpenHandCursor);
	}

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		QColor ink = palette().color(QPalette::WindowText);
		ink.setAlphaF(0.55);
		QPen pen(ink);
		pen.setWidthF(qMax(1.0, height() / 12.0));
		pen.setCapStyle(Qt::RoundCap);
		painter.setPen(pen);
		const int side = qMin(width(), height());
		const qreal left = (width() - side) / 2.0;
		for (int i = 0; i < 3; ++i) {
			const qreal y = height() * (0.32 + i * 0.18);
			painter.drawLine(QPointF(left + side * 0.24, y), QPointF(left + side * 0.76, y));
		}
	}

	void mousePressEvent(QMouseEvent *event) override {
		if (event->button() != Qt::LeftButton) return;
		m_from = event->position().toPoint();
		m_armed = true;
		event->accept();
	}

	void mouseMoveEvent(QMouseEvent *event) override {
		if (!m_armed) return;
		if ((event->position().toPoint() - m_from).manhattanLength()
		    < QApplication::startDragDistance())
			return;
		m_armed = false;
		emit m_corner->drag_requested();
		event->accept();
	}

	void mouseReleaseEvent(QMouseEvent *event) override {
		m_armed = false;
		event->accept();
	}

private:
	bssh_pane_corner *m_corner;
	QPoint m_from;
	bool m_armed = false;
};

} /* namespace */

bssh_pane_corner::bssh_pane_corner(QWidget *parent)
    : QWidget(parent),
      m_grip(new corner_grip(this)),
      m_selector(new bssh_input_selector(this)),
      m_multiplexer(new QToolButton(this)),
      m_tabs(new QToolButton(this)),
      m_menu(new QToolButton(this)),
      m_opacity(new QGraphicsOpacityEffect(this)) {
	setObjectName(QStringLiteral("pane_corner"));
	m_opacity->setOpacity(1.0);
	setGraphicsEffect(m_opacity);

	m_multiplexer->setObjectName(QStringLiteral("multiplexer_handle"));
	m_multiplexer->setToolTip(QStringLiteral("tmux and screen sessions running here"));
	m_multiplexer->setAutoRaise(true);
	m_multiplexer->setFocusPolicy(Qt::NoFocus);
	m_multiplexer->hide();
	connect(m_multiplexer, &QToolButton::clicked, this, &bssh_pane_corner::multiplexer_clicked);

	/*
	 * The two bars' stand-ins, each there only while its bar is hidden.
	 * The menu's is a vertical ellipsis, not three lines: the grip above
	 * is already drawn as three lines, and two of them stacked would be
	 * one control twice.
	 */
	m_tabs->setObjectName(QStringLiteral("corner_tabs"));
	m_tabs->setToolTip(QStringLiteral("This pane's tabs"));
	m_menu->setObjectName(QStringLiteral("corner_menu"));
	m_menu->setText(QString::fromUtf8("\u22ee"));
	m_menu->setToolTip(QStringLiteral("Menus"));
	m_menu->setAccessibleName(QStringLiteral("Menus"));
	for (QToolButton *button : { m_tabs, m_menu }) {
		button->setAutoRaise(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->hide();
	}
	connect(m_tabs, &QToolButton::clicked, this, [this] {
		emit tabs_clicked(m_tabs->mapToGlobal(QPoint(0, m_tabs->height())));
	});
	connect(m_menu, &QToolButton::clicked, this, [this] {
		emit menu_clicked(m_menu->mapToGlobal(QPoint(0, m_menu->height())));
	});

	m_grip->hide();
	relayout();
}

void bssh_pane_corner::set_tabs_text(const QString &text) {
	m_tabs->setText(text);
	m_tabs->setVisible(!text.isEmpty());
	relayout();
}

void bssh_pane_corner::set_menu_shown(bool shown) {
	if (shown == !m_menu->isHidden()) return;
	m_menu->setVisible(shown);
	relayout();
}

void bssh_pane_corner::set_grip_shown(bool shown) {
	if (shown == !m_grip->isHidden()) return;
	m_grip->setVisible(shown);
	relayout();
}

bool bssh_pane_corner::grip_shown() const {
	return !m_grip->isHidden();
}

void bssh_pane_corner::set_drum_rows(int rows) {
	if (rows == m_selector->rows()) return;
	m_selector->set_rows(rows);
	relayout();
}

void bssh_pane_corner::set_grip_side(int side) {
	if (side == m_grip_side) return;
	m_grip_side = side;
	relayout();
}

QRect bssh_pane_corner::grip_rect() const {
	return m_grip->isHidden() ? QRect() : m_grip->geometry();
}

void bssh_pane_corner::set_multiplexer_text(const QString &text) {
	m_multiplexer->setText(text);
	m_multiplexer->setVisible(!text.isEmpty());
	relayout();
}

void bssh_pane_corner::set_faint_when_idle(bool faint) {
	if (faint == m_faint_when_idle) return;
	m_faint_when_idle = faint;
	sync_opacity();
}

bool bssh_pane_corner::is_faint() const {
	return m_opacity->opacity() < 1.0;
}

void bssh_pane_corner::sync_opacity() {
	m_opacity->setOpacity(m_faint_when_idle && !m_hovered ? IDLE_OPACITY : 1.0);
}

void bssh_pane_corner::enterEvent(QEnterEvent *event) {
	m_hovered = true;
	sync_opacity();
	QWidget::enterEvent(event);
}

void bssh_pane_corner::leaveEvent(QEvent *event) {
	m_hovered = false;
	sync_opacity();
	QWidget::leaveEvent(event);
}

QSize bssh_pane_corner::sizeHint() const {
	int wide = m_selector->sizeHint().width();
	int high = m_selector->sizeHint().height();
	if (!m_grip->isHidden()) {
		wide = qMax(wide, m_grip_side);
		high += m_grip_side;
	}
	for (const QToolButton *button : { m_multiplexer, m_tabs, m_menu }) {
		if (button->isHidden()) continue;
		wide = qMax(wide, button->sizeHint().width());
		high += button->sizeHint().height();
	}
	return QSize(wide + 2 * PAD, high + 2 * PAD);
}

void bssh_pane_corner::relayout() {
	const QSize size = sizeHint();
	const int inner = size.width() - 2 * PAD;
	int y = PAD;
	if (!m_grip->isHidden()) {
		m_grip->setGeometry(PAD, y, inner, m_grip_side);
		y += m_grip_side;
	}
	/* The menus first, under the grip: the top of the corner is where
	 * the menu bar they stand in for would be (the holder's placement). */
	if (!m_menu->isHidden()) {
		const int high = m_menu->sizeHint().height();
		m_menu->setGeometry(PAD, y, inner, high);
		y += high;
	}
	const int drum = m_selector->sizeHint().height();
	m_selector->setGeometry(PAD, y, inner, drum);
	y += drum;
	/* Under the drum: the tmux picker, then the tab bar's stand-in. */
	for (QToolButton *button : { m_multiplexer, m_tabs }) {
		if (button->isHidden()) continue;
		const int high = button->sizeHint().height();
		button->setGeometry(PAD, y, inner, high);
		y += high;
	}
	updateGeometry();
	if (size != this->size()) resize(size);
}

void bssh_pane_corner::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);
	update();
}

void bssh_pane_corner::paintEvent(QPaintEvent *) {
	/*
	 * Opaque enough to read over any text, the window's own colour so it
	 * reads as a control rather than as part of the terminal's output.
	 */
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);
	QColor fill = palette().color(QPalette::Window);
	fill.setAlphaF(0.92);
	QColor edge = palette().color(QPalette::Mid);
	painter.setPen(edge);
	painter.setBrush(fill);
	painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
}
