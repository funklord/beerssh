#include "ui/session_dialog.h"

#include <QCloseEvent>

#include "ui/ssh_config_import.h"

#include "model/settings_resolver.h"
#include "platform/platform.h"
#include "term/colour_scheme.h"
#include "term/font_catalog.h"
#include "ui/font_picker.h"
#include "ui/override_badge.h"
#include "ui/app_settings_view.h"
#include "ui/licences_view.h"
#include "ui/preset_editor.h"

#include <QDir>
#include <QFileDialog>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>

#include "ui/wrapped_note.h"
#include <QLineEdit>
#include <QFormLayout>
#include <QListWidget>
#include <QMessageBox>
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
      m_font_picker(nullptr),
      m_preset_editor(nullptr),
      m_font_scope(nullptr),
      m_font_revert(nullptr) {
	setWindowTitle(QStringLiteral("BeerSSH Configuration"));

	/*
	 * The session list is rebuilt from the store rather than edited
	 * alongside it -- sec 8.4's rule for the tab bar and the registry,
	 * which applies to every view onto a shared thing. It was rebuilt
	 * only from this dialog's own saves, so with two windows open a host
	 * added or deleted in one left the other's list stale, and that
	 * window's next save wrote from a list the store no longer matched.
	 *
	 * The SELECTION is carried across, because this reload can now be
	 * caused by somebody else: a list that jumps under the user's cursor
	 * because another window saved is a new annoyance in place of an old
	 * staleness. The form is unaffected either way -- it is driven by
	 * m_current, which reload_sessions does not touch.
	 */
	if (m_store) {
		connect(m_store, &bssh_profile_store::changed, this, [this] {
			const int selected = m_sessions && m_sessions->currentItem()
			        ? m_sessions->currentItem()->data(Qt::UserRole).toInt()
			        : 0;

			reload_sessions();

			if (selected <= 0 || !m_sessions) return;
			for (int i = 0; i < m_sessions->count(); ++i) {
				if (m_sessions->item(i)->data(Qt::UserRole).toInt() == selected) {
					m_sessions->setCurrentRow(i);
					return;
				}
			}
		});
	}

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
	/*
	 * "Appearance", not "Window". The page has always been headed
	 * Appearance and holds the font, its size and the colours; the tree
	 * said Window, so the two words disagreed and the category named
	 * something the page does not set. PuTTY nests Appearance UNDER
	 * Window, which needs a sub-tree to be worth doing -- one accurate
	 * word is better than two that argue.
	 */
	QTreeWidgetItem *window = new QTreeWidgetItem(m_categories,
	        QStringList(QStringLiteral("Appearance")));
	/*
	 * Not a category PuTTY has, because presets are not a concept PuTTY
	 * has (sec 10). It sits at the top level rather than under Session
	 * because a preset is not a property of the session being opened --
	 * it is the thing many sessions share.
	 */
	QTreeWidgetItem *presets = new QTreeWidgetItem(m_categories,
	        QStringList(QStringLiteral("Presets")));

	QTreeWidgetItem *connection = new QTreeWidgetItem(m_categories,
	        QStringList(QStringLiteral("Connection")));

	/*
	 * sec 9.4's Open Source Licences screen. A category rather than an
	 * About button, because what it holds is not a boast about this app
	 * but a list of other people's terms, and those belong where settings
	 * are read rather than behind a splash.
	 */
	QTreeWidgetItem *application = new QTreeWidgetItem(m_categories,
	        QStringList(QStringLiteral("This device")));

	QTreeWidgetItem *licences = new QTreeWidgetItem(m_categories,
	        QStringList(QStringLiteral("Licences")));
	/*
	 * No SSH child any more. It was a signpost -- prose about keys and a
	 * button to the Session page -- and what it explained belongs beside
	 * the key field it is explaining, which is where it is now. A
	 * category that sets nothing is a category that reads as unfinished,
	 * and this one could not be given settings because it had none to
	 * give: the key is per host and lives on Session, and keepalive and
	 * compression are per preset and live on Connection.
	 */

	m_pages = new QStackedWidget(this);
	m_pages->addWidget(build_session_page());
	/*
	 * Terminal and Connection are REAL pages now (sec 8.5). They were
	 * prose plus a button reading "Open Presets", because what they name
	 * lives in a preset rather than in a session -- but PuTTY has both
	 * categories and both hold settings, so a tree that borrows PuTTY's
	 * shape and then leads nowhere reads as unfinished. Reported as
	 * exactly that.
	 *
	 * They are the same preset editor showing a subset of its fields, so
	 * there is one implementation of what TERM means, one save and one
	 * propagation -- and the preset list stays visible on each, which is
	 * what tells a reader they are editing something SHARED rather than
	 * this one session.
	 */
	m_pages->addWidget(new bssh_preset_editor(m_store, bssh_preset_editor::DESKTOP,
	                                           bssh_preset_editor::TERMINAL, this));
	m_pages->addWidget(build_window_page());
	m_preset_editor = new bssh_preset_editor(m_store, bssh_preset_editor::DESKTOP,
	                                          bssh_preset_editor::EVERYTHING, this);
	m_pages->addWidget(m_preset_editor);
	connect(m_preset_editor, &bssh_preset_editor::preset_saved,
	         this, &bssh_session_dialog::presets_changed);
	connect(m_preset_editor, &bssh_preset_editor::preset_removed,
	         this, &bssh_session_dialog::presets_changed);

	m_pages->addWidget(new bssh_preset_editor(m_store, bssh_preset_editor::DESKTOP,
	                                           bssh_preset_editor::CONNECTION, this));
	bssh_app_settings_view *device = new bssh_app_settings_view(this);
	m_pages->addWidget(device);
	connect(device, &bssh_app_settings_view::changed,
	         this, &bssh_session_dialog::device_settings_changed);

	m_pages->addWidget(new bssh_licences_view(this));

	Q_UNUSED(session);
	Q_UNUSED(terminal);
	Q_UNUSED(window);
	Q_UNUSED(presets);
	Q_UNUSED(connection);
	Q_UNUSED(licences);
	Q_UNUSED(application);

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

	/*
	 * One form, not four rows of label-then-field.
	 *
	 * They were four independent QHBoxLayouts, so nothing lined the
	 * fields up: "Host Name (or IP address)" is twice the width of "User
	 * name", and each field started wherever its own label ended. Four
	 * left edges for what reads as one column, on the front door, in a
	 * dialog whose whole premise (sec 8.5) is that it borrows an
	 * interface people already know -- and PuTTY aligns them.
	 *
	 * A form layout also makes each label the field's BUDDY, which is
	 * what the accessible names below were compensating for by hand.
	 * They stay, because they say more than the visible label does.
	 */
	QFormLayout *destination = new QFormLayout();
	destination->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	/*
	 * Host and port on one row, because a port is part of a destination
	 * rather than a setting of its own -- which is also where PuTTY puts
	 * it.
	 */
	QHBoxLayout *host_row = new QHBoxLayout();
	m_host = new QLineEdit(page);
	m_host->setAccessibleName(QStringLiteral("Host name or IP address"));
	host_row->addWidget(m_host, 1);

	host_row->addWidget(new QLabel(QStringLiteral("Port"), page));
	m_port = new QSpinBox(page);
	m_port->setRange(1, 65535);
	m_port->setValue(22);
	m_port->setMaximumWidth(80);
	m_port->setAccessibleName(QStringLiteral("Port"));
	host_row->addWidget(m_port);

	destination->addRow(QStringLiteral("Host Name (or IP address)"), host_row);

	m_user = new QLineEdit(page);
	m_user->setAccessibleName(QStringLiteral("User name"));
	destination->addRow(QStringLiteral("User name"), m_user);

	/*
	 * The key to offer this host, which is where an imported OpenSSH
	 * IdentityFile lands (sec 10.5). PuTTY files this under
	 * Connection > SSH > Auth; it sits with the destination here because
	 * that page is still a placeholder and a field nobody can find is a
	 * field that may as well not exist.
	 *
	 * Blank means the default identities, which is what almost everyone
	 * wants, so the placeholder says so rather than leaving an empty box
	 * looking unfinished. Browse is offered here and not on the touch
	 * editor because here the key really is a file on this machine.
	 */
	QHBoxLayout *identity_row = new QHBoxLayout();
	m_identity = new QLineEdit(page);
	m_identity->setPlaceholderText(QStringLiteral("default keys (~/.ssh/id_ed25519 and friends)"));
	m_identity->setAccessibleName(QStringLiteral("Private key file"));
	m_identity->setAccessibleDescription(
	        QStringLiteral("Leave empty to offer your default keys."));
	identity_row->addWidget(m_identity, 1);

	QPushButton *browse_key = new QPushButton(QStringLiteral("Browse..."), page);
	identity_row->addWidget(browse_key);

	destination->addRow(QStringLiteral("Private key file"), identity_row);

	connect(browse_key, &QPushButton::clicked, this, [this] {
		/*
		 * Starting in ~/.ssh, and with no name filter: a private key has
		 * no extension, so filtering by one would hide every file worth
		 * choosing.
		 */
		const QString start = QDir::homePath() + QStringLiteral("/.ssh");
		const QString chosen = QFileDialog::getOpenFileName(
		        this, QStringLiteral("Private key file"),
		        QDir(start).exists() ? start : QDir::homePath());
		if (!chosen.isEmpty()) m_identity->setText(chosen);
	});

	/*
	 * The bastion, if this host sits behind one (sec 10.5). Beside the
	 * key rather than on a page of its own: both answer "how do I get in
	 * to this machine", and both arrive together from an OpenSSH import.
	 *
	 * No Browse, because there is nothing to browse -- it is a host, and
	 * the field is spelled the way OpenSSH's ProxyJump and -J are
	 * spelled, since that is the string the user already has.
	 */
	m_jump = new QLineEdit(page);
	m_jump->setPlaceholderText(QStringLiteral("direct (or user@bastion[:port])"));
	m_jump->setAccessibleName(QStringLiteral("Connect via jump host"));
	m_jump->setAccessibleDescription(
	        QStringLiteral("Leave empty to connect straight to the host."));
	destination->addRow(QStringLiteral("Connect via"), m_jump);

	/*
	 * What the key field means, beside the key field (sec 8.5). This was
	 * a whole category of its own -- an SSH page holding a paragraph and
	 * a button back to here -- which put the explanation one click away
	 * from the control it explains.
	 */
	QLabel *keys_note = bssh_wrapped_note(
	        QStringLiteral("Keys are offered before any password is asked for: the agent "
	                        "first, then your default identities. Name a file above only "
	                        "if this host needs a particular key. Host keys are trusted "
	                        "on first use and checked every time after."),
	        page);
	destination->addRow(keys_note);

	layout->addLayout(destination);

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
	m_session_name->setAccessibleName(QStringLiteral("Saved session name"));
	saved_layout->addWidget(m_session_name);

	QHBoxLayout *list_row = new QHBoxLayout();
	m_sessions = new QListWidget(saved);

	/* By name: findChild order is not a contract, and this widget holds
	 * more than one list. */
	m_sessions->setObjectName(QStringLiteral("session_list"));
	list_row->addWidget(m_sessions, 1);

	QVBoxLayout *buttons = new QVBoxLayout();
	QPushButton *load = new QPushButton(QStringLiteral("Load"), saved);
	QPushButton *save = new QPushButton(QStringLiteral("Save"), saved);
	QPushButton *remove = new QPushButton(QStringLiteral("Delete"), saved);
	/*
	 * Beside Load/Save/Delete because importing IS a way of getting a
	 * saved session, and sec 10.5 notes this is worth most on the desktop
	 * -- where the user's real ~/.ssh/config is usually sitting right
	 * there.
	 */
	QPushButton *import_config = new QPushButton(QStringLiteral("From SSH config..."), saved);
	import_config->setToolTip(
	        QStringLiteral("Copy hosts in from an OpenSSH config file. Read once, "
	                        "not followed afterwards."));
	buttons->addWidget(load);
	buttons->addWidget(save);
	buttons->addWidget(remove);
	buttons->addWidget(import_config);
	buttons->addStretch(1);
	list_row->addLayout(buttons);
	saved_layout->addLayout(list_row);

	layout->addWidget(saved, 1);

	QHBoxLayout *actions = new QHBoxLayout();
	actions->addStretch(1);
	QPushButton *open = new QPushButton(QStringLiteral("Open"), page);
	QPushButton *cancel = new QPushButton(QStringLiteral("Cancel"), page);
	/* Named so a test can press it rather than emit the signal behind it. */
	cancel->setObjectName(QStringLiteral("front_door_cancel"));
	open->setDefault(true);
	actions->addWidget(open);
	actions->addWidget(cancel);
	layout->addLayout(actions);

	connect(load, &QPushButton::clicked, this, &bssh_session_dialog::on_load);
	connect(save, &QPushButton::clicked, this, &bssh_session_dialog::on_save);
	connect(remove, &QPushButton::clicked, this, &bssh_session_dialog::on_delete);
	connect(import_config, &QPushButton::clicked, this, &bssh_session_dialog::on_import_ssh_config);
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

/*
 * A category whose settings live somewhere else, saying where and taking
 * you there.
 *
 * These three were placeholders promising the settings "arrive at M5/M6/
 * M10". All three milestones have since landed and every setting they
 * named is real -- it just lives in Presets, because sec 10 made these
 * properties of a preset rather than of one session. A page that still
 * says "not built yet" about shipped work is worse than one that is
 * merely empty: it sends the user away from the thing they were looking
 * for, and it is the kind of stale text nobody re-reads.
 */
QWidget *bssh_session_dialog::build_signpost_page(const QString &title,
                                                   const QString &body_text,
                                                   const QString &destination) {
	QWidget *page = new QWidget(this);
	QVBoxLayout *layout = new QVBoxLayout(page);

	QLabel *heading = new QLabel(QStringLiteral("<b>%1</b>").arg(title), page);
	QLabel *body = bssh_wrapped_note(body_text, page);

	layout->addWidget(heading);
	layout->addWidget(body);

	QPushButton *go = new QPushButton(QStringLiteral("Open %1").arg(destination), page);
	go->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
	connect(go, &QPushButton::clicked, this, [this, destination] {
		select_category(destination);
	});
	layout->addWidget(go);

	layout->addStretch(1);
	return page;
}

/* Selects a category by name, so a signpost can act rather than only point. */
void bssh_session_dialog::select_category(const QString &name) {
	const QList<QTreeWidgetItem *> found =
	        m_categories->findItems(name, Qt::MatchExactly | Qt::MatchRecursive);
	if (found.isEmpty()) return;

	m_categories->setCurrentItem(found.first());
	on_category_changed();
}


void bssh_session_dialog::on_category_changed() {
	const QList<QTreeWidgetItem *> selected = m_categories->selectedItems();
	if (selected.isEmpty()) return;

	const QString name = selected.first()->text(0);
	/*
	 * Indices, and they must match the order the pages were added in the
	 * constructor. Removing the SSH page shifted the two below it, which
	 * a list of magic numbers cannot notice -- so the categories are
	 * asserted against these by a test rather than trusted.
	 */
	if (name == QStringLiteral("Session")) m_pages->setCurrentIndex(0);
	else if (name == QStringLiteral("Terminal")) m_pages->setCurrentIndex(1);
	else if (name == QStringLiteral("Appearance")) m_pages->setCurrentIndex(2);
	else if (name == QStringLiteral("Presets")) m_pages->setCurrentIndex(3);
	else if (name == QStringLiteral("Connection")) m_pages->setCurrentIndex(4);
	else if (name == QStringLiteral("This device")) m_pages->setCurrentIndex(5);
	else if (name == QStringLiteral("Licences")) m_pages->setCurrentIndex(6);
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
	/*
	 * Starts from the loaded profile so that everything this form does not
	 * show -- the preset it follows, its other overrides, whether a
	 * password is stored -- survives a save.
	 */
	bssh_profile_t profile = m_current;
	profile.name = m_session_name->text().trimmed();
	profile.host = m_host->text().trimmed();
	profile.user = m_user->text().trimmed();
	profile.port = static_cast<quint16>(m_port->value());
	profile.identity_file = m_identity->text().trimmed();
	profile.proxy_jump = m_jump->text().trimmed();

	/*
	 * An unnamed host is named after itself, as the touch editor has
	 * always done. Only this form let a nameless one through, and it
	 * shows up as a blank row in the saved-sessions list -- the one place
	 * a host has to be recognisable.
	 */
	if (profile.name.isEmpty()) profile.name = profile.host;

	/*
	 * The appearance fields as sparse overrides, written only where they
	 * differ from what this host inherits and dropped where they agree
	 * (sec 10.1). Shared with the touch editor, because the two used to
	 * compute it differently and this one got it wrong: it compared the
	 * picker against what the host resolves to WITH its override applied,
	 * so choosing the value the preset already gives recorded an override
	 * pinning it, while the badge two lines away said "Following".
	 */
	if (m_font_picker) {
		profile.overrides = bssh_appearance_overrides(
		        m_store, m_current, m_font_picker->font_id(),
		        m_font_picker->point_size(), m_font_picker->scheme_id());
	}

	return profile;
}

QJsonObject bssh_session_dialog::resolved_settings(const bssh_profile_t &profile) const {
	if (!m_store || profile.preset_id == 0) return bssh_settings_resolver::app_defaults();

	return bssh_settings_resolver::resolve(m_store->preset(profile.preset_id),
	                                        profile.overrides);
}

void bssh_session_dialog::set_current_profile(const bssh_profile_t &profile) {
	m_current = profile;
	m_session_name->setText(profile.name);
	m_host->setText(profile.host);
	m_user->setText(profile.user);
	m_port->setValue(profile.port);
	m_identity->setText(profile.identity_file);
	m_jump->setText(profile.proxy_jump);

	if (m_font_picker) {
		const QJsonObject resolved = resolved_settings(profile);
		m_font_picker->set_font_choice(
		        resolved.value(QStringLiteral("font_family")).toString(),
		        resolved.value(QStringLiteral("font_size")).toInt());
		m_font_picker->set_scheme(
		        resolved.value(QStringLiteral("colour_scheme")).toString());
		update_override_badges();
	}
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

	const int saved = m_store->save(profile);
	if (saved == 0) {
		/*
		 * The same silence the touch editor had: the store can now say
		 * a write did not reach the disk (sec 10.6), and returning as
		 * though it had is how a session is lost between here and the
		 * next start.
		 */
		QMessageBox::warning(
		        this, QStringLiteral("Not saved"),
		        QStringLiteral("This session could not be written to your "
		                        "configuration.\n\n%1").arg(m_store->last_error()));
		return;
	}

	m_current.id = saved;
	reload_sessions();
}

void bssh_session_dialog::on_import_ssh_config() {
	const int added = bssh_ssh_config_import::run(m_store, this);
	if (added < 0) return;   /* backed out; not an error */

	reload_sessions();

	QMessageBox::information(
	        this, QStringLiteral("Hosts imported"),
	        added == 1 ? QStringLiteral("1 host was added to your saved sessions.")
	                   : QStringLiteral("%1 hosts were added to your saved sessions.").arg(added));
}

void bssh_session_dialog::on_delete() {
	const int id = selected_profile_id();
	if (id == 0 || !m_store) return;

	m_store->remove(id);
	if (m_current.id == id) m_current = bssh_profile_t();
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

QWidget *bssh_session_dialog::build_window_page() {
	QWidget *page = new QWidget(this);
	QVBoxLayout *layout = new QVBoxLayout(page);

	QLabel *heading = new QLabel(QStringLiteral("<b>Appearance</b>"), page);
	layout->addWidget(heading);

	m_font_picker = new bssh_font_picker(page);

	/*
	 * Named, because the dialog contains several pickers -- this one and
	 * one inside each embedded preset editor -- and a test that finds it
	 * by what it is DISPLAYING is guessing. That guess held while there
	 * were two and broke when Terminal and Connection became real pages:
	 * a preset with no font of its own shows the platform default, so
	 * more than one picker started reporting the same family.
	 */
	m_font_picker->setObjectName(QStringLiteral("host_font_picker"));
	layout->addWidget(m_font_picker);

	/*
	 * sec 10.1's badge and revert control. The three layers only mean
	 * something if a person can see which one a value is coming from:
	 * without this, "this host looks different from the others" and "this
	 * host overrides its preset" are the same observation, and the way
	 * back to following the preset is deleting and re-adding the host.
	 */
	QHBoxLayout *scope_row = new QHBoxLayout();

	m_font_scope = new QLabel(page);

	/* By name, as the picker beside it is: findChild order is not a
	 * contract, and this dialog holds more than one QLabel. */
	m_font_scope->setObjectName(QStringLiteral("host_font_scope"));
	bssh_wrap_note(m_font_scope);
	scope_row->addWidget(m_font_scope, 1);

	m_font_revert = new QPushButton(QStringLiteral("Use the preset's font"), page);
	scope_row->addWidget(m_font_revert, 0);
	layout->addLayout(scope_row);

	connect(m_font_revert, &QPushButton::clicked, this, [this] {
		/*
		 * Drops the override rather than setting the field to the
		 * preset's current value. The difference matters and is the
		 * whole point of sec 10.1's revert: a copied value stops
		 * following the preset the next time the preset changes, which
		 * is precisely what the user asked to undo.
		 */
		m_current.overrides =
		        bssh_settings_resolver::revert_field(m_current.overrides,
		                                              QStringLiteral("font_family"));
		m_current.overrides =
		        bssh_settings_resolver::revert_field(m_current.overrides,
		                                              QStringLiteral("font_size"));

		const QJsonObject resolved = resolved_settings(m_current);
		m_font_picker->set_font_choice(
		        resolved.value(QStringLiteral("font_family")).toString(),
		        resolved.value(QStringLiteral("font_size")).toInt());

		update_override_badges();
	});

	connect(m_font_picker, &bssh_font_picker::font_chosen,
	         this, [this] { update_override_badges(); });

	/*
	 * And the colours, which had no listener anywhere in the tree. The
	 * badge weighed only the font and the size, so a scheme-only change
	 * wrote an override and said "Following preset X" one line below it.
	 */
	connect(m_font_picker, &bssh_font_picker::scheme_chosen,
	         this, [this] { update_override_badges(); });

	layout->addStretch(1);
	return page;
}

void bssh_session_dialog::update_override_badges() {
	if (!m_font_scope || !m_font_picker) return;

	const bssh_override_badge_t badge = bssh_appearance_override_badge(
	        m_store, m_current, m_font_picker->font_id(), m_font_picker->point_size(),
	        m_font_picker->scheme_id());

	m_font_scope->setText(badge.text);
	if (m_font_revert) m_font_revert->setEnabled(badge.overridden);
}

void bssh_session_dialog::closeEvent(QCloseEvent *event) {
	QWidget::closeEvent(event);
	emit cancelled();
}
