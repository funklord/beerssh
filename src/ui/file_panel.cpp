#include "ui/file_panel.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMimeData>
#include <QStandardPaths>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#include "model/session.h"

namespace {

enum column_t { NAME, SIZE, MODIFIED };

/* Opens as a directory: one, or a link that leads to one. */
bool opens(const bssh_remote_entry_t &entry) {
	return entry.kind == bssh_remote_entry_t::DIRECTORY || entry.leads_to_directory;
}

QString parent_of(const QString &path) {
	if (path.isEmpty() || path == QStringLiteral("/")) return path;
	const int slash = path.lastIndexOf(QLatin1Char('/'));
	return slash <= 0 ? QStringLiteral("/") : path.left(slash);
}

}

bssh_file_panel::bssh_file_panel(bssh_session *session, QWidget *parent)
    : QWidget(parent),
      m_session(session),
      m_download_directory(QStandardPaths::writableLocation(QStandardPaths::DownloadLocation)) {
	setObjectName(QStringLiteral("file-panel"));
	setAcceptDrops(true);

	QVBoxLayout *column = new QVBoxLayout(this);
	column->setContentsMargins(4, 4, 4, 4);
	column->setSpacing(4);

	/* Up, the path (editable: a typed path is listed on Enter), the
	 * shell's directory, again, and close. */
	QHBoxLayout *row = new QHBoxLayout;
	row->setSpacing(2);
	m_up = new QToolButton(this);
	m_up->setIcon(style()->standardIcon(QStyle::SP_FileDialogToParent));
	m_up->setToolTip(tr("Up a directory"));
	m_up->setAccessibleName(tr("Up a directory"));
	row->addWidget(m_up);
	m_location = new QLineEdit(this);
	m_location->setAccessibleName(tr("Remote directory"));
	m_location->setPlaceholderText(tr("Directory on the host"));
	row->addWidget(m_location, 1);
	QToolButton *shell = new QToolButton(this);
	shell->setIcon(style()->standardIcon(QStyle::SP_DirHomeIcon));
	shell->setToolTip(tr("Where the shell is"));
	shell->setAccessibleName(tr("Where the shell is"));
	row->addWidget(shell);
	QToolButton *again = new QToolButton(this);
	again->setIcon(style()->standardIcon(QStyle::SP_BrowserReload));
	again->setToolTip(tr("List again"));
	again->setAccessibleName(tr("List again"));
	row->addWidget(again);
	QToolButton *close = new QToolButton(this);
	close->setIcon(style()->standardIcon(QStyle::SP_TitleBarCloseButton));
	close->setToolTip(tr("Close the file panel"));
	close->setAccessibleName(tr("Close the file panel"));
	row->addWidget(close);
	column->addLayout(row);

	m_list = new QTreeWidget(this);
	m_list->setObjectName(QStringLiteral("file-panel-list"));
	m_list->setAccessibleName(tr("Files on the host"));
	m_list->setHeaderLabels({ tr("Name"), tr("Size"), tr("Modified") });
	m_list->setRootIsDecorated(false);
	m_list->setUniformRowHeights(true);
	m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
	m_list->setContextMenuPolicy(Qt::CustomContextMenu);
	m_list->header()->setStretchLastSection(false);
	m_list->header()->setSectionResizeMode(NAME, QHeaderView::Stretch);
	m_list->header()->setSectionResizeMode(SIZE, QHeaderView::ResizeToContents);
	m_list->header()->setSectionResizeMode(MODIFIED, QHeaderView::ResizeToContents);
	column->addWidget(m_list, 1);

	QHBoxLayout *foot = new QHBoxLayout;
	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	foot->addWidget(m_status, 1);
	m_stop = new QToolButton(this);
	m_stop->setText(tr("Stop"));
	m_stop->setAccessibleName(tr("Stop the copy"));
	m_stop->hide();
	foot->addWidget(m_stop);
	column->addLayout(foot);

	connect(m_up, &QToolButton::clicked, this, [this] {
		if (!m_path.isEmpty()) open_directory(parent_of(m_path));
	});
	connect(m_location, &QLineEdit::returnPressed, this, [this] { open_directory(m_location->text()); });
	connect(shell, &QToolButton::clicked, this, &bssh_file_panel::open_shells_directory);
	connect(again, &QToolButton::clicked, this, [this] { open_directory(m_path); });
	connect(close, &QToolButton::clicked, this, &bssh_file_panel::close_requested);
	connect(m_list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) { activate(item); });
	connect(m_list, &QWidget::customContextMenuRequested, this, &bssh_file_panel::show_menu);
	connect(m_stop, &QToolButton::clicked, this, [this] {
		if (!m_session) return;
		if (m_session->download_in_progress()) m_session->cancel_download();
		if (m_session->upload_in_progress()) m_session->cancel_upload();
	});

	if (!m_session) return;
	connect(m_session, &bssh_session::directory_listed, this,
	        [this](const QString &path, const QList<bssh_remote_entry_t> &entries, const QString &why) {
		/* Only the answers to this panel's own questions. */
		if (m_listings_asked <= 0) return;
		--m_listings_asked;
		if (m_listings_asked == 0) show_listing(path, entries, why);
	});
	connect(m_session, &bssh_session::download_progress, this, [this](qint64 done, qint64 total) {
		say(tr("Copying here: %1 of %2").arg(QLocale().formattedDataSize(done),
		                                     QLocale().formattedDataSize(total)), true);
	});
	connect(m_session, &bssh_session::download_finished, this,
	        [this](bool ok, const QString &directory, const QStringList &written, const QString &why) {
		if (ok) say(tr("Copied %1 to %2").arg(written.join(QStringLiteral(", ")), directory));
		else if (why == QLatin1String(BSSH_COPY_STOPPED)) say(tr("Copy stopped"));
		else say(tr("Copy failed: %1").arg(why));
	});
	connect(m_session, &bssh_session::upload_progress, this, [this](qint64 done, qint64 total) {
		say(tr("Copying there: %1 of %2").arg(QLocale().formattedDataSize(done),
		                                      QLocale().formattedDataSize(total)), true);
	});
	connect(m_session, &bssh_session::upload_finished, this,
	        [this](bool ok, const QString &directory, const QStringList &written, const QString &why) {
		if (ok) say(tr("Copied %1 to %2").arg(written.join(QStringLiteral(", ")), directory));
		else if (why == QLatin1String(BSSH_COPY_STOPPED)) say(tr("Copy stopped"));
		else say(tr("Copy failed: %1").arg(why));
		/* What arrived in the directory shown is shown. */
		if (directory == m_path || directory.isEmpty()) open_directory(m_path);
	});
}

