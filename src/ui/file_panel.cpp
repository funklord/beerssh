#include "ui/file_panel.h"

#include <QApplication>
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QProgressBar>
#include <QStandardPaths>
#include <QStyle>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>

#include "model/session.h"
#include "ui/remote_drag.h"

#include <QFileInfo>
#include <QGuiApplication>
#include <QTimer>

#if QT_CONFIG(dbus)
#include <QDBusConnection>
#endif
#include <QSysInfo>

#if !defined(Q_OS_ANDROID) && QT_CONFIG(xcb)
#include <xcb/xcb.h>
#endif

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
#ifdef Q_OS_ANDROID
	/* Android's Downloads location is this app's own, out of the user's
	 * sight: a file opened asks where to go instead (the document picker). */
	m_download_directory.clear();
#endif

	/*
	 * A grip on the edge it shares with the terminal, dragged to make the
	 * panel wider or narrower; the width is the session's, as its
	 * directory is, so a rebuild keeps it.
	 */
	QHBoxLayout *edge = new QHBoxLayout(this);
	edge->setContentsMargins(0, 0, 0, 0);
	edge->setSpacing(0);
	m_grip = new QWidget(this);
	m_grip->setObjectName(QStringLiteral("file-panel-grip"));
	m_grip->setFixedWidth(5);
	m_grip->setCursor(Qt::SizeHorCursor);
	m_grip->setAccessibleName(tr("Drag to resize the file panel"));
	m_grip->installEventFilter(this);
	edge->addWidget(m_grip);
	QVBoxLayout *column = new QVBoxLayout;
	column->setContentsMargins(4, 4, 4, 4);
	column->setSpacing(4);
	edge->addLayout(column, 1);

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
	/* The menu from a button too: on touch nothing opens the list's own. */
	QToolButton *more = new QToolButton(this);
	more->setText(QStringLiteral("\u22ef"));
	more->setToolTip(tr("More"));
	more->setAccessibleName(tr("More"));
	row->insertWidget(row->count() - 1, more);
	row->addWidget(close);
	column->addLayout(row);

	m_list = new QTreeWidget(this);
	m_list->setObjectName(QStringLiteral("file-panel-list"));
	m_list->setAccessibleName(tr("Files on the host"));
	m_list->setHeaderLabels({ tr("Name"), tr("Size"), tr("Modified") });
	m_list->setRootIsDecorated(false);
	m_list->setUniformRowHeights(true);
	m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
	/* Entries dragged out to a file manager (sec 6.4): the drag is the
	 * panel's own, started from the list's viewport. */
	m_list->viewport()->installEventFilter(this);
	m_list->setContextMenuPolicy(Qt::CustomContextMenu);
	m_list->header()->setStretchLastSection(false);
	m_list->header()->setSectionResizeMode(NAME, QHeaderView::Stretch);
	m_list->header()->setSectionResizeMode(SIZE, QHeaderView::ResizeToContents);
	m_list->header()->setSectionResizeMode(MODIFIED, QHeaderView::ResizeToContents);
	column->addWidget(m_list, 1);

	/*
	 * The listing's own line, then one row per direction of copy with a
	 * bar and its own Stop: browsing while a copy runs no longer writes
	 * over its progress, and a copy up and one back can be watched and
	 * stopped apart.
	 */
	m_status = new QLabel(this);
	m_status->setWordWrap(true);
	column->addWidget(m_status);
	m_copy_back = make_copy_row(column, tr("Stop copying here"));
	m_copy_up = make_copy_row(column, tr("Stop copying there"));

	connect(m_up, &QToolButton::clicked, this, [this] {
		if (!m_path.isEmpty()) open_directory(parent_of(m_path));
	});
	connect(m_location, &QLineEdit::returnPressed, this, [this] { open_directory(m_location->text()); });
	connect(shell, &QToolButton::clicked, this, &bssh_file_panel::open_shells_directory);
	connect(again, &QToolButton::clicked, this, [this] { open_directory(m_path); });
	connect(close, &QToolButton::clicked, this, &bssh_file_panel::close_requested);
	connect(m_list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) { activate(item); });
	connect(m_list, &QWidget::customContextMenuRequested, this, [this](const QPoint &at) {
		show_menu(m_list->viewport()->mapToGlobal(at));
	});
	connect(more, &QToolButton::clicked, this, [this, more] {
		show_menu(more->mapToGlobal(QPoint(0, more->height())));
	});
	connect(m_copy_back.stop, &QToolButton::clicked, this, [this] {
		if (m_session && m_session->download_in_progress()) m_session->cancel_download();
	});
	connect(m_copy_up.stop, &QToolButton::clicked, this, [this] {
		if (m_session && m_session->upload_in_progress()) m_session->cancel_upload();
	});

	if (!m_session) return;
	connect(m_session, &bssh_session::directory_listed, this,
	        [this](const QString &path, const QList<bssh_remote_entry_t> &entries, const QString &why) {
		/* Only the answers to this panel's own questions. */
		if (m_listings_asked <= 0) return;
		--m_listings_asked;
		if (m_listings_asked == 0) show_listing(path, entries, why);
	});
	connect(m_session, &bssh_session::remote_changed, this, &bssh_file_panel::changed);
	connect(m_session, &bssh_session::download_progress, this, [this](qint64 done, qint64 total) {
		show_copy(m_copy_back, tr("Copying here"), done, total);
	});
	connect(m_session, &bssh_session::download_finished, this,
	        [this](bool ok, const QString &directory, const QStringList &written, const QString &why) {
		if (!m_discard_dir.isEmpty() && directory == m_discard_dir) {
			/* An abandoned drag's staging, stopped: now it goes. */
			QDir(m_discard_dir).removeRecursively();
			m_discard_dir.clear();
			m_staging_dir.clear();
			if (!m_after_discard_dir.isEmpty() && m_session) {
				/* The staging made way for an XDS save: now it starts. */
				const QStringList paths = m_after_discard_paths;
				const QString dir = m_after_discard_dir;
				m_after_discard_paths.clear();
				m_after_discard_dir.clear();
				m_session->download(paths, dir);
				return;
			}
			end_copy(m_copy_back, ok, directory, written, why);
			return;
		}
		if (!m_staging_dir.isEmpty() && directory == m_staging_dir) {
			/* A drag's copies, whole: a drop from now on gets them. */
			QStringList staged;
			for (const QString &name : written) staged << QDir(directory).filePath(name);
			if (ok && m_staging_data) m_staging_data->set_staged(staged);
			m_staging_dir.clear();
			end_copy(m_copy_back, ok, tr("here, ready to drop"), written, why);
			return;
		}
		end_copy(m_copy_back, ok, directory, written, why);
	});
	connect(m_session, &bssh_session::upload_progress, this, [this](qint64 done, qint64 total) {
		show_copy(m_copy_up, tr("Copying there"), done, total);
	});
	connect(m_session, &bssh_session::upload_finished, this,
	        [this](bool ok, const QString &directory, const QStringList &written, const QString &why) {
		end_copy(m_copy_up, ok, directory, written, why);
		/* What arrived in the directory shown is shown. */
		if (directory == m_path || directory.isEmpty()) open_directory(m_path);
	});
}

