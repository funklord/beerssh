#include "ui/pane_bar.h"

#include <QBoxLayout>
#include <QIcon>
#include <QApplication>
#include <QMouseEvent>
#include <QPainter>
#include <QProxyStyle>
#include <QScrollArea>
#include <QScrollBar>
#include <QScroller>
#include <QStyleOptionTab>
#include <QTabBar>
#include <QToolButton>

#include "model/session.h"
#include "ui/long_press.h"
#include "ui/tab_status.h"

Qt::Edge bssh_bar_side_at(const QPoint &at, const QSize &area) {
	if (area.width() <= 0 || area.height() <= 0) return Qt::TopEdge;
	const double left = double(at.x()) / area.width();
	const double top = double(at.y()) / area.height();
	const double nearest = qMin(qMin(left, 1.0 - left), qMin(top, 1.0 - top));
	if (nearest == left) return Qt::LeftEdge;
	if (nearest == 1.0 - left) return Qt::RightEdge;
	if (nearest == top) return Qt::TopEdge;
	return Qt::BottomEdge;
}

namespace {

/*
 * The bar's handle: a block of dots, not the three lines of a pane's grip
 * (sec 8.7a), since the two move different things. A drag starts once the
 * pointer has gone as far as the platform says a drag must, so a click on
 * it is not a gesture; the bar reports where the pointer is and the window
 * decides which side that means.
 */
class bar_handle : public QWidget {
public:
	bar_handle(bssh_pane_bar *bar, bool vertical) : QWidget(bar), m_bar(bar), m_vertical(vertical) {
		setObjectName(QStringLiteral("pane_bar_handle"));
		setToolTip(QStringLiteral("Drag to put the tabs on another side"));
		setAccessibleName(QStringLiteral("Move the tab bar"));
		setCursor(Qt::SizeAllCursor);
	}

	QSize sizeHint() const override {
		const int side = fontMetrics().height();
		return m_vertical ? QSize(side + 6, side) : QSize(side, side + 6);
	}

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		QColor ink = palette().color(QPalette::WindowText);
		ink.setAlphaF(0.55);
		painter.setPen(Qt::NoPen);
		painter.setBrush(ink);
		const qreal dot = qMax(1.5, qMin(width(), height()) / 10.0);
		const QPointF centre = QRectF(rect()).center();
		for (int row = -1; row <= 1; ++row) {
			for (int col = 0; col < 2; ++col) {
				const qreal along = row * dot * 3;
				const qreal across = (col - 0.5) * dot * 3;
				const QPointF at = m_vertical ? QPointF(centre.x() + along, centre.y() + across)
				                              : QPointF(centre.x() + across, centre.y() + along);
				painter.drawEllipse(at, dot, dot);
			}
		}
	}

	void mousePressEvent(QMouseEvent *event) override {
		if (event->button() != Qt::LeftButton) return;
		m_from = event->position().toPoint();
		m_armed = true;
		m_dragging = false;
		event->accept();
	}

	void mouseMoveEvent(QMouseEvent *event) override {
		if (!m_armed) return;
		if (!m_dragging && (event->position().toPoint() - m_from).manhattanLength()
		                           < QApplication::startDragDistance())
			return;
		m_dragging = true;
		emit m_bar->side_drag_moved(event->globalPosition().toPoint());
		event->accept();
	}

	void mouseReleaseEvent(QMouseEvent *event) override {
		const bool dragged = m_armed && m_dragging;
		m_armed = false;
		m_dragging = false;
		if (dragged) emit m_bar->side_drag_finished(event->globalPosition().toPoint());
		event->accept();
	}

private:
	bssh_pane_bar *m_bar;
	bool m_vertical;
	QPoint m_from;
	bool m_armed = false;
	bool m_dragging = false;
};

} /* namespace */

