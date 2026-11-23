#include "ui/terminals_panel.h"

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QListWidget>
#include <QToolButton>
#include <QVBoxLayout>

namespace {

/* Where each row's entry is kept: its index in m_entries, or -1 for a heading. */
const int ENTRY_ROLE = Qt::UserRole + 1;

QString heading_for(bssh_terminal_entry_t::place_t place) {
	switch (place) {
	case bssh_terminal_entry_t::HERE:         return QStringLiteral("This window");
	case bssh_terminal_entry_t::OTHER_WINDOW: return QStringLiteral("Other windows");
	case bssh_terminal_entry_t::SCREEN:       return QStringLiteral("screen sessions");
	case bssh_terminal_entry_t::TMUX:         return QStringLiteral("tmux sessions");
	default:                                  break;
	}
	return QStringLiteral("Other BeerSSH windows");
}

}  // namespace

bssh_terminals_panel::bssh_terminals_panel(QWidget *parent)
    : QWidget(parent), m_list(new QListWidget(this)) {
	setObjectName(QStringLiteral("terminals_panel"));
	m_list->setObjectName(QStringLiteral("terminals_list"));
	m_list->setAccessibleName(QStringLiteral("Terminals"));

	/*
	 * A panel over the pane it was opened in, as the tmux and screen
	 * drawer was, rather than a column beside every pane (the holder,
	 * 2026-11-23: one list, in the pane, not a second one for the
	 * window). Opaque, framed, with an x.
	 */
	setAutoFillBackground(true);
	setBackgroundRole(QPalette::Window);
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(4, 2, 4, 4);
	layout->setSpacing(2);
	QHBoxLayout *top = new QHBoxLayout;
	QLabel *title = new QLabel(QStringLiteral("Terminals"), this);
	title->setContentsMargins(2, 2, 2, 2);
	QFont bold = title->font();
	bold.setBold(true);
	title->setFont(bold);
	top->addWidget(title, 1);
	QToolButton *close = new QToolButton(this);
	close->setObjectName(QStringLiteral("terminals_close"));
	close->setText(QStringLiteral("\u00d7"));
	close->setToolTip(QStringLiteral("Close"));
	close->setAccessibleName(QStringLiteral("Close the terminals list"));
	close->setAutoRaise(true);
	connect(close, &QToolButton::clicked, this, &QWidget::hide);
	top->addWidget(close, 0);
	layout->addLayout(top, 0);
	layout->addWidget(m_list, 1);

	/*
	 * Closed when focus moves out of the terminal it is over -- another
	 * pane, another widget, another program -- as the drawer is
	 * (bssh_multiplexer_drawer): focus within it, the list included,
	 * keeps it, and only a move out counts.
	 */
	connect(qApp, &QApplication::focusChanged, this, [this](QWidget *was, QWidget *now) {
		if (isHidden()) return;
		QWidget *owner = parentWidget() ? parentWidget() : this;
		const auto inside = [owner](QWidget *widget) {
			return widget && owner->isAncestorOf(widget); /* itself included */
		};
		if (!inside(was) || inside(now)) return;
		hide();
	});

	/*
	 * A click opens, as a tab does; Return as well, for the keyboard.
	 * Headings carry no entry and do nothing.
	 *
	 * Queued, and once: where the style activates on a single click, one
	 * click is both signals, and a terminal taken from another process
	 * twice is a second take that fails and says so. And the list is
	 * rebuilt by what opening does, which must not happen under the
	 * signal that is reading it.
	 */
	auto pick = [this](QListWidgetItem *item) {
		if (!item || m_pick_queued) return;
		const int at = item->data(ENTRY_ROLE).toInt();
		if (at < 0 || at >= m_entries.size()) return;
		const bssh_terminal_entry_t entry = m_entries.at(at);
		m_pick_queued = true;
		QMetaObject::invokeMethod(this, [this, entry] {
			m_pick_queued = false;
			emit picked(entry);
		}, Qt::QueuedConnection);
	};
	connect(m_list, &QListWidget::itemClicked, this, pick);
	connect(m_list, &QListWidget::itemActivated, this, pick);
}

void bssh_terminals_panel::hideEvent(QHideEvent *event) {
	QWidget::hideEvent(event);
	if (!event->spontaneous()) emit closed();
}

QSize bssh_terminals_panel::sizeHint() const {
	return QSize(320, 360);
}

void bssh_terminals_panel::show_multiplexers() {
	for (int i = 0; i < m_entries.size(); ++i) {
		const bssh_terminal_entry_t::place_t place = m_entries.at(i).place;
		if (place != bssh_terminal_entry_t::SCREEN && place != bssh_terminal_entry_t::TMUX) continue;
		for (int row = 0; row < m_list->count(); ++row) {
			if (m_list->item(row)->data(ENTRY_ROLE).toInt() != i) continue;
			/* The heading above it, so the section reads from its top. */
			m_list->scrollToItem(m_list->item(qMax(0, row - 1)), QAbstractItemView::PositionAtTop);
			m_list->setCurrentRow(row);
			return;
		}
	}
}

void bssh_terminals_panel::set_entries(const QList<bssh_terminal_entry_t> &entries) {
	m_entries = entries;
	m_list->clear();

	int last_place = -1;
	for (int i = 0; i < m_entries.size(); ++i) {
		const bssh_terminal_entry_t &entry = m_entries.at(i);
		if (int(entry.place) != last_place) {
			last_place = int(entry.place);
			/*
			 * A heading is a label, not a row: no flags, so neither a
			 * click, the keyboard's current row nor a hover lands on it.
			 * It read as one more row, in italics only (the holder,
			 * 2026-11-23: "if the 3 legends aren't something that can be
			 * used, they shouldn't be selectable. They are also not
			 * different clearly enough"). So bold on a shaded band, its
			 * text in the list's own colour rather than a disabled grey,
			 * and in small capitals, since the current terminal's row is
			 * bold too and the two read alike. The band is the list's
			 * text mixed into its background, so it shows in a light
			 * theme and a dark one alike.
			 */
			QListWidgetItem *heading = new QListWidgetItem(heading_for(entry.place), m_list);
			heading->setFlags(Qt::NoItemFlags);
			heading->setData(ENTRY_ROLE, -1);
			QFont font = heading->font();
			font.setBold(true);
			font.setCapitalization(QFont::AllUppercase);
			if (font.pointSizeF() > 0) font.setPointSizeF(font.pointSizeF() * 0.85);
			heading->setFont(font);
			const QColor base = m_list->palette().color(QPalette::Base);
			const QColor text = m_list->palette().color(QPalette::Text);
			heading->setBackground(QColor::fromRgbF(base.redF() * 0.8f + text.redF() * 0.2f,
			                                        base.greenF() * 0.8f + text.greenF() * 0.2f,
			                                        base.blueF() * 0.8f + text.blueF() * 0.2f));
			heading->setForeground(text);
		}
		QListWidgetItem *item = new QListWidgetItem(entry.title, m_list);
		item->setData(ENTRY_ROLE, i);
		if (entry.current) {
			QFont font = item->font();
			font.setBold(true);
			item->setFont(font);
		}
	}
}