bssh_file_panel::copy_row bssh_file_panel::make_copy_row(QVBoxLayout *column, const QString &stop_name) {
	copy_row row;
	row.row = new QWidget(this);
	QVBoxLayout *stack = new QVBoxLayout(row.row);
	stack->setContentsMargins(0, 0, 0, 0);
	stack->setSpacing(2);
	row.label = new QLabel(row.row);
	row.label->setWordWrap(true);
	stack->addWidget(row.label);
	QHBoxLayout *line = new QHBoxLayout;
	row.bar = new QProgressBar(row.row);
	row.bar->setTextVisible(true);
	line->addWidget(row.bar, 1);
	row.stop = new QToolButton(row.row);
	row.stop->setText(tr("Stop"));
	row.stop->setAccessibleName(stop_name);
	line->addWidget(row.stop);
	stack->addLayout(line);
	row.row->hide();
	column->addWidget(row.row);
	return row;
}

void bssh_file_panel::show_copy(const copy_row &row, const QString &verb, qint64 done, qint64 total) {
	/*
	 * In kilobytes, so a file past 2 GiB fits the bar's int range. A
	 * directory copied back is listed as it goes, so its total grows and
	 * the bar can step back; the label says the bytes either way.
	 */
	row.label->setText(tr("%1: %2 of %3").arg(verb, QLocale().formattedDataSize(done),
	                                          QLocale().formattedDataSize(total)));
	row.bar->setRange(0, int(qMax<qint64>(1, total / 1024)));
	row.bar->setValue(int(qMin(done, total) / 1024));
	row.bar->show();
	row.stop->show();
	row.row->show();
}