namespace {

/*
 * A bar down the side lays its tabs out as a browser's vertical tabs do:
 * one under the next, each label written across (sec 8.7a). QTabBar's own
 * West and East turn the label on its side -- read by tilting your head --
 * and size a tab by its whole title, so a long OSC title made a bar as
 * wide as the title was long.
 *
 * So the bar keeps its West or East shape, which is what everything else
 * reads to know it runs down the side (the drop slots, the tear-out), and
 * this style lays each tab out as a North one in the rectangle the bar
 * gives it: across, the label and its close button where they sit on a
 * bar along the top. A tab is as wide as its title up to WIDEST_CHARS,
 * and elided past that.
 */
class sideways_tabs : public QProxyStyle {
public:
	static const int WIDEST_CHARS = 24;

	QSize sizeFromContents(ContentsType type, const QStyleOption *option,
	                       const QSize &size, const QWidget *widget) const override {
		QSize out = QProxyStyle::sizeFromContents(type, option, size, widget);
		if (type != CT_TabBarTab) return out;
		out.transpose();
		out.setWidth(qMin(out.width(), option->fontMetrics.averageCharWidth() * WIDEST_CHARS));
		return out;
	}

	void drawControl(ControlElement element, const QStyleOption *option,
	                 QPainter *painter, const QWidget *widget) const override {
		const QStyleOptionTab *tab = qstyleoption_cast<const QStyleOptionTab *>(option);
		if (element == CE_TabBarTabLabel && tab) {
			QStyleOptionTab across(*tab);
			across.shape = QTabBar::RoundedNorth;
			QProxyStyle::drawControl(element, &across, painter, widget);
			return;
		}
		QProxyStyle::drawControl(element, option, painter, widget);
	}

	QRect subElementRect(SubElement element, const QStyleOption *option,
	                     const QWidget *widget) const override {
		const QStyleOptionTab *tab = qstyleoption_cast<const QStyleOptionTab *>(option);
		if (tab && (element == SE_TabBarTabText || element == SE_TabBarTabLeftButton
		            || element == SE_TabBarTabRightButton)) {
			QStyleOptionTab across(*tab);
			across.shape = QTabBar::RoundedNorth;
			return QProxyStyle::subElementRect(element, &across, widget);
		}
		return QProxyStyle::subElementRect(element, option, widget);
	}
};

/*
 * How far past the bar a tab has to be dragged before it tears out
 * (sec 8.3, 8.4). Generous on purpose: a bar is thin and a pointer
 * reordering tabs crosses its edge constantly, so tearing out on "left
 * the widget" would make reordering nearly impossible.
 */
const int TEAR_OUT_DP = 40;

} /* namespace */

