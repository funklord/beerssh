#include "ui/terminals_panel.h"

#include <QFont>
#include <QLabel>
#include <QListWidget>
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

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);
	QLabel *title = new QLabel(QStringLiteral("Terminals"), this);
	title->setContentsMargins(6, 4, 6, 4);
	QFont bold = title->font();
	bold.setBold(true);
	title->setFont(bold);
	layout->addWidget(title, 0);
	layout->addWidget(m_list, 1);

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

void bssh_terminals_panel::set_entries(const QList<bssh_terminal_entry_t> &entries) {
	m_entries = entries;
	m_list->clear();

	int last_place = -1;
	for (int i = 0; i < m_entries.size(); ++i) {
		const bssh_terminal_entry_t &entry = m_entries.at(i);
		if (int(entry.place) != last_place) {
			last_place = int(entry.place);
			QListWidgetItem *heading = new QListWidgetItem(heading_for(entry.place), m_list);
			heading->setFlags(Qt::ItemIsEnabled);
			heading->setData(ENTRY_ROLE, -1);
			QFont font = heading->font();
			font.setItalic(true);
			heading->setFont(font);
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