void bssh_file_panel::end_copy(const copy_row &row, bool ok, const QString &directory,
                               const QStringList &written, const QString &why) {
	if (ok) row.label->setText(tr("Copied %1 to %2").arg(written.join(QStringLiteral(", ")), directory));
	else if (why == QLatin1String(BSSH_COPY_STOPPED)) row.label->setText(tr("Copy stopped"));
	else row.label->setText(tr("Copy failed: %1").arg(why));
	row.bar->hide();
	row.stop->hide();
	row.row->show();
}

QSize bssh_file_panel::sizeHint() const {
	return QSize(fontMetrics().averageCharWidth() * 40, QWidget::sizeHint().height());
}

void bssh_file_panel::set_overlay(bool overlay) {
	/*
	 * Over the terminal rather than beside it (a phone, sec 6.4): beside
	 * one a few columns wide it would leave the terminal nothing. Its
	 * parent is the view, whose size it follows; the view keeps its own,
	 * so the remote's window is not resized by the panel opening.
	 */
	m_overlay = overlay;
	m_grip->setVisible(!overlay);
	setAutoFillBackground(overlay);
	if (!overlay || !parentWidget()) return;
	parentWidget()->installEventFilter(this);
	setGeometry(parentWidget()->rect());
	raise();
}

void bssh_file_panel::set_panel_width(int width) {
	/* No narrower than its buttons fit, and never more than most of the
	 * pane: the terminal beside it keeps a quarter at least. */
	const int least = fontMetrics().averageCharWidth() * 20;
	/* The pane's width means nothing before it is laid out: a restored
	 * width is taken as it was, the terminal's share kept once dragged. */
	const bool laid_out = parentWidget() && parentWidget()->isVisible();
	const int most = laid_out ? parentWidget()->width() * 3 / 4 : width;
	setFixedWidth(qMax(least, qMin(width, qMax(least, most))));
}

