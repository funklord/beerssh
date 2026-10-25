#include "ui/multiplexer_drawer.h"

#include "platform/platform.h"
#include "ui/touch_metrics.h"

#include <QAbstractButton>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

/*
 * The closed drawer: a tab on the terminal's edge with its label running
 * up it, as a tab on the side of a binder does, so it takes a sliver of
 * width rather than a corner of the text.
 */
class drawer_handle : public QAbstractButton {
public:
	explicit drawer_handle(QWidget *parent) : QAbstractButton(parent) {
		setFocusPolicy(Qt::NoFocus);
		setCursor(Qt::PointingHandCursor);
	}

	QSize sizeHint() const override {
		const QFontMetrics metrics(font());
		const bssh_platform *platform = bssh_platform::instance();
		const bool touch = platform && platform->is_touch_primary();
		const int thick = qMax(metrics.height() + 10, touch ? BSSH_TOUCH_TARGET_DP / 2 + 8 : 0);
		return QSize(thick, metrics.horizontalAdvance(text()) + 24);
	}

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		const QPalette &colours = palette();
		painter.setPen(colours.color(QPalette::Mid));
		painter.setBrush(isDown() ? colours.color(QPalette::Midlight)
		                          : colours.color(QPalette::Button));
		painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, 4, -0.5), 5, 5);

		painter.setPen(colours.color(QPalette::ButtonText));
		painter.translate(0, height());
		painter.rotate(-90);
		painter.drawText(QRect(0, 0, height(), width()), Qt::AlignCenter, text());
	}
};

} /* namespace */

bssh_multiplexer_drawer::bssh_multiplexer_drawer(QWidget *parent) : QWidget(parent) {
	setObjectName(QStringLiteral("multiplexer_drawer"));

	QHBoxLayout *row = new QHBoxLayout(this);
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(0);

	m_handle = new drawer_handle(this);
	m_handle->setObjectName(QStringLiteral("multiplexer_handle"));
	m_handle->setToolTip(QStringLiteral("tmux and screen sessions running here"));
	row->addWidget(m_handle, 0, Qt::AlignTop);
	connect(m_handle, &QAbstractButton::clicked, this, [this] { set_open(!m_open); });

	m_panel = new QFrame(this);
	m_panel->setFrameShape(QFrame::StyledPanel);
	m_panel->setAutoFillBackground(true);
	m_panel->setBackgroundRole(QPalette::Window);
	row->addWidget(m_panel, 1);

	QVBoxLayout *column = new QVBoxLayout(m_panel);
	QHBoxLayout *top = new QHBoxLayout;
	QLabel *heading = new QLabel(QStringLiteral("Running here"), m_panel);
	QFont bold = heading->font();
	bold.setBold(true);
	heading->setFont(bold);
	top->addWidget(heading, 1);

	QToolButton *close = new QToolButton(m_panel);
	close->setObjectName(QStringLiteral("multiplexer_close"));
	close->setText(QStringLiteral("×"));
	close->setToolTip(QStringLiteral("Shell only: leave the shell as it is"));
	close->setAutoRaise(true);
	top->addWidget(close);
	column->addLayout(top);

	QLabel *note = new QLabel(QStringLiteral("Click one to switch this tab to it."), m_panel);
	note->setWordWrap(true);
	column->addWidget(note);

	m_list = new QListWidget(m_panel);
	m_list->setObjectName(QStringLiteral("multiplexer_sessions"));
	column->addWidget(m_list, 1);

	connect(close, &QToolButton::clicked, this, [this] { emit declined(); });

	/*
	 * One click switches, as choosing from a list does everywhere else;
	 * itemActivated as well so Enter on a highlighted row does it. The
	 * drawer closes to its handle either way, where it stays for the next
	 * switch. The one already shown just closes it.
	 */
	const auto choose_row = [this](QListWidgetItem *item) {
		const int row = m_list->row(item);
		set_open(false);
		if (row < 0 || row >= m_found.size() || m_found.at(row).current) return;
		emit session_chosen(m_found.at(row));
	};
	connect(m_list, &QListWidget::itemClicked, this, choose_row);
	connect(m_list, &QListWidget::itemActivated, this, choose_row);

	m_panel->hide();
}

void bssh_multiplexer_drawer::set_sessions(const QList<bssh_mux_session_t> &found) {
	m_found = found;
	m_list->clear();
	for (const bssh_mux_session_t &session : m_found) {
		/* The one this tab shows, marked and bold. */
		QListWidgetItem *item = new QListWidgetItem(
		        (session.current ? QStringLiteral("\u25b8 ") : QStringLiteral("   "))
		                + bssh_mux_label(session),
		        m_list);
		/* Clicked, not ticked: Qt makes an item checkable by default. */
		item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
		if (session.current) {
			QFont bold = item->font();
			bold.setBold(true);
			item->setFont(bold);
			item->setToolTip(QStringLiteral("This tab is showing it"));
		}
	}
	m_handle->setText(handle_text());
	m_handle->updateGeometry();
}

void bssh_multiplexer_drawer::set_open(bool open) {
	if (open == m_open) return;
	m_open = open;
	m_panel->setVisible(open);
	updateGeometry();
	emit open_changed(open);
}

QString bssh_multiplexer_drawer::handle_text() const {
	bool tmux = false;
	bool screen = false;
	for (const bssh_mux_session_t &session : m_found)
		(session.kind == bssh_mux_session_t::TMUX ? tmux : screen) = true;
	const QString kind = tmux && screen ? QStringLiteral("tmux/screen")
	                     : screen      ? QStringLiteral("screen")
	                                   : QStringLiteral("tmux");
	return QStringLiteral("%1 · %2").arg(kind).arg(m_found.size());
}

QSize bssh_multiplexer_drawer::sizeHint() const {
	const QSize handle = m_handle->sizeHint();
	if (!m_open) return handle;
	const QSize panel = m_panel->sizeHint();
	return QSize(handle.width() + qMax(panel.width(), 280), qMax(handle.height(), panel.height()));
}