QSize bssh_file_panel::sizeHint() const {
	return QSize(fontMetrics().averageCharWidth() * 40, QWidget::sizeHint().height());
}

void bssh_file_panel::open_directory(const QString &path) {
	if (!m_session) return;
	++m_listings_asked;
	say(tr("Listing %1").arg(path.isEmpty() ? tr("the home directory") : path));
	m_session->list_directory(path);
}

void bssh_file_panel::open_shells_directory() {
	if (!m_session) return;
	/*
	 * As a drop finds it (sec 6.4): one answer, then let go. Where the
	 * shell is elsewhere -- ssh onward -- or nowhere known, the home
	 * directory, said so by the listing's path.
	 */
	const QPointer<bssh_file_panel> self = this;
	auto answered = std::make_shared<QMetaObject::Connection>();
	*answered = connect(m_session, &bssh_session::working_directory_found, this,
	                    [self, answered](const bssh_working_directory_t &found) {
		disconnect(*answered);
		if (!self) return;
		self->open_directory(found.elsewhere ? QString() : found.path);
	});
	say(tr("Finding the shell's directory"));
	m_session->find_working_directory();
}

void bssh_file_panel::show_listing(const QString &path, const QList<bssh_remote_entry_t> &entries,
                                   const QString &why) {
	if (!why.isEmpty()) {
		say(tr("Could not list %1: %2").arg(path, why));
		m_location->setText(m_path);
		emit listed();
		return;
	}
	m_path = path;
	m_location->setText(path);
	m_up->setEnabled(path != QStringLiteral("/"));

	/* Directories first, then by name with case aside, as file managers do. */
	m_entries = entries;
	std::stable_sort(m_entries.begin(), m_entries.end(),
	                 [](const bssh_remote_entry_t &a, const bssh_remote_entry_t &b) {
		if (opens(a) != opens(b)) return opens(a);
		return QString::compare(a.name, b.name, Qt::CaseInsensitive) < 0;
	});

	m_list->clear();
	const QIcon folder = style()->standardIcon(QStyle::SP_DirIcon);
	const QIcon file = style()->standardIcon(QStyle::SP_FileIcon);
	const QIcon link_to_folder = style()->standardIcon(QStyle::SP_DirLinkIcon);
	const QIcon link = style()->standardIcon(QStyle::SP_FileLinkIcon);
	for (int i = 0; i < m_entries.size(); ++i) {
		const bssh_remote_entry_t &entry = m_entries.at(i);
		QTreeWidgetItem *item = new QTreeWidgetItem(m_list);
		item->setText(NAME, entry.name);
		item->setData(NAME, Qt::UserRole, i);
		const bool is_link = entry.kind == bssh_remote_entry_t::LINK;
		item->setIcon(NAME, opens(entry) ? (is_link ? link_to_folder : folder) : (is_link ? link : file));
		if (!opens(entry)) {
			item->setText(SIZE, QLocale().formattedDataSize(entry.size));
			item->setTextAlignment(SIZE, Qt::AlignRight | Qt::AlignVCenter);
		}
		if (entry.modified.isValid())
			item->setText(MODIFIED, QLocale().toString(entry.modified, QLocale::ShortFormat));
	}
	say(entries.isEmpty() ? tr("Empty") : tr("%n item(s)", nullptr, int(entries.size())));
	emit listed();
}

