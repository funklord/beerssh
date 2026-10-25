#include "ui/multiplexer_menu.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

bssh_multiplexer_menu::bssh_multiplexer_menu(const QString &host,
                                             const QList<bssh_mux_session_t> &found,
                                             QWidget *parent)
    : QDialog(parent), m_found(found) {
	setWindowTitle(QStringLiteral("tmux and screen"));

	QVBoxLayout *layout = new QVBoxLayout(this);
	QLabel *heading = new QLabel(
	        QStringLiteral("Sessions are running on %1. Tick the ones to open; "
	                        "more than one opens a tab each.").arg(host.toHtmlEscaped()),
	        this);
	heading->setWordWrap(true);
	layout->addWidget(heading);

	m_list = new QListWidget(this);
	m_list->setObjectName(QStringLiteral("multiplexer_sessions"));
	for (const bssh_mux_session_t &session : m_found) {
		QListWidgetItem *item = new QListWidgetItem(bssh_mux_label(session), m_list);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(Qt::Unchecked);
	}
	/*
	 * The first one ticked, since one is what most people want and
	 * Enter should open it. A double-click opens just that one.
	 */
	if (m_list->count() > 0) {
		m_list->item(0)->setCheckState(Qt::Checked);
		m_list->setCurrentRow(0);
	}
	layout->addWidget(m_list);

	QDialogButtonBox *buttons = new QDialogButtonBox(this);
	m_open = buttons->addButton(QStringLiteral("Open"), QDialogButtonBox::AcceptRole);
	buttons->addButton(QStringLiteral("Shell only"), QDialogButtonBox::RejectRole);
	m_open->setDefault(true);
	layout->addWidget(buttons);

	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(m_list, &QListWidget::itemChanged, this, [this] { sync_open(); });
	connect(m_list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *clicked) {
		for (int row = 0; row < m_list->count(); ++row)
			m_list->item(row)->setCheckState(m_list->item(row) == clicked ? Qt::Checked
			                                                               : Qt::Unchecked);
		accept();
	});
	sync_open();
}

QList<bssh_mux_session_t> bssh_multiplexer_menu::chosen() const {
	QList<bssh_mux_session_t> ticked;
	for (int row = 0; row < m_list->count() && row < m_found.size(); ++row)
		if (m_list->item(row)->checkState() == Qt::Checked) ticked.append(m_found.at(row));
	return ticked;
}

void bssh_multiplexer_menu::sync_open() {
	/* Open with nothing ticked would be Shell only by another name. */
	m_open->setEnabled(!chosen().isEmpty());
}