bssh_pane_bar::bssh_pane_bar(bssh_session *front, const QList<bssh_session *> &tabs,
                             Qt::Edge edge, bool pointer, QWidget *parent)
    : QWidget(parent),
      m_front(front),
      m_bar(new QTabBar(this)),
      m_add(new QToolButton(this)),
      m_long_press(nullptr) {
	setObjectName(QStringLiteral("pane-bar"));
	const bool vertical = edge == Qt::LeftEdge || edge == Qt::RightEdge;

	QBoxLayout *row = new QBoxLayout(vertical ? QBoxLayout::TopToBottom
	                                          : QBoxLayout::LeftToRight, this);
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(0);

	switch (edge) {
	case Qt::BottomEdge: m_bar->setShape(QTabBar::RoundedSouth); break;
	case Qt::LeftEdge:   m_bar->setShape(QTabBar::RoundedWest); break;
	case Qt::RightEdge:  m_bar->setShape(QTabBar::RoundedEast); break;
	default:             m_bar->setShape(QTabBar::RoundedNorth); break;
	}

	/*
	 * ELIDED, because a label is a string the REMOTE writes (an OSC
	 * title, sec 8.3), and unexpanding so a bar of two tabs is two tabs
	 * wide rather than two halves of the pane.
	 */
	m_bar->setExpanding(false);
	m_bar->setTabsClosable(true);
	m_bar->setDrawBase(false);
	m_bar->setElideMode(Qt::ElideMiddle);
	m_bar->setMovable(pointer);
	m_bar->setContextMenuPolicy(Qt::CustomContextMenu);

	for (bssh_session *session : tabs) {
		const int index = m_bar->addTab(QString());
		m_bar->setTabData(index, QVariant::fromValue(static_cast<void *>(session)));
	}
	m_bar->setCurrentIndex(qMax(0, int(tabs.indexOf(front))));
	refresh();

	m_add->setText(QStringLiteral("+"));
	m_add->setAutoRaise(true);
	m_add->setToolTip(QStringLiteral("Open another session in this pane"));
	m_add->setAccessibleName(QStringLiteral("New tab"));

	m_handle = new bar_handle(this, vertical);
	row->addWidget(m_handle, 0);
	if (vertical) {
		/*
		 * Down the side, the tabs scroll rather than stretch (sec 8.7a).
		 * A bar's height was the sum of its tabs', and the pane under it
		 * could be no shorter -- so a dozen tabs pushed the terminal, and
		 * then the window, taller than the screen. In a scroll area the
		 * pane decides the height, and the tabs past it are a wheel or a
		 * thumb away; the "+" follows the last tab, as in a browser.
		 */
		QWidget *list = new QWidget;
		QVBoxLayout *column = new QVBoxLayout(list);
		column->setContentsMargins(0, 0, 0, 0);
		column->setSpacing(0);
		column->addWidget(m_bar, 0);
		column->addWidget(m_add, 0, Qt::AlignLeft);
		column->addStretch(1);

		/*
		 * Owned by the list, and made its child AFTER the bar: children
		 * go in the order they came, so the bar is gone before the style
		 * it paints with is.
		 */
		m_side_style = new sideways_tabs;
		m_side_style->setParent(list);
		m_bar->setStyle(m_side_style);
		m_bar->setUsesScrollButtons(false);

		m_scroll = new QScrollArea(this);
		m_scroll->setObjectName(QStringLiteral("pane-bar-scroll"));
		m_scroll->setFrameShape(QFrame::NoFrame);
		m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		m_scroll->setWidgetResizable(true);
		m_scroll->setWidget(list);
		if (!pointer) QScroller::grabGesture(m_scroll->viewport(), QScroller::LeftMouseButtonGesture);
		row->addWidget(m_scroll, 1);
		fit_side();
	} else {
		row->addWidget(m_bar, 0);
		row->addWidget(m_add, 0);
		row->addStretch(1);
	}

	connect(m_add, &QToolButton::clicked, this, &bssh_pane_bar::new_tab_requested);
	connect(m_bar, &QTabBar::currentChanged, this, [this](int index) {
		bssh_session *session = session_at(index);
		if (!session) return;
		if (m_pressed) m_pending = session;
		else emit tab_picked(session);
	});
	connect(m_bar, &QTabBar::tabCloseRequested, this, [this](int index) {
		if (bssh_session *session = session_at(index)) emit close_requested(session);
	});
	connect(m_bar, &QTabBar::tabMoved, this, [this](int, int to) {
		bssh_session *session = session_at(to);
		if (!session) return;

		/*
		 * Under a held press, held too: applying a reorder rebuilds the
		 * panes, and this bar with them, while the pointer is still down --
		 * which ended the gesture, so a tab dragged sideways past its
		 * neighbour could never go on to tear out.
		 */
		if (m_pressed) {
			m_moved = session;
			m_moved_to = to;
		} else {
			emit tab_moved(session, to);
		}
	});
	connect(m_bar, &QWidget::customContextMenuRequested, this, [this](const QPoint &at) {
		if (bssh_session *session = session_at(m_bar->tabAt(at)))
			emit menu_requested(session, m_bar->mapToGlobal(at));
	});

	/* Touch has no right button: a held finger is the menu (sec 6.4). */
	m_long_press = new bssh_long_press(m_bar, this);
	m_long_press->set_enabled(!pointer);
	connect(m_long_press, &bssh_long_press::triggered, this, [this](const QPoint &global) {
		const QPoint at = m_bar->mapFromGlobal(global);
		if (bssh_session *session = session_at(m_bar->tabAt(at)))
			emit menu_requested(session, global);
	});

	m_bar->installEventFilter(this);
}