bool bssh_file_panel::eventFilter(QObject *watched, QEvent *event) {
	if (m_overlay && watched == parentWidget() && event->type() == QEvent::Resize)
		setGeometry(parentWidget()->rect());
	if (watched == m_list->viewport()) {
		if (event->type() == QEvent::MouseButtonPress) {
			const QMouseEvent *press = static_cast<QMouseEvent *>(event);
			m_drag_press = press->button() == Qt::LeftButton ? press->position().toPoint() : QPoint();
		} else if (event->type() == QEvent::MouseMove && !m_drag_press.isNull()) {
			const QMouseEvent *move = static_cast<QMouseEvent *>(event);
			if ((move->buttons() & Qt::LeftButton)
			    && (move->position().toPoint() - m_drag_press).manhattanLength() >= QApplication::startDragDistance()) {
				m_drag_press = QPoint();
				start_drag();
				return true;
			}
		} else if (event->type() == QEvent::MouseButtonRelease) {
			m_drag_press = QPoint();
		}
		return QWidget::eventFilter(watched, event);
	}
	if (watched != m_grip) return QWidget::eventFilter(watched, event);
	switch (event->type()) {
	case QEvent::MouseButtonPress: {
		const QMouseEvent *press = static_cast<QMouseEvent *>(event);
		if (press->button() != Qt::LeftButton) break;
		m_drag_from = press->globalPosition().toPoint().x();
		m_drag_width = width();
		return true;
	}
	case QEvent::MouseMove:
		if (m_drag_from < 0) break;
		/* The grip is on the left: dragging left widens the panel. */
		set_panel_width(m_drag_width + m_drag_from
		                - static_cast<QMouseEvent *>(event)->globalPosition().toPoint().x());
		return true;
	case QEvent::MouseButtonRelease:
		if (m_drag_from < 0) break;
		m_drag_from = -1;
		emit width_chosen(width());
		return true;
	default:
		break;
	}
	return QWidget::eventFilter(watched, event);
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
	/* A change that failed says so over the listing that followed it. */
	if (!m_failure.isEmpty()) say(m_failure);
	else say(entries.isEmpty() ? tr("Empty") : tr("%n item(s)", nullptr, int(entries.size())));
	m_failure.clear();
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
	if (m_download_directory.isEmpty()) copy_selected_elsewhere();
	else copy_selected_to(m_download_directory);
}

void bssh_file_panel::copy_selected_elsewhere() {
	copy_selected_to(QFileDialog::getExistingDirectory(this, tr("Copy to"), m_download_directory));
}

