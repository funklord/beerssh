#include "ui/ssh_config_import.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QUrl>
#include <QVBoxLayout>

#include "model/profile_store.h"
#include "platform/platform.h"

namespace {

/* Everything the app cannot honour, phrased for someone who did not write it. */
QStringList caveats_of(const bssh_ssh_config_host_t &host) {
	QStringList out;
	if (!host.identity_file.isEmpty()) {
		out.append(QStringLiteral("uses a specific key (%1)").arg(host.identity_file));
	}
	if (!host.proxy_jump.isEmpty()) {
		out.append(QStringLiteral("connects via %1").arg(host.proxy_jump));
	}
	return out;
}

} /* namespace */

bssh_ssh_config_import::bssh_ssh_config_import(bssh_profile_store *store, QWidget *parent)
    : QDialog(parent),
      m_store(store),
      m_list(new QListWidget(this)),
      m_summary(new QLabel(this)),
      m_caveat(new QLabel(this)),
      m_ok(nullptr) {
	setWindowTitle(QStringLiteral("Import from OpenSSH config"));

	QVBoxLayout *layout = new QVBoxLayout(this);

	QLabel *intro = new QLabel(
	        QStringLiteral("These hosts were found. Importing copies them in once -- "
	                        "BeerSSH does not read the file again, so editing a host here "
	                        "will not change your SSH config, and editing your SSH config "
	                        "will not change these."),
	        this);
	intro->setWordWrap(true);
	layout->addWidget(intro);

	m_list->setSelectionMode(QAbstractItemView::NoSelection);
	layout->addWidget(m_list, 1);

	m_caveat->setWordWrap(true);
	layout->addWidget(m_caveat);

	layout->addWidget(m_summary);

	QDialogButtonBox *buttons =
	        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	m_ok = buttons->button(QDialogButtonBox::Ok);
	m_ok->setText(QStringLiteral("Import"));
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	layout->addWidget(buttons);

	/*
	 * Roomier rows on touch, for the same reason the host list has them
	 * (sec 8.5): a checkbox is the smallest thing here and the one that
	 * has to be hit accurately.
	 */
	const bssh_platform *platform = bssh_platform::instance();
	if (platform && platform->is_touch_primary()) {
		m_list->setStyleSheet(QStringLiteral("QListWidget::item { min-height: 48px; }"));
	}

	resize(520, 420);
}

void bssh_ssh_config_import::set_hosts(const QList<bssh_ssh_config_host_t> &hosts) {
	m_hosts = hosts;
	m_list->clear();

	for (const bssh_ssh_config_host_t &host : m_hosts) {
		const bssh_profile_t profile = host.to_profile();

		QString label = host.alias;
		if (profile.target() != host.alias) {
			label += QStringLiteral("  --  ") + profile.target();
		}

		const QStringList caveats = caveats_of(host);
		if (!caveats.isEmpty()) {
			label += QStringLiteral("   [%1]").arg(caveats.join(QStringLiteral(", ")));
		}

		QListWidgetItem *item = new QListWidgetItem(label, m_list);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(Qt::Checked);
	}

	connect(m_list, &QListWidget::itemChanged, this, &bssh_ssh_config_import::update_summary);
	update_summary();
}

void bssh_ssh_config_import::update_summary() {
	int checked = 0;
	int flagged = 0;

	for (int i = 0; i < m_list->count() && i < m_hosts.size(); ++i) {
		if (m_list->item(i)->checkState() != Qt::Checked) continue;
		++checked;
		if (m_hosts.at(i).has_unsupported()) ++flagged;
	}

	m_summary->setText(checked == 1 ? QStringLiteral("1 host selected.")
	                                : QStringLiteral("%1 hosts selected.").arg(checked));
	if (m_ok) m_ok->setEnabled(checked > 0);

	/*
	 * Said before the import rather than after, and said as what it means
	 * for connecting rather than as the name of a directive. A host that
	 * quietly loses its jump host looks like an app that cannot reach the
	 * machine, and the user has no reason to suspect the importer.
	 */
	if (flagged == 0) {
		m_caveat->clear();
		return;
	}

	m_caveat->setText(QStringLiteral(
	        "%1 of them use IdentityFile or ProxyJump, in brackets above. Those are "
	        "not imported: BeerSSH offers your default keys and has no jump-host "
	        "support yet, so a host needing either may not connect. The rest of its "
	        "settings still come across.").arg(flagged));
}

