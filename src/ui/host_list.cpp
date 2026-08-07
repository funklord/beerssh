#include "ui/touch_metrics.h"
#include "platform/platform.h"
#include "ui/host_list.h"

#include "ui/ssh_config_import.h"

#include <QLabel>
#include <QListWidget>
#include <QResizeEvent>
#include <QScroller>
#include <QEvent>
#include <QMenu>
#include <QMessageBox>
#include <QToolButton>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include "term/font_catalog.h"

#include "model/profile_store.h"

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
	/*
	 * Both controls on this screen are a single glyph, so a screen reader
	 * had the two of them to work with and announced "vertical ellipsis"
	 * and "plus" -- which is what they LOOK like and not what they do.
	 * On the phone front door that was every control on the first screen.
	 */
	m_menu->setAccessibleName(QStringLiteral("More options"));
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
	connect(menu->addAction(QStringLiteral("This device")), &QAction::triggered,
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

	/*
	 * A target elides in the MIDDLE, not at the end. A row's second line
	 * is user@host:port, and the two ends are what identify it -- who you
	 * are, and which port -- while the middle of a long internal domain
	 * is the part nobody needs. Elided at the end,
	 * nabbe.testing@build.internal.example.com:2299 loses the port, which
	 * is exactly the detail that tells two rows apart.
	 */
	m_list->setTextElideMode(Qt::ElideMiddle);

	layout->addWidget(m_list, 1);

	/*
	 * The add button sits over the list, bottom right, where a floating
	 * action button goes on this platform. Round, because a circle is
	 * what says "primary action" here, and a QToolButton with a stylesheet
	 * gets there without leaving Widgets.
	 */
	m_add = new QToolButton(this);
	m_add->setAccessibleName(QStringLiteral("Add host"));
	m_add->setText(QStringLiteral("+"));
	m_add->setFixedSize(56, 56);
	style_add_button();
	style_rows();
	m_add->raise();

	connect(m_add, &QToolButton::clicked, this, &bssh_host_list::add_requested);

	/*
	 * One tap connects. No "open" button and no double-tap: a list of
	 * things you connect to should connect when you touch one, and the
	 * desktop's double-click idiom has no meaning under a finger.
	 */
	connect(m_list, &QListWidget::itemClicked, this, &bssh_host_list::on_row_activated);

	/*
	 * Press and hold for Edit and Delete. A tap still connects, which is
	 * the everyday action and must stay the cheapest one -- so the rarer
	 * pair goes behind the gesture Android already teaches for "act on
	 * this item" rather than onto a row of buttons that would compete
	 * with it on every row.
	 */
	m_list->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(m_list, &QListWidget::customContextMenuRequested,
	         this, &bssh_host_list::on_row_menu);

	/*
	 * And by press-and-hold, which is what sec 8.3 has always said and
	 * what did not work: Qt raises customContextMenuRequested from a
	 * right-click, and does NOT raise it from a long press on Android --
	 * checked on a device, where holding a row opened the host as though
	 * it had been tapped. So the touch half of "long-press or right-click"
	 * had no implementation at all.
	 *
	 * Filtered on the VIEWPORT, which is where a scrolling view delivers
	 * its mouse events, and enabled only where touch is the primary
	 * input: with a pointer, press-and-hold on a list row means nothing
	 * and should keep meaning nothing.
	 */
	m_long_press = new bssh_long_press(m_list->viewport(), this);
	m_long_press->set_enabled(bssh_platform::instance()
	                           && bssh_platform::instance()->is_touch_primary());

	connect(m_long_press, &bssh_long_press::triggered, this, [this](const QPoint &at) {
		on_row_menu(m_list->viewport()->mapFromGlobal(at));
	});

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

/*
 * The row for the shell on this machine (sec 2). Negative because a stored
 * profile's id is positive and the empty-list placeholder uses 0, so all
 * three kinds of row stay distinguishable by one field.
 */
static const int BSSH_LOCAL_SHELL_ROW = -1;

void bssh_host_list::reload() {
	m_list->clear();
	if (!m_store) return;

	/*
	 * The local shell first, and always present (sec 2).
	 *
	 * First because it is the one entry that is always available and
	 * needs no setting up -- the same reason a browser puts a new tab at
	 * the near end rather than sorted among the bookmarks. It is
	 * synthetic rather than stored: there is nothing to save, and a row
	 * that could be edited or deleted would let somebody remove the only
	 * session that always works.
	 *
	 * Offered on touch as well, even though a phone does not OPEN with
	 * one (sec 8.5): an Android sandbox has little for a shell to do, but
	 * "little" is not "nothing", and being unable to reach it at all is a
	 * different thing from it not being the default.
	 */
	{
		QListWidgetItem *item = new QListWidgetItem(m_list);
		item->setText(QStringLiteral("Local shell\nthis device"));
		item->setData(Qt::UserRole, BSSH_LOCAL_SHELL_ROW);
		item->setSizeHint(QSize(0, BSSH_TOUCH_ROW_DP));
	}

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
		item->setSizeHint(QSize(0, BSSH_TOUCH_ROW_DP));
	}

	if (profiles.isEmpty()) {
		/*
		 * An empty list that explains itself, rather than a blank
		 * rectangle -- this is the first screen a new user sees.
		 */
		QListWidgetItem *item = new QListWidgetItem(
		        QStringLiteral("No hosts yet\nTap + to add one"), m_list);
		item->setFlags(Qt::NoItemFlags);
		item->setSizeHint(QSize(0, BSSH_TOUCH_ROW_DP));
	}
}

