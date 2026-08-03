#include "ui/host_list.h"

#include "ui/ssh_config_import.h"

#include <QLabel>
#include <QListWidget>
#include <QResizeEvent>
#include <QScroller>
#include <QMenu>
#include <QMessageBox>
#include <QToolButton>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include "term/font_catalog.h"

#include "model/profile_store.h"

/*
 * Row height, and the reason it is not a paragraph of guidance: a list row
 * on a phone is a touch target, and 64dp gives a comfortable one with room
 * for a name and its user@host underneath. The key row uses 48dp because
 * its keys are small and many; a host row is large and few, and being
 * generous costs nothing (sec 6.2, sec 8.3).
 */
static const int BSSH_ROW_HEIGHT_DP = 64;

bssh_host_list::bssh_host_list(bssh_profile_store *store, QWidget *parent)
    : QWidget(parent),
      m_store(store) {
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	/*
	 * The heading row carries the one action that is not about a specific
	 * host. A phone front door has no menu bar to hang it from (sec 8.5),
	 * and presets are the thing hosts are configured THROUGH -- so it
	 * belongs on the screen that lists them, not buried behind a host.
	 */
	QWidget *heading = new QWidget(this);
	QHBoxLayout *heading_row = new QHBoxLayout(heading);
	heading_row->setContentsMargins(0, 0, 8, 0);

	QLabel *title = new QLabel(QStringLiteral("  Hosts"), heading);
	QFont title_font = bssh_scaled_font(title->font(), 1.4);
	title_font.setBold(true);
	title->setFont(title_font);
	title->setMinimumHeight(56);
	heading_row->addWidget(title, 1);

	/*
	 * One overflow menu rather than a button per destination. There are
	 * three app-level pages now, and three buttons across the top of a
	 * phone leaves the heading fighting the host list for width -- while
	 * the menu is the gesture this platform already teaches. The
	 * destinations are the same ones the desktop dialog lists as
	 * categories; only the shape differs (sec 8.5).
	 */
	m_menu = new QToolButton(heading);
	/*
	 * Built from the codepoint rather than written into the source.
	 * Pasting the character in went through a Latin-1 round trip and
	 * left three mojibake bytes in the file, which is what the button
	 * then displayed.
	 */
	m_menu->setText(QString(QChar(0x22EE)));
	m_menu->setMinimumSize(48, 48);
	m_menu->setPopupMode(QToolButton::InstantPopup);

	QMenu *menu = new QMenu(m_menu);
	/*
	 * In the overflow menu rather than beside the add button: importing
	 * is a once-in-a-while gesture and the floating button belongs to the
	 * everyday one (sec 8.5).
	 */
	connect(menu->addAction(QStringLiteral("Import from SSH config")), &QAction::triggered,
	         this, [this] {
		const int added = bssh_ssh_config_import::run(m_store, this);
		if (added < 0) return;

		reload();
		QMessageBox::information(
		        this, QStringLiteral("Hosts imported"),
		        added == 1 ? QStringLiteral("1 host was added.")
		                   : QStringLiteral("%1 hosts were added.").arg(added));
	});
	connect(menu->addAction(QStringLiteral("Presets")), &QAction::triggered,
	         this, &bssh_host_list::presets_requested);
	connect(menu->addAction(QStringLiteral("Settings")), &QAction::triggered,
	         this, &bssh_host_list::settings_requested);
	connect(menu->addAction(QStringLiteral("Licences")), &QAction::triggered,
	         this, &bssh_host_list::licences_requested);

	/*
	 * A width, because the popup sized itself narrower than its own
	 * entries and elided them: the menu read "Lic...ces", which is a menu
	 * that has to be guessed at.
	 */
	menu->setMinimumWidth(240);

	m_menu->setMenu(menu);
	heading_row->addWidget(m_menu, 0);

	layout->addWidget(heading);

	m_list = new QListWidget(this);
	m_list->setFrameShape(QFrame::NoFrame);

	/*
	 * Kinetic scrolling, because dragging a desktop scrollbar with a
	 * thumb is the single most obvious way a Widgets app announces it was
	 * not written for touch. QScroller is the Widgets answer to it -- no
	 * QML required, which is the point sec 2 keeps making.
	 */
	QScroller::grabGesture(m_list->viewport(), QScroller::LeftMouseButtonGesture);

	layout->addWidget(m_list, 1);

	/*
	 * The add button sits over the list, bottom right, where a floating
	 * action button goes on this platform. Round, because a circle is
	 * what says "primary action" here, and a QToolButton with a stylesheet
	 * gets there without leaving Widgets.
	 */
	m_add = new QToolButton(this);
	m_add->setText(QStringLiteral("+"));
	m_add->setFixedSize(56, 56);
	m_add->setStyleSheet(QStringLiteral(
	        "QToolButton { border-radius: 28px; background: #2b90d9; color: white;"
	        " font-size: 28px; }"
	        "QToolButton:pressed { background: #1c6ea4; }"));
	m_add->raise();

	connect(m_add, &QToolButton::clicked, this, &bssh_host_list::add_requested);

	/*
	 * One tap connects. No "open" button and no double-tap: a list of
	 * things you connect to should connect when you touch one, and the
	 * desktop's double-click idiom has no meaning under a finger.
	 */
	connect(m_list, &QListWidget::itemClicked, this, &bssh_host_list::on_row_activated);

	reload();
}

void bssh_host_list::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);

	/*
	 * A floating button floats, which means it is positioned rather than
	 * laid out: putting it in the layout would reserve a strip of the
	 * list for it, and leaving it unpositioned parks it at the top-left
	 * on top of the title -- which is exactly what the first render of
	 * this screen showed.
	 *
	 * Bottom right, one thumb-width in from each edge.
	 */
	const int margin = 16;
	m_add->move(width() - m_add->width() - margin,
	             height() - m_add->height() - margin);
	m_add->raise();
}

void bssh_host_list::reload() {
	m_list->clear();
	if (!m_store) return;

	const QList<bssh_profile_t> profiles = m_store->profiles();
	for (const bssh_profile_t &profile : profiles) {
		QListWidgetItem *item = new QListWidgetItem(m_list);

		/*
		 * Name over target, unless they are the same string -- an
		 * unnamed host is named after itself, and printing that twice
		 * makes the row look like a bug to the person reading it.
		 */
		const QString target = profile.target();
		item->setText(profile.name == target ? target
		                                     : profile.name + QLatin1Char('\n') + target);
		item->setData(Qt::UserRole, profile.id);
		item->setSizeHint(QSize(0, BSSH_ROW_HEIGHT_DP));
	}

	if (profiles.isEmpty()) {
		/*
		 * An empty list that explains itself, rather than a blank
		 * rectangle -- this is the first screen a new user sees.
		 */
		QListWidgetItem *item = new QListWidgetItem(
		        QStringLiteral("No hosts yet\nTap + to add one"), m_list);
		item->setFlags(Qt::NoItemFlags);
		item->setSizeHint(QSize(0, BSSH_ROW_HEIGHT_DP));
	}
}

void bssh_host_list::on_row_activated() {
	const QListWidgetItem *item = m_list->currentItem();
	if (!item || !m_store) return;

	const int id = item->data(Qt::UserRole).toInt();
	if (id == 0) return;

	emit open_requested(m_store->profile(id));
}