void bssh_file_panel::copy_selected_to(const QString &directory) {
	if (!m_session || directory.isEmpty()) return;
	if (m_session->download_in_progress()) {
		say(tr("A copy is still running; stop it or let it finish"));
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
	if (m_path.isEmpty()) return;
	const QList<QTreeWidgetItem *> selected = m_list->selectedItems();
	QMenu menu(this);
	QAction *downloads = nullptr;
	QAction *elsewhere = nullptr;
	QAction *rename = nullptr;
	QAction *remove = nullptr;
	if (!selected.isEmpty()) {
		if (!m_download_directory.isEmpty()) downloads = menu.addAction(tr("Copy to Downloads"));
		elsewhere = menu.addAction(tr("Copy to..."));
		menu.addSeparator();
		rename = menu.addAction(tr("Rename..."));
		rename->setEnabled(selected.size() == 1);
		remove = menu.addAction(tr("Remove..."));
		menu.addSeparator();
	}
	QAction *send = menu.addAction(tr("Send Files Here..."));
	QAction *folder = menu.addAction(tr("New Folder..."));
	const QAction *chosen = menu.exec(at);
	if (!chosen) return;
	if (chosen == send) {
		/* The platform's picker; on Android, its document picker. */
		copy_here(QFileDialog::getOpenFileNames(this, tr("Send to %1").arg(m_path)));
		return;
	}
	if (downloads && chosen == downloads) copy_selected_to(m_download_directory);
	else if (chosen == elsewhere) copy_selected_elsewhere();
	else if (chosen == rename) {
		const bssh_remote_entry_t &entry = m_entries.at(selected.first()->data(NAME, Qt::UserRole).toInt());
		bool ok = false;
		const QString name = QInputDialog::getText(this, tr("Rename"), tr("New name:"), QLineEdit::Normal,
		                                           entry.name, &ok);
		if (ok) rename_entry(entry.name, name);
	}
	else if (chosen == remove) {
		QStringList names;
		for (QTreeWidgetItem *item : selected) names << m_entries.at(item->data(NAME, Qt::UserRole).toInt()).name;
		const QString question = names.size() == 1
		        ? tr("Remove %1 from the host? A folder goes with everything in it.").arg(names.first())
		        : tr("Remove these %n items from the host? A folder goes with everything in it.", nullptr,
		             int(names.size()));
		if (QMessageBox::question(this, tr("Remove"), question) == QMessageBox::Yes) remove_entries(names);
	}
	else if (chosen == folder) {
		bool ok = false;
		const QString name = QInputDialog::getText(this, tr("New Folder"), tr("Name:"), QLineEdit::Normal,
		                                           QString(), &ok);
		if (ok) make_folder(name);
	}
}

/* A name for an entry here: one path component, nothing that leaves it. */
static bool plain_name(const QString &name) {
	return !name.isEmpty() && !name.contains(QLatin1Char('/')) && name != QStringLiteral(".")
	       && name != QStringLiteral("..");
}

QString bssh_file_panel::path_here(const QString &name) const {
	return m_path.endsWith(QLatin1Char('/')) ? m_path + name : m_path + QLatin1Char('/') + name;
}

bool bssh_file_panel::rename_entry(const QString &name, const QString &to) {
	if (!m_session || m_path.isEmpty() || !plain_name(name) || !plain_name(to) || name == to) return false;
	m_changes_asked = 1;
	say(tr("Renaming %1").arg(name));
	m_session->change_remote(bssh_ssh_session::RENAME, path_here(name), path_here(to));
	return true;
}

bool bssh_file_panel::make_folder(const QString &name) {
	if (!m_session || m_path.isEmpty() || !plain_name(name)) return false;
	m_changes_asked = 1;
	say(tr("Making %1").arg(name));
	m_session->change_remote(bssh_ssh_session::MAKE_DIRECTORY, path_here(name));
	return true;
}

bool bssh_file_panel::remove_entries(const QStringList &names) {
	if (!m_session || m_path.isEmpty() || names.isEmpty()) return false;
	for (const QString &name : names)
		if (!plain_name(name)) return false;
	/*
	 * One after another: the transport removes one tree at a time, and
	 * each answer asks for the next, so a failure stops the rest.
	 */
	m_removals = names;
	m_changes_asked = 1;
	say(tr("Removing %1").arg(names.first()));
	m_session->change_remote(bssh_ssh_session::REMOVE, path_here(m_removals.takeFirst()));
	return true;
}

void bssh_file_panel::changed(bool ok, const QString &path, const QString &why) {
	if (m_changes_asked <= 0) return;
	if (ok && !m_removals.isEmpty()) {
		say(tr("Removing %1").arg(m_removals.first()));
		m_session->change_remote(bssh_ssh_session::REMOVE, path_here(m_removals.takeFirst()));
		return;
	}
	m_changes_asked = 0;
	m_removals.clear();
	open_directory(m_path);
	if (!ok) m_failure = tr("Could not change %1: %2").arg(path, why);
}

void bssh_file_panel::say(const QString &text) {
	m_status->setText(text);
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
		say(tr("A copy is still running; stop it or let it finish"));
		return false;
	}
	m_session->upload(paths, m_path);
	return true;
}

