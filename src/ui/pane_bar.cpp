#include "ui/pane_bar.h"

#include <QBoxLayout>
#include <QIcon>
#include <QMouseEvent>
#include <QTabBar>
#include <QToolButton>

#include "model/session.h"
#include "ui/long_press.h"
#include "ui/tab_status.h"

namespace {

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

	row->addWidget(m_bar, 0);
	row->addWidget(m_add, 0);
	row->addStretch(1);

	connect(m_add, &QToolButton::clicked, this, &bssh_pane_bar::new_tab_requested);
	connect(m_bar, &QTabBar::currentChanged, this, [this](int index) {
		if (bssh_session *session = session_at(index)) emit tab_picked(session);
	});
	connect(m_bar, &QTabBar::tabCloseRequested, this, [this](int index) {
		if (bssh_session *session = session_at(index)) emit close_requested(session);
	});
	connect(m_bar, &QTabBar::tabMoved, this, [this](int, int to) {
		if (bssh_session *session = session_at(to)) emit tab_moved(session, to);
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
}

bool bssh_pane_bar::eventFilter(QObject *watched, QEvent *event) {
	if (watched != m_bar) return QWidget::eventFilter(watched, event);

	/*
	 * Torn out on DISTANCE in any direction, measured from the whole
	 * bar's row (sec 8.3): dragging along a half-empty row is still
	 * reordering, and a tab pulled up, down or off either end leaves.
	 */
	if (event->type() == QEvent::MouseMove && m_bar->isMovable()) {
		const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
		if (mouse->buttons().testFlag(Qt::LeftButton)) {
			const QPoint in_row = m_bar->mapTo(this, mouse->position().toPoint());
			const int clear = qMax(qMax(-in_row.y(), in_row.y() - height()),
			                       qMax(-in_row.x(), in_row.x() - width()));
			if (clear > TEAR_OUT_DP) {
				if (bssh_session *session = session_at(m_bar->currentIndex())) {
					m_bar->releaseMouse();
					emit drag_requested(session);
					return true;
				}
			}
		}
	}

	/* A middle click closes, as in every browser and in konsole. */
	if (event->type() == QEvent::MouseButtonRelease) {
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