QList<bssh_session *> bssh_pane_bar::sessions() const {
	QList<bssh_session *> out;
	for (int i = 0; i < m_bar->count(); ++i) out.append(session_at(i));
	return out;
}

bssh_session *bssh_pane_bar::session_at(int index) const {
	if (index < 0 || index >= m_bar->count()) return nullptr;
	return static_cast<bssh_session *>(m_bar->tabData(index).value<void *>());
}

void bssh_pane_bar::refresh() {
	const int size = m_bar->fontMetrics().height() / 2;
	for (int i = 0; i < m_bar->count(); ++i) {
		bssh_session *session = session_at(i);
		if (!session) continue;
		m_bar->setTabText(i, session->title());
		m_bar->setTabToolTip(i, session->profile().target());
		m_bar->setTabIcon(i, QIcon(bssh_tab_status::dot(size, bssh_tab_status::state_for(session))));
	}
	fit_side();
}

void bssh_pane_bar::fit_side() {
	/*
	 * As wide as the widest tab, plus the scrollbar's width whether or
	 * not it shows, so the tabs do not shrink under it when one more tab
	 * makes the list scroll.
	 */
	if (!m_scroll) return;
	const int wide = qMax(m_bar->sizeHint().width(), m_add->sizeHint().width());
	m_scroll->setFixedWidth(wide + m_scroll->verticalScrollBar()->sizeHint().width());
}

bool bssh_pane_bar::eventFilter(QObject *watched, QEvent *event) {
	if (watched != m_bar) return QWidget::eventFilter(watched, event);
	const QEvent::Type type = event->type();

	/*
	 * Down the side the wheel scrolls the tabs, as it does a browser's.
	 * QTabBar's own use of it is to switch tabs, which here would bring
	 * a different terminal forward under a pointer that meant to look.
	 */
	if (type == QEvent::Wheel && m_scroll) {
		QCoreApplication::sendEvent(m_scroll->verticalScrollBar(), event);
		return true;
	}

	if (type == QEvent::MouseButtonPress
	    && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
		m_pressed = true;
	}

	/*
	 * Torn out on DISTANCE in any direction, measured from the whole
	 * bar's row (sec 8.3): dragging along a half-empty row is still
	 * reordering, and a tab pulled up, down or off either end leaves.
	 * Once per press: the drag the first one starts owns the rest.
	 */
	if (type == QEvent::MouseMove && m_pressed && m_bar->isMovable()) {
		const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
		if (mouse->buttons().testFlag(Qt::LeftButton)) {
			const QPoint in_row = m_bar->mapTo(this, mouse->position().toPoint());
			const int clear = qMax(qMax(-in_row.y(), in_row.y() - height()),
			                       qMax(-in_row.x(), in_row.x() - width()));
			if (clear > TEAR_OUT_DP) {
				if (bssh_session *session = session_at(m_bar->currentIndex())) {
					/* Carried away, not picked: the drag decides where it goes. */
					m_pressed = false;
					m_pending.clear();
					m_moved.clear();
					m_bar->releaseMouse();
					emit drag_requested(session);
					return true;
				}
			}
		}
	}

	if (type == QEvent::MouseButtonRelease && m_pressed
	    && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
		m_pressed = false;
		if (m_moved) {
			QPointer<bssh_session> moved = m_moved;
			m_moved.clear();
			emit tab_moved(moved, m_moved_to);
		}
		if (m_pending) {
			QPointer<bssh_session> picked = m_pending;
			m_pending.clear();
			emit tab_picked(picked);
		}
	}

	/* A middle click closes, as in every browser and in konsole. */
	if (type == QEvent::MouseButtonRelease) {
		const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
		if (mouse->button() == Qt::MiddleButton) {
			if (bssh_session *session = session_at(m_bar->tabAt(mouse->position().toPoint()))) {
				emit close_requested(session);
				return true;
			}
		}
	}
	return QWidget::eventFilter(watched, event);
}