int bssh_ssh_config_import::import_selected() {
	if (!m_store) return 0;

	/*
	 * Added, never merged onto an existing host of the same name. The
	 * config file and the store are two independent lists after this
	 * point (see the dialog's own comment), so a second import must not
	 * silently rewrite hosts the user has since edited here.
	 */
	QStringList existing;
	for (const bssh_profile_t &profile : m_store->profiles()) existing.append(profile.name);

	int added = 0;
	for (int i = 0; i < m_list->count() && i < m_hosts.size(); ++i) {
		if (m_list->item(i)->checkState() != Qt::Checked) continue;

		bssh_profile_t profile = m_hosts.at(i).to_profile();
		if (existing.contains(profile.name, Qt::CaseInsensitive)) {
			profile.name += QStringLiteral(" (imported)");
		}

		if (m_store->save(profile) == 0) continue;
		existing.append(profile.name);
		++added;
	}

	return added;
}

int bssh_ssh_config_import::run(bssh_profile_store *store, QWidget *parent) {
	if (!store) return -1;

	const bssh_platform *platform = bssh_platform::instance();
	const bool touch = platform && platform->is_touch_primary();

	QFileDialog dialog(parent, QStringLiteral("Open OpenSSH config"));
	dialog.setAcceptMode(QFileDialog::AcceptOpen);
	dialog.setFileMode(QFileDialog::ExistingFile);

	if (touch) {
		/* Same wildcard, and the same known limitation, as the preset
		 * import -- see bssh_preset_editor::import_preset. */
		dialog.setMimeTypeFilters({ QStringLiteral("*/*") });
	} else {
		/*
		 * The config has no extension, so a name filter would hide the
		 * one file this dialog exists to open. Starting IN ~/.ssh is
		 * what saves the typing instead.
		 */
		const QFileInfo config(bssh_ssh_config::default_path());
		if (config.exists()) {
			dialog.setDirectory(config.absolutePath());
			dialog.selectFile(config.absoluteFilePath());
		}
	}

	if (dialog.exec() != QDialog::Accepted) return -1;

	const QList<QUrl> chosen = dialog.selectedUrls();
	if (chosen.isEmpty()) return -1;

	const QUrl url = chosen.first();
	const QString path = url.isLocalFile() ? url.toLocalFile() : url.toString();
	if (path.isEmpty()) return -1;

	bool ok = false;
	const QList<bssh_ssh_config_host_t> hosts = bssh_ssh_config::read(path, &ok);

	if (!ok) {
		QMessageBox::warning(parent, QStringLiteral("Import failed"),
		                      QStringLiteral("%1 could not be opened.").arg(path));
		return -1;
	}

	/*
	 * An empty result is reported rather than shown as an empty list. A
	 * config of nothing but `Host *` and Match blocks parses perfectly
	 * and describes no machine anyone can connect to, and "0 hosts" in a
	 * blank dialog reads as a broken importer.
	 */
	if (hosts.isEmpty()) {
		QMessageBox::information(parent, QStringLiteral("Nothing to import"),
		                          QStringLiteral("%1 parsed, but it names no specific hosts -- "
		                                          "only wildcard or Match rules.")
		                                  .arg(QFileInfo(path).fileName()));
		return -1;
	}

	bssh_ssh_config_import importer(store, parent);
	importer.set_hosts(hosts);
	if (importer.exec() != QDialog::Accepted) return -1;

	return importer.import_selected();
}