bssh_remote_drag *bssh_file_panel::drag_data_for_selection() {
	if (!m_session || m_path.isEmpty()) return nullptr;
	QStringList paths;
	QList<QUrl> remote;
	qint64 bytes = 0;
	bool folder = false;
	const bssh_profile_t &profile = m_session->profile();
	for (QTreeWidgetItem *item : m_list->selectedItems()) {
		const int at = item->data(NAME, Qt::UserRole).toInt();
		if (at < 0 || at >= m_entries.size()) continue;
		const bssh_remote_entry_t &entry = m_entries.at(at);
		if (opens(entry)) folder = true;
		bytes += entry.size;
		const QString path = remote_path_of(entry);
		paths << path;
		/* The last resort's link: the host as the profile names it. A jump
		 * host is not in a URL; the file manager's ~/.ssh/config decides. */
		QUrl url;
		url.setScheme(QStringLiteral("sftp"));
		url.setUserName(profile.user);
		url.setHost(profile.host);
		if (profile.port != 22) url.setPort(profile.port);
		url.setPath(path);
		remote << url;
	}
	if (paths.isEmpty()) return nullptr;

	bssh_remote_drag *data = new bssh_remote_drag(remote);
	/* One entry, on X11: XDS too, which a target that speaks it prefers --
	 * it names the folder, and the copy is ours, with its progress here. */
	if (paths.size() == 1 && QGuiApplication::platformName() == QLatin1String("xcb")) {
		set_direct_save_name(paths.first().section(QLatin1Char('/'), -1));
		data->enable_direct_save([this, paths] { return direct_save(paths); });
	}
#if QT_CONFIG(dbus) && !defined(Q_OS_ANDROID)
	/* Ark's way too, any number of entries, wherever there is a session
	 * bus: Dolphin calls it with the folder, on Wayland as on X11. */
	drop_ark();
	QDBusConnection bus = QDBusConnection::sessionBus();
	if (bus.isConnected()) {
		const QString object = QStringLiteral("/beerssh/drag/%1").arg(m_drags + 1);
		bssh_ark_extract *extract = new bssh_ark_extract([this, paths](const QString &folder) {
			save_into(paths, folder);
		}, this);
		if (bus.registerObject(object, extract, QDBusConnection::ExportAllSlots)) {
			/* The panel's, not the drag's: Dolphin calls after the drop,
			 * when the drag and its data are gone. */
			m_ark_path = object;
			m_ark_object = extract;
			data->enable_ark(bus.baseService(), object);
		} else {
			delete extract;
		}
	}
#endif
	/*
	 * Staged from now, into a directory of this drag's own, on the SSH
	 * worker a piece at a time -- the window does not wait for it, and the
	 * drop is answered with whatever there is by then. Not while another
	 * copy back runs (one at a time), not a folder (its size is unknown
	 * until walked), and not past the limit: those get the link alone.
	 */
	m_last_staged_dir.clear();
	if (!m_session->download_in_progress() && !folder && bytes <= m_staging_limit) {
		const QString dir = bssh_drag_staging_root() + QLatin1Char('/') + QString::number(++m_drags);
		if (QDir().mkpath(dir)) {
			m_staging_dir = dir;
			m_last_staged_dir = dir;
			m_staging_data = data;
			m_session->download(paths, dir);
		}
	}
	return data;
}

void bssh_file_panel::start_drag() {
	bssh_remote_drag *data = drag_data_for_selection();
	if (!data) return;
	/* Copy only: a move would ask the host's files to be removed. The
	 * event loop runs during exec, so the staging goes on under it. */
	QDrag *drag = new QDrag(this);
	drag->setMimeData(data);
	drag_ended(drag->exec(Qt::CopyAction));
	set_direct_save_name(QString());
	/* Kept a moment past the drop: Dolphin calls after it has returned. */
	QTimer::singleShot(30000, this, [this, path = m_ark_path] {
		if (m_ark_path == path) drop_ark();
	});
}

void bssh_file_panel::drag_ended(Qt::DropAction action) {
	if (action != Qt::IgnoreAction || m_last_staged_dir.isEmpty()) return;
	/* Let go over nothing: the staging stops, and what it wrote goes. */
	const QString dir = m_last_staged_dir;
	m_last_staged_dir.clear();
	if (!dir.startsWith(bssh_drag_staging_root() + QLatin1Char('/'))) return;
	if (m_staging_dir == dir && m_session && m_session->download_in_progress()) {
		/* Removed once the copy says it has stopped: the worker may be
		 * writing a piece into it this moment. */
		m_discard_dir = dir;
		m_session->cancel_download();
		return;
	}
	QDir(dir).removeRecursively();
}

#if !defined(Q_OS_ANDROID) && QT_CONFIG(xcb)
namespace {

xcb_atom_t intern(xcb_connection_t *connection, const char *name) {
	xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(connection,
	        xcb_intern_atom(connection, 0, uint16_t(qstrlen(name)), name), nullptr);
	const xcb_atom_t atom = reply ? reply->atom : xcb_atom_t(XCB_ATOM_NONE);
	free(reply);
	return atom;
}

xcb_connection_t *x11_connection() {
	auto *x11 = qApp->nativeInterface<QNativeInterface::QX11Application>();
	return x11 ? x11->connection() : nullptr;
}

}
#endif