QString bssh_file_panel::remote_path_of(const bssh_remote_entry_t &entry) const {
	return m_path.endsWith(QLatin1Char('/')) ? m_path + entry.name : m_path + QLatin1Char('/') + entry.name;
}

void bssh_file_panel::activate(QTreeWidgetItem *item) {
	if (!item) return;
	const int at = item->data(NAME, Qt::UserRole).toInt();
	if (at < 0 || at >= m_entries.size()) return;
	const bssh_remote_entry_t &entry = m_entries.at(at);
	if (opens(entry)) {
		open_directory(remote_path_of(entry));
		return;
	}
	copy_selected_to(m_download_directory);
}

void bssh_file_panel::copy_selected_to(const QString &directory) {
	if (!m_session || directory.isEmpty()) return;
	if (m_session->download_in_progress()) {
		say(tr("A copy is still running; stop it or let it finish"), true);
		return;
	}
	QStringList paths;
	for (QTreeWidgetItem *item : m_list->selectedItems()) {
		const int at = item->data(NAME, Qt::UserRole).toInt();
		if (at >= 0 && at < m_entries.size()) paths << remote_path_of(m_entries.at(at));
	}
	if (paths.isEmpty()) return;
	m_session->download(paths, directory);
}

void bssh_file_panel::show_menu(const QPoint &at) {
	if (m_list->selectedItems().isEmpty()) return;
	QMenu menu(this);
	QAction *downloads = menu.addAction(tr("Copy to Downloads"));
	downloads->setEnabled(!m_download_directory.isEmpty());
	QAction *elsewhere = menu.addAction(tr("Copy to..."));
	const QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(at));
	if (chosen == downloads) copy_selected_to(m_download_directory);
	else if (chosen == elsewhere) {
		const QString directory = QFileDialog::getExistingDirectory(this, tr("Copy to"), m_download_directory);
		copy_selected_to(directory);
	}
}

void bssh_file_panel::say(const QString &text, bool stoppable) {
	m_status->setText(text);
	m_stop->setVisible(stoppable);
}

void bssh_file_panel::dragEnterEvent(QDragEnterEvent *event) {
	if (event->mimeData()->hasUrls() && !m_path.isEmpty()) event->acceptProposedAction();
}

/*
 * Local files dropped here go to the directory shown. The event only
 * gathers them: Qt swallows a synthetic QDropEvent, so no test reaches
 * this, and copy_here() is what they drive (as the terminal's drop is
 * driven from its signal).
 */
void bssh_file_panel::dropEvent(QDropEvent *event) {
	QStringList paths;
	for (const QUrl &url : event->mimeData()->urls())
		if (url.isLocalFile()) paths << url.toLocalFile();
	if (copy_here(paths)) event->acceptProposedAction();
}

bool bssh_file_panel::copy_here(const QStringList &paths) {
	if (!m_session || m_path.isEmpty() || paths.isEmpty()) return false;
	if (m_session->upload_in_progress()) {
		say(tr("A copy is still running; stop it or let it finish"), true);
		return false;
	}
	m_session->upload(paths, m_path);
	return true;
}
