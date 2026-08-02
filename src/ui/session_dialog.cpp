#include "ui/session_dialog.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "model/profile_store.h"

bssh_session_dialog::bssh_session_dialog(bssh_profile_store *store, QWidget *parent)
    : QWidget(parent),
      m_store(store),
      m_current_id(0) {
	setWindowTitle(QStringLiteral("BeerSSH Configuration"));

	QHBoxLayout *columns = new QHBoxLayout(this);

	/*
	 * The category tree, left, exactly where PuTTY puts it. Its width is
	 * fixed rather than stretchy for the same reason PuTTY's is: the
	 * category names are short and known, and a tree that grows with the
	 * window wastes the space the settings need.
	 */
	m_categories = new QTreeWidget(this);
	m_categories->setHeaderLabel(QStringLiteral("Category:"));
	m_categories->setMaximumWidth(190);
	m_categories->setMinimumWidth(150);

	QTreeWidgetItem *session = new QTreeWidgetItem(m_categories,
	        QStringList(QStringLiteral("Session")));
	QTreeWidgetItem *terminal = new QTreeWidgetItem(m_categories,
	        QStringList(QStringLiteral("Terminal")));
	QTreeWidgetItem *window = new QTreeWidgetItem(m_categories,
	        QStringList(QStringLiteral("Window")));
	QTreeWidgetItem *connection = new QTreeWidgetItem(m_categories,
	        QStringList(QStringLiteral("Connection")));
	new QTreeWidgetItem(connection, QStringList(QStringLiteral("SSH")));

	m_pages = new QStackedWidget(this);
	m_pages->addWidget(build_session_page());
	m_pages->addWidget(build_placeholder_page(QStringLiteral("Terminal"),
	                                           QStringLiteral("M10, with the preset system")));
	m_pages->addWidget(build_placeholder_page(QStringLiteral("Window"),
	                                           QStringLiteral("M9, with the font pipeline")));
	m_pages->addWidget(build_placeholder_page(QStringLiteral("Connection"),
	                                           QStringLiteral("M5, with keepalive and reconnect")));
	m_pages->addWidget(build_placeholder_page(QStringLiteral("SSH"),
	                                           QStringLiteral("M6, with key authentication")));

	Q_UNUSED(session);
	Q_UNUSED(terminal);
	Q_UNUSED(window);

	columns->addWidget(m_categories);
	columns->addWidget(m_pages, 1);

	connect(m_categories, &QTreeWidget::itemSelectionChanged,
	         this, &bssh_session_dialog::on_category_changed);

	m_categories->setCurrentItem(m_categories->topLevelItem(0));
	reload_sessions();
}

QWidget *bssh_session_dialog::build_session_page() {
	QWidget *page = new QWidget(this);
	QVBoxLayout *layout = new QVBoxLayout(page);

	QLabel *heading = new QLabel(
	        QStringLiteral("Specify the destination you want to connect to"), page);
	layout->addWidget(heading);

	QHBoxLayout *destination = new QHBoxLayout();
	destination->addWidget(new QLabel(QStringLiteral("Host Name (or IP address)"), page));
	m_host = new QLineEdit(page);
	destination->addWidget(m_host, 1);
	destination->addWidget(new QLabel(QStringLiteral("Port"), page));
	m_port = new QSpinBox(page);
	m_port->setRange(1, 65535);
	m_port->setValue(22);
	m_port->setMaximumWidth(80);
	destination->addWidget(m_port);
	layout->addLayout(destination);

	QHBoxLayout *user_row = new QHBoxLayout();
	user_row->addWidget(new QLabel(QStringLiteral("User name"), page));
	m_user = new QLineEdit(page);
	user_row->addWidget(m_user, 1);
	layout->addLayout(user_row);

	/*
	 * PuTTY offers Raw/Telnet/Rlogin/SSH/Serial here. This is an SSH
	 * client and sec 1 says so, so the row exists to sit where the eye
	 * expects it and to say what it is, with nothing to choose.
	 */
	QHBoxLayout *type_row = new QHBoxLayout();
	type_row->addWidget(new QLabel(QStringLiteral("Connection type:"), page));
	QRadioButton *ssh = new QRadioButton(QStringLiteral("SSH"), page);
	ssh->setChecked(true);
	ssh->setEnabled(false);
	type_row->addWidget(ssh);
	type_row->addStretch(1);
	layout->addLayout(type_row);

	QGroupBox *saved = new QGroupBox(
	        QStringLiteral("Load, save or delete a stored session"), page);
	QVBoxLayout *saved_layout = new QVBoxLayout(saved);

	saved_layout->addWidget(new QLabel(QStringLiteral("Saved Sessions"), saved));
	m_session_name = new QLineEdit(saved);
	saved_layout->addWidget(m_session_name);

	QHBoxLayout *list_row = new QHBoxLayout();
	m_sessions = new QListWidget(saved);
	list_row->addWidget(m_sessions, 1);

	QVBoxLayout *buttons = new QVBoxLayout();
	QPushButton *load = new QPushButton(QStringLiteral("Load"), saved);
	QPushButton *save = new QPushButton(QStringLiteral("Save"), saved);
	QPushButton *remove = new QPushButton(QStringLiteral("Delete"), saved);
	buttons->addWidget(load);
	buttons->addWidget(save);
	buttons->addWidget(remove);
	buttons->addStretch(1);
	list_row->addLayout(buttons);
	saved_layout->addLayout(list_row);

	layout->addWidget(saved, 1);

	QHBoxLayout *actions = new QHBoxLayout();
	actions->addStretch(1);
	QPushButton *open = new QPushButton(QStringLiteral("Open"), page);
	QPushButton *cancel = new QPushButton(QStringLiteral("Cancel"), page);
	open->setDefault(true);
	actions->addWidget(open);
	actions->addWidget(cancel);
	layout->addLayout(actions);

	connect(load, &QPushButton::clicked, this, &bssh_session_dialog::on_load);
	connect(save, &QPushButton::clicked, this, &bssh_session_dialog::on_save);
	connect(remove, &QPushButton::clicked, this, &bssh_session_dialog::on_delete);
	connect(open, &QPushButton::clicked, this, &bssh_session_dialog::on_open);
	connect(cancel, &QPushButton::clicked, this, &bssh_session_dialog::cancelled);

	/* Double-clicking a saved session opens it, as PuTTY does. */
	connect(m_sessions, &QListWidget::itemDoubleClicked,
	         this, &bssh_session_dialog::on_session_activated);
	connect(m_sessions, &QListWidget::itemSelectionChanged, this, [this] {
		const int id = selected_profile_id();
		if (id != 0) m_session_name->setText(m_store->profile(id).name);
	});

	return page;
}