/*
 * The accent comes from the palette rather than a literal, so the button
 * matches whatever theme is applied (sec 6.2) -- it was the third copy of
 * "#2b90d9" in the tree, and the one that would have been missed.
 *
 * A stylesheet cannot read the palette for itself, and does not
 * re-evaluate when the palette changes, so it is rebuilt on the change
 * event below.
 */
void bssh_host_list::style_add_button() {
	if (!m_add) return;

	const QColor accent = palette().color(QPalette::Highlight);
	const QColor pressed = accent.darker(130);

	m_add->setStyleSheet(QStringLiteral(
	        "QToolButton { border-radius: 28px; background: %1; color: %2;"
	        " font-size: 28px; }"
	        "QToolButton:pressed { background: %3; }")
	                              .arg(accent.name(),
	                                    palette().color(QPalette::HighlightedText).name(),
	                                    pressed.name()));
}

/*
 * Row separators, for the same reason and by the same route as the button
 * above: a stylesheet cannot read the palette and does not re-evaluate
 * when it changes, so this is rebuilt on the change event too.
 *
 * The rows were 64dp touch targets carrying two lines of text and NOTHING
 * saying they were rows -- no rule between them, no selection, no pressed
 * state. The first screen an Android user meets read as two paragraphs of
 * text rather than as a list you tap, against sec 8.5 asking for
 * full-bleed rows and one tap to connect. Found by rendering it and
 * looking: every size check was satisfied, because 64dp of nothing is
 * still 64dp.
 *
 * Derived from the palette rather than picked. A fixed grey that reads as
 * a hairline against the light theme is invisible against the dark one --
 * the trap sec 6.2 records the tab status dots falling into -- and Mid
 * sits between the window and its text in both.
 *
 * The list keeps its own Base background rather than being made
 * transparent. Transparent looked tidier in isolation and cost the
 * distinction between the heading band and the content under it: the
 * screen became one flat panel with a title floating on it.
 */
void bssh_host_list::style_rows() {
	if (!m_list) return;

	m_list->setStyleSheet(QStringLiteral(
	        "QListWidget::item { border-bottom: 1px solid %1; padding-left: 8px; }"
	        "QListWidget::item:selected { background: %2; color: %3; }")
	                               .arg(palette().color(QPalette::Mid).name(),
	                                     palette().color(QPalette::Highlight).name(),
	                                     palette().color(QPalette::HighlightedText).name()));
}

void bssh_host_list::changeEvent(QEvent *event) {
	QWidget::changeEvent(event);
	if (event && event->type() == QEvent::PaletteChange) {
		style_add_button();
		style_rows();
	}
}

void bssh_host_list::on_row_menu(const QPoint &where) {
	const QListWidgetItem *item = m_list->itemAt(where);
	if (!item || !m_store) return;

	const int id = item->data(Qt::UserRole).toInt();
	if (id == 0) return;

	/*
	 * No menu on the local shell. Edit would have nothing to edit -- it
	 * is not stored -- and Delete would offer to remove the one session
	 * that always works, from a store it was never in. An absent menu is
	 * the honest answer; a menu of two disabled entries would only invite
	 * the question again.
	 */
	if (id == BSSH_LOCAL_SHELL_ROW) return;

	const bssh_profile_t profile = m_store->profile(id);

	QMenu menu(this);
	menu.setMinimumWidth(240);
	QAction *edit = menu.addAction(QStringLiteral("Edit"));
	QAction *remove = menu.addAction(QStringLiteral("Delete"));

	const QAction *chosen = menu.exec(m_list->viewport()->mapToGlobal(where));
	if (chosen == edit) {
		emit edit_requested(profile);
		return;
	}
	if (chosen != remove) return;

	/*
	 * Confirmed, because the two menu entries are adjacent and one of
	 * them is irreversible. Naming the host rather than asking "are you
	 * sure": the question a misfire needs answered is WHICH one.
	 */
	const QMessageBox::StandardButton answer = QMessageBox::question(
	        this, QStringLiteral("Delete host"),
	        QStringLiteral("Remove \"%1\" from this device?\n\nIts saved settings go with "
	                        "it. Nothing on the server changes.").arg(profile.name),
	        QMessageBox::Cancel | QMessageBox::Yes, QMessageBox::Cancel);

	if (answer == QMessageBox::Yes) emit delete_requested(profile);
}

void bssh_host_list::on_row_activated() {
	const QListWidgetItem *item = m_list->currentItem();
	if (!item || !m_store) return;

	const int id = item->data(Qt::UserRole).toInt();
	if (id == 0) return;

	if (id == BSSH_LOCAL_SHELL_ROW) {
		emit open_requested(bssh_profile_t::local_shell());
		return;
	}

	emit open_requested(m_store->profile(id));
}