/*
 * XDS's proposal: the file's name on the dragging window's XdndDirectSave0
 * property, which the target replaces with where it wants it. Empty takes
 * the property away, as the drag ends.
 */
void bssh_file_panel::set_direct_save_name(const QString &name) {
#if !defined(Q_OS_ANDROID) && QT_CONFIG(xcb)
	xcb_connection_t *connection = x11_connection();
	if (!connection || !window()->windowHandle()) return;
	const xcb_window_t source = xcb_window_t(window()->winId());
	const xcb_atom_t property = intern(connection, "XdndDirectSave0");
	if (name.isEmpty()) {
		xcb_delete_property(connection, source, property);
	} else {
		const QByteArray bytes = name.toUtf8();
		xcb_change_property(connection, XCB_PROP_MODE_REPLACE, source, property,
		                    intern(connection, "text/plain"), 8, uint32_t(bytes.size()), bytes.constData());
	}
	xcb_flush(connection);
#else
	Q_UNUSED(name);
#endif
}

/*
 * The target has written where it wants the file and asks: begin the copy
 * there and say "S" -- not after it, since the target waits on the answer
 * and a large file would hold it up; the file appears when it is whole,
 * its progress in the copy-back row. "E" where it cannot begin.
 */
QByteArray bssh_file_panel::direct_save(const QStringList &paths) {
#if !defined(Q_OS_ANDROID) && QT_CONFIG(xcb)
	xcb_connection_t *connection = x11_connection();
	if (!connection || !m_session) return QByteArrayLiteral("E");
	const xcb_window_t source = xcb_window_t(window()->winId());
	xcb_get_property_reply_t *reply = xcb_get_property_reply(connection,
	        xcb_get_property(connection, 0, source, intern(connection, "XdndDirectSave0"),
	                         XCB_GET_PROPERTY_TYPE_ANY, 0, 4096), nullptr);
	QByteArray written;
	if (reply) {
		written = QByteArray(static_cast<const char *>(xcb_get_property_value(reply)),
		                     xcb_get_property_value_length(reply));
		free(reply);
	}
	/* A file:// URL on this machine; a host part naming another is not ours
	 * to write to, and a bare name means the target wrote nothing back. */
	const QUrl url = QUrl::fromEncoded(written);
	if (!url.isLocalFile()) return QByteArrayLiteral("E");
	const QString host = url.host();
	if (!host.isEmpty() && host != QStringLiteral("localhost") && host != QSysInfo::machineHostName())
		return QByteArrayLiteral("E");
	return save_into(paths, QFileInfo(url.toLocalFile()).absolutePath()) ? QByteArrayLiteral("S")
	                                                                     : QByteArrayLiteral("E");
#else
	Q_UNUSED(paths);
	return QByteArrayLiteral("E");
#endif
}

/*
 * A copy of the dragged entries into the folder a target named -- by XDS
 * or by Ark's call -- with its progress in the copy-back row. A staging
 * under way is stopped first and this begins when the worker says so, one
 * copy back at a time. False where it cannot begin.
 */
bool bssh_file_panel::save_into(const QStringList &paths, const QString &folder) {
	if (!m_session || !QFileInfo(folder).isDir()) return false;
	if (!m_staging_dir.isEmpty() && m_session->download_in_progress()) {
		m_discard_dir = m_staging_dir;
		m_after_discard_paths = paths;
		m_after_discard_dir = folder;
		m_session->cancel_download();
		return true;
	}
	if (m_session->download_in_progress()) return false;
	m_session->download(paths, folder);
	return true;
}

/* The drag's Ark object, taken off the bus as the drag ends. */
void bssh_file_panel::drop_ark() {
#if QT_CONFIG(dbus) && !defined(Q_OS_ANDROID)
	if (m_ark_path.isEmpty()) return;
	QDBusConnection::sessionBus().unregisterObject(m_ark_path);
	m_ark_path.clear();
	delete m_ark_object;
#endif
}