QWidget *bssh_session_dialog::build_placeholder_page(const QString &title,
                                                      const QString &milestone) {
	QWidget *page = new QWidget(this);
	QVBoxLayout *layout = new QVBoxLayout(page);

	QLabel *heading = new QLabel(QStringLiteral("<b>%1</b>").arg(title), page);
	QLabel *body = new QLabel(
	        QStringLiteral("These settings are not built yet — they arrive at %1.\n\n"
	                        "They will be owned by the host's preset rather than set here "
	                        "per host, so that editing one preset changes every host using it.")
	                .arg(milestone), page);
	body->setWordWrap(true);

	layout->addWidget(heading);
	layout->addWidget(body);
	layout->addStretch(1);
	return page;
}

void bssh_session_dialog::on_category_changed() {
	const QList<QTreeWidgetItem *> selected = m_categories->selectedItems();
	if (selected.isEmpty()) return;

	const QString name = selected.first()->text(0);
	if (name == QStringLiteral("Session")) m_pages->setCurrentIndex(0);
	else if (name == QStringLiteral("Terminal")) m_pages->setCurrentIndex(1);
	else if (name == QStringLiteral("Window")) m_pages->setCurrentIndex(2);
	else if (name == QStringLiteral("Connection")) m_pages->setCurrentIndex(3);
	else if (name == QStringLiteral("SSH")) m_pages->setCurrentIndex(4);
}

void bssh_session_dialog::reload_sessions() {
	m_sessions->clear();
	if (!m_store) return;

	const QList<bssh_profile_t> profiles = m_store->profiles();
	for (const bssh_profile_t &profile : profiles) {
		QListWidgetItem *item = new QListWidgetItem(profile.name, m_sessions);
		item->setData(Qt::UserRole, profile.id);
		item->setToolTip(profile.target());
	}
}

int bssh_session_dialog::selected_profile_id() const {
	const QListWidgetItem *item = m_sessions->currentItem();
	return item ? item->data(Qt::UserRole).toInt() : 0;
}

bssh_profile_t bssh_session_dialog::current_profile() const {
	bssh_profile_t profile;
	profile.id = m_current_id;
	profile.name = m_session_name->text();
	profile.host = m_host->text().trimmed();
	profile.user = m_user->text().trimmed();
	profile.port = static_cast<quint16>(m_port->value());
	return profile;
}

void bssh_session_dialog::set_current_profile(const bssh_profile_t &profile) {
	m_current_id = profile.id;
	m_session_name->setText(profile.name);
	m_host->setText(profile.host);
	m_user->setText(profile.user);
	m_port->setValue(profile.port);
}

void bssh_session_dialog::on_load() {
	const int id = selected_profile_id();
	if (id == 0 || !m_store) return;

	set_current_profile(m_store->profile(id));
}

void bssh_session_dialog::on_save() {
	if (!m_store) return;

	bssh_profile_t profile = current_profile();
	if (!profile.is_valid()) return;

	/*
	 * Saving under a name that already exists updates it rather than
	 * creating a second entry, which is what PuTTY does and what anyone
	 * pressing Save twice expects.
	 */
	const QList<bssh_profile_t> existing = m_store->profiles();
	for (const bssh_profile_t &candidate : existing) {
		if (candidate.name == profile.name) {
			profile.id = candidate.id;
			break;
		}
	}

	m_current_id = m_store->save(profile);
	reload_sessions();
}

void bssh_session_dialog::on_delete() {
	const int id = selected_profile_id();
	if (id == 0 || !m_store) return;

	m_store->remove(id);
	if (m_current_id == id) m_current_id = 0;
	reload_sessions();
}

void bssh_session_dialog::on_session_activated() {
	on_load();
	on_open();
}

void bssh_session_dialog::on_open() {
	const bssh_profile_t profile = current_profile();
	if (!profile.is_valid()) return;

	emit open_requested(profile);
}
