#include "ui/multiplexer_drawer.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include "platform/platform.h"
#include "ui/touch_metrics.h"

namespace {

/*
 * On touch the sessions and the x are a thumb's targets (sec 6.2's 48dp):
 * at their own size the rows were 17dp and the x 24, and this panel is
 * built inside a pane at run time, where the sweep that holds the app's
 * surfaces to the floor never opened it.
 */
bool touch_primary() {
	const bssh_platform *platform = bssh_platform::instance();
	return platform && platform->is_touch_primary();
}

} /* namespace */

bssh_multiplexer_drawer::bssh_multiplexer_drawer(QWidget *parent) : QWidget(parent) {
	setObjectName(QStringLiteral("multiplexer_drawer"));

	QHBoxLayout *row = new QHBoxLayout(this);
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(0);

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
	close->setToolTip(QStringLiteral("Close"));
	close->setAutoRaise(true);
	if (touch_primary()) close->setMinimumSize(BSSH_TOUCH_TARGET_DP, BSSH_TOUCH_TARGET_DP);
	top->addWidget(close);
	column->addLayout(top);

	QLabel *note = new QLabel(QStringLiteral("Click one to switch this tab to it."), m_panel);
	note->setWordWrap(true);
	column->addWidget(note);

	m_list = new QListWidget(m_panel);
	m_list->setObjectName(QStringLiteral("multiplexer_sessions"));
	column->addWidget(m_list, 1);

	/*
	 * Closes the panel, and nothing more. It used to decline the offer and
	 * take the handle with it, which suited a handle on the text's edge;
	 * with the button in the pane's corner (sec 8.7a), where it is hidden
	 * anyway, the holder found the button going "now unexpected".
	 */
	connect(close, &QToolButton::clicked, this, [this] { set_open(false); });

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
		if (touch_primary()) item->setSizeHint(QSize(0, BSSH_TOUCH_TARGET_DP));
		if (session.current) {
			QFont bold = item->font();
			bold.setBold(true);
			item->setFont(bold);
			item->setToolTip(QStringLiteral("This tab is showing it"));
		}
	}
	updateGeometry();
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

QString bssh_multiplexer_drawer::button_text() const {
	bool tmux = false;
	bool screen = false;
	for (const bssh_mux_session_t &session : m_found)
		(session.kind == bssh_mux_session_t::TMUX ? tmux : screen) = true;
	const QString kind = tmux && screen ? QStringLiteral("mux")
	                     : screen      ? QStringLiteral("scr")
	                                   : QStringLiteral("tmux");
	return QStringLiteral("%1 %2").arg(kind).arg(m_found.size());
}

QSize bssh_multiplexer_drawer::sizeHint() const {
	const QSize panel = m_panel->sizeHint();
	return QSize(qMax(panel.width(), 280), panel.height());
}
