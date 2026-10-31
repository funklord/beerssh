#include <QFileInfo>
#include <QApplication>
#include <climits>
#include "ui/interface_scale.h"
#include "ui/touch_metrics.h"
#include "softkeys/metrics.h"
#include "term/font_catalog.h"

#include <QAbstractItemView>
#include <QListWidget>
#include <QScroller>
#include <QListWidgetItem>
#include "ui/main_window.h"

#include <QCloseEvent>
#include <QResizeEvent>
#include <QFile>
#include <QDesktopServices>
#include <QUrl>
#include <QClipboard>
#include <QEvent>
#include <QCheckBox>
#include <QMessageBox>
#include <QPushButton>
#include <QGuiApplication>
#include <QInputMethod>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaEnum>
#include <QMouseEvent>
#include <QPointer>
#include <QFontMetrics>
#include <QDrag>
#include <QCursor>
#include <QEventLoop>
#include <functional>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QShowEvent>
#include <QProcess>
#include <QTimer>
#include <QWindow>
#include <QScreen>
#include <QLayout>
#include <QMenuBar>
#include <QToolButton>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <QKeyEvent>
#include <QStackedWidget>

#include <QIcon>
#include <QPainter>
#include <QPixmap>
#include <QTabBar>

#if !defined(Q_OS_ANDROID) && QT_CONFIG(xcb)
#include <xcb/xcb.h>
#define BSSH_ASK_X11 1
#endif

/* Defined beside drop_spot_at(), which it serves. */
static bool another_beerssh_under_pointer(const QWidget *ours);
static qint64 other_beerssh_pid_under_pointer(const QWidget *ours);

#include "model/profile_store.h"
#include "model/session.h"
#include "model/conn_manager.h"
#include "model/session_registry.h"
#include "model/handoff_point.h"
#include "model/app_settings.h"
#include "model/command_line.h"
#include "model/window_title.h"
#include "model/settings_resolver.h"
#include "ui/layout_manager.h"
#include "ui/find_bar.h"
#include "ui/pane_bar.h"
#include "ui/pane_container.h"
#include "input/input_router.h"
#include "term/emulator.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>

#include "platform/platform.h"
#include "platform/secret_store.h"
#include "ui/host_editor.h"
#include "ui/host_list.h"
#include "softkeys/key_row.h"
#include "softkeys/keyboard.h"
#include "softkeys/keyboard_switch.h"
#include "ui/app_settings_view.h"
#include "ui/licences_view.h"
#include "model/preset.h"
#include "ui/preset_editor.h"
#include "term/colour_scheme.h"
#include "softkeys/key_row_layout.h"
#include "softkeys/keyboard_layout.h"
#include "ui/session_dialog.h"
#include "ui/password_prompt.h"
#include "ui/tab_status.h"
#include "ui/terminal_view.h"

/*
 * How far CLEAR of the tab bar a drag goes before the tab becomes its own
 * window (sec 8.3, sec 8.4).
 *
 * Generous on purpose. A tab bar is thin and a pointer reordering tabs
 * crosses its edge constantly, so tearing out on "left the widget" would
 * make reordering nearly impossible to do without losing the tab.
 */
static const int BSSH_TEAR_OUT_DP = 40;

namespace {

bssh_layout_manager::arrangement_choice_t chosen_arrangement() {
	/*
	 * sec 8.7's arrangement, asked of the setting rather than decided
	 * from the idiom. AUTO is what the idiom would have picked, which is
	 * the behaviour this setting was added on top of -- a phone opens on
	 * the column and a desktop on sec 8.4's tiles.
	 *
	 * The setting exists because sec 8.7 asked for it in as many words:
	 * "tiled has to be runnable on a phone and column on a desktop, or
	 * neither can be tested without two devices". The constructor chose
	 * once from is_touch_primary() and offered no way back, so on a
	 * desktop the column could only be reached by a test fixture.
	 */
	const bssh_app_settings::arrangement_t wanted = bssh_app_settings().arrangement();

	const bssh_layout_manager::preference_t preference =
	        wanted == bssh_app_settings::ARRANGEMENT_TILED    ? bssh_layout_manager::PREFER_TILED
	        : wanted == bssh_app_settings::ARRANGEMENT_COLUMN ? bssh_layout_manager::PREFER_COLUMN
	                                                          : bssh_layout_manager::PREFER_AUTO;

	/*
	 * AUTOMATIC follows how the device is held (the holder's answers of
	 * 2026-09-25, sec 8.7), which the rule itself spells out.
	 */
	const bssh_platform *platform = bssh_platform::instance();
	return bssh_layout_manager::choose_arrangement(
	        preference, platform && platform->is_touch_primary(),
	        platform ? platform->screen_size() : QSize());
}

/*
 * The session drag on X11, run by this app rather than handed to the
 * platform's drag and drop (sec 8.4).
 *
 * A platform drag is offered to every program under the pointer, and a
 * desktop that accepts any payload takes it. TDE's does: dropping a pane
 * outside the window asked for a filename to save the drag's contents
 * under, and since the drop had been ACCEPTED the pane never tore out. No
 * payload format prevents that, because that desktop accepts all of them,
 * so the drag must not be offered at all. This is a small picture that
 * follows the pointer while the app holds the mouse; nothing outside the
 * app sees a drag, and where it ends is decided here.
 *
 * It stops on the button's release, on Esc, or when the button is found
 * up without a release having arrived -- a grab lost to another program
 * would otherwise leave the loop running with nothing to end it.
 */
class session_drag_ghost : public QWidget {
public:
	explicit session_drag_ghost(const QPixmap &picture)
	    : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint),
	      m_picture(picture) {
		resize(picture.deviceIndependentSize().toSize());

		m_watch.setInterval(200);
		QObject::connect(&m_watch, &QTimer::timeout, this, [this] {
			if (!QGuiApplication::mouseButtons().testFlag(Qt::LeftButton)) end(true);
		});
	}

	/* Called with each new pointer position, for the drop zone. */
	std::function<void(const QPoint &)> moved;

	/*
	 * Over a window that will not take the drop -- another BeerSSH
	 * (sec 8.4): the forbidden cursor, and a no-drop sign on the ghost.
	 */
	void set_refused(bool refused) {
		if (refused == m_refused) return;
		m_refused = refused;
		if (m_running) grabMouse(refused ? Qt::ForbiddenCursor : Qt::ClosedHandCursor);
		update();
	}

	/*
	 * Runs until the drag ends and says where: the release point, or
	 * nothing when it was cancelled.
	 */
	bool run(QPoint *released) {
		follow(QCursor::pos());
		show();
		grabMouse(m_refused ? Qt::ForbiddenCursor : Qt::ClosedHandCursor);
		grabKeyboard();
		m_watch.start();
		m_running = true;

		m_loop.exec();
		m_running = false;

		m_watch.stop();
		releaseKeyboard();
		releaseMouse();
		hide();

		*released = m_at;
		return m_dropped;
	}

protected:
	void paintEvent(QPaintEvent *) override {
		QPainter painter(this);
		painter.drawPixmap(0, 0, m_picture);
		if (m_refused) {
			painter.setRenderHint(QPainter::Antialiasing, true);
			QPen pen(QColor(200, 40, 40));
			pen.setWidthF(3.0);
			painter.setPen(pen);
			const qreal side = qMin(width(), height()) - 8;
			const QRectF sign(4, 4, side, side);
			painter.drawEllipse(sign);
			const qreal inset = side * 0.1464;
			painter.drawLine(sign.topLeft() + QPointF(inset, inset),
			                 sign.bottomRight() - QPointF(inset, inset));
		}
	}

	void mouseMoveEvent(QMouseEvent *event) override {
		follow(event->globalPosition().toPoint());
	}

	void mouseReleaseEvent(QMouseEvent *event) override {
		if (event->button() != Qt::LeftButton) return;
		m_at = event->globalPosition().toPoint();
		end(true);
	}

	void keyPressEvent(QKeyEvent *event) override {
		if (event->key() == Qt::Key_Escape) end(false);
	}

private:
	void follow(const QPoint &at) {
		m_at = at;

		/*
		 * Beside the pointer, not under it, so the window under the
		 * pointer is the one a drop would land in rather than this.
		 */
		move(at + QPoint(14, 14));
		if (moved) moved(at);
	}

	void end(bool dropped) {
		if (!m_loop.isRunning()) return;
		m_dropped = dropped;
		m_loop.quit();
	}

	QPixmap m_picture;
	QEventLoop m_loop;
	QTimer m_watch;
	QPoint m_at;
	bool m_dropped = false;
	bool m_refused = false;
	bool m_running = false;
};

} /* namespace */

/*
 * The tab row as a drop target while a session is being dragged (sec 8.4,
 * the holder's rule of 2026-10-09: only the place where the tab bar is, or
 * would be, makes a tab).
 *
 * Over the bar when there is one, marking the slot a drop would take; over
 * the edge of the panes where the bar would appear when there is none, as a
 * strip that says so. An overlay rather than the hidden row shown, because
 * showing the row shrinks every terminal at the start of every drag. Shown
 * only while a drag is up.
 */
class bssh_tab_drop : public QWidget {
public:
	/* The slot a drop at `at` takes, and where its mark goes; -1 for none. */
	std::function<int(const QMimeData *, const QPoint &at, int *mark)> slot_at;
	std::function<void(const QMimeData *, int slot)> dropped;

	/* The pane whose bar or strip this lies over (sec 8.3's bar per pane). */
	bssh_session *front = nullptr;

	explicit bssh_tab_drop(QWidget *parent) : QWidget(parent) {
		setAcceptDrops(true);
		hide();
	}

	void set_shape(bool strip, bool vertical) {
		m_strip = strip;
		m_vertical = vertical;
		update();
	}

	void hover(int slot, int mark) {
		if (slot == m_slot && mark == m_mark) return;
		m_slot = slot;
		m_mark = mark;
		update();
	}
	int slot() const { return m_slot; }

protected:
	void dragEnterEvent(QDragEnterEvent *event) override {
		if (!event->mimeData()->hasFormat(bssh_session_mime_type())) return;
		if (refused(event)) return;
		follow(event->mimeData(), event->position().toPoint());
		event->acceptProposedAction();
	}

	void dragMoveEvent(QDragMoveEvent *event) override {
		if (refused(event)) return;
		follow(event->mimeData(), event->position().toPoint());
		event->acceptProposedAction();
	}

	void dragLeaveEvent(QDragLeaveEvent *) override { hover(-1, -1); }

	/*
	 * Accepted even where it does nothing, so the source does not read the
	 * release as "nobody wanted it" and tear a window out over this one.
	 */
	void dropEvent(QDropEvent *event) override {
		if (refused(event)) return;
		int mark = -1;
		const int slot = slot_at ? slot_at(event->mimeData(), event->position().toPoint(), &mark) : -1;
		hover(-1, -1);
		event->acceptProposedAction();
		if (slot >= 0 && dropped) dropped(event->mimeData(), slot);
	}

	void paintEvent(QPaintEvent *) override {
		QPainter painter(this);
		const QColor accent = palette().color(QPalette::Highlight);

		if (m_strip) {
			QColor fill(accent);
			fill.setAlphaF(m_slot >= 0 ? 0.5 : 0.22);
			painter.fillRect(rect(), fill);
			QPen edge(accent);
			edge.setStyle(Qt::DashLine);
			painter.setPen(edge);
			painter.drawRect(rect().adjusted(0, 0, -1, -1));

			painter.setPen(palette().color(QPalette::WindowText));
			const QString label = QStringLiteral("Drop here for a tab");
			if (m_vertical) {
				painter.translate(width() / 2, height() / 2);
				painter.rotate(-90);
				painter.drawText(QRect(-height() / 2, -width() / 2, height(), width()),
				                 Qt::AlignCenter, label);
			} else {
				painter.drawText(rect(), Qt::AlignCenter, label);
			}
			return;
		}

		/* The bar is there: a bar of the accent where the tab would go. */
		if (m_slot < 0 || m_mark < 0) return;
		const QRect mark = m_vertical ? QRect(0, m_mark - 2, width(), 4)
		                              : QRect(m_mark - 2, 0, 4, height());
		painter.fillRect(mark, accent);
	}

private:
	/*
	 * Another process's session: ignored here, and Qt hands an ignored
	 * drag up to the window, which answers it with a link (sec 8.4) -- one
	 * place that answers, rather than two that could disagree.
	 */
	bool refused(QDropEvent *event) {
		bool foreign = false;
		bssh_session_payload_index(event->mimeData(), &foreign);
		if (!foreign) return false;
		hover(-1, -1);
		event->ignore();
		return true;
	}

	void follow(const QMimeData *mime, const QPoint &at) {
		int mark = -1;
		const int slot = slot_at ? slot_at(mime, at, &mark) : -1;
		hover(slot, mark);
	}

	bool m_strip = false;
	bool m_vertical = false;
	int m_slot = -1;
	int m_mark = -1;
};

bssh_main_window::bssh_main_window(const bssh_profile_t &launch, QWidget *parent)
    : QWidget(parent),
      m_keyboard_hidden_by_user(false),
      m_device_settings(nullptr),
      m_settings_sections(nullptr),
      m_pages(new QStackedWidget(this)),
      m_front_door_up(false),
      m_menu_bar(nullptr),
      m_copy_action(nullptr),
      m_paste_action(nullptr),
      m_split_action(nullptr),
      m_detach_action(nullptr),
      m_terminal_page(new QWidget(this)),
      m_profile_store(bssh_profile_store::shared()),
      m_session_dialog(nullptr),
      m_host_list(nullptr),
      m_host_editor(nullptr),
      m_preset_editor(nullptr),
      m_terminal_view(nullptr),
      m_panes(nullptr),
      m_input_mode(bssh_default_input_mode()),
      m_find_bar(nullptr),
      m_wired_view(nullptr),
      m_status_label(new QLabel(this)),
      m_registry(bssh_session_registry::shared()),
      m_layout(nullptr),
      m_current(nullptr),
      m_key_row(nullptr),
      m_keyboard(nullptr),
      m_status_line(bssh_app_settings::STATUS_LINE_SESSION),
      m_window_title_format(QStringLiteral("%t - BeerSSH")),
      m_asked_about_closing(false),
      m_background_on_close(false) {
	setWindowTitle(QStringLiteral("BeerSSH"));

	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

	/*
	 * The menu bar, on desktop only (sec 8.3).
	 *
	 * This class is deliberately a plain QWidget rather than a
	 * QMainWindow, and that decision stands: what was rejected was
	 * QMainWindow's menubar/toolbar/statusbar/dock scaffolding, which has
	 * to be fought on a phone. A QMenuBar dropped into this layout is not
	 * that -- it is one widget, added on the platform that wants one, and
	 * both platforms still run the same window class.
	 *
	 * Why it exists at all: every way to open a tab, a window or a split
	 * was a right-click or a chord. Both are things you have to be told
	 * about, and the complaint that produced this was exactly that -- no
	 * visible way to add windows, tabs or tiles. A menu bar is how a
	 * desktop application says what it can do without being asked.
	 */
	/* The tray's way back (sec 5.5): each window put away answers it. */
	if (bssh_platform::instance()) {
		connect(bssh_platform::instance(), &bssh_platform::show_requested,
		        this, &bssh_main_window::come_back);
	}

	if (front_door_for(bssh_platform::instance()->is_touch_primary())
	    == FRONT_DOOR_SESSION_DIALOG) {
		build_menu_bar();
		if (m_menu_bar) outer->addWidget(m_menu_bar, 0);
	} else {
		/*
		 * No minimum size for the window, where the system sizes it.
		 *
		 * A top-level layout pins its window's minimum to what the
		 * contents need, and on a phone that minimum is stale exactly
		 * when it matters: turning to landscape delivers the new height
		 * BEFORE resizeEvent swaps the bottom keyboard for the split
		 * one, so the height was clamped to the upright layout's 395dp
		 * and never given back once the split needed only 243. The
		 * system does not grow a window to fit, so the bottom 99dp --
		 * the status line and the input selector -- were drawn off the
		 * Fold's cover screen. Measured with uiautomator: both at
		 * [0,0][0,0], the key rows ending on the screen's last pixel.
		 *
		 * The desktop keeps its minimum: there the window manager does
		 * honour it, and a window shrunk past its contents is worse.
		 */
		outer->setSizeConstraint(QLayout::SetNoConstraint);
	}

	outer->addWidget(m_pages, 1);

	/*
	 * The forms' keyboard (sec 6.2), on the touch front door only: every
	 * page but the terminal's types with the system keyboard until the
	 * switch over it is pressed, and then with softkeys -- whose editing
	 * page has the arrows, undo and the clipboard Android's keyboards do
	 * badly, which is the reason softkeys exists. Under the pages, so a
	 * page shrinks to make room for it as it does for the system one.
	 *
	 * Its own keyboard rather than the terminal's: that one types into a
	 * session's router and lives on the terminal page, and the two want
	 * different pages. Compact always, since a split keyboard's halves
	 * belong beside a terminal and there is none here.
	 */
	if (front_door_for(bssh_platform::instance()->is_touch_primary())
	    != FRONT_DOOR_SESSION_DIALOG) {
		m_form_keyboard = new sk_keyboard(&m_form_target, this);
		m_form_keyboard->setObjectName(QStringLiteral("form-keyboard"));
		m_form_keyboard->set_pages({ QStringLiteral("letters"), QStringLiteral("editing"),
		                             QStringLiteral("numbers") });
		m_form_keyboard->set_system_key_shown(true);
		m_form_keyboard->hide();
		outer->addWidget(m_form_keyboard, 0);

		m_keyboard_switch = new sk_keyboard_switch(this);
		connect(m_keyboard_switch, &sk_keyboard_switch::switch_requested,
		         this, [this] { use_softkeys_in_forms(true); });
		connect(m_form_keyboard, &sk_keyboard::system_keyboard_requested,
		         this, [this] { use_softkeys_in_forms(false); });

		/*
		 * Hide leaves the field, the way putting Android's keyboard
		 * away does: the keyboard follows the focus, so the next tap on
		 * a field brings it back, and a remembered "hidden" would need
		 * its own way to be undone.
		 */
		connect(m_form_keyboard, &sk_keyboard::hide_requested, this, [] {
			if (QWidget *focus = QApplication::focusWidget()) focus->clearFocus();
		});
		connect(m_pages, &QStackedWidget::currentChanged,
		         this, [this](int) { update_form_keyboard(); });
		connect(qApp, &QApplication::focusChanged,
		         this, [this](QWidget *, QWidget *) { update_form_keyboard(); });
	}

	QVBoxLayout *layout = new QVBoxLayout(m_terminal_page);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	/*
	 * No tab bar of the window's own any more: each pane has one
	 * (sec 8.3, agreed 2026-10-10), built by make_pane_bar() whenever the
	 * pane container builds the pane. What the single bar's comments
	 * argued -- a "+" where every browser puts it, labels elided in the
	 * middle, no bar while there is nothing to switch to -- is in
	 * bssh_pane_bar now, per pane.
	 */
	m_page_layout = layout;

	/*
	 * The pane container, not a lone terminal view. Both it and the layout
	 * manager existed, tested, since M8 -- and nothing ever constructed
	 * one, so every split, the size classes, the collapse-to-tabs rule and
	 * the pane navigation chords were unreachable from the running app.
	 * The window showed exactly one view because it only ever made one.
	 *
	 * m_terminal_view survives as a POINTER TO THE FOCUSED PANE rather
	 * than as a widget this window owns: everything that acts on "the
	 * terminal" -- copy, paste, scroll, the grid size in the status line
	 * -- means the focused one.
	 */
	m_layout = new bssh_layout_manager(this);

	/*
	 * The idiom picks the arrangement's default and then lets go (sec
	 * 8.7): a phone opens on the portrait column, a desktop on sec 8.4's
	 * tiles. is_touch_primary() stands in for the idiom until the
	 * three-way split lands -- the same proxy every idiom site uses today,
	 * and set here before the pane container exists so the first rebuild
	 * already reads the right shape.
	 */
	m_layout->set_arrangement(chosen_arrangement().arrangement);

	m_panes = new bssh_pane_container(m_layout, m_terminal_page);

	/* Its size is the room a desktop's tiled panes have (queue_room). */
	m_panes->installEventFilter(this);
	m_panes->set_bar_factory([this](bssh_session *front, QWidget *parent) {
		return make_pane_bar(front, parent);
	});
	/*
	 * In a row of their own, so a tab bar placed on the left or the right
	 * (sec 8.7) sits beside the panes rather than above them.
	 */
	m_body = new QHBoxLayout();
	m_body->setContentsMargins(0, 0, 0, 0);
	m_body->setSpacing(0);
	m_body->addWidget(m_panes, 1);
	layout->addLayout(m_body, 1);
	/*
	 * The status line's row. It moves into the split keyboard's left half
	 * while the halves show -- see place_status_line.
	 */
	QWidget *status_row = new QWidget(m_terminal_page);
	m_status_row = new QHBoxLayout(status_row);
	m_status_row->setContentsMargins(0, 0, 0, 0);
	m_status_row->setSpacing(0);
	m_status_row->addWidget(m_status_label, 1);
	layout->addWidget(status_row, 0);

	/*
	 * The input selector (sec 8.7) is not a row of the window any more:
	 * it is the drum in the focused pane's corner (sec 8.7a), which gave
	 * a narrow phone's 48dp row back to the terminal.
	 */

	/*
	 * The arrangement's other two halves -- how many terminals a column
	 * shows, and the Move state, offered only where there is a column to
	 * reorder -- now the container and the selector both exist.
	 */
	apply_arrangement();

	connect(m_panes, &bssh_pane_container::input_mode_chosen,
	         this, &bssh_main_window::set_input_mode);
	connect(m_panes, &bssh_pane_container::corner_tabs_requested,
	         this, &bssh_main_window::show_corner_tabs);
	connect(m_panes, &bssh_pane_container::corner_menu_requested,
	         this, &bssh_main_window::show_corner_menu);

	/*
	 * The find bar (sec 6.4) lives at the bottom too, below the selector,
	 * hidden until the find chord opens it. The search itself is the
	 * focused pane's; the window only routes the bar's query and steps to
	 * whichever view has focus, and puts the count back.
	 */
	m_find_bar = new bssh_find_bar(m_terminal_page);
	layout->addWidget(m_find_bar, 0);
	m_find_bar->hide();
	connect(m_find_bar, &bssh_find_bar::search_changed,
	         this, [this](const QString &query) { run_find(query); });
	connect(m_find_bar, &bssh_find_bar::step_requested,
	         this, [this](int direction) { step_find(direction); });
	connect(m_find_bar, &bssh_find_bar::dismissed, this, [this] { close_find(); });

	connect(m_panes, &bssh_pane_container::focus_changed,
	         this, &bssh_main_window::on_pane_focus_changed);

	/*
	 * Alt+drag on a pane, which is the gesture that makes a pane a state
	 * you can leave (sec 8.4). Dropped on the tab bar it becomes a tab,
	 * on another window's panes a tile there, and on nothing at all a
	 * window of its own -- the same three outcomes a dragged tab has, by
	 * the same code, because a terminal should not care which state it
	 * was in when it was picked up.
	 */
	/*
	 * A tap in the terminal brings the built-in keyboard back after its
	 * Hide key has put it away (sec 6.2). One tap, because a keyboard you
	 * have to find a menu to restore is one the Hide key made a trap.
	 *
	 * Only when a built-in keyboard is the one chosen -- with the system
	 * panel, the tap has already asked Qt for it and this would be a
	 * second answer to the same gesture.
	 */
	connect(m_panes, &bssh_pane_container::input_panel_requested, this, [this] {
		if (!m_keyboard_hidden_by_user) return;

		const bssh_app_settings settings;
		if (settings.keyboard() == bssh_app_settings::KEYBOARD_SYSTEM) return;

		m_keyboard_hidden_by_user = false;
		apply_key_row_visibility();
	});

	connect(m_panes, &bssh_pane_container::pane_drag_requested,
	         this, &bssh_main_window::start_session_drag);
	connect(m_panes, &bssh_pane_container::pane_move_requested,
	         this, &bssh_main_window::move_pane);

	/*
	 * A pane or tab dropped on a pane (sec 8.3), from EVERY pane. This was
	 * connected to the focused view alone, so a drop on any other pane --
	 * the one a user drags onto, nearly always -- was accepted by the view
	 * and delivered to nothing: the drag looked taken and nothing moved.
	 *
	 * The session is read now, while the drop's data exists, and placed a
	 * turn later: placing rebuilds the panes, and the view delivering this
	 * drop is one of them, still inside its dropEvent.
	 */
	connect(m_panes, &bssh_pane_container::session_dropped, this,
	         [this](const QMimeData *mime, bssh_session *target, int edge) {
		QPointer<bssh_session> dropped = session_from_mime(mime);
		QPointer<bssh_session> onto = target;
		if (!dropped || !onto) return;

		QMetaObject::invokeMethod(this, [this, dropped, onto, edge] {
			if (dropped && onto) drop_session_on(dropped, onto, edge);
		}, Qt::QueuedConnection);
	});
	/*
	 * And what such a drop WOULD do, asked by the pane as the pointer
	 * moves so that the zone it lights is the outcome rather than the
	 * geometry -- the same function the drop then obeys.
	 */
	m_panes->set_drop_planner([this](const QMimeData *mime, bssh_session *target, int edge) {
		return drop_zone_for(session_from_mime(mime), target, edge);
	});
	connect(m_panes, &bssh_pane_container::find_matches_changed,
	         this, &bssh_main_window::update_find_status);

	connect(m_panes, &bssh_pane_container::panes_rebuilt, this, [this] {
		on_pane_focus_changed(m_panes->focused_session());

		/* What a pane can be shrunk to may have changed with its bar. */
		queue_room();

		/*
		 * What is a pane just changed, so what is a TAB just changed
		 * too -- they are the same list split two ways (sec 8.3).
		 * Rebuilt here rather than from on_registry_changed alone,
		 * which fires when the registry changes and not when a split
		 * does: splitting a window left the new pane still listed in
		 * the bar above itself until something else happened to
		 * refresh it.
		 */
		refresh_tab_bar();

		/*
		 * A pane built after the setting was changed starts from the
		 * view's own default, so the device settings are re-applied to
		 * every rebuild rather than only when the user edits them --
		 * otherwise opening a second host quietly ignores the choice.
		 */
		apply_device_settings();

		/* The same for the input mode: a new pane starts from the view's
		 * default and has to be told the window's current selection. */
		apply_input_mode();

		/* And each pane its session's note (sec 8.7a). */
		for (bssh_session *front : m_layout->visible_panes()) {
			if (bssh_terminal_view *view = m_panes->view_for(front))
				view->set_status_note(m_notes.value(front));
		}
	});

	/*
	 * The bars' gestures -- pick, close, reorder, middle-click, the menu
	 * by right-click or a held finger, tearing out -- are wired per pane
	 * in make_pane_bar() now (sec 8.3's bar per pane).
	 *
	 * A tab pulled clear of one window's bar can land on another's
	 * (sec 8.4). Gated on a pointer, because the drag it receives can
	 * only be started where there is a pointer to start it -- offering
	 * to receive one on a phone would be a capability nothing can
	 * exercise.
	 */
	const bool pointer = bssh_platform::instance()
	                      && !bssh_platform::instance()->is_touch_primary();
	setAcceptDrops(pointer);

	connect(m_registry, &bssh_session_registry::changed,
	         this, &bssh_main_window::on_registry_changed);

	/*
	 * A host row changing has to reach the sessions already open on it.
	 * The store announces every write through one signal, and this is the
	 * only listener it has ever had -- before this, an edit made while
	 * connected was inert until the tab was closed.
	 */
	connect(m_profile_store, &bssh_profile_store::changed,
	         this, &bssh_main_window::refresh_sessions_from_store);

	/*
	 * A session about to be destroyed must be let go of first: the view
	 * holds pointers into its screen, scrollback and images, and the next
	 * repaint would read freed cells (sec 3, core principle 2).
	 */
	connect(m_registry, &bssh_session_registry::session_closing,
	         this, [this](bssh_session *session) {
		/*
		 * Forget it was wired. The set exists to stop a session being
		 * wired twice, and a stale entry would stop a LATER session
		 * being wired at all, since the allocator is free to hand back
		 * the same address.
		 *
		 * NOT pinned by a test, and deliberately not given a sabotage
		 * entry either: whether an address is ever recycled is the
		 * allocator's business, so a test would pass with or without
		 * this line. A check that cannot fail is worse than none.
		 */
		m_wired_sessions.remove(session);
		m_remember_once_in.remove(session);

		/*
		 * Out of the layout whether or not it is the one on screen. A
		 * session closed while ANOTHER pane is focused never reached
		 * detach_session() at all, so nothing dropped it from the
		 * intent and its view stayed in the splitter attached to memory
		 * about to be freed -- the same fatal read as above, needing a
		 * split to reach and therefore rarer, which is exactly the
		 * shape that survives testing.
		 */
		if (m_layout) m_layout->forget(session);

		if (m_current == session) {
			detach_session();
		} else if (m_panes) {
			m_panes->rebuild();
		}

		/*
		 * The last pane of a window whose session ENDED takes the
		 * window with it, on a device that opens with a shell
		 * (sec 8.6). A terminal whose program has finished is done,
		 * and falling back to the PuTTY-shaped front door instead
		 * would mean `beerssh -e ls` printed its output and then
		 * offered a connection dialog.
		 *
		 * Asked of opens_a_local_shell() rather than of the OS: a
		 * device that opens with its host list returns to that list,
		 * which is where a phone's Back button already goes (sec 8.3).
		 * And only for an ENDING -- closing a tab by hand leaves the
		 * user driving the app, so that keeps going up to the front
		 * door as it always has.
		 */
		if (!session->exited_cleanly()) return;
		if (m_shown.size() != 1 || m_shown.first() != session) return;
		if (!opens_a_local_shell(bssh_platform::instance()
		                          && bssh_platform::instance()->is_touch_primary())) {
			return;
		}

		/* Not from inside the registry's own signal. */
		QTimer::singleShot(0, this, [this] { close(); });
	});

	/*
	 * Another view has taken a session this window may be showing
	 * (sec 8.4). Let go of it -- the session is untouched and is now
	 * somebody else's to display.
	 */
	/*
	 * Taken by another BeerSSH (sec 8.4): when it was the last thing this
	 * window showed, the window goes, even as the last window of its
	 * process. The holder: "when moving the last terminal out of a process
	 * it stays". The rule below spares the last window because closing it
	 * would quit the program on a gesture about rearranging it; with every
	 * window its own process (sec 8.4), the terminal has gone on to another
	 * BeerSSH, and what would stay is an empty window holding a process
	 * that serves nothing. After the release, which empties the list.
	 */
	connect(m_registry, &bssh_session_registry::session_given_away,
	         this, [this](bssh_session *session) {
		if (!m_shown.contains(session) || m_shown.size() != 1) return;
		QTimer::singleShot(0, this, [this] {
			if (!m_shown.isEmpty()) return;
			close();
		});
	});

	connect(m_registry, &bssh_session_registry::session_taken,
	         this, [this](bssh_session *session) {
		if (!m_shown.contains(session)) return;

		m_shown.removeAll(session);
		if (m_current == session) detach_session();
		on_registry_changed();

		/*
		 * And if that was the last one, this window has no reason to
		 * exist. It stayed open, empty, titled "BeerSSH" with "no
		 * session" in the status line -- found by dragging the only
		 * pane of a torn-out window back into the one it came from,
		 * which is the whole point of being able to tear one out.
		 *
		 * The rule is not new. tear_out() already refuses to empty a
		 * window, on the grounds that the result is useless; a drop
		 * into ANOTHER window empties one just as thoroughly and
		 * nothing enforced it there.
		 *
		 * Never the last window, which would quit the program on a
		 * gesture that was about rearranging it.
		 *
		 * close() rather than deleteLater(), and a test aborted to say
		 * so: a window need not be heap-owned. One built on the stack
		 * -- which is how most of this suite builds them -- met
		 * deleteLater() and Qt tried to free stack memory, SIGABRT.
		 * close() lets each window keep its own policy: one made by
		 * tear_out() carries WA_DeleteOnClose and goes, and one that
		 * does not simply hides. Nothing here prompts, because the
		 * confirmation in closeEvent() is about live sessions and this
		 * window has just lost its last.
		 */
		if (m_shown.isEmpty() && other_windows_exist()) close();
	});

	/*
	 * The front door, and only the one this device wants (sec 8.5). This
	 * block was lost in the rewrite that introduced the session registry,
	 * and nothing noticed for eight commits: every test drives the widgets
	 * directly, and every screenshot rendered them directly, so the app
	 * opening straight into an empty terminal was invisible until it ran
	 * on a phone. showing_front_door() now has a test.
	 */
	if (front_door_for(bssh_platform::instance()->is_touch_primary())
	    == FRONT_DOOR_HOST_LIST) {
		m_host_list = new bssh_host_list(m_profile_store, this);
		connect(m_host_list, &bssh_host_list::open_requested,
		         this, &bssh_main_window::open_profile);
		connect(m_host_list, &bssh_host_list::add_requested,
		         this, &bssh_main_window::on_add_host);

		connect(m_host_list, &bssh_host_list::edit_requested,
		         this, [this](const bssh_profile_t &profile) {
			if (!m_host_editor) return;
			m_host_editor->set_profile(profile);
			m_host_editor->set_editing(true);
			show_page(m_host_editor);
		});

		connect(m_host_list, &bssh_host_list::delete_requested,
		         this, [this](const bssh_profile_t &profile) {
			if (!m_profile_store || profile.id == 0) return;

			/*
			 * The same report the save above makes. Silence here left a
			 * host that had apparently been deleted sitting in the list
			 * again at the next start, with no reason given.
			 */
			if (!m_profile_store->remove(profile.id)) {
				QMessageBox::warning(
				        this, QStringLiteral("Not deleted"),
				        QStringLiteral("This host could not be removed from your "
				                        "configuration.\n\n%1")
				                .arg(m_profile_store->last_error()));
			} else if (!m_profile_store->last_error().isEmpty()) {
				/* Removed, but its password outlived it. */
				QMessageBox::warning(this, QStringLiteral("Password not removed"),
				                      m_profile_store->last_error());
			}
			m_host_list->reload();
		});
		m_pages->addWidget(m_host_list);

		/*
		 * Its own page rather than a dialog (sec 8.5). A phone has one
		 * screen and the user is holding it; a form that covers it is
		 * the native shape, and going back is the system's own gesture.
		 */
		m_host_editor = new bssh_host_editor(m_profile_store, this);
		m_pages->addWidget(m_host_editor);

		connect(m_host_editor, &bssh_host_editor::save_requested,
		         this, [this](const bssh_profile_t &profile) {
			/*
			 * Said rather than swallowed. The store now reports a
			 * write it could not make -- a read-only home, a full
			 * disk, permissions somebody changed (sec 10.6) -- and
			 * returning to the list as though it had worked is how
			 * a host is lost between here and the next start.
			 */
			if (m_profile_store->save(profile) == 0) {
				QMessageBox::warning(
				        this, QStringLiteral("Not saved"),
				        QStringLiteral("This host could not be written to "
				                        "your configuration.\n\n%1")
				                .arg(m_profile_store->last_error()));
				return;
			}

			m_host_list->reload();
			go_back();
		});
		connect(m_host_editor, &bssh_host_editor::cancelled, this, [this] {
			go_back();
		});

		/*
		 * The same editor the desktop dialog embeds, as its own page
		 * (sec 8.5: same job, different shape). sec 14 is explicit that a
		 * milestone lands on both platforms before the next one starts,
		 * and a preset system editable only on a desktop would leave the
		 * phone -- the device this app is mostly for -- stuck with
		 * whatever was shipped.
		 */
		m_preset_editor = new bssh_preset_editor(m_profile_store,
		                                          bssh_preset_editor::TOUCH,
		                                          bssh_preset_editor::EVERYTHING, this);
		m_pages->addWidget(m_preset_editor);

		connect(m_preset_editor, &bssh_preset_editor::preset_saved,
		         this, &bssh_main_window::reresolve_sessions);
		connect(m_preset_editor, &bssh_preset_editor::preset_removed,
		         this, &bssh_main_window::reresolve_sessions);
		connect(m_preset_editor, &bssh_preset_editor::back_requested, this, [this] {
			go_back();
		});

		bssh_licences_view *licences = new bssh_licences_view(this);
		m_pages->addWidget(licences);

		connect(licences, &bssh_licences_view::back_requested, this, [this] {
			go_back();
		});

		/*
		 * SETTINGS IS AN INDEX AND A PAGE PER SECTION (sec 8.5).
		 *
		 * One page carrying everything was 1929px tall at 320dp -- three
		 * screens of a 640px phone, and the tallest surface in the app.
		 * Split, four of the five sections need no scrolling at all:
		 * Keyboard 483, Appearance 687, Terminal 215, Passwords 141,
		 * Shortcuts 633.
		 *
		 * NOT a category tree, which sec 8.5 rules out by name as "a
		 * desktop idiom that survives neither a thumb nor a 5-inch
		 * screen". This is the other shape: full-screen pages in
		 * sequence, reached by tapping a row and left with Back --
		 * which is what show_page()/go_back() already does for the host
		 * editor and the licences, so it adds no navigation concept.
		 *
		 * The desktop is untouched. There the same view is built whole,
		 * as the "This device" category inside sec 8.5's PuTTY tree
		 * (session_dialog), where it already sits behind one level of
		 * navigation and a second would be a tree inside a tree.
		 */
		QWidget *index = new QWidget(this);
		index->setObjectName(QStringLiteral("settings-index"));
		QVBoxLayout *index_layout = new QVBoxLayout(index);
		index_layout->setContentsMargins(0, 0, 0, 0);
		index_layout->setSpacing(0);

		/*
		 * A heading, the same shape the host list gives itself: a
		 * 1.4x bold title in a 56px band. Rendered on the device the
		 * index had none, so its first row sat directly under the
		 * status bar and the page read as a list that had lost its
		 * title rather than as a screen. Every assertion passed
		 * throughout -- sec 12.3c's point about a picture again.
		 */
		QLabel *index_title = new QLabel(QStringLiteral("  Settings"), index);
		QFont index_font = bssh_scaled_font(index_title->font(), 1.4);
		index_font.setBold(true);
		index_title->setFont(index_font);
		index_title->setMinimumHeight(56);
		index_layout->addWidget(index_title);

		QListWidget *sections = new QListWidget(index);
		sections->setFrameShape(QFrame::NoFrame);
		sections->setTextElideMode(Qt::ElideRight);

		/*
		 * Nothing is chosen until a thumb chooses it. A QListWidget
		 * gives its first row the current-item focus rectangle, which
		 * on a touch list reads as a selection the user did not make --
		 * on the device the Keyboard row came up already boxed.
		 */
		sections->setFocusPolicy(Qt::NoFocus);
		sections->setSelectionMode(QAbstractItemView::NoSelection);

		/*
		 * A rule between the rows, for the reason sec 8.5 records
		 * about the host list: without one the screen read as
		 * paragraphs of text rather than as a list to tap. The index
		 * arrived with the same fault, which is what a gallery is for.
		 */
		m_settings_sections = sections;
		style_settings_index();
		QScroller::grabGesture(sections->viewport(),
		                        QScroller::LeftMouseButtonGesture);
		index_layout->addWidget(sections, 1);

		const bssh_app_settings_view::section_t order[] = {
			bssh_app_settings_view::SECTION_KEYBOARD,
			bssh_app_settings_view::SECTION_APPEARANCE,
			bssh_app_settings_view::SECTION_TERMINAL,
			bssh_app_settings_view::SECTION_PASSWORDS,
			bssh_app_settings_view::SECTION_SHORTCUTS,
		};

		QList<QWidget *> section_pages;

		/*
		 * PRESETS IS A ROW HERE, not a sibling of this page (sec 8.7).
		 *
		 * It was in the host list's overflow menu beside "This device",
		 * which made a preset -- the thing every host's font, colours,
		 * scrollback and keepalive resolve through (sec 10.1) -- a
		 * peer of config rather than part of it. The holder asked for
		 * "exactly one config menu" with "the presets in the config
		 * menu, editable", and this is that: same editor, same page,
		 * reached from the one road.
		 *
		 * First rather than last because it is the setting that reaches
		 * furthest -- the five below it describe this device, and a
		 * preset describes every host that follows it.
		 */
		QListWidgetItem *presets_row =
		        new QListWidgetItem(QStringLiteral("Presets"), sections);
		presets_row->setSizeHint(QSize(0, BSSH_TOUCH_TARGET_DP));
		section_pages.append(m_preset_editor);

		for (bssh_app_settings_view::section_t which : order) {
			bssh_app_settings_view *page =
			        new bssh_app_settings_view(which, this);
			m_pages->addWidget(page);
			connect(page, &bssh_app_settings_view::changed,
			         this, &bssh_main_window::apply_device_settings);
			connect(page, &bssh_app_settings_view::back_requested,
			         this, [this] { go_back(); });

			QListWidgetItem *row = new QListWidgetItem(
			        bssh_app_settings_view::section_name(which), sections);
			row->setSizeHint(QSize(0, BSSH_TOUCH_TARGET_DP));
			section_pages.append(page);
		}

		/* The licences reach the same list, being the other thing a
		 * person looks for under settings. */
		QListWidgetItem *licences_row =
		        new QListWidgetItem(QStringLiteral("Licences"), sections);
		licences_row->setSizeHint(QSize(0, BSSH_TOUCH_TARGET_DP));
		section_pages.append(licences);

		connect(sections, &QListWidget::itemClicked, this,
		         [this, section_pages](QListWidgetItem *item) {
			const int row = item->listWidget()->row(item);
			if (row < 0 || row >= section_pages.size()) return;

			QWidget *page = section_pages.at(row);

			/*
			 * The preset editor reads the store when it is opened
			 * rather than when it was built, which is what the
			 * overflow menu's route did before this. Without it a
			 * preset saved on one visit is missing on the next --
			 * and the page would look correct, being a list that
			 * simply has not been told.
			 */
			if (page == m_preset_editor) m_preset_editor->reload();

			show_page(page);
		});

		QPushButton *index_back = new QPushButton(QStringLiteral("Back"), index);
		index_back->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
		connect(index_back, &QPushButton::clicked, this, [this] { go_back(); });
		index_layout->addWidget(index_back);

		m_pages->addWidget(index);
		m_device_settings = index;

		connect(m_host_list, &bssh_host_list::settings_requested, this, [this, index] {
			show_page(index);
		});

		/*
		 * Down to the session tapped, the way up from a terminal reversed
		 * (decided 2026-10-07, sec 8.5) -- to THAT one since 2026-10-22,
		 * when the list started naming them.
		 */
		connect(m_host_list, &bssh_host_list::session_picked, this,
		         [this](bssh_session *session) {
			if (!session || !m_shown.contains(session)) return;
			m_pages->setCurrentWidget(m_terminal_page);
			show_session(session);
			if (m_terminal_view) m_terminal_view->setFocus();
		});
	} else {
		m_session_dialog = new bssh_session_dialog(m_profile_store, this);
		connect(m_session_dialog, &bssh_session_dialog::open_requested,
		         this, [this](const bssh_profile_t &profile) {
			open_profile(profile);
			hide_front_door();
		});
		/*
		 * Cancel means "I am done with the settings", not "throw away
		 * my work". Wired straight to close() it took the window and
		 * every live session in it: the config screen is a PAGE in the
		 * same stack as the terminal, so leaving it looked to the code
		 * like leaving the app. Reported from use -- open the settings
		 * from a working shell, press Cancel, lose the shell, and an
		 * SSH session is not something a reconnect brings back (sec
		 * 5.3).
		 *
		 * Quitting is still right when there is nothing behind it,
		 * which is the case it was written for: a desktop launch with
		 * no target named shows the front door first, and declining it
		 * there has always meant "do not start".
		 *
		 * The touch side never had this bug -- its Back goes to the
		 * host list -- which is the tell that the wiring was about the
		 * startup screen and got applied to a screen that is also
		 * reachable mid-session.
		 */
		connect(m_session_dialog, &bssh_session_dialog::cancelled, this, [this] {
			hide_front_door();

			/*
			 * With nothing behind it, declining the front door still
			 * means "do not start" -- the startup case the original
			 * close() was written for, and the only one it was ever
			 * right about.
			 */
			if (m_shown.isEmpty()) close();
		});

		/*
		 * Propagation, made visible (sec 10.1). Editing a preset that
		 * seven hosts follow has to change those seven hosts, and the
		 * ones the user can actually see are the connected ones -- a
		 * change that only reached the NEXT connection would look like
		 * the edit had not worked.
		 */
		connect(m_session_dialog, &bssh_session_dialog::presets_changed,
		         this, &bssh_main_window::reresolve_sessions);
		connect(m_session_dialog, &bssh_session_dialog::device_settings_changed,
		         this, &bssh_main_window::apply_device_settings);
		/*
		 * A WINDOW, not a page in the same stack as the terminal.
		 *
		 * As a page it produced both halves of one complaint. Leaving
		 * it closed the window and every live session in it, because
		 * "done with the settings" and "done with the app" were the
		 * same gesture. And a window could exist containing nothing but
		 * configuration, because the stack was happy to show that --
		 * so the app stayed running with no terminal in it, which is
		 * the opposite failure from the same cause.
		 *
		 * As a window neither is expressible: closing it cannot reach
		 * the terminal, and a main window is a terminal by
		 * construction. Parented to this one so it stays associated
		 * with it and is destroyed with it, but Qt::Window so it gets
		 * its own frame, its own place in the task bar and its own
		 * close button.
		 */
		m_session_dialog->setParent(this, Qt::Window);
		m_session_dialog->setWindowTitle(QStringLiteral("BeerSSH Configuration"));
		m_session_dialog->hide();
	}

	m_pages->addWidget(m_terminal_page);

	/*
	 * With every page built, take the hidden ones out of the window's
	 * minimum -- and keep doing it as the shown page changes.
	 */
	connect(m_pages, &QStackedWidget::currentChanged,
	         this, [this](int) { keep_the_minimum_to_the_shown_page(); });
	keep_the_minimum_to_the_shown_page();

	m_status_label->setObjectName(QStringLiteral("status_line"));
	m_status_label->setContentsMargins(6, 3, 6, 3);

	/*
	 * The status line must never decide how wide the window is.
	 *
	 * A QLabel asks for room for its whole text, and a failure reason is
	 * a sentence -- "the jump host X is not a known host yet, ssh to it
	 * once so its key is recorded" is far wider than a phone. Left to
	 * itself the label pushed the layout past the screen and everything
	 * in it, including the terminal, went off the right-hand edge: a
	 * connection error blanked the app. So it is given no say in the
	 * width and its text is shortened to whatever room it ends up with.
	 */
	m_status_label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

	/*
	 * The status line is scaffolding, not a feature: it reports which
	 * platform implementation was built in, the grid size, and where the
	 * session is. At M7 this space becomes the per-session status (sec
	 * 8.3).
	 */
	/*
	 * No grid yet: the views belong to the pane container and there are
	 * no panes until a session is shown. on_pane_focus_changed connects
	 * grid_resized and reads the size when one appears.
	 */

	/*
	 * The key row is built with the window but its router is not: a
	 * router belongs to a session, and the key row is retargeted at
	 * whichever session is showing (sec 3, core principle 2).
	 */
	/* One touch floor for this app and its keyboard (sec 6.2). */
	static_assert(SK_TOUCH_TARGET_DP == BSSH_TOUCH_TARGET_DP,
	              "softkeys' touch target and this app's disagree");

	m_key_row = new sk_key_row(nullptr, this);

	/* AUTO follows this app's own answer, which its tests can force. */
	m_key_row->set_touch_primary([] {
		return bssh_platform::instance() && bssh_platform::instance()->is_touch_primary();
	});
	/*
	 * Last in the page, under the status line, the input selector and the
	 * find bar: the keys are the bottom edge, where Android puts its own,
	 * and the key row sits directly on whichever keyboard is up. They were
	 * inserted under the status line instead, which left the selector's
	 * five buttons below the keyboard (the holder, 2026-10-23).
	 */
	layout->addWidget(m_key_row, 0);

	/*
	 * And the built-in keyboard, in the same slot, for the same session.
	 * Exactly one of the two is ever visible (sec 6.2): they carry the
	 * same modifiers, and two Ctrl buttons over one router is two answers
	 * to what is armed.
	 *
	 * Built on every platform rather than behind an ifdef, for the reason
	 * the key row is: a widget compiled only on Android is a widget
	 * tested only on Android, and this one is laid out to a phone's width
	 * by a test that runs on the desktop.
	 */
	m_keyboard = new sk_keyboard(nullptr, this);
	m_keyboard->setObjectName(QStringLiteral("terminal-keyboard"));
	layout->addWidget(m_keyboard, 0);
	m_keyboard->hide();

	/*
	 * The split keyboard's halves go either side of the terminal, at the
	 * outer edges of the row the panes sit in, so in landscape the keys
	 * take width and the terminal keeps its height (sec 6.2). The
	 * keyboard still decides when they show.
	 */
	m_body->insertWidget(0, m_keyboard->half(0), 0);
	m_body->addWidget(m_keyboard->half(1), 0);

	/*
	 * Whether the halves show decides where the status line goes (see
	 * place_status_line), and showing or hiding the keyboard changes
	 * no size the window is told about -- so their own show and hide.
	 */
	m_keyboard->half(1)->installEventFilter(this);

	/*
	 * The keyboard's own two keys (sec 6.2). Hide is remembered rather
	 * than merely applied: apply_device_settings recomputes visibility on
	 * every session event and every settings change, so a keyboard that
	 * only called hide() would come back on the next one, which from the
	 * user's side is a key that does not work.
	 */
	connect(m_keyboard, &sk_keyboard::hide_requested, this, &bssh_main_window::hide_keyboard);

	/*
	 * Cfg goes to the same page the front door's menu opens, rather than
	 * a second settings surface -- sec 14's M10 records what two front doors to
	 * one setting cost when only one of them was kept in step.
	 *
	 * On a desktop there is no touch front door and no page stack, so
	 * there is nothing to show; the button is only ever pressed on a
	 * device that has one.
	 */
	/*
	 * A resize bar let go: the size is kept for next time, per keyboard
	 * (sec 6.2), so a shrunk compact keyboard does not shrink the full one.
	 */
	connect(m_keyboard, &sk_keyboard::size_chosen,
	         this, [](sk_catalog::style_t style, int size_dp) {
		bssh_app_settings settings;
		if (style == sk_catalog::STYLE_SPLIT) {
			settings.set_keyboard_split_width(size_dp);
		} else {
			settings.set_keyboard_height(style == sk_catalog::STYLE_FULL,
			                             size_dp);
		}
	});

	connect(m_keyboard, &sk_keyboard::settings_requested, this, [this] {
		if (m_pages && m_device_settings) show_page(m_device_settings);
	});

	/*
	 * sec 6.2's three-state setting, which the key row has supported since
	 * M3 and nothing ever set: AUTO was the only reachable mode, so a
	 * convertible laptop could not ask for the row and a tablet with a
	 * keyboard docked could not dismiss it.
	 */
	apply_device_settings();

	/*
	 * Shortcuts are the window's, because they act on the window: copy
	 * takes the view's selection, paste and scrolling act on whatever
	 * session is currently showing. Connected once here, retargeted by
	 * show_session().
	 */
	/*
	 * Wired to the registry here, and called directly by adopt_session()
	 * for a session that arrives from another window -- see wire_session.
	 */
	connect(m_registry, &bssh_session_registry::session_added,
	         this, &bssh_main_window::wire_session);



	/*
	 * X11/Wayland's primary selection: selecting text makes it available
	 * to middle-click paste, without touching the ordinary clipboard.
	 * Qt reports the mode as unsupported on platforms that lack it, so
	 * this needs no platform check of its own.
	 */
	update_status();

	/*
	 * A session named on the command line skips the front door entirely,
	 * which is what anyone typing one wants. Otherwise the front door is
	 * the first thing shown, as it is in PuTTY, and main() decides
	 * whether to restore last time's tabs on top of it.
	 */
	if (launch.is_valid()) {
		m_pages->setCurrentWidget(m_terminal_page);
		open_profile(launch);
		return;
	}

	/*
	 * No target named, so this window opens on the front door.
	 *
	 * Recorded here and acted on in showEvent(). It used to happen by
	 * accident: the front door was the stack's first page, so a window
	 * with nothing else in it showed one without anybody saying so. Now
	 * that the desktop's is a window of its own, the intent has to be
	 * stated -- and the two tests that guard "the app must not open
	 * straight into an empty terminal" are what caught its absence.
	 */
	show_front_door();
}

/*
 * Everything opening a host involves except the connecting: the session,
 * its resolved settings, the prompts it may raise, and showing it.
 *
 * Split out because a restore wants all of that and none of the last step
 * (sec 8.4). A restored host arrives as a tab that is not connected, and
 * nothing about it may put a credential prompt on screen at startup.
 */
bssh_session *bssh_main_window::prepare_profile(const bssh_profile_t &profile) {
	if (!profile.is_valid()) return nullptr;

	/*
	 * No password prompt here. Keys are tried first and most people have
	 * one, so asking up front is asking for a credential that is usually
	 * not needed -- the session asks through needs_password() only when
	 * the keys have actually failed.
	 */
	bssh_session *session = m_registry->create(profile);

	/*
	 * Shown by the window that opened it (sec 8.4). The registry holds
	 * every session in the process; which window displays one is view
	 * state, and it belongs to whichever window asked for it.
	 */
	if (!m_shown.contains(session)) m_shown.append(session);

	/*
	 * Resolve the host's settings once, here, where the store is (sec
	 * 10.1). The session carries them from now on, so a pane rebuilt by a
	 * layout change does not have to find a preset table to know what font
	 * to draw in.
	 */
	session->set_settings(resolved_settings(profile));

	show_session(session);
	return session;
}

void bssh_main_window::open_profile(const bssh_profile_t &profile) {
	bssh_session *session = prepare_profile(profile);
	if (session) session->connect_to_host();
}

void bssh_main_window::on_add_host() {
	/*
	 * The touch front door's "+". A full-screen form on this platform;
	 * the desktop reaches the same profile through the PuTTY-shaped
	 * Session page instead, which is why this path only exists where the
	 * host list does.
	 */
	if (!m_host_editor || !m_host_list) return;

	m_host_editor->set_profile(bssh_profile_t());
	m_host_editor->set_editing(false);
	show_page(m_host_editor);
}

void bssh_main_window::show_session(bssh_session *session) {
	if (!session || !m_layout || !m_panes) return;

	/*
	 * Showing a session means showing the page it is on, and this is the
	 * one function that means "show this session" -- so it belongs here
	 * rather than at each caller.
	 *
	 * open_profile() did it separately and the other three callers did
	 * not, which is the two-places-computing-one-thing this document
	 * keeps unpicking. What it cost: adopt_session() is how a torn-out
	 * tab and a dropped one arrive, so a window made by tearing a tab out
	 * came up showing the FRONT DOOR, with the session it was made for
	 * loaded, connected and behind it -- and the tab bar hidden with it.
	 * Found by a drop test asking whether the bar it meant to aim at was
	 * on screen.
	 */
	m_pages->setCurrentWidget(m_terminal_page);

	/*
	 * And on desktop the front door is a WINDOW, so bringing the terminal
	 * to the front means putting that away too -- a page swap does
	 * nothing to a separate window sitting over it.
	 *
	 * The comment above records this exact bug in its page-stack form: a
	 * window made by tearing out a tab came up showing the front door
	 * with its session behind it. The same mistake is available again in
	 * a new shape, which is what a structural change buys you.
	 */
	hide_front_door();

	/*
	 * The window no longer attaches a view to a session: it says which
	 * sessions should be on screen, and the pane container builds the
	 * views for them. That indirection is the whole of M8 -- one pane or
	 * four, collapsing to tabs when the window narrows, is a question
	 * about the LIST rather than about any one view.
	 */
	/*
	 * Every session is in some pane's stack (sec 8.3's bar per pane), so
	 * showing one brings it to the front of its pane -- and a pane a
	 * narrow window could not show trades places with one it can. In a
	 * column as on the desktop: a stack per pane in both.
	 */
	sync_tree_with_shown();
	m_layout->bring_to_front(session);

	m_panes->rebuild();
	m_panes->focus_session(session);

	if (m_terminal_view) {
		bssh_apply_appearance(m_terminal_view, session->settings());
		m_terminal_view->setFocus();
	}

	update_status();
}

void bssh_main_window::split_beside_current() {
	if (!m_current || !m_layout) return;

	/*
	 * A NEW terminal beside the current one, which is what a split means
	 * in every multiplexer and is what the user asking for one wants.
	 *
	 * Both menus used to call split_with(m_current), and m_current is by
	 * construction ALWAYS in the layout intent -- show_session() puts it
	 * there and m_current is read back out of the visible panes -- so
	 * split_with's "already showing" guard was unconditionally taken and
	 * the action did nothing at all. Enabled, clickable, silent. The
	 * guard is right for its other caller, a drop, which must decline a
	 * session this window already shows; it was never a sensible thing to
	 * ask of the current session, because a terminal cannot go beside
	 * itself.
	 *
	 * Tiling an EXISTING session is the tab context menu's "Show beside
	 * current", which passes the session that was right-clicked and has
	 * always worked. The two verbs stay distinct: that one rearranges
	 * what you have, this one makes another.
	 */
	/*
	 * The intent is taken BEFORE the session exists, because creating one
	 * shows it -- prepare_profile() ends up in show_session(), which sets
	 * the intent to that session alone. Calling split_with() afterwards
	 * walks into the same guard the menus did: the session is already in
	 * the intent, so it declines and the window is left showing the new
	 * terminal INSTEAD of the old one rather than beside it.
	 *
	 * Naming both sides here is what makes "beside" mean beside.
	 */
	const QList<bssh_session *> before = m_layout->intent();
	bssh_session *front_before = m_layout->front_of(m_current);

	bssh_session *session = prepare_profile(bssh_profile_t::local_shell());
	if (!session) return;

	/*
	 * Opening it showed it -- at the front of the focused pane (sec 8.3's
	 * bar per pane) -- so that pane's front is handed back first, or the
	 * split below moves the session the user was looking at into the new
	 * pane and leaves the new one behind. Seen on the phone.
	 */
	if (front_before && front_before != session) m_layout->bring_to_front(front_before);

	QList<bssh_session *> beside = before;
	if (!beside.contains(session)) beside.append(session);
	m_layout->set_intent(beside);

	if (m_panes) {
		m_panes->rebuild();
		m_panes->focus_session(session);
	}

	session->connect_to_host();
	update_status();
}

void bssh_main_window::show_as_tab(bssh_session *session) {
	if (!session || !m_layout || !m_panes) return;

	const QList<bssh_session *> panes = m_layout->visible_panes();
	const int at = panes.indexOf(session);
	if (at < 0) return;

	/*
	 * The last pane cannot become a tab: a window with no pane is a
	 * window showing nothing. Refused rather than silently ignored is the
	 * same answer tear_out gives ("a window will not empty itself",
	 * sec 8.4) applied one level in.
	 */
	if (panes.size() < 2) return;

	/*
	 * Into the bar of the pane beside it -- the one before, or after the
	 * first -- every tab of it, behind what that pane shows (sec 8.3's
	 * bar per pane). The pane it leaves closes and its neighbours take
	 * the room.
	 */
	bssh_session *into = panes.at(at > 0 ? at - 1 : 1);
	for (bssh_session *tab : m_layout->tabs(session)) m_layout->add_tab(tab, into, INT_MAX);
	m_layout->bring_to_front(into);

	/*
	 * Focus goes somewhere, and it has to be said explicitly: the pane
	 * that was focused may be the one that just left, which leaves a
	 * window whose panes are all unfocused -- no border, no cursor
	 * filled, and keystrokes going to whatever Qt picked.
	 */
	m_panes->focus_session(into);

	refresh_tab_bar();
	update_status();
}

void bssh_main_window::split_with(bssh_session *session) {
	if (!session || !m_layout || !m_panes) return;

	QList<bssh_session *> intent = m_layout->intent();
	if (intent.contains(session)) return;

	intent.append(session);
	m_layout->set_intent(intent);

	/*
	 * The layout decides how many of those actually fit: asking for a
	 * fourth pane in a phone-width window is not refused, it is
	 * REMEMBERED and shown when there is room (sec 8.2). What the user
	 * gets now is whatever the current size class allows.
	 */
	m_panes->rebuild();
	m_panes->focus_session(session);

	update_status();
}

void bssh_main_window::showEvent(QShowEvent *event) {
	QWidget::showEvent(event);

	/*
	 * Armed once, here rather than in the constructor: windowHandle() is
	 * null until the window is created, so a connection made earlier is a
	 * connection to nothing -- and one made on every show would stack up.
	 */
	/*
	 * The front door, if the constructor decided this window opens on one
	 * (sec 8.5). Here rather than there, so that constructing a window
	 * never puts anything on screen.
	 */
	if (m_front_door_up && m_session_dialog && !m_session_dialog->isVisible()) {
		m_session_dialog->show();
		m_session_dialog->raise();
	}

	if (m_screen_watched || !windowHandle()) return;

	m_screen_watched = true;
	connect(windowHandle(), &QWindow::screenChanged,
	         this, &bssh_main_window::on_screen_changed);
}

void bssh_main_window::on_screen_changed() {
	/*
	 * sec 9.5: dragged between monitors of different scaling, the grid
	 * must be re-laid-out rather than scaled from the old monitor's cell.
	 *
	 * The cell is snapped to whole DEVICE pixels, so it is a function of
	 * the device pixel ratio -- and nothing recomputed it when that ratio
	 * changed underneath. Qt rescales what is already drawn, which is the
	 * "scale a stale one" that section names: box-drawing stops meeting
	 * at the cell edge, and the remote is still laying out for the column
	 * count the other monitor gave.
	 */
	if (m_panes) m_panes->remeasure_panes();

	/*
	 * And the layout class with it. Qt reports logical pixels, so a
	 * window moved to a monitor with different scaling can change width
	 * in dp without a resizeEvent -- which is what decides how many panes
	 * fit (sec 8.2).
	 */
	/*
	 * Through set_width's own signal, for the reason resizeEvent gives:
	 * a rebuild that is not conditioned on the class changing destroys
	 * live views for nothing.
	 */
	if (m_layout) m_layout->set_width(width());

	update_status();
}

void bssh_main_window::apply_tab_position(int position) {
	if (!m_panes) return;
	if (position == m_tab_position) return;
	m_tab_position = position;

	/*
	 * Hidden: every bar is built and kept -- refresh_tab_bar compares a
	 * pane's tabs against its bar -- and none is shown; a pane's tabs are
	 * in its corner instead (sec 8.7a). The rebuild is what makes
	 * make_pane_bar decide again.
	 */
	const bool hidden = position == bssh_app_settings::TABS_HIDDEN;
	if (hidden != m_tabs_hidden) {
		m_tabs_hidden = hidden;
		m_panes->rebuild();
	}

	/* Every pane's bar on that side of its pane (sec 8.3's bar per pane). */
	Qt::Edge edge = Qt::TopEdge;
	switch (position) {
	case bssh_app_settings::TABS_BOTTOM: edge = Qt::BottomEdge; break;
	case bssh_app_settings::TABS_LEFT:   edge = Qt::LeftEdge; break;
	case bssh_app_settings::TABS_RIGHT:  edge = Qt::RightEdge; break;
	default:                             break;
	}
	m_panes->set_bar_edge(edge);
}

bool bssh_main_window::apply_arrangement() {
	if (!m_layout) return false;

	const bssh_layout_manager::arrangement_choice_t choice = chosen_arrangement();
	m_arranged_for = bssh_platform::instance() ? bssh_platform::instance()->screen_size()
	                                           : QSize();

	const bool changed = m_layout->arrangement() != choice.arrangement;
	if (changed) m_layout->set_arrangement(choice.arrangement);

	if (m_panes) m_panes->set_column_visible(choice.visible);

	/*
	 * Side by side has a tab bar to place; a column shows every session
	 * as a pane and has none, so it keeps the default, unseen.
	 */
	apply_tab_position(choice.arrangement == bssh_layout_manager::TILED
	                           ? int(bssh_app_settings().tab_position())
	                           : int(bssh_app_settings::TABS_TOP));

	/*
	 * The Move state needs a column to reorder. This was set once, at
	 * construction, and never again -- so a window switched to a column
	 * in the settings, or turned into one by being folded, offered no way
	 * to reorder it.
	 */
	m_column_available = choice.arrangement == bssh_layout_manager::COLUMN;
	if (!m_column_available && is_column_mode(m_input_mode))
		m_input_mode = bssh_default_input_mode();
	apply_input_mode();
	return changed;
}

/*
 * Where the status line goes. With the split keyboard showing, into the
 * band above the left half's keys: the keys sit at the bottom where thumbs
 * are, so that band was empty, while the status line's row under the
 * terminal cost it 48dp of a landscape phone's 296 -- the holder asked for
 * it back (2026-10-09). Otherwise its own row under the terminal. Moved
 * only when the answer changes, since this runs on every frame of a system
 * keyboard opening.
 *
 * The input selector used to be placed here too, in a row of its own or at
 * the status line's end; it is the drum in the focused pane's corner now
 * (sec 8.7a).
 */
void bssh_main_window::place_status_line() {
	if (!m_status_row) return;

	const bool halves = m_keyboard
	                    && m_keyboard->style() == sk_catalog::STYLE_SPLIT
	                    && m_keyboard->half(1)->isVisibleTo(this);
	if (halves) {
		QLayout *left = m_keyboard->half_header(0)->layout();
		if (left->indexOf(m_status_label) < 0) left->addWidget(m_status_label);
		return;
	}
	if (m_status_row->indexOf(m_status_label) < 0) m_status_row->insertWidget(0, m_status_label, 1);
}

void bssh_main_window::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);

	place_status_line();

	/* Re-shortened from the original: what fits changed. */
	elide_status();

	if (!m_layout) return;

	/*
	 * Device-independent pixels, which is what sec 8.2's thresholds are
	 * written in -- and Qt's logical pixels already ARE those, so this
	 * hands width() over unconverted.
	 *
	 * It used to divide by logicalDpiX()/160, on the reasoning that 600
	 * and 840dp must mean the same physical width on a phone and on a 4K
	 * monitor. That converts twice. Qt folds display density into
	 * devicePixelRatio and reports logical pixels above it, which on
	 * Android is precisely the density bucket -- so width() is dp on the
	 * platform the thresholds were borrowed from, and dividing again
	 * inflated every desktop width by 1.67x at 96 dpi. A 504-pixel window
	 * came out as 840dp and was given FOUR panes of about thirty columns
	 * each, which is the arrangement sec 8 calls worse than useless. The
	 * first thing ever to measure it was a drop onto a 360-pixel window
	 * that tiled instead of declining.
	 */
	/*
	 * set_width() rebuilds through layout_changed, and ONLY when the size
	 * class actually changed -- which is why there is no rebuild here.
	 *
	 * There was one, unconditional, and on Android it made the app
	 * untypable. Opening the on-screen keyboard resizes the window, so
	 * every frame of the keyboard's own appearance tore down every pane
	 * view; the view that had just asked for the panel was destroyed
	 * mid-animation, and Qt answers a focus object that no longer accepts
	 * input by calling QtInputDelegate.hideSoftwareKeyboard(). The
	 * keyboard flashed up and vanished on every tap, on a platform where
	 * that is the only way to type.
	 *
	 * The width does not even change when a keyboard opens -- only the
	 * height does -- so the rebuild could never have been the right
	 * answer to this event. set_width() has said as much in a comment
	 * since it was written: a rebuild per pixel is what it exists to
	 * prevent, and this walked straight past it.
	 */
	m_layout->set_width(width());

	/*
	 * A rotation or a fold changes the SCREEN, and the arrangement follows
	 * the screen (sec 8.7). Asked only when the screen moved: this runs for
	 * every frame of the on-screen keyboard opening, which changes the
	 * window and never the screen, and must not rebuild anything.
	 */
	const QSize screen = bssh_platform::instance() ? bssh_platform::instance()->screen_size()
	                                              : QSize();
	if (screen != m_arranged_for) apply_arrangement();

	/*
	 * The keyboard's own width question, which is not the layout
	 * manager's: it asks how many panes fit, this asks whether the
	 * full-size keyboard does (sec 6.2). Cheap on every resize, because
	 * set_style returns at once when the answer has not changed.
	 */
	apply_keyboard_style();
}

void bssh_main_window::set_input_mode(bssh_input_mode_t mode) {
	m_input_mode = mode;
	apply_input_mode();
}

void bssh_main_window::apply_input_mode() {
	if (!m_panes) return;

	for (bssh_terminal_view *view : m_panes->views()) {
		if (!view) continue;
		view->set_column_available(m_column_available);
		view->set_input_mode(m_input_mode);
	}
}

void bssh_main_window::apply_device_settings() {
	/*
	 * "Follow the system font size" takes effect when it is toggled, not
	 * at the next start (sec 9.5).
	 */
	bssh_apply_interface_scale();

	apply_key_row_visibility();
	apply_menu_bar();

	const bssh_app_settings settings;
	const bool raise = settings.raise_contrast();
	const int brightness = settings.text_brightness();

	/*
	 * The forms -- and so the password prompt -- start on the keyboard the
	 * settings chose: BeerSSH's own unless the system's was picked. They
	 * started on the system's whatever the setting said, so a phone set to
	 * BeerSSH's own still brought up Android's for the password (the
	 * holder, 2026-10-22: "we should be consistent with the chosen
	 * keyboard"). Reset only when the SETTING changes, since this runs on
	 * every settings pass and the switch is still the user's for the run.
	 */
	if (int(settings.keyboard()) != m_forms_keyboard_setting) {
		m_forms_keyboard_setting = int(settings.keyboard());
		m_forms_use_softkeys = settings.keyboard() != bssh_app_settings::KEYBOARD_SYSTEM;
		if (m_form_keyboard) update_form_keyboard();
	}

	/*
	 * The custom palette, pushed into the catalog before anything reads a
	 * scheme out of it. term/ does not reach into the settings store --
	 * the dependency would run the wrong way -- so whoever owns the
	 * settings hands it over, and this is the one place that happens.
	 */
	bssh_colour_scheme_catalog::instance().set_custom(settings.custom_scheme());

	/*
	 * And the custom key row, for exactly the same reason and in the same
	 * place: softkeys does not read the settings store either.
	 *
	 * An empty spec is left alone rather than pushed, so the catalogue
	 * keeps the copy of the default it starts with. Pushing an empty row
	 * would be refused anyway -- it carries no Escape -- but relying on
	 * the refusal would mean the difference between "not composed" and
	 * "composed badly" showed up only as a return value nobody reads.
	 */
	const QString row_spec = settings.custom_key_row();
	if (!row_spec.isEmpty()) {
		sk_key_row_catalog::instance().set_custom(sk_parse_key_row(row_spec));
	}

	/*
	 * And the custom keyboard layout (sec 6.3), pushed the same way and
	 * for the same reason: softkeys does not read the settings store.
	 *
	 * The return is read. A layout that cannot type is refused by the
	 * catalogue and the reason comes back here -- which is the only
	 * place that can tell a user, since the catalogue has no way to say
	 * anything to anybody. Ignoring it would leave a person who wrote a
	 * broken layout with a chooser that silently does not list it.
	 */
	const QMap<QString, QString> layout_rows = settings.custom_keyboard_layout();
	if (!layout_rows.isEmpty()) {
		QString why;
		if (!sk_set_custom_layout(settings.custom_keyboard_layout_name(),
		                             sk_parse_layout_slots(layout_rows), &why)) {
			m_error = QStringLiteral("custom keyboard layout refused: %1").arg(why);
			if (m_terminal_view) m_terminal_view->flash_notice(QStringLiteral("Keyboard layout refused: %1").arg(why));
			update_status();
		}
	}

	/*
	 * Press-and-hold opens the menu wherever the touch layout is in use,
	 * which is where there is no right button to open it with. A pointer
	 * keeps press-and-hold meaning "start a selection and think about
	 * it", which is what it has always meant.
	 */
	const bool hold = context_menu_layout(settings.context_menu(),
	                                       bssh_platform::instance()
	                                               && bssh_platform::instance()
	                                                          ->is_touch_primary())
	        == bssh_app_settings::CONTEXT_MENU_TOUCH;

	/*
	 * sec 8.3's focus-follows-mouse, off unless asked for. Never where
	 * touch is the input: there is no pointer to hover, and a stray
	 * synthesised enter would move focus with nothing visible causing it.
	 */
	const bool follows = settings.focus_follows_mouse()
	        && bssh_platform::instance()
	        && !bssh_platform::instance()->is_touch_primary();

	/*
	 * Where the scrollbar goes (sec 8.3). AUTOMATIC is the right-hand
	 * side where there is a pointer to drag it with and nothing where
	 * there is not -- a thumb scrolls the terminal itself (sec 8.5), and
	 * a bar it would have to hit is a strip of screen a phone cannot
	 * spare.
	 */
	const bool touch = bssh_platform::instance()
	        && bssh_platform::instance()->is_touch_primary();

	bssh_terminal_view::scrollbar_position_t bar = bssh_terminal_view::SCROLLBAR_NONE;
	switch (settings.scrollbar()) {
	case bssh_app_settings::SCROLLBAR_RIGHT:
		bar = bssh_terminal_view::SCROLLBAR_RIGHT;
		break;
	case bssh_app_settings::SCROLLBAR_LEFT:
		bar = bssh_terminal_view::SCROLLBAR_LEFT;
		break;
	case bssh_app_settings::SCROLLBAR_HIDDEN:
		break;
	case bssh_app_settings::SCROLLBAR_AUTO:
		if (!touch) bar = bssh_terminal_view::SCROLLBAR_RIGHT;
		break;
	}

	/*
	 * Both of these are read once here and kept, for the reason
	 * update_status gives: they are consulted far too often to be a file
	 * read each time, and they change when somebody edits them -- which
	 * is exactly when this function runs.
	 */
	const bssh_app_settings::status_line_t status_line = settings.status_line();
	const bool status_line_changed = status_line != m_status_line;
	m_status_line = status_line;
	m_window_title_format = settings.window_title();
	update_window_title();

	/*
	 * Kept is not shown: the line takes the setting when it is next
	 * written, and nothing writes it on a settings edit -- so turning it
	 * off left it until a restart (the holder, 2026-10-30).
	 */
	if (status_line_changed) update_status();

	/*
	 * The scrollbar is a view's shape rather than one of its properties,
	 * so it is set as the DEFAULT and the panes are rebuilt once when it
	 * actually changes -- never poked into each live view.
	 *
	 * Poking it in place is what this did first, and it cannot be done
	 * safely from here: this function runs on panes_rebuilt, changing a
	 * view's width re-measures the grid, and a re-measure can cross the
	 * size class that decides how many panes fit (sec 8.2) -- which
	 * rebuilds the panes, which calls this again. That loop ran for five
	 * minutes and died on a stack overflow. Rebuilding only on a real
	 * change terminates, because the second pass finds the default
	 * already right.
	 */
	const bool bar_changed = bssh_terminal_view::default_scrollbar_position() != bar;
	bssh_terminal_view::set_default_scrollbar_position(bar);

	/*
	 * Ligatures are the font's shape (sec 9.2), so they ride the same
	 * default-and-rebuild path as the scrollbar: set the device default,
	 * and rebuild the panes once when it changes so every view's font is
	 * built afresh with the new value. Unlike the scrollbar this cannot
	 * feed back into the layout -- a ligature does not change a monospace
	 * cell's width -- but the mechanism is worth sharing rather than
	 * poking each live view's font by a second route.
	 */
	const bool ligatures = settings.font_ligatures();
	const bool ligatures_changed = bssh_terminal_view::default_font_ligatures() != ligatures;
	bssh_terminal_view::set_default_font_ligatures(ligatures);

	/*
	 * Whether the remote may drive the cursor shape (sec 6.4). Read live
	 * when a DECSCUSR arrives, so unlike the two above it needs no rebuild
	 * -- toggling it changes what the next shape change does, at once.
	 */
	bssh_terminal_view::set_honour_remote_cursor_shape(
	        settings.honour_remote_cursor_shape());

	if (m_panes) {
		for (bssh_terminal_view *view : m_panes->views()) {
			if (!view) continue;
			view->set_raise_contrast(raise);
			view->set_text_brightness(brightness);

			/*
			 * Re-read, not re-set: a view already showing "custom"
			 * has the id it wants, and only its CONTENTS changed.
			 */
			view->reload_colour_scheme();
			view->set_long_press_menu(hold);
			view->set_focus_follows_mouse(follows);
		}
	}

	/*
	 * sec 8.7's arrangement, which rides the same default-and-rebuild
	 * path: setting it changes the splitter's orientation and whether
	 * the panes live in a scroll area, and only a rebuild reaches both.
	 * Compared before assigning for the reason the scrollbar's comment
	 * gives at length -- rebuilding on every settings pass re-enters this
	 * function and does not terminate.
	 */
	const bool arrangement_changed = apply_arrangement();

	if ((bar_changed || ligatures_changed || arrangement_changed) && m_panes)
		m_panes->rebuild();

	/*
	 * Caps as Ctrl reaches every session's router, not just the focused
	 * one (sec 6.1). It is a fact about the keyboard in front of the
	 * user, so a setting that only took effect on the tab that happened
	 * to be open would be one they had to set again by switching tabs.
	 */
	const bool caps = settings.caps_as_ctrl();
	const qint64 images = qint64(settings.image_history_mb()) * 1024 * 1024;
	for (bssh_session *session : m_registry->sessions()) {
		if (session->input_router()) session->input_router()->set_caps_as_ctrl(caps);

		/* Picture history, per terminal (sec 7.1), for the same reason. */
		if (session->emulator()) session->emulator()->set_image_budget(images);
	}

	/*
	 * And the chords the user has rebound (sec 6.1). Process-wide rather
	 * than per-router, because the set this app keeps for itself is a
	 * fact about the keyboard rather than about a session -- and because
	 * "Keys this app keeps" has to show the same list the router
	 * matches against, from anywhere.
	 */
	QHash<QString, QString> bindings;
	for (const bssh_input_router::chord_t &chord : bssh_input_router::default_chords()) {
		const QString name = QString::fromLatin1(chord.name);

		/*
		 * Only what has actually been written. An absent key is the
		 * built-in binding, and an empty one is a chord deliberately
		 * given back to the shell -- two different things, and asked
		 * as two different questions rather than through whether a
		 * QString came back null.
		 */
		if (settings.has_chord(name)) bindings.insert(name, settings.chord(name));
	}
	bssh_input_router::set_bindings(bindings);

	/*
	 * And the general key remaps (sec 6.1), from the fixed slots into a
	 * from->to map, dropping any slot with an empty half. Process-wide for
	 * the same reason the chords are: a fact about the keyboard, not the
	 * session, so it reaches every router rather than the focused one.
	 */
	QHash<QString, QString> remaps;
	for (const QPair<QString, QString> &rule : settings.key_remaps()) {
		if (!rule.first.isEmpty() && !rule.second.isEmpty()) {
			remaps.insert(rule.first, rule.second);
		}
	}
	bssh_input_router::set_key_remaps(remaps);

	/*
	 * sec 11's FLAG_SECURE. Applied unconditionally rather than only
	 * where the platform can honour it: the desktop implementation is a
	 * no-op and says so, and a caller that had to check first would be
	 * the second place deciding what each platform can do.
	 */
	if (bssh_platform::instance()) {
		bssh_platform::instance()->set_hide_from_screenshots(settings.hide_from_screenshots());
	}
}

void bssh_main_window::apply_key_row_visibility() {
	if (!m_key_row) return;

	const bssh_app_settings settings;

	switch (settings.key_row_visibility()) {
	case bssh_app_settings::KEY_ROW_ALWAYS:
		m_key_row->set_visibility_mode(sk_key_row::ALWAYS);
		break;
	case bssh_app_settings::KEY_ROW_NEVER:
		m_key_row->set_visibility_mode(sk_key_row::NEVER);
		break;
	case bssh_app_settings::KEY_ROW_AUTO:
		m_key_row->set_visibility_mode(sk_key_row::AUTO);
		break;
	}

	apply_keyboard_choice();
}

void bssh_main_window::apply_keyboard_choice() {
	if (!m_keyboard || !m_key_row) return;

	const bssh_app_settings settings;
	/*
	 * Asked as "not the system one" rather than by naming a value: there
	 * are two built-in keyboards now (compact and full-size), and a test
	 * for one of them would leave the other drawn as nothing while the
	 * system panel stayed declined -- a phone with no keyboard at all.
	 * sec 14's M10 records the same shape for preset fields, where a setting
	 * reached the first consumer anybody thought of and not the second.
	 */
	const bool built_in = settings.keyboard() != bssh_app_settings::KEYBOARD_SYSTEM;

	/*
	 * The row's own visibility rule still decides whether IT shows -- the
	 * three-state setting, the platform, all of it. What this adds is
	 * that the built-in keyboard replaces it rather than joining it.
	 */
	apply_keyboard_style();

	/*
	 * Shown when the setting says so AND the user has not put it away.
	 * The hide key is theirs; this function runs on every session event
	 * and would otherwise overrule it moments later.
	 */
	m_keyboard->setVisible(built_in && !m_keyboard_hidden_by_user);
	if (built_in) m_key_row->hide();
	else m_key_row->setVisible(m_key_row->should_be_visible());

	/*
	 * And the system keyboard is asked not to come up, because the point
	 * of choosing ours is that theirs is not wanted -- two keyboards
	 * stacked on a phone leave no terminal.
	 *
	 * Qt shows an IME when a widget with WA_InputMethodEnabled takes
	 * focus, so clearing the attribute is what declines it. The terminal
	 * still receives inputMethodEvent from anything that delivers text
	 * without an IME, which on Android is where some physical keys arrive
	 * (sec 6.1) -- so this turns the panel off and not the path.
	 */
	/*
	 * EVERY pane's view, not only the focused one. Each view is built able
	 * to raise the system keyboard, and on the Fold, 2026-10-07, the panes
	 * beside the focused one kept that: opening a second and a third
	 * session brought Samsung's keyboard up under the built-in one, where
	 * it stayed. Focus passes through those views while the panes are
	 * rebuilt, and that was enough.
	 */
	if (m_panes) {
		for (bssh_terminal_view *view : m_panes->views()) {
			view->setAttribute(Qt::WA_InputMethodEnabled, !built_in);
		}
	}
	if (m_terminal_view) {
		m_terminal_view->setAttribute(Qt::WA_InputMethodEnabled, !built_in);
	}

	/*
	 * And one already up is put away. Clearing the attribute stops a view
	 * asking for the panel; it does not take down a panel that one asked
	 * for a moment ago, which is the state the Fold was left in. The
	 * desktop has nothing to hide, and the call is harmless there.
	 */
	if (built_in) QGuiApplication::inputMethod()->hide();
}

/*
 * Which keyboard is DRAWN: what the user chose, narrowed by what fits
 * (sec 6.2).
 *
 * Called from apply_device_settings, where the setting changes, and from
 * resizeEvent, where the room does -- because both halves of the answer
 * can move and a fallback applied only when the setting changes is one
 * that never fires on the device it was written for. A folding phone
 * changes width without anybody touching a setting.
 *
 * The stored setting is not rewritten. Unfolding gives the full keyboard
 * back, which is sec 8.2's rule for splits collapsing to tabs: the intent
 * is remembered rather than overwritten by the screen it last met.
 */
void bssh_main_window::apply_keyboard_style() {
	if (!m_keyboard) return;

	const bssh_app_settings settings;

	/*
	 * The LAYOUT first, because it rebuilds the catalog the style is
	 * then read out of. The other order draws a keyboard from the old
	 * letters and corrects it on the next resize, which on a desktop is
	 * never.
	 *
	 * An unknown id leaves the layout alone -- a setting written by a
	 * newer version, or by hand, should not quietly become a keyboard
	 * whose letters are somewhere else.
	 */
	if (sk_set_keyboard_layout(settings.keyboard_layout())) {
		m_keyboard->rebuild_keys();
	}

	sk_catalog::style_t wanted = sk_catalog::STYLE_COMPACT;
	if (settings.keyboard() == bssh_app_settings::KEYBOARD_BUILT_IN_FULL) {
		wanted = sk_catalog::STYLE_FULL;
	} else if (settings.keyboard() == bssh_app_settings::KEYBOARD_BUILT_IN_SPLIT) {
		wanted = sk_catalog::STYLE_SPLIT;
	}

	/*
	 * Each half 22% of the width unless a resize bar chose otherwise: 30%
	 * was tried first and, on the Fold's cover screen in landscape, left
	 * the terminal about 30 columns between halves the holder called way
	 * too big. 22% gives it about 54 and keys near the full board's 36dp
	 * floor. A chosen width is kept within what this window can spare.
	 */
	const int chosen_width = settings.keyboard_split_width();
	const int widest = qMax(120, width() * 2 / 5);
	m_keyboard->set_half_width(chosen_width > 0 ? qBound(120, chosen_width, widest)
	                                            : qBound(140, width() * 22 / 100, 260));
	m_keyboard->set_grips_shown(settings.keyboard_grips());

	const sk_catalog::style_t drawn =
	        sk_keyboard_style_for(wanted, width(), height());
	m_keyboard->set_style(drawn);
	m_keyboard->set_bottom_height(
	        settings.keyboard_height(drawn == sk_catalog::STYLE_FULL));
}

void bssh_main_window::on_pane_focus_changed(bssh_session *session) {
	m_terminal_view = m_panes ? m_panes->focused_view() : nullptr;

	/*
	 * Whether a view may raise the system keyboard is a property of the
	 * VIEW, and views are destroyed and rebuilt whenever the panes change
	 * -- so the choice is applied where the view changes and not only
	 * where the setting does.
	 *
	 * Belt and braces, and said plainly because it was nearly recorded as
	 * something stronger. The reasoning was that choosing the built-in
	 * keyboard at the front door, where m_terminal_view is null, would
	 * leave the next session's view carrying the attribute its
	 * constructor set. A test was written for exactly that order and
	 * PASSED with this line removed -- in both directions, once it was
	 * made to assert both -- so whatever else applies the choice on the
	 * way to a view, this is not the only thing doing it, and the fault
	 * could not be produced. The test was removed rather than kept: one
	 * that cannot fail is worse than none, because it reads as cover.
	 *
	 * The line stays because it makes the invariant hold at the moment
	 * the view changes rather than by whatever currently arranges it,
	 * which is cheap. It is NOT a fix for an observed defect and must not
	 * be cited as one.
	 */
	apply_keyboard_choice();

	if (!m_terminal_view) {
		m_current = nullptr;
		update_status();

		/* After the state changed, never before: this reads m_current. */
		refresh_menu_bar();
		return;
	}

	/*
	 * Wired once per VIEW, not once per focus change. The view is
	 * destroyed and rebuilt whenever the panes change -- that is what
	 * makes a pane cheap -- so its signals have to be reconnected each
	 * time one appears, while focus moving between two existing panes
	 * must not connect anything twice.
	 *
	 * Qt::UniqueConnection cannot do this job: it is rejected outright
	 * for lambdas ("unique connections require a pointer to member
	 * function"), so the connection is not made AT ALL rather than made
	 * once. That is how the link prompt and the primary-selection copy
	 * quietly stopped working when this moved out of the constructor --
	 * a warning on stderr and two features gone.
	 */
	if (m_terminal_view != m_wired_view) {
		m_wired_view = m_terminal_view;

		connect(m_terminal_view, &bssh_terminal_view::grid_resized,
		         this, &bssh_main_window::on_grid_resized);

		connect(m_terminal_view, &bssh_terminal_view::context_menu_requested,
		         this, &bssh_main_window::show_terminal_menu);

		/*
		 * Something dropped on this pane (sec 6.4). It comes through
		 * the same door as every other paste, which is the point: a
		 * dropped file's path is text on a command line about to be
		 * run, and dropped TEXT can carry newlines -- so it has to meet
		 * the same question about pasting more than one line, and the
		 * same bracketed-paste handling in the router.
		 */
		connect(m_terminal_view, &bssh_terminal_view::paste_requested,
		         this, [this](const QString &text) { paste_into_current(text); });

		/* Files, which may be copied rather than named (sec 6.4). */
		connect(m_terminal_view, &bssh_terminal_view::files_dropped, this, &bssh_main_window::offer_file_drop);

		/*
		 * Middle-click pastes the PRIMARY selection, not the clipboard.
		 * They are different buffers and conflating them is what makes
		 * middle-click paste surprising: it should deliver whatever was
		 * last selected, including in another program, and never what
		 * was last copied.
		 */
		connect(m_terminal_view, &bssh_terminal_view::primary_paste_requested, this, [this] {
			if (!m_current) return;

			QClipboard *clipboard = QGuiApplication::clipboard();
			if (!clipboard->supportsSelection()) return;

			const QString text = clipboard->text(QClipboard::Selection);
			paste_into_current(text);
		});

		/*
	 * Opening a URL the remote chose is a decision, so it is made here
	 * rather than in the widget, and it asks first. A terminal that opens
	 * whatever a compromised host puts on screen is a terminal being used
	 * as a delivery mechanism.
	 */
		connect(m_terminal_view, &bssh_terminal_view::link_activated,
		       this, &bssh_main_window::open_remote_link);

		connect(m_terminal_view, &bssh_terminal_view::selection_changed, this, [this] {
			/*
		 * Copy is greyed out with nothing selected, which is why this
		 * runs before the primary-selection work below and not inside
		 * its early returns.
		 */
			refresh_menu_bar();

			if (!QGuiApplication::clipboard()->supportsSelection()) return;

			const QString selected = m_terminal_view->selection_text();
			if (selected.isEmpty()) return;

			QGuiApplication::clipboard()->setText(selected, QClipboard::Selection);
		});
	}


	if (session && session != m_current) {
		/*
		 * On-screen is NOT set here. It follows the panes, not the
		 * focus, and the pane container is what knows the difference --
		 * with a split, the pane beside this one is on screen too.
		 */
		m_current = session;

		if (m_keyboard) m_keyboard->set_target(session->input_router());
		if (m_key_row) {
			m_key_row->set_target(session->input_router());
			m_key_row->set_layout(
			        session->settings().value(QStringLiteral("key_row_layout")).toString());
		}

		/*
		 * The bar no longer follows the focused pane, because a pane
		 * has no tab to select: the two are different states now
		 * (sec 8.3). What the bar has to do instead is DROP the entry
		 * for a session that has just become a pane, which
		 * refresh_tab_bar does from the pane list itself.
		 */
		refresh_tab_bar();
	}

	update_status();

	/* After the state changed, never before: this reads m_current. */
	refresh_menu_bar();
}

void bssh_main_window::detach_session() {
	/*
	 * Held BEFORE m_current is cleared, and that one line is the whole
	 * bug this function used to be.
	 *
	 * The panes own the views and attach them; letting go here is a
	 * matter of dropping the session from the layout and rebuilding, so
	 * that no view is left pointing into a session about to be freed
	 * (sec 3, core principle 2). That is what the code below intends and
	 * it is what it did not do: `forget(m_current)` ran AFTER
	 * `m_current = nullptr`, so it was always `forget(nullptr)`, and
	 * forget() returns immediately for a session its intent does not
	 * hold. It is the only caller of forget() in the tree, so forget()
	 * had never removed anything.
	 *
	 * What that cost: the dying session stayed in the layout intent, so
	 * rebuild() built a BRAND NEW view for it and attached the screen,
	 * scrollback, images, emulator and router of a session about to be
	 * deleted. One turn of the event loop later the deleteLater fired and
	 * the blink timer read a dangling screen. Closing the tab you are
	 * looking at, which is the ordinary case, because the close button is
	 * on it.
	 *
	 * NOT independently pinned by a test, and deliberately given no
	 * sabotage entry: the session-closing handler forgets the session
	 * unconditionally, so either fix alone covers a closing tab and
	 * breaking this one leaves the suite green. It is kept because it is
	 * the only thing covering a detach_session() reached for some other
	 * reason, and because forget(nullptr) is wrong whether or not
	 * anything currently notices.
	 */
	bssh_session *going = m_current;
	m_current = nullptr;

	if (m_layout) m_layout->forget(going);
	if (m_panes) m_panes->rebuild();

	m_terminal_view = m_panes ? m_panes->focused_view() : nullptr;
	if (m_key_row) m_key_row->set_target(nullptr);
	if (m_keyboard) m_keyboard->set_target(nullptr);
}

bool bssh_main_window::other_windows_exist() const {
	for (QWidget *widget : QApplication::topLevelWidgets()) {
		const bssh_main_window *other = qobject_cast<bssh_main_window *>(widget);
		if (other && other != this && !other->isHidden()) return true;
	}
	return false;
}

void bssh_main_window::take_session(bssh_session *session) {
	if (!session || m_shown.contains(session)) return;

	/*
	 * Out of whoever had it first. A session is shown in exactly one
	 * place (sec 8.4), so adopting is a MOVE rather than a copy.
	 *
	 * Announced through the registry rather than by reaching into the
	 * other window, because windows are meant to behave as though they
	 * were separate processes -- and emitted BEFORE this window takes it,
	 * so the announcement finds only windows that should let go.
	 *
	 * This used to emit changed(), which did nothing: that handler prunes
	 * sessions no longer in the REGISTRY, and a moved session is still in
	 * it. The session stayed visible in both windows, which tear_out_tab
	 * hid by removing it itself first and a drop would have exposed
	 * immediately.
	 */
	emit m_registry->session_taken(session);

	m_shown.append(session);
	on_registry_changed();
}

void bssh_main_window::handle_shortcut(bssh_input_router::shortcut_t shortcut) {
	if (!m_terminal_view) return;

	/*
	 * Pane navigation (sec 8.3). The chords were emitted by the
	 * router and the container knew how to move focus; nothing
	 * joined the two, so with four panes on screen the only way to
	 * change pane was the mouse.
	 */
	if (shortcut == bssh_input_router::SHORTCUT_FOCUS_PANE_LEFT && m_panes) {
		m_panes->focus_adjacent(-1);
		return;
	}
	if (shortcut == bssh_input_router::SHORTCUT_FOCUS_PANE_RIGHT && m_panes) {
		m_panes->focus_adjacent(1);
		return;
	}

	if (shortcut == bssh_input_router::SHORTCUT_COPY) {
		const QString selected = m_terminal_view->selection_text();
		if (!selected.isEmpty()) QGuiApplication::clipboard()->setText(selected);
	}
	if (shortcut == bssh_input_router::SHORTCUT_PASTE && m_current) {
		paste_into_current(QGuiApplication::clipboard()->text());
	}
	if (shortcut == bssh_input_router::SHORTCUT_SCROLL_PAGE_UP) {
		m_terminal_view->scroll_by(m_terminal_view->rows() / 2);
	}
	if (shortcut == bssh_input_router::SHORTCUT_SCROLL_PAGE_DOWN) {
		m_terminal_view->scroll_by(-m_terminal_view->rows() / 2);
	}

	/*
	 * Konsole's Ctrl+Shift+T and Ctrl+Shift+N (sec 6.1). What they
	 * open is what this device opens with -- a local shell on a
	 * desktop (sec 8.5) -- because that is what every terminal's new
	 * tab does and what makes the chord predictable. Duplicating the
	 * host you are on is a different intent and already has its own
	 * entry on the tab menu.
	 *
	 * Where the device does NOT open with a shell, the front door is
	 * the honest answer: a phone has nothing for a second shell to
	 * do, and offering the host list is what its Back button does.
	 */
	if (shortcut == bssh_input_router::SHORTCUT_NEW_TAB) open_another_session(false);
	if (shortcut == bssh_input_router::SHORTCUT_NEW_WINDOW) open_another_session(true);

	if (shortcut == bssh_input_router::SHORTCUT_FIND) open_find();

	/*
	 * Zoom (sec 9.5), on the FOCUSED session rather than on the view:
	 * m_current is what the chord handler above acts on throughout, and
	 * the session is where a zoom has to live to survive the pane being
	 * rebuilt.
	 */
	if (shortcut == bssh_input_router::SHORTCUT_ZOOM_IN) zoom_current(1);
	if (shortcut == bssh_input_router::SHORTCUT_ZOOM_OUT) zoom_current(-1);
	if (shortcut == bssh_input_router::SHORTCUT_ZOOM_RESET) reset_zoom_current();
}

void bssh_main_window::zoom_current(int steps) {
	if (!m_current) return;
	m_current->set_font_zoom(m_current->font_zoom() + steps);
}

void bssh_main_window::reset_zoom_current() {
	if (!m_current) return;
	m_current->set_font_zoom(0);
}

void bssh_main_window::reset_terminal_state() {
	/*
	 * The same call a reconnect makes (sec 5.3), for the same reason:
	 * the modes belong to programs that are no longer running. One
	 * implementation, so a mode the reconnect learns to clear is one
	 * this clears too.
	 */
	if (m_current && m_current->emulator()) m_current->emulator()->reset_modes();
}


void bssh_main_window::clear_scrollback() {
	/*
	 * The emulator's own method, which announces -- see its comment. The
	 * view re-clamps a scroll offset that now points past the end, and
	 * the accessibility layer rebuilds its flattened text, both off that
	 * one signal.
	 */
	if (m_current && m_current->emulator()) m_current->emulator()->clear_scrollback();
}

void bssh_main_window::open_find() {
	if (!m_find_bar) return;

	m_find_bar->show();
	m_find_bar->take_focus();

	/* Re-run the standing query against the pane now in focus. */
	run_find(m_find_bar->query());
}

void bssh_main_window::run_find(const QString &query) {
	if (bssh_terminal_view *view = m_panes ? m_panes->focused_view() : nullptr)
		view->find_text(query);
	update_find_status();
}

void bssh_main_window::step_find(int direction) {
	if (bssh_terminal_view *view = m_panes ? m_panes->focused_view() : nullptr)
		view->find_step(direction);
	update_find_status();
}

void bssh_main_window::close_find() {
	if (!m_find_bar) return;

	m_find_bar->hide();
	if (bssh_terminal_view *view = m_panes ? m_panes->focused_view() : nullptr) {
		view->clear_find();
		view->setFocus();
	}
}

void bssh_main_window::update_find_status() {
	if (!m_find_bar) return;

	bssh_terminal_view *view = m_panes ? m_panes->focused_view() : nullptr;
	m_find_bar->set_status(view ? view->current_find_index() : 0,
	                        view ? view->find_match_count() : 0);
}

void bssh_main_window::ask_for_password(bssh_session *session) {
	if (!session) return;

	/*
	 * One implementation, because two roads now arrive here: a host with
	 * no stored password, and one whose stored password the user declined
	 * or failed to unlock (sec 11). A second copy of this would be a
	 * second place to forget the remember-me offer.
	 */
	bssh_password_prompt dialog(session->profile().target(),
	                             session->profile().id != 0, this);

	/*
	 * Asked again because the server said no, and saying so: the same
	 * prompt with nothing different about it reads as the app having
	 * forgotten what was typed.
	 */
	if (session->password_refused()) {
		dialog.set_note(QStringLiteral("The server did not accept that password."));
	}

	/* The forms' choice of keyboard, and a switch made here is kept. */
	dialog.use_softkeys(m_forms_use_softkeys);

	/*
	 * The terminal's keyboard goes while the prompt is open: the prompt
	 * no longer fills the screen (2026-10-22), and with this one showing
	 * below it, the prompt's own keyboard read as a second, smaller one.
	 * Put back by the window's own rule afterwards.
	 */
	if (m_keyboard) m_keyboard->hide();

	m_remember_once_in.remove(session);
	const int answer = dialog.exec();
	apply_key_row_visibility();
	if (m_form_keyboard) {
		m_forms_use_softkeys = dialog.uses_softkeys();
		update_form_keyboard();
	}
	if (answer != QDialog::Accepted) {
		session->disconnect_from_host();
		return;
	}

	const QString password = dialog.password();

	/*
	 * Kept for saving BEFORE it is offered: a fake or a fast server can
	 * answer inside supply_password(), and the answer is what saves it.
	 */
	if (dialog.remember()) m_remember_once_in.insert(session, password);
	session->supply_password(password);
}

void bssh_main_window::wire_session(bssh_session *session) {
	if (!session) return;

	/*
	 * Once per session per window. A session reaches a window by two
	 * roads -- created in it, or torn out of another one and adopted --
	 * and only the first used to wire anything, so every per-session
	 * signal in a torn-out tab was still pointing at the window it came
	 * from. The worst of it was the shortcut router: Ctrl+Shift+V in the
	 * new window pasted into the OLD window's session, because
	 * handle_shortcut acts on m_terminal_view and has no idea which
	 * window asked.
	 *
	 * Guarded by a set rather than by Qt::UniqueConnection, which does
	 * not work with a lambda and has already cost this project a feature
	 * twice (sec 6.4). Entries are dropped when a session closes, so a
	 * later session landing on a recycled address is still wired.
	 */
	if (m_wired_sessions.contains(session)) return;
	m_wired_sessions.insert(session);

	/*
	 * The prompts and the trust decisions, moved here from
	 * prepare_profile() so that BOTH roads into a window make them.
	 *
	 * They used to be connected only where a session was created, with
	 * that window as the context object -- so a session torn out into a
	 * second window went on asking the FIRST one. The password dialog
	 * opened behind the window the user was looking at, and the session
	 * sat in AUTHENTICATING until they found it. Worse once that window
	 * had closed: Qt drops a connection when its context object dies, so
	 * the session emitted needs_password into nothing and waited for a
	 * credential nobody would ever be asked for. Same for a changed host
	 * key, which is a warning that simply never appeared.
	 *
	 * Every one of them is guarded on m_shown, and that guard is what
	 * makes this safe HERE: wire_session runs off the registry's
	 * session_added, so every window wires every session, and an
	 * unguarded prompt would raise one dialog per window (sec 8.4). The
	 * guard is read when the signal arrives rather than when it is
	 * connected, which is what lets the answer follow the session as it
	 * moves.
	 *
	 * failed, shell_exited and state_changed guard on m_current instead:
	 * they write the status line, which is about the focused session and
	 * not merely a shown one.
	 */
	/*
	 * Only while this is the session the line is about.
	 *
	 * update_status() composes ONE sentence -- the current session's
	 * target and state, then m_error appended after a dash -- so an error
	 * set by any other session is printed as though it described this
	 * one. Measured: a background tab dropping put
	 * "prod.example disconnected -- connection reset" on screen, naming
	 * the host that was fine and describing the one that was not.
	 *
	 * Both siblings below already guard this way -- shell_exited returns
	 * unless the session is current, state_changed acts only when it is.
	 * This one could not, being a slot that takes the reason and not the
	 * session, which is how it became the exception.
	 *
	 * And nothing cleaned it up afterwards. The reconnect message that
	 * would have replaced it is guarded the same way and returns for a
	 * session that is not current, so the wrong text stayed on the line
	 * until something else happened to set one.
	 *
	 * A background tab is not silenced by this: sec 8.3's per-tab dot is
	 * where a session nobody is looking at reports itself, and
	 * bssh_tab_status::state_for() already answers DISCONNECTED for it.
	 */
	connect(session, &bssh_session::failed, this,
	         [this, session](const QString &reason) {
		if (!session->profile().local)
			set_session_note(session, reason.isEmpty() ? QStringLiteral("Connection failed")
			                                           : QStringLiteral("Connection failed: ") + reason);
		if (session != m_current) return;
		on_failed(reason);
	});
	connect(session, &QObject::destroyed, this, [this, session] {
		m_notes.remove(session);
		m_ended.remove(session);
	});

	/*
	 * First contact with a host. The fingerprint is shown because it is
	 * the only thing the user can actually check -- against what the
	 * server's owner published, or against `ssh-keyscan` from somewhere
	 * they already trust.
	 */
	connect(session, &bssh_session::host_key_unknown, this, [this, session](const QString &fp) {
		/* Only the window showing it may ask; see wire_session. */
		if (!m_shown.contains(session)) return;
		QMessageBox box(this);
		box.setIcon(QMessageBox::Question);
		box.setWindowTitle(QStringLiteral("Unknown host key"));
		box.setText(QStringLiteral("The host %1 is not in BeerSSH's known-hosts store.")
		                    .arg(session->profile().target()));
		box.setInformativeText(QStringLiteral(
		        "Key fingerprint:\n%1\n\nCheck it against a source you already trust "
		        "before accepting. Accepting records it for next time.").arg(fp));

		QPushButton *accept = box.addButton(QStringLiteral("Accept and connect"),
		                                     QMessageBox::AcceptRole);
		box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);

		/*
		 * Cancel is the default, so leaning on Enter does not trust a
		 * host nobody looked at.
		 */
		box.setDefaultButton(QMessageBox::Cancel);
		box.exec();

		if (box.clickedButton() == static_cast<QAbstractButton *>(accept)) {
			session->accept_host_key();
		} else {
			session->disconnect_from_host();
		}
	});

	/*
	 * A changed key is a different question wearing the same clothes, and
	 * the dialog says so: this is what a man-in-the-middle looks like, and
	 * it is also what a reinstalled server looks like. Only a person can
	 * tell those apart, which is why it is asked rather than decided.
	 */
	connect(session, &bssh_session::host_key_changed, this, [this, session](const QString &fp) {
		/* Only the window showing it may ask; see wire_session. */
		if (!m_shown.contains(session)) return;
		QMessageBox box(this);
		box.setIcon(QMessageBox::Warning);
		box.setWindowTitle(QStringLiteral("HOST KEY CHANGED"));
		box.setText(QStringLiteral("The key for %1 is NOT the one recorded.")
		                    .arg(session->profile().target()));
		box.setInformativeText(QStringLiteral(
		        "New fingerprint:\n%1\n\nThis happens when a server is rebuilt -- and it "
		        "is also exactly what an interception attack looks like. Do not accept "
		        "unless you know why the key changed.").arg(fp));

		QPushButton *accept = box.addButton(QStringLiteral("Accept the new key"),
		                                     QMessageBox::DestructiveRole);
		box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
		box.setDefaultButton(QMessageBox::Cancel);
		box.exec();

		if (box.clickedButton() == static_cast<QAbstractButton *>(accept)) {
			session->accept_host_key();
		} else {
			session->disconnect_from_host();
		}
	});

	/*
	 * Revoked: told, not asked (sec 11). Somebody deliberately marked
	 * this key as no longer trusted, and "shall I trust it anyway?" is
	 * not a question this app gets to put to the user -- so there is no
	 * accept button here, unlike the changed-key dialog above. The
	 * connection is dropped.
	 */
	connect(session, &bssh_session::host_key_revoked, this, [this, session](const QString &fp) {
		/* Only the window showing it may ask; see wire_session. */
		if (!m_shown.contains(session)) return;
		QMessageBox box(this);
		box.setIcon(QMessageBox::Critical);
		box.setWindowTitle(QStringLiteral("HOST KEY REVOKED"));
		box.setText(QStringLiteral("The key for %1 has been revoked.")
		                    .arg(session->profile().target()));
		box.setInformativeText(QStringLiteral(
		        "Fingerprint:\n%1\n\nThis key is marked @revoked in a known_hosts file, "
		        "which means someone recorded that it must no longer be used. BeerSSH "
		        "will not connect with it.").arg(fp));
		box.addButton(QStringLiteral("Close"), QMessageBox::RejectRole);
		box.exec();

		session->disconnect_from_host();
	});

	connect(session, &bssh_session::needs_key_passphrase, this, [this, session] {
		/* Only the window showing it may ask; see wire_session. */
		if (!m_shown.contains(session)) return;
		bool ok = false;
		const QString passphrase = QInputDialog::getText(
		        this, QStringLiteral("BeerSSH"),
		        QStringLiteral("Passphrase for your SSH key:"),
		        QLineEdit::Password, QString(), &ok);

		/*
		 * Declining is not a cancel: the server may well take a
		 * password, so an empty answer falls through to that path rather
		 * than dropping the connection.
		 */
		session->supply_key_passphrase(ok ? passphrase : QString());
	});

	connect(session, &bssh_session::needs_password, this, [this, session] {
		/* Only the window showing it may ask; see wire_session. */
		if (!m_shown.contains(session)) return;
		/*
		 * A password already kept for this host is used without asking,
		 * which is the entire point of storing one -- and is what makes
		 * auto-reconnect work for password-authenticated hosts without
		 * holding the credential in memory all session (sec 5.3, 11).
		 */
		/*
		 * Not one the server has refused (sec 11): a stored password
		 * that has stopped working is asked about, not offered again --
		 * offering it again is the loop that gets an address blackholed.
		 */
		const QString stored = session->password_refused()
		                               ? QString()
		                               : stored_password_for(session->profile());
		if (!stored.isEmpty() && !session->was_refused(stored)) {
			/*
			 * Unless this host asks the user to prove it is them first
			 * (sec 11). Per host and off by default, because the
			 * question it was blocked on -- a reconnect at 3am with
			 * nobody holding the phone -- has no single answer: right
			 * for the box running tmux, wrong for the production
			 * database, and they are not the same host.
			 *
			 * Gated on the CAPABILITY as well as the flag, so a
			 * profile carrying the flag onto a machine that cannot ask
			 * does not lock its own password away for good. It falls
			 * through to the typed prompt in that case, which is the
			 * same road a failed or cancelled prompt takes: biometric
			 * protects the STORED password, not the account, and
			 * somebody who knows it can always type it.
			 */
			const bssh_platform *platform = bssh_platform::instance();
			const bool ask_first = session->profile().require_biometric
			        && platform && platform->has_biometric();

			if (!ask_first) {
				session->supply_password(stored);
				return;
			}

			QPointer<bssh_session> alive(session);
			const_cast<bssh_platform *>(platform)->authenticate(
			        QStringLiteral("Unlock the password for %1")
			                .arg(session->profile().target()),
			        [this, alive, stored](bool ok) {
				/* The session can be gone by the time a system
				 * dialog is answered -- the tab closed, the app
				 * backgrounded and killed. */
				if (!alive) return;

				if (ok) {
					alive->supply_password(stored);
					return;
				}

				/* Refused or cancelled: ask for it the ordinary
				 * way rather than leaving the session waiting on
				 * nothing. */
				ask_for_password(alive);
			});
			return;
		}

		ask_for_password(session);
	});

	/*
	 * What the status line says about an ending. Only worth anything
	 * under --hold, which is the one case where the pane is still there
	 * to read it -- the registry closes the rest (sec 8.6).
	 */
	connect(session, &bssh_session::shell_exited, this, [this, session](int status) {
		/* Over the pane, which stays for a held shell or a remote one. */
		m_ended.insert(session);
		set_session_note(session, status == 0 ? QStringLiteral("Session ended")
		                                      : QStringLiteral("Session ended (exit %1)").arg(status));
		if (session != m_current) return;

		m_error = status == 0
		                  ? QStringLiteral("session ended")
		                  : QStringLiteral("session ended (exit %1)").arg(status);
		update_status();
	});
	connect(session, &bssh_session::state_changed, this, [this, session] {
		/*
		 * The note over the pane (sec 8.7a): nothing while connected, and
		 * nothing for a local shell, whose connection is not a thing that
		 * can go wrong; an ended session keeps saying so until it is live.
		 */
		const bssh_ssh_session::state_t state = session->state();
		if (state == bssh_ssh_session::CONNECTED) m_ended.remove(session);
		if (!m_ended.contains(session)) {
			QString note;
			if (!session->profile().local) {
				switch (state) {
				case bssh_ssh_session::RESOLVING:
				case bssh_ssh_session::CONNECTING:     note = QStringLiteral("Connecting\u2026"); break;
				case bssh_ssh_session::AUTHENTICATING: note = QStringLiteral("Signing in\u2026"); break;
				case bssh_ssh_session::DISCONNECTED:   note = QStringLiteral("Disconnected"); break;
				case bssh_ssh_session::FAILED:         note = QStringLiteral("Connection failed"); break;
				default:                               break;
				}
			}
			if (state != bssh_ssh_session::FAILED || m_notes.value(session).isEmpty())
				set_session_note(session, note);
		}

		/* A successful (re)connect clears whatever the last message was. */
		if (session == m_current && session->is_live()) {
			m_error.clear();
			update_status();
		}

		/* In, so the password asked to be remembered is one that works. */
		if (session->state() == bssh_ssh_session::CONNECTED
		    && m_remember_once_in.contains(session)) {
			offer_to_remember(session, m_remember_once_in.take(session));
		}
	});

	/*
	 * The device settings apply to a session opened AFTER they were
	 * read, too. apply_device_settings() walks the registry, so a
	 * session created later would otherwise start with the router's
	 * own defaults and quietly ignore the choice (sec 6.1).
	 */
	if (session->input_router()) {
		session->input_router()->set_caps_as_ctrl(bssh_app_settings().caps_as_ctrl());
	}

	/*
	 * And the zoom goes back to the host (sec 9.5), so the next session
	 * of it opens where this one was left. After a pause rather than on
	 * every step: a pinch is a stream of steps, and each write is the
	 * whole hosts file and a reload of every list that shows one.
	 *
	 * Read fresh from the store and only the zoom changed, because the
	 * session's copy of its profile may be older than an edit made
	 * since. A session with no profile behind it -- a local shell, a
	 * command-line target -- has nowhere to keep it.
	 */
	if (session->profile().id != 0) {
		QTimer *remember = new QTimer(session);
		remember->setSingleShot(true);
		remember->setInterval(800);
		connect(session, &bssh_session::font_zoom_changed, remember,
		         QOverload<>::of(&QTimer::start));
		connect(remember, &QTimer::timeout, this, [this, session] {
			bssh_profile_t stored = m_profile_store->profile(session->profile().id);
			if (!stored.is_valid() || stored.font_zoom == session->font_zoom()) return;
			stored.font_zoom = session->font_zoom();
			m_profile_store->save(stored);
		});
	}

	/*
	 * Reconnection is only real to the user if they can see it. A
	 * terminal that has silently stopped receiving output looks
	 * identical to one whose remote is merely quiet (sec 5.3).
	 */
	if (bssh_conn_manager *manager = m_registry->conn_manager_for(session)) {
		connect(manager, &bssh_conn_manager::reconnect_scheduled,
		         this, [this, session](int delay_ms, int attempt) {
			set_session_note(session, QStringLiteral("Reconnecting in %1 s (attempt %2)")
			                                  .arg((delay_ms + 999) / 1000).arg(attempt));
			if (session != m_current) return;
			m_error = QStringLiteral("reconnecting in %1s (attempt %2)")
			                  .arg((delay_ms + 999) / 1000)
			                  .arg(attempt);
			update_status();
		});
		connect(manager, &bssh_conn_manager::reconnect_started, this, [this, session](int) {
			set_session_note(session, QStringLiteral("Reconnecting\u2026"));
			if (session != m_current) return;
			m_error = QStringLiteral("reconnecting...");
			update_status();
		});
		connect(manager, &bssh_conn_manager::gave_up, this, [this, session] {
			set_session_note(session, QStringLiteral("Gave up reconnecting"));
			if (session != m_current) return;
			m_error = QStringLiteral("gave up reconnecting -- the shell is gone, "
			                          "not suspended");
			update_status();
		});
	}

	/*
	 * Only the window whose pane the chord was typed into.
	 *
	 * Every other handler wired here guards on m_shown or m_current, and
	 * has to: this runs off the registry's session_added and the registry
	 * is process-wide (sec 8.4), so EVERY window wires every session. A
	 * second listener that returns immediately costs nothing, which is
	 * why the arrangement is right.
	 *
	 * This one had no guard, and the comment at the top of this function
	 * names why that is not survivable -- handle_shortcut acts on
	 * m_terminal_view, m_current and m_panes, all of them THIS window's,
	 * and has no idea which window asked. With two windows open, a paste
	 * typed in one was therefore also pasted into whatever the other
	 * happened to be showing: the text lands on a host the user was not
	 * aiming at, and a pasted line ending in a newline runs there. The
	 * earlier fix moved the wiring so a torn-out tab reached its NEW
	 * window; it did not stop the chord reaching every other one.
	 *
	 * m_current rather than m_shown, because handle_shortcut acts on the
	 * FOCUSED pane -- a background tab in this window is not where the
	 * keys went either, and m_current follows pane focus
	 * (on_pane_focus_changed).
	 */
	connect(session->input_router(), &bssh_input_router::shortcut_pressed,
	         this, [this, session](bssh_input_router::shortcut_t shortcut) {
		if (m_current != session) return;
		handle_shortcut(shortcut);
	});

	/*
	 * OSC 52 (sec 7.1): the remote asking to set the clipboard.
	 * Honoured only where the host's preset says so -- off by
	 * default -- and never silently: the status line says which host
	 * did it, because a clipboard that changed without the user
	 * acting is the whole hazard, and the useful case (yanking in
	 * vim over ssh) is one they just caused deliberately.
	 *
	 * Only the window SHOWING that session reacts. A session is on
	 * screen in exactly one place (sec 8.4), so this is what makes
	 * one remote request produce one clipboard write and one line of
	 * status, rather than one per window that has ever shown it.
	 */
	connect(session->emulator(), &bssh_emulator::clipboard_write_requested,
	         this, [this, session](const QString &text) {
		if (!m_shown.contains(session)) return;
		if (!session->settings()
		              .value(QStringLiteral("allow_clipboard_write")).toBool()) {
			return;
		}

		QGuiApplication::clipboard()->setText(text);

		m_error = QStringLiteral("%1 set the clipboard (%2 characters)")
		                  .arg(session->title())
		                  .arg(text.size());
		/* Told over the pane as well, the status line being hidden by default. */
		flash_session_notice(session, QStringLiteral("Set the clipboard (%1 characters)").arg(text.size()));
		update_status();
	});

	/* The settings a host resolves to, reaching the pane on screen. */
	connect(session, &bssh_session::settings_changed, this, [this, session] {
		if (m_current != session || !m_terminal_view) return;

		bssh_apply_appearance(m_terminal_view, session->settings());
		if (m_key_row) {
			m_key_row->set_layout(session->settings()
			                              .value(QStringLiteral("key_row_layout"))
			                              .toString());
		}
		session->emulator()->set_cell_pixels(m_terminal_view->cell_width(),
		                                      m_terminal_view->cell_height());
		session->resize(m_terminal_view->cols(), m_terminal_view->rows());
		update_status();
	});

	/* OSC title also drives the window title on desktop (sec 8.3). */
	connect(session, &bssh_session::title_changed, this, [this, session] {
		if (session == m_current) update_window_title();
	});
}
void bssh_main_window::adopt_session(bssh_session *session) {
	if (!session || m_shown.contains(session)) return;

	/*
	 * Wired before it is shown: show_session() retargets things that
	 * assume the session already has its own connections.
	 */
	wire_session(session);

	take_session(session);
	show_session(session);
}

int bssh_main_window::drop_zone_for(bssh_session *session, bssh_session *target,
                                     int edge) const {
	if (!session) return BSSH_DROP_REFUSED;
	if (!target || !m_layout || !m_layout->intent().contains(target)) return BSSH_DROP_CANCEL;

	/*
	 * A pane's front on its OWN pane (sec 8.3's bar per pane): with tabs
	 * behind it an edge splits it off into a pane of its own, the next
	 * tab taking the front; the middle, or a pane of one, is a cancel.
	 */
	const bool own = session == target;
	if (own && m_layout->tabs(session).size() < 2) return BSSH_DROP_CANCEL;

	typedef bssh_layout_manager layout;

	/*
	 * A band along a side (sec 8.7a): a tab of this pane, with the tab bar
	 * put on that side -- where the pane shows no bar for the drop to land
	 * on, side by side, and the dropped session is not already one of its
	 * tabs. Anywhere else the band is just part of the edge it lies on.
	 */
	if (bssh_is_tab_drop(edge)) {
		const bool bar_shown = !m_tabs_hidden && m_layout->bar_tabs(target).size() > 1;
		if (!own && !bar_shown && m_layout->arrangement() == layout::TILED
		    && !m_layout->tabs(target).contains(session))
			return edge;
		edge -= BSSH_DROP_TAB_BASE;
	}

	if (edge < layout::EDGE_LEFT || edge > layout::EDGE_MIDDLE) return BSSH_DROP_CANCEL;
	if (edge == layout::EDGE_MIDDLE) return own ? BSSH_DROP_CANCEL : edge;

	/*
	 * A side that cannot be honoured is the swap instead, drawn and done
	 * as one (the holder's rule, 2026-10-09) -- or, for a pane's own
	 * front, the cancel its own middle is. A new pane in a window at its
	 * pane cap would make a pane there is no room for, and a pane moved
	 * where its new place does not fit would push another out into a tab
	 * (room_to_split); and the portrait
	 * column stacks its panes whatever the tree says, so left and right
	 * have no meaning there -- they used to be drawn as halves and land
	 * as above and below.
	 */
	const int folded = own ? BSSH_DROP_CANCEL : int(layout::EDGE_MIDDLE);
	const bool new_pane = own || !m_layout->intent().contains(session);
	if (!m_layout->room_to_split(target, edge, new_pane ? nullptr : session)) return folded;
	if (m_layout->arrangement() == layout::COLUMN
	    && (edge == layout::EDGE_LEFT || edge == layout::EDGE_RIGHT))
		return folded;

	return edge;
}

bssh_main_window *bssh_main_window::window_showing(bssh_session *session) {
	for (QWidget *widget : QApplication::topLevelWidgets()) {
		bssh_main_window *window = qobject_cast<bssh_main_window *>(widget);
		if (window && window->m_shown.contains(session)) return window;
	}
	return nullptr;
}

void bssh_main_window::exchange_session(bssh_session *out, bssh_session *in) {
	const int at = m_shown.indexOf(out);
	if (at < 0 || !in || !m_layout || !m_panes) return;

	/*
	 * `in` takes `out`'s place exactly: the same leaf of the tree if `out`
	 * was a pane, the same position among the tabs if it was one. Done in
	 * place rather than by taking and letting go, because a window that
	 * loses a session closes up the space it had, and the space is the
	 * whole point of a swap.
	 */
	wire_session(in);
	if (m_layout->intent().contains(out)) {
		m_layout->place(in, out, bssh_layout_manager::EDGE_MIDDLE);
	} else if (bssh_session *front = m_layout->front_of(out)) {
		/*
		 * A tab behind its pane's front -- one dragged out from behind
		 * can be the side that leaves. Only the fronts were exchanged,
		 * so the arrival was listed here and in no bar, and the one that
		 * left stayed in this window's stack while another showed it.
		 */
		m_layout->add_tab(in, out, m_layout->tabs(out).indexOf(out));
		m_layout->bring_to_front(front);
		m_layout->forget(out);
	}
	m_shown[at] = in;

	/*
	 * After the place, not before: detaching forgets the session in the
	 * layout, which beforehand took `out`'s leaf with it and left `in`
	 * nothing to step into -- the swap landed `in` in this window's list
	 * with no pane, measured. Now `out` has already left the layout and
	 * the forget finds nothing.
	 */
	if (m_current == out) detach_session();

	m_panes->rebuild();
	if (m_layout->intent().contains(in)) m_panes->focus_session(in);
	refresh_tab_bar();
	update_status();
}

void bssh_main_window::swap_sessions(bssh_session *session, bssh_session *target) {
	bssh_main_window *source = window_showing(session);

	if (source == this) {
		/*
		 * Both here: a pane with a pane trades leaves, a tab with a pane
		 * trades places -- the tab shown where the pane was, the pane
		 * listed where the tab was.
		 */
		const int a = m_shown.indexOf(session);
		const int b = m_shown.indexOf(target);
		if (a >= 0 && b >= 0) m_shown.swapItemsAt(a, b);
		m_layout->place(session, target, bssh_layout_manager::EDGE_MIDDLE);
		m_panes->focus_session(session);
		refresh_tab_bar();
		update_status();
		return;
	}

	if (source) {
		/*
		 * From another window: the two trade windows and places. The
		 * target goes to the slot the arrival left -- a pane where it was
		 * a pane, a tab where it was a tab -- rather than becoming a tab
		 * here, which made a tab out of a drop aimed at a terminal.
		 */
		source->exchange_session(session, target);
		exchange_session(target, session);
		return;
	}

	/*
	 * A session no window shows -- nothing a drag produces, since every
	 * drag starts in a window, but the model is public. It takes the
	 * target's place, and the target, with nowhere to go back to, stays
	 * here as a tab: leaving the panes is never closing (sec 8.2).
	 */
	take_session(session);
	m_layout->place(session, target, bssh_layout_manager::EDGE_MIDDLE);
	m_panes->focus_session(session);
	refresh_tab_bar();
	update_status();
}

int bssh_main_window::arrival_zone_for(bssh_session *target, int edge) const {
	typedef bssh_layout_manager layout;
	if (!target || !m_layout || !m_layout->intent().contains(target)) return BSSH_DROP_CANCEL;

	/* drop_zone_for()'s rules for a session that is no tab here and no
	 * pane: a new arrival, every time. */
	if (bssh_is_tab_drop(edge)) {
		const bool shows_a_bar = !m_tabs_hidden && m_layout->bar_tabs(target).size() > 1;
		if (!shows_a_bar && m_layout->arrangement() == layout::TILED) return edge;
		edge -= BSSH_DROP_TAB_BASE;
	}
	if (edge < layout::EDGE_LEFT || edge > layout::EDGE_MIDDLE) return BSSH_DROP_CANCEL;
	if (edge == layout::EDGE_MIDDLE) return edge;
	if (!m_layout->room_to_split(target, edge)) return layout::EDGE_MIDDLE;
	if (m_layout->arrangement() == layout::COLUMN
	    && (edge == layout::EDGE_LEFT || edge == layout::EDGE_RIGHT))
		return layout::EDGE_MIDDLE;
	return edge;
}

void bssh_main_window::drop_session_on(bssh_session *session, bssh_session *target,
                                        int edge) {
	if (!session || !target || !m_layout || !m_panes) return;

	const int plan = drop_zone_for(session, target, edge);
	if (plan < 0) return;

	/*
	 * A band: the bar goes on that side, for every pane as the setting
	 * says, and the session joins the target's pane as a tab there.
	 */
	if (bssh_is_tab_drop(plan)) {
		bssh_app_settings::tab_position_t side = bssh_app_settings::TABS_TOP;
		switch (plan - BSSH_DROP_TAB_BASE) {
		case bssh_layout_manager::EDGE_LEFT:   side = bssh_app_settings::TABS_LEFT; break;
		case bssh_layout_manager::EDGE_RIGHT:  side = bssh_app_settings::TABS_RIGHT; break;
		case bssh_layout_manager::EDGE_BOTTOM: side = bssh_app_settings::TABS_BOTTOM; break;
		default:                               break;
		}
		bssh_app_settings().set_tab_position(side);
		apply_tab_position(int(side));
		drop_session_as_tab(session, target, INT_MAX);
		return;
	}

	const auto where = bssh_layout_manager::drop_edge_t(plan);
	if (where == bssh_layout_manager::EDGE_MIDDLE) {
		swap_sessions(session, target);
		return;
	}

	if (!m_layout->intent().contains(session)) take_session(session);

	/* Off its own pane: the tab beside it fronts the pane, and is the target. */
	if (session == target) {
		const QList<bssh_session *> tabs = m_layout->tabs(session);
		const int at = tabs.indexOf(session);
		bssh_session *other = tabs.value(at + 1 < tabs.size() ? at + 1 : at - 1);
		if (!other) return;
		m_layout->bring_to_front(other);
		target = other;
	}

	if (!m_layout->place(session, target, where)) return;

	/*
	 * In a column m_shown is the order of record (see move_pane), so the
	 * new order is written back there too: the panes' slots in m_shown
	 * take the intent's order, and the tabs around them stay put.
	 */
	if (m_layout->arrangement() == bssh_layout_manager::COLUMN) {
		const QList<bssh_session *> order = m_layout->intent();
		int next = 0;
		for (int i = 0; i < m_shown.size() && next < order.size(); ++i) {
			if (order.contains(m_shown.at(i))) m_shown[i] = order.at(next++);
		}
	}

	/*
	 * Focus goes with the pane that moved: the user just put it there,
	 * and it is the one they are about to type in.
	 */
	m_panes->focus_session(session);

	refresh_tab_bar();
	update_status();
}

void bssh_main_window::drop_session_as_tab(bssh_session *session, bssh_session *front,
                                            int slot) {
	if (!session || !m_layout || !m_panes) return;

	/* From another window: this one takes it first, then places it. */
	if (!m_shown.contains(session)) take_session(session);

	/* A window showing nothing, or a pane no longer here: simply shown. */
	if (!front || !m_layout->sessions().contains(front) || front == session) {
		show_session(session);
		return;
	}

	m_layout->add_tab(session, front, slot < 0 ? INT_MAX : slot);
	m_panes->focus_session(session);
	refresh_tab_bar();
	update_status();
}

int bssh_main_window::tab_drop_slot(bssh_session *session, bssh_session *front,
                                    const QPoint &at, int *mark) const {
	if (mark) *mark = -1;
	/* No session: one arriving from another process (preview_arrival). */
	if (!front || !m_layout || !m_panes) return -1;

	/* A pane's only tab dropped on its own bar or strip: nothing to do. */
	const QList<bssh_session *> tabs = m_layout->bar_tabs(front);
	if (session && tabs == QList<bssh_session *>{ session }) return -1;

	/* No bar: the strip is the one slot, after the pane's own, and lights whole. */
	bssh_pane_bar *row = bar_for(front);
	if (!row || row->isHidden()) return int(tabs.size());

	QWidget *overlay = tab_drop_target(front);
	QTabBar *bar = row->tab_bar();
	if (!overlay || !bar) return -1;

	/*
	 * Before the first tab whose middle is past the pointer, along the
	 * bar's own direction; after the last otherwise. The mark sits on the
	 * boundary the tab would take, in the overlay's coordinates. Through
	 * global coordinates: neither widget is the other's ancestor, which
	 * mapFrom() requires.
	 */
	const bool vertical = bar->shape() == QTabBar::RoundedWest
	                      || bar->shape() == QTabBar::RoundedEast;
	const QPoint in_bar = bar->mapFromGlobal(overlay->mapToGlobal(at));
	const int count = bar->count();
	int slot = count;
	for (int i = 0; i < count; ++i) {
		const QRect tab = bar->tabRect(i);
		if (vertical ? in_bar.y() < tab.center().y() : in_bar.x() < tab.center().x()) {
			slot = i;
			break;
		}
	}

	if (mark) {
		QPoint edge(0, 0);
		if (slot < count) {
			edge = bar->tabRect(slot).topLeft();
		} else if (count > 0) {
			const QRect last = bar->tabRect(count - 1);
			edge = vertical ? last.bottomLeft() + QPoint(0, 1) : last.topRight() + QPoint(1, 0);
		}
		const QPoint in_overlay = overlay->mapFromGlobal(bar->mapToGlobal(edge));
		*mark = vertical ? in_overlay.y() : in_overlay.x();
	}
	return slot;
}

QWidget *bssh_main_window::tab_drop_target(bssh_session *front) const {
	for (const QPointer<bssh_tab_drop> &overlay : m_tab_drops) {
		if (overlay && overlay->front == front) return overlay;
	}
	return nullptr;
}

QList<bssh_pane_bar *> bssh_main_window::pane_bars() const {
	QList<bssh_pane_bar *> bars;
	if (!m_panes || !m_layout) return bars;
	for (bssh_session *front : m_layout->visible_panes()) {
		if (bssh_pane_bar *bar = bar_for(front)) bars.append(bar);
	}
	return bars;
}

bssh_pane_bar *bssh_main_window::bar_for(bssh_session *front) const {
	return m_panes ? qobject_cast<bssh_pane_bar *>(m_panes->bar_for(front)) : nullptr;
}

void bssh_main_window::set_session_drag_active(bool active) {
	/* Out of sight at once, gone a turn later: one may be delivering the drop. */
	for (const QPointer<bssh_tab_drop> &overlay : m_tab_drops) {
		if (!overlay) continue;
		overlay->hide();
		overlay->deleteLater();
	}
	m_tab_drops.clear();
	if (!active || !m_terminal_page || !m_panes || !m_layout) return;

	const bool vertical = m_tab_position == bssh_app_settings::TABS_LEFT
	                      || m_tab_position == bssh_app_settings::TABS_RIGHT;
	const int deep = fontMetrics().height() * 2;

	/*
	 * Each pane's bar, or with none the strip along the pane's edge where
	 * it would be (sec 8.3): that is where somebody who wants a tab in
	 * THAT pane looks.
	 */
	for (bssh_session *front : m_layout->visible_panes()) {
		QWidget *frame = m_panes->frame_for(front);
		if (!frame || !frame->isVisible()) continue;

		bssh_pane_bar *bar = bar_for(front);
		const bool strip = !bar || bar->isHidden();
		const QRect pane(frame->mapTo(m_terminal_page, QPoint(0, 0)), frame->size());

		QRect where;
		if (!strip) {
			where = QRect(bar->mapTo(m_terminal_page, QPoint(0, 0)), bar->size());
		} else {
			switch (m_tab_position) {
			case bssh_app_settings::TABS_BOTTOM:
				where = QRect(pane.left(), pane.bottom() - deep + 1, pane.width(), deep);
				break;
			case bssh_app_settings::TABS_LEFT:
				where = QRect(pane.left(), pane.top(), deep, pane.height());
				break;
			case bssh_app_settings::TABS_RIGHT:
				where = QRect(pane.right() - deep + 1, pane.top(), deep, pane.height());
				break;
			default:
				where = QRect(pane.left(), pane.top(), pane.width(), deep);
				break;
			}
		}

		bssh_tab_drop *overlay = new bssh_tab_drop(m_terminal_page);
		overlay->front = front;
		overlay->slot_at = [this, front](const QMimeData *mime, const QPoint &at, int *mark) {
			return tab_drop_slot(session_from_mime(mime), front, at, mark);
		};
		overlay->dropped = [this, front](const QMimeData *mime, int slot) {
			QPointer<bssh_session> session = session_from_mime(mime);
			QPointer<bssh_session> into = front;
			if (!session) return;
			QMetaObject::invokeMethod(this, [this, session, into, slot] {
				if (session) drop_session_as_tab(session, into, slot);
			}, Qt::QueuedConnection);
		};
		overlay->set_shape(strip, vertical);
		overlay->setGeometry(where);
		overlay->show();
		overlay->raise();
		m_tab_drops.append(overlay);
	}
}

/* Every window of this process, for the length of one session drag. */
static void set_session_drag_active_everywhere(bool active) {
	for (QWidget *widget : QApplication::topLevelWidgets()) {
		if (bssh_main_window *window = qobject_cast<bssh_main_window *>(widget))
			window->set_session_drag_active(active);
	}
}

/*
 * What preview_arrival() lit, so the next hover clears it, and the timer
 * that clears everything if the other process goes quiet mid-drag -- it
 * may have been killed, and a lit zone nobody will ever end would stay.
 */
namespace {
QPointer<bssh_terminal_view> g_arrival_lit;
QPointer<bssh_main_window> g_arrival_row;
bool g_arrival_active = false;
QTimer *g_arrival_quiet = nullptr;
}

void bssh_main_window::preview_arrival(const QPoint &global, bool active) {
	if (!active) {
		if (g_arrival_lit) g_arrival_lit->show_drop_zone(-1);
		if (g_arrival_row) {
			for (const QPointer<bssh_tab_drop> &overlay : g_arrival_row->m_tab_drops)
				if (overlay) overlay->hover(-1, -1);
		}
		g_arrival_lit.clear();
		g_arrival_row.clear();
		if (g_arrival_active) set_session_drag_active_everywhere(false);
		g_arrival_active = false;
		if (g_arrival_quiet) g_arrival_quiet->stop();
		return;
	}

	/* The tab strips a drag of this process's own would put up. */
	if (!g_arrival_active) {
		set_session_drag_active_everywhere(true);
		g_arrival_active = true;
	}
	if (!g_arrival_quiet) {
		g_arrival_quiet = new QTimer(qApp);
		g_arrival_quiet->setSingleShot(true);
		QObject::connect(g_arrival_quiet, &QTimer::timeout, qApp, [] { preview_arrival(QPoint(), false); });
	}
	g_arrival_quiet->start(1500);

	const drop_spot_t spot = drop_spot_at(global, nullptr);
	bssh_terminal_view *view = spot.window && spot.target && !spot.on_tab_row
	        ? spot.window->panes()->view_for(spot.target) : nullptr;
	if (g_arrival_lit && g_arrival_lit != view) g_arrival_lit->show_drop_zone(-1);
	if (view) view->show_drop_zone(spot.window->arrival_zone_for(spot.target, spot.edge));
	g_arrival_lit = view;

	bssh_main_window *row = spot.on_tab_row ? spot.window : nullptr;
	for (bssh_main_window *each : { g_arrival_row.data(), row }) {
		if (!each) continue;
		for (const QPointer<bssh_tab_drop> &overlay : each->m_tab_drops) {
			if (!overlay) continue;
			const bool lit = each == row && overlay->front == spot.front;
			overlay->hover(lit ? spot.slot : -1, lit ? spot.mark : -1);
		}
	}
	g_arrival_row = row;
}

QString bssh_main_window::session_mime_type() {
	/* One spelling, and it lives with the view because that is where a
	 * drop actually lands (sec 8.4). */
	return bssh_session_mime_type();
}

QMimeData *bssh_main_window::session_drag_mime(bssh_session *session) const {
	const int index = m_registry && session ? m_registry->index_of(session) : -1;
	if (index < 0) return nullptr;

	QMimeData *mime = new QMimeData;
	const bssh_handoff_point *point = bssh_handoff_point::instance();
	mime->setData(session_mime_type(),
	              bssh_session_payload(index, point ? session->handoff_id() : QString(),
	                                   point ? point->path() : QString()));
	return mime;
}

bssh_session *bssh_main_window::session_from_mime(const QMimeData *mime) const {
	/* Another process's payload names nothing here, whatever its index. */
	const int index = bssh_session_payload_index(mime, nullptr);
	if (index < 0) return nullptr;

	/*
	 * Resolved out of the registry rather than dereferenced: the payload
	 * is an index, and an index that no longer names a session -- closed
	 * mid-drag, say -- must decline rather than land on whichever session
	 * moved into that slot.
	 */
	return m_registry ? m_registry->at(index) : nullptr;
}


/*
 * What the pointer carries during a session drag.
 *
 * Painted rather than loaded: this program ships no icon theme, and a
 * missing asset would leave the gesture looking like nothing again.
 */
/* A no-drop sign, for the cursor while another process refuses a drop. */
static QPixmap refused_cursor() {
	QPixmap canvas(32, 32);
	canvas.fill(Qt::transparent);
	QPainter painter(&canvas);
	painter.setRenderHint(QPainter::Antialiasing, true);
	QPen pen(QColor(200, 40, 40));
	pen.setWidthF(3.0);
	painter.setPen(pen);
	painter.drawEllipse(QRectF(4, 4, 24, 24));
	painter.drawLine(QPointF(8.5, 8.5), QPointF(23.5, 23.5));
	painter.end();
	return canvas;
}

QPixmap bssh_main_window::session_drag_pixmap(bssh_session *session) const {
	const QString name = session && !session->title().isEmpty()
	        ? session->title()
	        : QStringLiteral("session");

	QFontMetrics metrics(font());
	const int text_width = metrics.horizontalAdvance(name);
	const int box = 34;
	const int width = qMin(320, box + 8 + text_width + 10);
	const int height = qMax(box, metrics.height()) + 8;

	QPixmap canvas(width, height);
	canvas.fill(Qt::transparent);

	QPainter painter(&canvas);
	painter.setRenderHint(QPainter::Antialiasing, true);

	/* A little terminal: a dark screen, a lit border, a prompt. */
	const QRect screen(2, (height - box) / 2, box - 4, box - 8);
	painter.fillRect(screen, QColor(20, 20, 20));

	QPen edge(palette().color(QPalette::Highlight));
	edge.setWidth(2);
	painter.setPen(edge);
	painter.drawRect(screen);

	QFont prompt = font();
	prompt.setPointSizeF(qMax(6.0, prompt.pointSizeF() * 0.8));
	painter.setFont(prompt);
	painter.setPen(QColor(220, 220, 220));
	painter.drawText(screen.adjusted(4, 2, -2, -2), Qt::AlignLeft | Qt::AlignVCenter,
	                  QStringLiteral("$_"));

	painter.setFont(font());
	painter.setPen(palette().color(QPalette::WindowText));
	painter.drawText(QRect(box + 4, 0, width - box - 6, height),
	                  Qt::AlignLeft | Qt::AlignVCenter,
	                  metrics.elidedText(name, Qt::ElideMiddle, width - box - 8));
	painter.end();

	return canvas;
}

static int g_session_drags_started = 0;

int bssh_main_window::session_drags_started() {
	return g_session_drags_started;
}

void bssh_main_window::start_session_drag(bssh_session *session) {
	if (!session || !m_registry) return;
	++g_session_drags_started;

	const int in_registry = m_registry->index_of(session);
	if (in_registry < 0) return;

	if (QGuiApplication::platformName() == QStringLiteral("xcb")) {
		run_own_session_drag(session);
		return;
	}

	QMimeData *mime = session_drag_mime(session);
	if (!mime) return;

	QDrag *drag = new QDrag(this);
	drag->setMimeData(mime);

	/*
	 * Something under the pointer that says what is being carried.
	 *
	 * Qt's default is no pixmap at all, so a session drag looked exactly
	 * like a text drag, a file drag, or nothing -- and the one question
	 * a user has mid-gesture is what they picked up. A small terminal
	 * with the session's name under it answers it without a theme asset.
	 */
	drag->setPixmap(session_drag_pixmap(session));
	drag->setHotSpot(QPoint(12, 12));

	/*
	 * And the pane it came FROM is hatched for as long as the drag is
	 * up. exec() runs a nested loop, so this is set before it and
	 * cleared after -- through a QPointer, because the view is destroyed
	 * when the session lands somewhere else and the pointer would
	 * otherwise be dangling by the time exec() returns.
	 */
	QPointer<bssh_terminal_view> source =
	        m_panes ? m_panes->view_for(session) : nullptr;
	if (source) source->set_being_dragged(true);
	set_session_drag_active_everywhere(true);

	/*
	 * A move, not a copy: a session is shown in exactly one place
	 * (sec 8.4), and there is no meaning to be had from two views of one
	 * connection.
	 *
	 * exec() runs a nested loop until the pointer is released. Whoever
	 * accepted has already taken the session by the time it returns --
	 * through their own dropEvent, never through anything this window
	 * called on them.
	 */
	/*
	 * A LINK is what another BeerSSH answers with: a live session cannot
	 * cross processes, so it takes nothing and says so (sec 8.4). The
	 * cursor for it is a no-drop sign, which is the refusal the user sees.
	 */
	drag->setDragCursor(refused_cursor(), Qt::LinkAction);
	const Qt::DropAction landed = drag->exec(Qt::MoveAction | Qt::LinkAction, Qt::MoveAction);

	set_session_drag_active_everywhere(false);
	if (source) source->set_being_dragged(false);

	if (landed != Qt::IgnoreAction) return;

	/*
	 * Nobody took it -- or the drag was cancelled, which exec() reports
	 * the same way, so Escape used to tear out a window. Every spot on a
	 * window of ours accepts (a cancel included), so ignored over one of
	 * them is a cancel; and the button still down is Escape pressed
	 * mid-drag. Only a release over nothing of ours is the tear-out.
	 */
	if (QApplication::topLevelAt(QCursor::pos())) return;
	if (QGuiApplication::mouseButtons() & Qt::LeftButton) return;

	/*
	 * Nobody wanted it, so the gesture means what it meant before there
	 * was anywhere to drop: a window of its own (sec 8.4). Dropping on
	 * the desktop and dropping on nothing are the same event to Qt, which
	 * is exactly the behaviour this needs.
	 */
	tear_out(session);
}

void bssh_main_window::run_own_session_drag(bssh_session *session) {
	QPointer<bssh_session> carried = session;
	++g_session_drags_started;

	QPointer<bssh_terminal_view> source =
	        m_panes ? m_panes->view_for(session) : nullptr;
	if (source) source->set_being_dragged(true);

	/* The pane whose zone is lit, so the last one is cleared as the pointer moves on. */
	QPointer<bssh_terminal_view> lit;
	/* And the tab row whose slot is marked, likewise. */
	QPointer<bssh_main_window> lit_row;

	set_session_drag_active_everywhere(true);

	/* The other BeerSSH told about this drag, and when it last was. */
	qint64 hovered_pid = 0;
	QElapsedTimer hover_clock;

	session_drag_ghost ghost(session_drag_pixmap(session));
	ghost.moved = [this, &ghost, &lit, &lit_row, carried, &hovered_pid, &hover_clock](const QPoint &at) {
		const drop_spot_t spot = drop_spot_at(at, carried);
		bssh_terminal_view *view = spot.window && spot.target
		        ? spot.window->panes()->view_for(spot.target) : nullptr;

		if (lit && lit != view) lit->show_drop_zone(-1);
		if (view) view->show_drop_zone(spot.window->drop_zone_for(carried, spot.target, spot.edge));
		lit = view;

		/* Another BeerSSH takes it when this process can be asked
		 * (sec 8.4); without a handoff point it is still refused. */
		ghost.set_refused(!spot.window && !bssh_handoff_point::instance() &&
		                  another_beerssh_under_pointer(this));

		/*
		 * Over another BeerSSH, it is told where, so it can draw the zone
		 * a drop would land in -- it could not see this drag otherwise
		 * (sec 8.7a). At most every 30 ms, and told when the pointer
		 * leaves it.
		 */
		const qint64 over = !spot.window && bssh_handoff_point::instance()
		                            ? other_beerssh_pid_under_pointer(this) : 0;
		if (hovered_pid && hovered_pid != over)
			bssh_handoff_point::hover(bssh_handoff_point::path_for(hovered_pid), QPoint(), false);
		if (over && (over != hovered_pid || !hover_clock.isValid() || hover_clock.elapsed() >= 30)) {
			bssh_handoff_point::hover(bssh_handoff_point::path_for(over), at, true);
			hover_clock.restart();
		}
		hovered_pid = over;

		bssh_main_window *row = spot.on_tab_row ? spot.window : nullptr;
		for (bssh_main_window *each : { lit_row.data(), row }) {
			if (!each) continue;
			for (const QPointer<bssh_tab_drop> &overlay : each->m_tab_drops) {
				if (!overlay) continue;
				const bool lit = each == row && overlay->front == spot.front;
				overlay->hover(lit ? spot.slot : -1, lit ? spot.mark : -1);
			}
		}
		lit_row = row;
	};

	QPoint released;
	const bool dropped = ghost.run(&released);

	/*
	 * Released over the other BeerSSH it was showing: the offer that
	 * follows ends its preview, after placing (take_from_another_process).
	 * Anything else -- cancelled, or released over nothing of theirs --
	 * ends it here.
	 */
	if (hovered_pid && !(dropped && other_beerssh_pid_under_pointer(this) == hovered_pid))
		bssh_handoff_point::hover(bssh_handoff_point::path_for(hovered_pid), QPoint(), false);

	if (lit) lit->show_drop_zone(-1);
	if (source) source->set_being_dragged(false);

	/*
	 * Placed while the tab strips are still up, and only then taken down:
	 * a pane with one tab has no bar, only the strip drawn for the drag,
	 * so asked afterwards the release fell through to the pane's top edge
	 * and split it under a strip that had just said "a tab".
	 */
	if (dropped && carried) finish_session_drag(carried, released);
	set_session_drag_active_everywhere(false);
}

#ifdef BSSH_ASK_X11
namespace {

/* A window's WM_CLASS -- "instance\0class\0" -- or empty. */
QByteArray x11_wm_class(xcb_connection_t *connection, xcb_window_t window) {
	xcb_get_property_reply_t *reply = xcb_get_property_reply(connection,
	        xcb_get_property(connection, 0, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, 0, 64),
	        nullptr);
	QByteArray value;
	if (reply) {
		value = QByteArray(static_cast<const char *>(xcb_get_property_value(reply)),
		                   xcb_get_property_value_length(reply));
		free(reply);
	}
	return value;
}

/* A window's _NET_WM_PID, or 0. */
qint64 x11_window_pid(xcb_connection_t *connection, xcb_window_t window) {
	static const char name[] = "_NET_WM_PID";
	xcb_intern_atom_reply_t *atom = xcb_intern_atom_reply(connection,
	        xcb_intern_atom(connection, 1, sizeof(name) - 1, name), nullptr);
	if (!atom) return 0;
	const xcb_atom_t pid_atom = atom->atom;
	free(atom);
	if (pid_atom == XCB_ATOM_NONE) return 0;

	xcb_get_property_reply_t *reply = xcb_get_property_reply(connection,
	        xcb_get_property(connection, 0, window, pid_atom, XCB_ATOM_CARDINAL, 0, 1), nullptr);
	qint64 pid = 0;
	if (reply) {
		if (xcb_get_property_value_length(reply) >= 4)
			pid = *static_cast<const quint32 *>(xcb_get_property_value(reply));
		free(reply);
	}
	return pid;
}

} /* namespace */
#endif

/*
 * Whether the window under the pointer is another BeerSSH process's
 * (sec 8.4). On X11 the session drag is the app's own ghost, invisible to
 * other programs, so nothing else can say so: the X server is asked which
 * window is under the pointer, down the tree from the root to the deepest,
 * and the first one carrying a WM_CLASS decides -- ours if it is the same
 * class as `ours` and belongs to another pid. False wherever the question
 * cannot be asked, which is every platform but X11.
 */
static bool another_beerssh_under_pointer(const QWidget *ours) {
	return other_beerssh_pid_under_pointer(ours) > 0;
}

/* The pid of the BeerSSH under the pointer when it is another one, or 0. */
static qint64 other_beerssh_pid_under_pointer(const QWidget *ours) {
#ifdef BSSH_ASK_X11
	if (!ours || QGuiApplication::platformName() != QStringLiteral("xcb")) return 0;
	auto *x11 = qApp->nativeInterface<QNativeInterface::QX11Application>();
	xcb_connection_t *connection = x11 ? x11->connection() : nullptr;
	if (!connection) return 0;

	const QByteArray own_class = x11_wm_class(connection, xcb_window_t(ours->window()->winId()));
	if (own_class.isEmpty()) return 0;

	const xcb_setup_t *setup = xcb_get_setup(connection);
	xcb_window_t at = xcb_setup_roots_iterator(setup).data->root;
	QList<xcb_window_t> chain;
	for (int depth = 0; depth < 16; ++depth) {
		xcb_query_pointer_reply_t *reply = xcb_query_pointer_reply(connection,
		        xcb_query_pointer(connection, at), nullptr);
		const xcb_window_t child = reply ? reply->child : XCB_WINDOW_NONE;
		free(reply);
		if (child == XCB_WINDOW_NONE) break;
		chain.prepend(child);
		at = child;
	}

	for (const xcb_window_t window : chain) {
		const QByteArray cls = x11_wm_class(connection, window);
		if (cls.isEmpty()) continue;
		if (cls != own_class) return 0;
		const qint64 pid = x11_window_pid(connection, window);
		return pid > 0 && pid != QCoreApplication::applicationPid() ? pid : 0;
	}
#else
	Q_UNUSED(ours);
#endif
	return 0;
}

bssh_main_window::drop_spot_t bssh_main_window::drop_spot_at(const QPoint &global,
                                                              bssh_session *session) {
	drop_spot_t spot;

	bssh_main_window *window =
	        qobject_cast<bssh_main_window *>(QApplication::topLevelAt(global));
	if (!window) return spot;
	spot.window = window;

	/*
	 * The tab row while a drag is up: the overlay over the bar or where it
	 * would be, asked for the slot it draws. A slot of -1 means a drop
	 * there does nothing, which is a spot on nothing.
	 */
	for (const QPointer<bssh_tab_drop> &overlay : window->m_tab_drops) {
		if (!overlay || !overlay->isVisible()) continue;
		const QPoint at = overlay->mapFromGlobal(global);
		if (overlay->rect().contains(at)) {
			spot.front = overlay->front;
			spot.slot = window->tab_drop_slot(session, overlay->front, at, &spot.mark);
			spot.on_tab_row = spot.slot >= 0;
			return spot;
		}
	}
	if (window->m_tab_drops.isEmpty()) {
		if (bssh_session *front = window->tab_row_at(window->mapFromGlobal(global))) {
			spot.on_tab_row = true;
			spot.front = front;
			return spot;
		}
	}

	bssh_pane_container *panes = window->panes();
	if (!panes) return spot;

	for (bssh_session *each : window->m_shown) {
		bssh_terminal_view *view = panes->view_for(each);
		if (!view || !view->isVisible()) continue;

		const QPoint local = view->mapFromGlobal(global);
		if (!view->rect().contains(local)) continue;

		spot.target = each;
		spot.edge = bssh_drop_zone_at(local, view->size(), bssh_drop_band(view->cell_height()));
		break;
	}
	return spot;
}

void bssh_main_window::finish_session_drag(bssh_session *session, const QPoint &global) {
	if (!session) return;

	const drop_spot_t spot = drop_spot_at(global, session);

	/*
	 * Outside every window of ours: the gesture's original meaning, a
	 * window of its own (sec 8.4), put where the pointer let go rather
	 * than wherever a new window lands by default.
	 */
	if (!spot.window) {
		/*
		 * Over another BeerSSH: offered to it (sec 8.4), and it comes and
		 * takes the session -- after this returns, since taking needs this
		 * process's event loop to answer. Without a handoff point here, or
		 * if the offer cannot be made, the session stays where it was
		 * rather than tearing out on top of a window that will not take it
		 * (the holder's choice, 2026-10-09).
		 */
		if (const qint64 pid = other_beerssh_pid_under_pointer(this)) {
			if (const bssh_handoff_point *point = bssh_handoff_point::instance()) {
				QString why;
				if (!bssh_handoff_point::offer(bssh_handoff_point::path_for(pid), point->path(),
				                               session->handoff_id(), global, &why))
					qWarning("handoff: could not offer the session: %s", qPrintable(why));
			}
			return;
		}
		tear_out(session, global - QPoint(40, 20), true);
		return;
	}

	/*
	 * On a window but not on a terminal or its tab row -- the status
	 * line, the keyboard, the front door's page: nothing (the holder,
	 * 2026-10-09). It used to tile beside, with nothing drawn to say so.
	 */
	if (spot.on_tab_row) spot.window->drop_session_as_tab(session, spot.front, spot.slot);
	else if (spot.target) spot.window->drop_session_on(session, spot.target, spot.edge);
}

/*
 * A session that another BeerSSH let go over this process (sec 8.4): taken
 * from it and put where it landed, as a drop from this process would be.
 * Anywhere that would place nothing -- the status line, say -- makes it a
 * tab in the window it landed on, or a window of its own: a session that
 * arrives and is shown nowhere would be a running terminal no window can
 * reach.
 */
void bssh_main_window::take_from_another_process(const QString &path, const QString &handoff_id,
                                                 const QPoint &global) {
	bssh_session_registry *registry = bssh_session_registry::shared();
	QString why;
	bssh_session *session = bssh_handoff_point::take(path, handoff_id, registry, &why);
	if (!session) {
		qWarning("handoff: could not take the session: %s", qPrintable(why));
		preview_arrival(QPoint(), false);
		return;
	}
	registry->adopt(session);

	const drop_spot_t spot = drop_spot_at(global, session);
	/*
	 * Placed while the preview's tab strips are still up, as a drag of
	 * this process's own is (see run_own_session_drag), and only then
	 * taken down.
	 */
	const auto end_preview = qScopeGuard([] { preview_arrival(QPoint(), false); });
	if (spot.on_tab_row && spot.front) spot.window->drop_session_as_tab(session, spot.front, spot.slot);
	else if (spot.window && spot.target) spot.window->drop_session_on(session, spot.target, spot.edge);
	else if (spot.window) spot.window->adopt_session(session);
	else {
		bssh_main_window *window = new bssh_main_window;
		window->setAttribute(Qt::WA_DeleteOnClose);
		window->show();
		window->adopt_session(session);
		window->move(global - QPoint(40, 20));
	}
	if (bssh_main_window *window = window_showing(session)) {
		window->raise();
		window->activateWindow();
	}
}

/*
 * A session drag from ANOTHER BeerSSH that can be asked for: accepted as a
 * move, and taken a turn later, outside the drop -- the source's drag runs
 * its own loop until this returns, and that loop is what answers the take.
 */
static bool take_foreign_drag(QDropEvent *event, bool dropping) {
	QString path, id;
	if (!bssh_session_payload_handoff(event->mimeData(), &path, &id)) return false;
	if (!(event->possibleActions() & Qt::MoveAction)) return false;
	event->setDropAction(Qt::MoveAction);
	event->accept();
	if (dropping) {
		const QPoint global = QCursor::pos();
		QTimer::singleShot(0, qApp, [path, id, global] {
			bssh_main_window::take_from_another_process(path, id, global);
		});
	}
	return true;
}

/*
 * A session drag from ANOTHER BeerSSH: accepted as a link, which moves
 * nothing. Refusing outright would read, at the source, as a release over
 * nothing -- and tear a window out over this one. The source draws its
 * no-drop cursor for a link, so the refusal is what the user sees.
 */
static bool answer_foreign_drag(QDropEvent *event) {
	bool foreign = false;
	bssh_session_payload_index(event->mimeData(), &foreign);
	if (!foreign || !(event->possibleActions() & Qt::LinkAction)) return foreign;
	event->setDropAction(Qt::LinkAction);
	event->accept();
	return true;
}

void bssh_main_window::dragEnterEvent(QDragEnterEvent *event) {
	if (take_foreign_drag(event, false)) return;
	if (answer_foreign_drag(event)) return;
	if (session_from_mime(event->mimeData())) event->acceptProposedAction();
}

bssh_session *bssh_main_window::tab_row_at(const QPoint &at) const {
	/*
	 * Across each pane's bar ROW, whichever way it runs, not the QTabBar
	 * widget: a bar is only as wide as its tabs, and somebody aiming at
	 * "the tab bar" means the band, not the widget -- a drop at x=340 on
	 * the same row as one at x=60 once missed for exactly that.
	 */
	for (bssh_pane_bar *bar : pane_bars()) {
		if (!bar->isVisible()) continue;
		if (bar->rect().contains(bar->mapFrom(this, at))) return bar->front();
	}
	return nullptr;
}

void bssh_main_window::dropEvent(QDropEvent *event) {
	if (take_foreign_drag(event, true)) return;
	if (answer_foreign_drag(event)) return;
	bssh_session *session = session_from_mime(event->mimeData());
	if (!session) return;

	/*
	 * Where it lands is what it means (sec 8.3, 8.4): the tab row makes a
	 * tab. A drop on a terminal never arrives here -- the pane accepts it
	 * and the window places it through drop_session_on().
	 *
	 * Mapped into the bar's own coordinates rather than compared against
	 * its geometry(), which is expressed in its parent's and would be
	 * offset by however the window is laid out.
	 *
	 * The ROW, not the widget, and that distinction is a bug this found.
	 * A QTabBar is only as wide as its tabs, so with one tab open it
	 * occupies about 180 pixels of a 1200-pixel window -- and dropping
	 * anywhere else along that row missed it. Measured: a drop at x=60
	 * un-split correctly and the identical drop at x=340, the same row
	 * and visually the same strip, did nothing at all.
	 *
	 * Nobody aiming at "the tab bar" is aiming at a widget. They mean
	 * the band across the top where tabs live, so the test is on Y
	 * alone and the whole row answers.
	 */
	/*
	 * Off the tab row and off every terminal -- the drops onto a terminal
	 * are the panes' own -- a release does nothing, and is ACCEPTED doing
	 * it: refused, the source would read "nobody wanted it" and tear out
	 * a window over the one the user was aiming at.
	 */
	if (bssh_session *front = tab_row_at(event->position().toPoint()))
		drop_session_as_tab(session, front, -1);
	event->acceptProposedAction();
}

void bssh_main_window::open_another_session(bool in_a_window) {
	const bool touch = bssh_platform::instance()
	        && bssh_platform::instance()->is_touch_primary();

	if (!opens_a_local_shell(touch)) {
		show_front_door();
		return;
	}

	if (!in_a_window) {
		open_profile(bssh_profile_t::local_shell());
		return;
	}

	/*
	 * A window in a process of its own, as TDE Konsole's are, so ending
	 * one -- a crash, a kill -- ends no other window (the holder,
	 * 2026-10-25, sec 8.4). Desktop only, where the hand-off point that
	 * marks a multi-process desktop exists; the same grid as this one.
	 */
	if (bssh_handoff_point::instance() && m_terminal_view) {
		const QStringList arguments = {
			QStringLiteral("--geometry"),
			QStringLiteral("%1x%2").arg(m_terminal_view->cols()).arg(m_terminal_view->rows())
		};
		QString why;
		if (start_this_program(arguments, &why)) return;
		qWarning("window: %s, so it opens in this process", qPrintable(why));
	}

	/*
	 * Otherwise a second window is a second VIEW in this process, never a
	 * second registry (sec 8.4) -- the tear-out gesture's window without
	 * the tearing.
	 */
	bssh_main_window *window = new bssh_main_window;
	window->setAttribute(Qt::WA_DeleteOnClose);
	window->resize(size());
	window->show();
	window->open_profile(bssh_profile_t::local_shell());
}

/*
 * Move a pane one place earlier or later. See the header for why this
 * exists rather than a touch drag.
 *
 * The INTENT is what moves, not the visible list: the intent is what the
 * layout is rebuilt from and what survives a rotation that changes how
 * many panes fit, so reordering the visible subset would be undone the
 * next time the window resized.
 */
/*
 * Move a tab one place along its pane's bar (sec 8.3's bar per pane),
 * for the menu items; the desktop drag reorders through the bar itself.
 * The pane's own order, held by the layout manager: which order a bar
 * shows its tabs in is the pane's business, not the registry's -- moving
 * them there once rearranged another window's tabs as a side effect.
 */
void bssh_main_window::move_tab(bssh_session *session, int direction) {
	if (!session || !m_layout || direction == 0) return;

	bssh_session *front = m_layout->front_of(session);
	const QList<bssh_session *> tabs = m_layout->tabs(session);
	const int from = tabs.indexOf(session);
	const int to = from + (direction < 0 ? -1 : 1);
	if (!front || from < 0 || to < 0 || to >= tabs.size()) return;

	/* add_tab counts the slot in the bar as it is, the tab still in it. */
	m_layout->add_tab(session, front, direction < 0 ? to : to + 1);
	if (front != session) m_layout->bring_to_front(front);
	refresh_tab_bar();
}

void bssh_main_window::move_pane(bssh_session *session, int direction) {
	if (!session || !m_layout || !m_panes || direction == 0) return;

	QList<bssh_session *> intent = m_layout->intent();
	const int from = intent.indexOf(session);
	if (from < 0) return;

	const int to = from + (direction < 0 ? -1 : 1);
	if (to < 0 || to >= intent.size()) return;

	intent.move(from, to);
	m_layout->set_intent(intent);


	m_panes->rebuild();

	/*
	 * Focus follows the pane rather than the position. A user who moved
	 * this terminal is still working in it, and leaving focus where the
	 * pane used to be hands their next keystroke to a different host.
	 */
	m_panes->focus_session(session);

	refresh_tab_bar();
	update_status();
}

bssh_main_window *bssh_main_window::tear_out(bssh_session *session, const QPoint &at,
                                             bool placed) {
	if (!session || !m_shown.contains(session)) return nullptr;

	/*
	 * A window will not empty itself. Tearing out the last tab would
	 * leave a window with nothing in it and, on desktop, no front door
	 * showing either -- so the gesture does nothing rather than
	 * producing two windows one of which is useless.
	 */
	if (m_shown.size() < 2) return nullptr;

	/*
	 * Into a process of its own (sec 8.4): a new program takes the
	 * session live over the hand-off socket, as a drag into another
	 * BeerSSH does, and this window lets it go once it is taken. Its
	 * grid stays the size it was, and the window lands where it was let
	 * go.
	 */
	const bssh_handoff_point *point = bssh_handoff_point::instance();
	if (point && session->emulator()) {
		QString geometry = QStringLiteral("%1x%2")
		                           .arg(session->emulator()->screen().cols())
		                           .arg(session->emulator()->screen().rows());
		if (placed) geometry += QStringLiteral("+%1+%2").arg(at.x()).arg(at.y());

		const QStringList arguments = {
			QStringLiteral("--adopt-from"), point->path(),
			QStringLiteral("--adopt-session"), session->handoff_id(),
			QStringLiteral("--geometry"), geometry
		};
		QString why;
		if (start_this_program(arguments, &why)) return nullptr;
		qWarning("tear-out: %s, so it opens in this process", qPrintable(why));
	}

	bssh_main_window *window = new bssh_main_window;
	window->setAttribute(Qt::WA_DeleteOnClose);
	window->resize(size());
	if (placed) window->move(at);
	window->show();
	window->adopt_session(session);

	return window;
}

bool bssh_main_window::eventFilter(QObject *watched, QEvent *event) {
	/*
	 * The split halves shown or hidden: the status line follows them.
	 * Queued, because moving widgets into a half from inside its own show
	 * event is reparenting under a layout that is being laid out.
	 */
	if (m_keyboard && watched == m_keyboard->half(1)
	    && (event->type() == QEvent::Show || event->type() == QEvent::Hide)) {
		QTimer::singleShot(0, this, [this] { place_status_line(); });
		return false;
	}

	/* Tearing a tab out and middle-click close are bssh_pane_bar's now. */

	if (watched == m_panes && event->type() == QEvent::Resize) queue_room();

	return QWidget::eventFilter(watched, event);
}

/*
 * The room the tiled panes have, told to the layout (sec 8.2): how many
 * panes a wide desktop window shows is how many fit, the rest tabs until
 * there is room again. Queued and coalesced -- told from inside a resize
 * or a rebuild, the rebuild it may cause would delete the frames that
 * event is being delivered to -- and never on touch, whose window resizes
 * on every frame of the keyboard opening.
 */
void bssh_main_window::queue_room() {
	if (m_room_queued) return;
	m_room_queued = true;
	QTimer::singleShot(0, this, [this] {
		m_room_queued = false;
		if (!m_layout || !m_panes) return;
		if (bssh_platform::instance() && bssh_platform::instance()->is_touch_primary()) return;
		m_layout->set_room(m_panes->size(), m_panes->least_pane(), m_panes->divider());
	});
}

/*
 * The tab dots are pixmaps, drawn once with the colours of the theme in
 * force at the time (sec 6.2). A theme change repaints every widget but
 * cannot repaint a pixmap somebody already handed to the tab bar, so the
 * tabs are rebuilt -- otherwise switching to light leaves four dots that
 * were chosen to be legible on a dark bar and are not on this one.
 */
/*
 * Show a sub-page, remembering the page to return to.
 *
 * The four sub-pages used to return to the host list whatever they were
 * opened from, which is right from the front door and wrong from a
 * session: reaching the settings from a live shell's keyboard and
 * pressing Back put the shell out of sight. The page it was opened from
 * is the only answer that is right from both doors -- at every level, so
 * the pages are remembered as a trail rather than as the last one.
 */
void bssh_main_window::show_page(QWidget *page) {
	if (!m_pages || !page) return;

	QWidget *here = m_pages->currentWidget();
	if (here == page) return;

	/*
	 * A root page starts the trail afresh. Pages are also left by routes
	 * that are not Back -- a session opening, say -- and without this the
	 * trail would keep what they skipped for as long as the app runs.
	 */
	if (here == m_terminal_page || here == m_host_list) m_back_stack.clear();
	m_back_stack.append(here);
	m_pages->setCurrentWidget(page);
}

/*
 * Leave the page now showing. False when there is nothing to leave.
 *
 * The caller needs the answer: Android's Back must background the app
 * when it is already at the bottom of the stack, and swallowing it
 * there would leave a user pressing Back at a terminal with nothing
 * happening at all.
 */
bool bssh_main_window::go_back() {
	if (!m_pages) return false;

	QWidget *here = m_pages->currentWidget();
	if (here == m_terminal_page || here == m_host_list) return false;

	QWidget *to = nullptr;
	while (!to && !m_back_stack.isEmpty()) {
		QWidget *candidate = m_back_stack.takeLast();
		if (candidate && candidate != here) to = candidate;
	}
	if (!to) to = m_host_list;

	if (!to) return false;
	m_pages->setCurrentWidget(to);
	return true;
}

/*
 * Android's Back key leaves the PAGE, not the app (sec 8.5).
 *
 * Reported from a phone: opening the settings and pressing Back closed
 * nothing and put the whole app in the background, with the settings
 * page still showing underneath when it came forward again. Qt delivers
 * the system Back as Qt::Key_Back, and a window that does not handle it
 * lets Qt close the window -- so the app went away while the page it
 * should have left stayed exactly where it was.
 *
 * Only accepted when there was something to leave. Back at the host list
 * is a request to leave the APP, and swallowing it there would strand a
 * user pressing a key that does nothing. Back at a terminal is not: on
 * touch the host list is the root (sec 8.5), and Back goes up to it.
 */
void bssh_main_window::keyPressEvent(QKeyEvent *event) {
	/*
	 * Back puts the forms' keyboard away first, as it does Android's,
	 * rather than leaving the page with a field still being typed into.
	 */
	if (event->key() == Qt::Key_Back && m_form_keyboard && m_form_keyboard->isVisible()) {
		if (QWidget *focus = QApplication::focusWidget()) focus->clearFocus();
		event->accept();
		return;
	}

	if (event->key() == Qt::Key_Back && go_back()) {
		event->accept();
		return;
	}

	/*
	 * And Back at a terminal goes UP to the host list, as closeEvent says
	 * it does. closeEvent never saw it on the Fold, 2026-10-07: Qt 6
	 * delivers Android's Back as this KEY, and left unaccepted at the
	 * terminal it put the app in the background -- so with a live session
	 * the host list, and with it a second session, could only be reached
	 * by a slip in the settings' own Back, which is fixed. Only where
	 * there IS a host list: the desktop has none, and Back means nothing
	 * there.
	 */
	if (event->key() == Qt::Key_Back && m_host_list && m_pages
	    && m_pages->currentWidget() == m_terminal_page) {
		m_pages->setCurrentWidget(m_host_list);
		event->accept();
		return;
	}

	QWidget::keyPressEvent(event);
}

/*
 * Before the children go: deleting them moves the focus, and the forms'
 * keyboard listens for that application-wide -- which by then would be
 * calling into a window whose own members are already gone. And the
 * keyboard before the target it types into, which is a member here.
 */
bssh_main_window::~bssh_main_window() {
	disconnect(qApp, &QApplication::focusChanged, this, nullptr);
	delete m_form_keyboard;
	m_form_keyboard = nullptr;
	if (m_forms_use_softkeys) qApp->setAutoSipEnabled(true);
}

/*
 * The switch, either way (sec 6.2). For this run only: which keyboard a
 * form gets is a habit the user shows by pressing the switch, and a
 * choice kept across runs would come back as the keyboard that appeared
 * from nowhere -- the brightness that outlived its run, again.
 */
void bssh_main_window::use_softkeys_in_forms(bool use) {
	m_forms_use_softkeys = use;
	update_form_keyboard();

	QWidget *focus = QApplication::focusWidget();
	if (use) QGuiApplication::inputMethod()->hide();
	else if (focus && focus->inputMethodQuery(Qt::ImEnabled).toBool()) {
		QGuiApplication::inputMethod()->show();
	}
}

/*
 * Which keyboard a form has, from where the user is: off the terminal's
 * page, the switch while the system keyboard is the one, and ours, shown
 * while a field has the focus, once it is not.
 *
 * Keeping the system keyboard down is Qt's autoSipEnabled: a QLineEdit
 * asks for the panel on focus and on a tap through it, and nothing else
 * would stop it. Turned back on everywhere ours is not wanted, the
 * terminal's page included -- the terminal asks for the panel itself.
 */
void bssh_main_window::update_form_keyboard() {
	if (!m_form_keyboard || !m_pages || !m_keyboard_switch) return;

	const bool on_form = m_pages->currentWidget() != m_terminal_page;
	const bool ours = on_form && m_forms_use_softkeys;
	QWidget *focus = QApplication::focusWidget();

	/*
	 * Not while the focus is in another window -- the password prompt,
	 * which keeps the system keyboard down or up for itself.
	 */
	if (!focus || focus->window() == window()) qApp->setAutoSipEnabled(!ours);

	const bool typing = focus && isAncestorOf(focus) && focus->isVisible()
	                    && focus->inputMethodQuery(Qt::ImEnabled).toBool();
	m_form_keyboard->setVisible(ours && typing);
	m_keyboard_switch->set_offered(on_form && !m_forms_use_softkeys);
}

void bssh_main_window::style_settings_index() {
	if (!m_settings_sections) return;

	m_settings_sections->setStyleSheet(QStringLiteral(
	        "QListWidget::item { border-bottom: 1px solid %1; padding-left: 8px; }")
	                                            .arg(palette().color(QPalette::Mid).name()));
}

void bssh_main_window::changeEvent(QEvent *event) {
	QWidget::changeEvent(event);
	if (event && event->type() == QEvent::PaletteChange) on_registry_changed();
	if (event && event->type() == QEvent::PaletteChange) style_settings_index();
}

void bssh_main_window::refresh_tab_bar() {
	if (!m_panes || !m_layout) return;

	/*
	 * Each pane's bar against what the layout says it holds (sec 8.3's
	 * bar per pane). The same tabs: their labels and dots are brought up
	 * to date in place. Different tabs: the panes are rebuilt, which
	 * builds every bar afresh -- cheap, and the one way the bars cannot
	 * drift from the tree.
	 *
	 * On the TABS only, never on whether a bar shows. That is decided in
	 * make_pane_bar(), so a rebuild cannot change it, and a check that
	 * asked would rebuild for ever once the two disagreed -- found by
	 * breaking the decision on purpose, when the suite hung.
	 */
	bool stale = false;
	for (bssh_session *front : m_layout->visible_panes()) {
		const QList<bssh_session *> tabs = m_layout->bar_tabs(front);
		bssh_pane_bar *bar = bar_for(front);
		if (!bar || bar->sessions() != tabs) {
			stale = true;
			break;
		}
		bar->refresh();
	}
	if (stale) {
		m_panes->rebuild();
		return;
	}
	sync_corner_buttons();
}

void bssh_main_window::sync_corner_buttons() {
	if (!m_panes || !m_layout) return;

	const bool menus = m_menu_bar && m_menu_bar->isHidden();
	for (bssh_session *front : m_layout->visible_panes()) {
		bssh_terminal_view *view = m_panes->view_for(front);
		if (!view) continue;
		const int tabs = int(m_layout->bar_tabs(front).size());
		view->set_corner_tabs(m_tabs_hidden && tabs > 1 ? QStringLiteral("tabs %1").arg(tabs)
		                                                 : QString());
		view->set_corner_menu(menus);
	}
}

/*
 * The pane's tabs as a list, from its corner while the tab bar is hidden
 * (sec 8.7a): the one in front marked, a pick bringing it there, and a new
 * tab -- what the bar's own tabs and its + do.
 */
void bssh_main_window::show_corner_tabs(bssh_session *front, const QPoint &global) {
	if (!m_layout) return;

	QMenu menu(this);
	menu.setObjectName(QStringLiteral("corner_tabs_menu"));
	for (bssh_session *session : m_layout->bar_tabs(front)) {
		QAction *pick = menu.addAction(session->title());
		pick->setCheckable(true);
		pick->setChecked(session == front);
		const QPointer<bssh_session> held = session;
		connect(pick, &QAction::triggered, this, [this, held] {
			if (held) QMetaObject::invokeMethod(this, [this, held] {
				if (held) show_session(held);
			}, Qt::QueuedConnection);
		});
	}
	menu.addSeparator();
	QAction *add = menu.addAction(QStringLiteral("New Tab"));
	const QPointer<bssh_session> pane = front;
	connect(add, &QAction::triggered, this, [this, pane] {
		QMetaObject::invokeMethod(this, [this, pane] {
			if (pane && m_panes) m_panes->focus_session(pane);
			open_another_session(false);
		}, Qt::QueuedConnection);
	});
	menu.exec(global);
}

/*
 * The menu bar's menus, from the corner while the bar is hidden (sec
 * 8.7a), and the way back to the bar. The same menus, not copies, so an
 * item enabled or disabled there is the same here.
 */
void bssh_main_window::show_corner_menu(const QPoint &global) {
	if (!m_menu_bar) return;

	QMenu menu(this);
	menu.setObjectName(QStringLiteral("corner_menus"));
	for (QAction *each : m_menu_bar->actions()) menu.addAction(each);
	if (m_menu_bar_action) {
		menu.addSeparator();
		menu.addAction(m_menu_bar_action);
	}
	menu.exec(global);
}

/*
 * Drawn by the panes themselves, as a drop zone is: every pane whose edge
 * lies on that side of the panes' area shows a bar's band along it. An
 * overlay widget over the panes was tried first; it was laid out and
 * shown, and twin's screen never had it on, where the panes' own painting
 * is what the drop zones already use.
 */
static int panes_edge_for(Qt::Edge side) {
	switch (side) {
	case Qt::LeftEdge:   return bssh_layout_manager::EDGE_LEFT;
	case Qt::RightEdge:  return bssh_layout_manager::EDGE_RIGHT;
	case Qt::BottomEdge: return bssh_layout_manager::EDGE_BOTTOM;
	default:             return bssh_layout_manager::EDGE_TOP;
	}
}

/*
 * The side is read from the bar's OWN pane, and only that pane is lit:
 * the bar is that terminal's, and dragging it over another one moves
 * nothing there -- the holder, 2026-10-30: "a tab bar shouldn't be
 * movable to other terminals". It was read from the whole panes area and
 * lit every pane along that side, so a handle carried across the window
 * looked like it was taking the bar to whichever terminal it was over.
 * Outside its pane there is no side, and letting go there does nothing.
 */
bool bssh_main_window::bar_side_in_own_pane(bssh_session *front, const QPoint &global,
                                            Qt::Edge *side) const {
	bssh_terminal_view *view = m_panes ? m_panes->view_for(front) : nullptr;
	if (!view) return false;
	const QPoint at = view->mapFromGlobal(global);
	if (!view->rect().contains(at)) return false;
	*side = bssh_bar_side_at(at, view->size());
	return true;
}

void bssh_main_window::preview_bar_side(bssh_session *front, const QPoint &global) {
	if (!m_panes) return;

	Qt::Edge side = Qt::TopEdge;
	const bool inside = bar_side_in_own_pane(front, global, &side);
	bssh_terminal_view *own = m_panes->view_for(front);
	for (bssh_terminal_view *view : m_panes->views()) {
		if (!view) continue;
		view->show_drop_zone(inside && view == own ? BSSH_DROP_TAB_BASE + panes_edge_for(side) : -1);
	}
	m_bar_side_previewed = true;
}

void bssh_main_window::finish_bar_side(bssh_session *front, const QPoint &global) {
	if (!m_panes) return;
	for (bssh_terminal_view *view : m_panes->views())
		if (view) view->show_drop_zone(-1);
	m_bar_side_previewed = false;

	Qt::Edge edge = Qt::TopEdge;
	if (!bar_side_in_own_pane(front, global, &edge)) return;

	bssh_app_settings::tab_position_t side = bssh_app_settings::TABS_TOP;
	switch (edge) {
	case Qt::LeftEdge:   side = bssh_app_settings::TABS_LEFT; break;
	case Qt::RightEdge:  side = bssh_app_settings::TABS_RIGHT; break;
	case Qt::BottomEdge: side = bssh_app_settings::TABS_BOTTOM; break;
	default:             break;
	}
	bssh_app_settings().set_tab_position(side);
	apply_tab_position(int(side));
}

void bssh_main_window::apply_menu_bar() {
	if (!m_menu_bar) return;

	const bool shown = bssh_app_settings().menu_bar_shown();
	if (shown == !m_menu_bar->isHidden()) return;
	m_menu_bar->setVisible(shown);
	if (m_menu_bar_action) m_menu_bar_action->setChecked(shown);
	sync_corner_buttons();
}

/*
 * Every session the window shows sits in exactly one pane's stack
 * (sec 8.3). The window's list is the record of WHICH sessions it holds;
 * the tree says where each one is. One taken and not yet placed joins the
 * focused pane's stack, in front -- what New Tab is, and what adopting a
 * torn-out session is -- and one let go leaves the tree.
 */
void bssh_main_window::sync_tree_with_shown() {
	if (!m_layout) return;

	for (bssh_session *placed : m_layout->sessions()) {
		if (!m_shown.contains(placed)) m_layout->forget(placed);
	}

	for (bssh_session *session : m_shown) {
		const QList<bssh_session *> placed = m_layout->sessions();
		if (placed.contains(session)) continue;

		bssh_session *into = m_current && placed.contains(m_current)
		                             ? m_layout->front_of(m_current) : nullptr;
		if (!into && !m_layout->visible_panes().isEmpty()) into = m_layout->visible_panes().first();

		/*
		 * Behind what the pane shows: one put back by a drop that took
		 * its place must not cover the drop. A new tab comes to the
		 * front through show_session(), which is how one is opened.
		 */
		if (into) {
			m_layout->add_tab(session, into, INT_MAX);
			m_layout->bring_to_front(into);
		} else {
			m_layout->set_intent({ session });
		}
	}
}

QWidget *bssh_main_window::make_pane_bar(bssh_session *front, QWidget *parent) {
	if (!m_layout || !m_panes) return nullptr;

	const QList<bssh_session *> tabs = m_layout->bar_tabs(front);
	const bool pointer = bssh_platform::instance()
	                     && !bssh_platform::instance()->is_touch_primary();
	bssh_pane_bar *bar = new bssh_pane_bar(front, tabs, m_panes->bar_edge(), pointer, parent);

	/* No bar for a pane of one: the bar appears with the second tab (sec 8.3);
	 * and none at all while bars are hidden (sec 8.7a). */
	bar->setVisible(tabs.size() > 1 && !m_tabs_hidden);

	/*
	 * Queued, each of these: what they do rebuilds the panes, and with
	 * them the bar whose signal is being delivered.
	 */
	const QPointer<bssh_main_window> self(this);
	const auto later = [self](std::function<void()> act) {
		QMetaObject::invokeMethod(self.data(), [self, act] { if (self) act(); },
		                          Qt::QueuedConnection);
	};

	connect(bar, &bssh_pane_bar::tab_picked, this, [this, later, front](bssh_session *session) {
		QPointer<bssh_session> picked = session;
		if (session == front) return;
		later([this, picked] { if (picked) show_session(picked); });
	});
	connect(bar, &bssh_pane_bar::close_requested, this, [this, later](bssh_session *session) {
		QPointer<bssh_session> closing = session;
		later([this, closing] { if (closing) m_registry->close(closing); });
	});
	connect(bar, &bssh_pane_bar::tab_moved, this,
	        [this, later, front](bssh_session *session, int slot) {
		QPointer<bssh_session> moved = session;
		QPointer<bssh_session> pane = front;
		later([this, moved, pane, slot] {
			if (!moved || !pane) return;
			m_layout->add_tab(moved, pane, slot);
			refresh_tab_bar();
		});
	});
	connect(bar, &bssh_pane_bar::menu_requested, this,
	        [this, later](bssh_session *session, const QPoint &global) {
		QPointer<bssh_session> held = session;
		later([this, held, global] { if (held) show_tab_menu(held, global); });
	});
	connect(bar, &bssh_pane_bar::drag_requested, this,
	        [this](bssh_session *session) { start_session_drag(session); });
	connect(bar, &bssh_pane_bar::side_drag_moved, this, [this, front](const QPoint &global) {
		preview_bar_side(front, global);
	});
	connect(bar, &bssh_pane_bar::side_drag_finished, this, [this, later, front](const QPoint &global) {
		/* Queued: moving the bars rebuilds the panes, this bar with them. */
		QPointer<bssh_session> pane = front;
		later([this, pane, global] { if (pane) finish_bar_side(pane, global); });
	});
	connect(bar, &bssh_pane_bar::new_tab_requested, this, [this, later, front] {
		QPointer<bssh_session> pane = front;
		later([this, pane] {
			if (pane && m_panes) m_panes->focus_session(pane);
			open_another_session(false);
		});
	});
	return bar;
}

void bssh_main_window::on_registry_changed() {
	/*
	 * The tab bar is rebuilt from the registry rather than edited
	 * alongside it: two lists kept in step by hand drift, and the one
	 * that drifts here shows the wrong session's name over the wrong
	 * terminal.
	 */

	/*
	 * Drop anything this window was showing that no longer exists --
	 * closed here, or closed in another window (sec 8.4).
	 */
	for (int i = m_shown.size() - 1; i >= 0; --i) {
		if (m_registry->index_of(m_shown.at(i)) < 0) m_shown.removeAt(i);
	}

	/*
	 * A column follows the shown list (sec 8.7), so a session that just
	 * left it has to leave the column too. Before the tab bar is rebuilt,
	 * since in a column the bar is what this empties.
	 */
	sync_tree_with_shown();

	refresh_tab_bar();

	/* The host list's way back to them, counted from the same list. */
	if (m_host_list) m_host_list->set_open_sessions(m_shown);

	/* Detach needs more than one tab, and this is where that changed. */
	refresh_menu_bar();

	/*
	 * The notification and tray tooltip count sessions, never panes or
	 * tabs (sec 5.2).
	 */
	/*
	 * sec 5.1: "started when the first session connects and stopped when
	 * the last disconnects". Tying this to the window's close event, as
	 * it was, is a desktop assumption -- Android has no close event,
	 * because an app is backgrounded with the home button, so the service
	 * would never have started where it matters most.
	 *
	 * On desktop the same call means the tray icon and staying resident;
	 * on Android it means the foreground service. Neither is named here.
	 */
	update_residency();

	/*
	 * And kept awake while a host that asks for it is connected (sec 5.5).
	 * Asked of the whole registry, not of this window: a session in another
	 * window is still a build that a sleep would drop.
	 */
	bssh_platform::instance()->set_sleep_inhibited(m_registry->keeps_awake());

	if (m_shown.isEmpty()) {
		detach_session();
		m_pages->setCurrentIndex(0);
	} else if (!m_current) {
		/*
		 * Something took the current session away and this window still
		 * has others -- closing the tab you were looking at, or another
		 * window adopting it. Show one of them.
		 *
		 * Nothing did, and the window went BLANK: the bar above is
		 * rebuilt under a QSignalBlocker, so currentChanged never fires
		 * and show_session() is never reached, and detach_session()
		 * only ever removes. Measured: close the visible tab of two and
		 * the pane count goes to zero with a live session still in the
		 * list. Every tabbed program since the browsers settled it
		 * promotes a neighbour instead.
		 *
		 * The bar's own current index decides which, so the promotion
		 * agrees with the tab that is drawn as selected rather than
		 * picking a second answer.
		 */
		/* The first pane still showing, or failing that the first session. */
		const QList<bssh_session *> panes = m_layout ? m_layout->visible_panes()
		                                             : QList<bssh_session *>();
		show_session(!panes.isEmpty() ? panes.first() : m_shown.first());
	}

	update_status();
}

void bssh_main_window::save_layout() {
	/*
	 * Profile ids, not session ids: sessions do not survive a restart,
	 * because plain SSH cannot resume a shell (sec 5.3). What can
	 * honestly be remembered is which hosts were open and how they were
	 * arranged.
	 *
	 * Sessions opened from a command-line target have no profile behind
	 * them and are skipped rather than invented into the store.
	 */
	bssh_profile_store::layout_state_t layout;

	for (bssh_session *session : m_registry->sessions()) {
		const int id = session->profile().id;
		if (id == 0) continue;

		if (session == m_current) layout.active_index = layout.profile_ids.size();
		layout.profile_ids.append(id);
	}

	layout.pane_count = qMax(1, m_layout ? m_layout->visible_panes().size() : 1);

	/*
	 * And how they were arranged -- the whole shape, not only what fits
	 * this window now, so a window reopened wider gets back what was
	 * built (sec 8.1). A pane with no profile behind it is left out and
	 * the shape closes around it, the way the host list above skips it.
	 */
	if (m_layout && m_layout->intent().size() > 1) {
		layout.shape = bssh_layout_manager::shape_text(m_layout->tree(),
		        [](bssh_session *session) {
			const int id = session->profile().id;
			return id == 0 ? QString() : QString::number(id);
		});
	}

	m_profile_store->save_layout(layout);
}

void bssh_main_window::restore_layout() {
	const bssh_profile_store::layout_state_t layout = m_profile_store->layout();
	if (layout.is_empty()) return;

	/*
	 * Every remembered host comes back as a tab, and **not one of them
	 * connects** (sec 8.4).
	 *
	 * This used to reopen nothing at all and merely preselect the
	 * last-active host in the front door, because reopening them was
	 * "a queue of password prompts". Key authentication (M6) removed
	 * most of that, but the decision taken here is stronger and simpler:
	 * a restore raises NO prompt of any kind. Not a password, not a
	 * passphrase, not a trust-on-first-use question.
	 *
	 * The reason is what a restore is. It happens before the user has
	 * asked for anything -- they launched the app, and that is all. A
	 * modal stack on startup is the worst possible moment to ask about a
	 * host key, because the one habit this app must not teach is
	 * clicking through a warning to get to the thing you actually wanted.
	 * And on a phone, connecting several hosts unasked wakes the radio
	 * for sessions nobody has looked at yet.
	 *
	 * So the arrangement comes back and the connections wait. A
	 * disconnected tab is one click from live -- Connect, on the tab's
	 * own menu (sec 8.3) -- which is a click the user makes knowing which
	 * host it is for.
	 */
	for (int id : layout.profile_ids) {
		const bssh_profile_t profile = m_profile_store->profile(id);

		/*
		 * A host deleted since the last run simply does not come back.
		 * Its id is left in the saved layout rather than repaired here:
		 * the next save writes what is actually open.
		 */
		if (profile.is_valid()) prepare_profile(profile);
	}

	if (m_shown.isEmpty()) return;

	/*
	 * The split arrangement too, which was saved from M8 onward and never
	 * restored, because nothing had ever reopened enough sessions for
	 * there to be an arrangement. Bounded by what the current window can
	 * actually show: the window may have been reopened narrower, or on a
	 * smaller screen, and sec 8.2's answer to that is tabs.
	 */
	/*
	 * The saved shape first. Each leaf is a profile id, resolved to a
	 * session of that host not already placed -- one host can be open
	 * twice -- and a leaf for a host that did not come back is dropped.
	 */
	int panes = 1;
	QList<bssh_session *> placed;
	const bssh_pane_node shape = bssh_layout_manager::parse_shape(layout.shape,
	        [this, &placed](const QString &name) -> bssh_session * {
		bool ok = false;
		const int id = name.toInt(&ok);
		if (!ok) return nullptr;
		for (bssh_session *session : m_shown) {
			if (session->profile().id == id && !placed.contains(session)) {
				placed.append(session);
				return session;
			}
		}
		return nullptr;
	});

	if (!shape.is_leaf() && !shape.is_empty()) {
		m_layout->set_tree(shape);

		/*
		 * The shape replaces the whole tree, and it names only what the
		 * window it was saved from showed -- the hosts list is the whole
		 * registry's, every window's. A host it does not name is put back
		 * as a tab, or it would be open here and in no pane and no bar.
		 */
		sync_tree_with_shown();
		panes = m_layout->intent().size();
	} else {
		/* A layout saved before panes could nest: the first N, side by side. */
		panes = qMin(layout.pane_count, m_shown.size());
		for (int i = 1; i < panes; ++i) split_with(m_shown.at(i));
	}

	/*
	 * Last, so it wins the focus: every prepare_profile above showed its
	 * own session as it arrived, which is the same one-pane-at-a-time
	 * path a tab switch takes.
	 */
	bssh_session *active = m_shown.value(layout.active_index, m_shown.first());
	if (active && panes <= 1) show_session(active);
	else if (active && m_panes && m_layout->intent().contains(active)) m_panes->focus_session(active);

	if (m_session_dialog && active) {
		m_session_dialog->set_current_profile(active->profile());
	}
}

void bssh_main_window::import_launch_document() {
	bssh_platform *platform = bssh_platform::instance();
	if (!platform || !m_profile_store) return;

	const QString document = platform->launch_document();
	if (document.isEmpty()) return;

	bool ok = false;
	QString why;
	bssh_preset_t imported = bssh_preset_t::read_ini(document, &ok, &why);

	if (!ok) {
		/*
		 * Said out loud rather than ignored. Being handed a file and
		 * silently doing nothing with it is indistinguishable from the
		 * app being broken, and the user picked this app on purpose.
		 */
		QMessageBox::warning(this, QStringLiteral("Not a preset"),
		                      why.isEmpty()
		                              ? QStringLiteral("BeerSSH could not read a preset "
		                                                "from that file.")
		                              : why);
		return;
	}

	/* Never merged over an existing one -- see bssh_preset_editor. */
	imported.name = m_profile_store->unique_preset_name(imported.name);

	const int id = m_profile_store->save_preset(imported);
	if (id == 0) {
		/*
		 * Two lines above "Preset imported" this used to return in
		 * silence, so a file that was read and then not stored said
		 * nothing -- the one outcome the not-a-preset warning above
		 * exists to prevent.
		 */
		QMessageBox::warning(this, QStringLiteral("Not imported"),
		                      QStringLiteral("The preset was read, but could not be "
		                                      "added to your presets.\n\n%1")
		                              .arg(m_profile_store->last_error()));
		return;
	}

	QMessageBox::information(
	        this, QStringLiteral("Preset imported"),
	        QStringLiteral("\"%1\" is now available to any host.").arg(imported.name));

	if (m_preset_editor) m_preset_editor->reload();
	reresolve_sessions(id);
}

void bssh_main_window::refresh_sessions_from_store() {
	if (!m_registry || !m_profile_store) return;

	for (bssh_session *session : m_registry->sessions()) {
		const int id = session->profile().id;
		if (id <= 0) continue;   /* a local shell has no row to follow */

		const bssh_profile_t stored = m_profile_store->profile(id);
		if (stored.id != id) continue;   /* deleted while open: leave it alone */

		session->set_profile(stored);
		session->set_settings(resolved_settings(stored));
	}
}

void bssh_main_window::reresolve_sessions(int preset_id) {
	if (!m_registry) return;

	for (bssh_session *session : m_registry->sessions()) {
		/*
		 * Only the hosts that follow this preset. Re-resolving every
		 * session would work, but it would also mean a font change on one
		 * preset briefly reflowing the grid of a host that has nothing to
		 * do with it, and a window-change message sent to a remote for no
		 * reason.
		 */
		if (session->profile().preset_id != preset_id) continue;

		session->set_settings(resolved_settings(session->profile()));
	}
}

QJsonObject bssh_main_window::resolved_settings(const bssh_profile_t &profile) const {
	if (!m_profile_store) return bssh_settings_resolver::app_defaults();

	/*
	 * preset_id 0 is "follows nothing", which is what a host created from
	 * a command-line target has. It resolves to the app defaults rather
	 * than to a preset that does not exist, so such a session still gets
	 * a font and a size rather than an empty object.
	 */
	if (profile.preset_id == 0) return bssh_settings_resolver::app_defaults();

	const bssh_preset_t preset = m_profile_store->preset(profile.preset_id);
	return bssh_settings_resolver::resolve(preset, profile.overrides);
}

void bssh_main_window::set_session_note(bssh_session *session, const QString &text) {
	if (!session) return;
	if (text.isEmpty()) m_notes.remove(session);
	else m_notes.insert(session, text);
	if (bssh_terminal_view *view = m_panes ? m_panes->view_for(session) : nullptr)
		view->set_status_note(text);
}

void bssh_main_window::flash_session_notice(bssh_session *session, const QString &text) {
	if (bssh_terminal_view *view = m_panes ? m_panes->view_for(session) : nullptr)
		view->flash_notice(text);
}

void bssh_main_window::on_failed(const QString &reason) {
	m_error = reason;
	update_status();
}

void bssh_main_window::on_grid_resized(int cols, int rows) {
	/*
	 * window-change for the session on screen only (sec 7). With panes at
	 * M8 each computes its own geometry, so this stays per-session rather
	 * than becoming a window-wide broadcast.
	 *
	 * From the focused view only. Every view that has ever had focus is
	 * still connected here (they are wired once each), so an unfocused
	 * pane's resize gave the FOCUSED session that pane's size -- the
	 * wrong window size for the remote -- and, while the container
	 * rebuilt, read a focused view it had already deleted. The pane's own
	 * session is resized by the container's connection either way.
	 */
	if (!m_terminal_view || sender() != m_terminal_view) return;

	if (m_current) {
		m_current->emulator()->set_cell_pixels(m_terminal_view->cell_width(),
		                                        m_terminal_view->cell_height());
		m_current->resize(cols, rows);
	}

	update_status();
}

/*
 * Which layout the terminal menu gets, from the setting and the device.
 *
 * The same shape as the key row's and the theme's: AUTO asks the device,
 * anything else is the user overruling it. Separated out so the rule is
 * one function rather than a condition inside a menu builder.
 */
bssh_app_settings::context_menu_t bssh_main_window::context_menu_layout(
        bssh_app_settings::context_menu_t setting, bool touch_primary) {
	if (setting != bssh_app_settings::CONTEXT_MENU_AUTO) return setting;
	return touch_primary ? bssh_app_settings::CONTEXT_MENU_TOUCH
	                      : bssh_app_settings::CONTEXT_MENU_DESKTOP;
}



/*
 * Copy, Paste, Select All -- and the arrangement is not arbitrary.
 *
 * The desktop terminals of the late 90s and 2000s disagreed about the
 * right mouse button and never reconciled. xterm gave it selection
 * extension and put its menus on Ctrl+click; PuTTY made it paste, in the
 * Windows console tradition, and offered a three-way setting because even
 * it could not pick; Konsole and GNOME Terminal put a menu there, and by
 * the mid-2000s that had won on the desktop this app is built for. So a
 * menu it is -- and PuTTY's answer to the disagreement, a setting, is the
 * one worth copying rather than its default.
 *
 * The order and the details are the Windows Interface Guidelines' (1995),
 * which every one of those programs was written against and which have
 * not been improved on: the edit verbs first in their canonical order,
 * object-specific commands after a separator, nothing hidden. Items stay
 * VISIBLE and disabled rather than disappearing, so the menu is the same
 * shape every time it opens and the hand learns where Paste is -- a menu
 * that reflows around what happens to be available is one nobody can use
 * without reading. Access keys on every item, for the same reason.
 *
 * Touch changes the metrics and nothing else. iOS and Android converged on
 * the same three verbs from a long press -- and on much larger targets, so
 * the rows grow to sec 6.2's floor. What does not change is which verbs
 * they are: a phone user reaching for Paste should find the same word in
 * the same place.
 */
/*
 * The menu bar (sec 8.3), and its shape is the 1995 Windows Interface
 * Guidelines' rather than anything invented here -- the same authority
 * the terminal's context menu already follows, and the one every desktop
 * program this app is modelled on was written against.
 *
 * File, Edit, View, Settings, Help, in that order. Konsole is the closest
 * sibling and uses the same set; mIRC and PuTTY differ in their middles
 * but not in their ends, because File-first and Help-last is the part
 * everybody agreed on and the part a hand learns.
 *
 * Items are VISIBLE and disabled rather than hidden, for the reason the
 * context menu gives: a menu that reflows around what happens to be
 * available is one nobody can use without reading it every time. Access
 * keys on every item, likewise.
 *
 * The shortcut text comes from the ROUTER rather than being written here.
 * Chords are rebindable (sec 6.1), and a menu that printed Ctrl+Shift+T
 * beside an item bound to something else would be the most confidently
 * wrong thing on the screen.
 */
void bssh_main_window::build_menu_bar() {
	m_menu_bar = new QMenuBar(this);

	const auto chord = [](const char *name) {
		return QKeySequence(bssh_input_router::binding_for(QString::fromLatin1(name)));
	};

	QMenu *file = m_menu_bar->addMenu(QStringLiteral("&File"));

	QAction *new_tab = file->addAction(QStringLiteral("New &Tab"));
	new_tab->setShortcut(chord("new-tab"));
	connect(new_tab, &QAction::triggered, this, [this] { open_another_session(false); });

	QAction *new_window = file->addAction(QStringLiteral("New &Window"));
	new_window->setShortcut(chord("new-window"));
	connect(new_window, &QAction::triggered, this, [this] { open_another_session(true); });

	file->addSeparator();

	QAction *close_tab = file->addAction(QStringLiteral("&Close Tab"));
	connect(close_tab, &QAction::triggered, this, [this] {
		/* The session in front of the focused pane: the tab being looked at. */
		if (m_current) m_registry->close(m_current);
	});
	m_session_actions.append(close_tab);

	file->addSeparator();

	/*
	 * A new BeerSSH, taking every live session from this one (sec 8.4):
	 * what an upgrade needs, so that installing a new version does not
	 * end every connection and every screen. Offered only where this
	 * process can hand sessions over.
	 */
	if (bssh_handoff_point::instance()) {
		QAction *restart = file->addAction(QStringLiteral("&Restart, Keeping Sessions"));
		restart->setObjectName(QStringLiteral("menu_restart_keeping_sessions"));
		connect(restart, &QAction::triggered, this, [] { restart_keeping_sessions(); });
	}

	QAction *quit = file->addAction(QStringLiteral("E&xit"));
	connect(quit, &QAction::triggered, this, &QWidget::close);

	QMenu *edit = m_menu_bar->addMenu(QStringLiteral("&Edit"));

	m_copy_action = edit->addAction(QStringLiteral("&Copy"));
	m_copy_action->setObjectName(QStringLiteral("menu_copy"));
	m_copy_action->setShortcut(chord("copy"));
	connect(m_copy_action, &QAction::triggered, this, [this] {
		if (!m_terminal_view) return;
		const QString selected = m_terminal_view->selection_text();
		if (!selected.isEmpty()) QGuiApplication::clipboard()->setText(selected);
	});

	m_paste_action = edit->addAction(QStringLiteral("&Paste"));
	m_paste_action->setObjectName(QStringLiteral("menu_paste"));
	m_paste_action->setShortcut(chord("paste"));
	connect(m_paste_action, &QAction::triggered, this, [this] {
		paste_into_current(QGuiApplication::clipboard()->text());
	});

	edit->addSeparator();

	QAction *select_all = edit->addAction(QStringLiteral("Select &All"));
	connect(select_all, &QAction::triggered, this, [this] {
		if (m_terminal_view) m_terminal_view->select_all();
	});
	m_session_actions.append(select_all);

	/*
	 * Find, which had exactly one route -- Ctrl+Shift+F -- and so was
	 * discoverable only by somebody who already knew it was there.
	 * sec 6.1 calls the search "the thing a long scrollback is FOR", and
	 * Edit is where every desktop application has kept it since the
	 * menus were invented.
	 */
	QAction *find = edit->addAction(QStringLiteral("&Find..."));
	find->setObjectName(QStringLiteral("menu_find"));
	find->setShortcut(chord("find"));
	connect(find, &QAction::triggered, this, [this] { open_find(); });
	m_session_actions.append(find);

	QAction *clear_history = edit->addAction(QStringLiteral("Clear Scroll&back"));
	clear_history->setObjectName(QStringLiteral("menu_clear_scrollback"));
	connect(clear_history, &QAction::triggered, this, [this] { clear_scrollback(); });
	m_session_actions.append(clear_history);

	/*
	 * Flat here, grouped on the context menu, and the difference is not
	 * an oversight: a menu BAR has no 640dp ceiling to respect, and
	 * konsole keeps both of these in Edit. The two menus already differ
	 * in shape for the same kind of reason -- this one has no Pane or
	 * Session group, because a bar has File and View to put them in.
	 */
	QAction *reset_terminal = edit->addAction(QStringLiteral("&Reset Terminal"));
	reset_terminal->setObjectName(QStringLiteral("menu_reset_terminal"));
	connect(reset_terminal, &QAction::triggered, this, [this] { reset_terminal_state(); });
	m_session_actions.append(reset_terminal);

	QMenu *view = m_menu_bar->addMenu(QStringLiteral("&View"));

	/*
	 * The menu bar itself (sec 8.7a). Hidden, this item is still in the
	 * corner's menus, under the bar's own -- the way back.
	 */
	m_menu_bar_action = view->addAction(QStringLiteral("Show &Menu Bar"));
	m_menu_bar_action->setObjectName(QStringLiteral("show_menu_bar"));
	m_menu_bar_action->setCheckable(true);
	m_menu_bar_action->setChecked(bssh_app_settings().menu_bar_shown());
	connect(m_menu_bar_action, &QAction::toggled, this, [this](bool shown) {
		bssh_app_settings().set_menu_bar_shown(shown);
		apply_menu_bar();
	});
	view->addSeparator();

	m_split_action = view->addAction(QStringLiteral("Split &Beside Current"));
	connect(m_split_action, &QAction::triggered, this, [this] {
		split_beside_current();
	});

	/*
	 * "Detach to a Window", and it takes the session you are LOOKING at
	 * rather than whichever tab happens to be current.
	 *
	 * It read m_tabs->currentIndex() and was called "Detach Tab to a
	 * Window", which is wrong twice over on a wide screen. A split shows
	 * sessions as PANES, and a pane is not in the tab bar (sec 8.3) --
	 * so the item either acted on a different session than the one with
	 * focus, or, with no tab at all, on nothing. Both read as "it does
	 * nothing" from the other side of the screen.
	 *
	 * It is also the only route a PANE has out of a window. Dragging one
	 * out is Alt and a drag, which works and which nobody discovers, so
	 * a menu item that does it is the discoverable half of the same
	 * gesture.
	 */
	m_detach_action = view->addAction(QStringLiteral("&Detach to a Window"));
	connect(m_detach_action, &QAction::triggered, this, [this] {
		if (m_current) tear_out(m_current);
	});

	/*
	 * Zoom, which had a chord and a touch menu and nothing a desktop
	 * user could find. The argument is the one Find already carries on
	 * this bar: the chords still work, so this costs nobody who already
	 * knew them and it costs everybody who did not. Every terminal with
	 * a menu bar keeps these in View, which is also where this one keeps
	 * the other things that change how the window looks.
	 *
	 * Flat rather than in a Text Size group, for the reason Clear
	 * Scrollback and Reset Terminal are flat in Edit: a menu bar has no
	 * 640dp ceiling to respect, and the group on the context menu exists
	 * because that one does.
	 *
	 * The chord is read from the router rather than written here, so a
	 * rebinding moves the label with it -- a literal would be a second
	 * list, wrong the moment somebody changes one.
	 */
	view->addSeparator();

	QAction *zoom_in = view->addAction(QStringLiteral("&Bigger Text"));
	zoom_in->setObjectName(QStringLiteral("menu_zoom_in"));
	zoom_in->setShortcut(chord("zoom-in"));
	connect(zoom_in, &QAction::triggered, this, [this] { zoom_current(1); });
	m_session_actions.append(zoom_in);

	QAction *zoom_out = view->addAction(QStringLiteral("&Smaller Text"));
	zoom_out->setObjectName(QStringLiteral("menu_zoom_out"));
	zoom_out->setShortcut(chord("zoom-out"));
	connect(zoom_out, &QAction::triggered, this, [this] { zoom_current(-1); });
	m_session_actions.append(zoom_out);

	QAction *zoom_reset = view->addAction(QStringLiteral("&Normal Text Size"));
	zoom_reset->setObjectName(QStringLiteral("menu_zoom_reset"));
	zoom_reset->setShortcut(chord("zoom-reset"));
	connect(zoom_reset, &QAction::triggered, this, [this] { reset_zoom_current(); });
	m_session_actions.append(zoom_reset);

	QMenu *settings = m_menu_bar->addMenu(QStringLiteral("&Settings"));

	QAction *front_door = settings->addAction(QStringLiteral("&Hosts and Settings..."));
	connect(front_door, &QAction::triggered, this, &bssh_main_window::show_front_door);

	QMenu *help = m_menu_bar->addMenu(QStringLiteral("&Help"));

	QAction *about = help->addAction(QStringLiteral("&About BeerSSH"));
	connect(about, &QAction::triggered, this, [this] {
		/*
		 * The version is a define the project files set. Both of them
		 * do now -- test/tests.pro gained it when the emulator started
		 * answering XTVERSION with this program's own name -- so the
		 * guard no longer distinguishes the app build from the test
		 * one. It stays because it is still true of a build that sets
		 * neither, and "development build" is a better thing to show
		 * than a compile error.
		 */
#ifdef BSSH_VERSION_STRING
		const QString version = QStringLiteral(BSSH_VERSION_STRING);
#else
		const QString version = QStringLiteral("development build");
#endif
		/*
		 * The copyright line comes from the same constant --version
		 * prints, so the two surfaces cannot drift. Escaped, because
		 * QMessageBox::about renders rich text and the address would
		 * otherwise be eaten as an unknown tag.
		 */
		QMessageBox::about(this, QStringLiteral("About BeerSSH"),
		                    QStringLiteral("<b>BeerSSH</b><br><br>"
		                                    "An SSH terminal for desktop and Android."
		                                    "<br>Version %1<br><br>%2")
		                            .arg(version,
		                                  QString(QStringLiteral(BSSH_COPYRIGHT_LINE))
		                                          .toHtmlEscaped()));
	});

	/*
	 * The clipboard is the third thing the menu bar depends on, and it
	 * was the one nothing watched. Copy is re-derived on
	 * selection_changed and both verbs on session and binding changes;
	 * Paste's own input had no subscription, so the bar could not have
	 * asked whether there was anything to paste and settled for whether
	 * there was a session.
	 *
	 * Cheap to answer often: refresh_menu_bar sets a handful of enabled
	 * flags, and this fires when a clipboard changes rather than while
	 * one is being read.
	 */
	connect(QGuiApplication::clipboard(), &QClipboard::dataChanged,
	         this, &bssh_main_window::refresh_menu_bar);

	refresh_menu_bar();
}

/*
 * Enabled state and shortcut labels, re-derived rather than remembered.
 * Called whenever the session, the bindings or the CLIPBOARD change --
 * all three can, and Paste depends on the last of them.
 */
bool bssh_main_window::can_copy() const {
	return m_terminal_view && m_terminal_view->has_selection();
}

bool bssh_main_window::can_paste() const {
	/*
	 * An empty clipboard means Paste does nothing, so it is offered only
	 * when there is something to paste -- which is what every editor
	 * does, and what the right-click menu already did while the menu bar
	 * settled for "is there a session".
	 *
	 * The bar could not have asked this before: it re-derives on session
	 * and binding changes, and the clipboard is neither, so the answer
	 * would have been whatever was true when the last session appeared.
	 * The constructor subscribes to QClipboard::dataChanged for exactly
	 * this, the way selection_changed already drives the Copy half.
	 */
	return m_current && !QGuiApplication::clipboard()->text().isEmpty();
}

void bssh_main_window::refresh_menu_bar() {
	if (!m_menu_bar) return;

	const bool has_session = m_current != nullptr;

	for (QAction *action : m_session_actions) action->setEnabled(has_session);

	if (m_copy_action) {
		m_copy_action->setEnabled(can_copy());
		m_copy_action->setShortcut(
		        QKeySequence(bssh_input_router::binding_for(QStringLiteral("copy"))));
	}

	if (m_paste_action) {
		m_paste_action->setEnabled(can_paste());
		m_paste_action->setShortcut(
		        QKeySequence(bssh_input_router::binding_for(QStringLiteral("paste"))));
	}

	/*
	 * A split needs somewhere to put the pane, which is the same question
	 * drop_zone_for() asks of a side -- so the menu says no where there is
	 * no room, rather than doing something else instead.
	 */
	if (m_split_action) {
		const bool room = m_layout && m_layout->room_to_split(m_current, bssh_layout_manager::EDGE_RIGHT);
		m_split_action->setEnabled(has_session && room);
	}

	/* Detaching the only tab would leave an empty window behind. */
	if (m_detach_action) m_detach_action->setEnabled(m_shown.size() > 1);
}

QString bssh_main_window::drop_destination(bssh_session *session, const QString &directory) const {
	const QString where = directory.isEmpty() ? QStringLiteral("~") : directory;
	if (!session || session->profile().local) return where;
	return session->profile().host + QLatin1Char(':') + where;
}

QMenu *bssh_main_window::build_drop_menu(QWidget *parent, bssh_session *session,
                                         const bssh_working_directory_t &found, int count) const {
	QMenu *menu = new QMenu(parent);
	menu->setObjectName(QStringLiteral("drop_menu"));

	/*
	 * Where it goes, in the entry itself: the place was found by looking,
	 * and the menu is where a wrong answer is caught before anything is
	 * written. A shell connected onward -- ssh to a third machine -- has
	 * its "here" on a host this connection cannot write to, and says so
	 * rather than copying to the wrong one; one whose directory could not
	 * be found offers the home directory under that name.
	 */
	QAction *copy = nullptr;
	if (found.elsewhere) {
		copy = menu->addAction(QStringLiteral("Copy here -- not from this tab: it is running %1")
		                                .arg(found.foreground));
		copy->setEnabled(false);
	} else if (found.path.isEmpty()) {
		copy = menu->addAction(QStringLiteral("Copy to the home directory (%1)")
		                                .arg(drop_destination(session, QString())));
	} else {
		copy = menu->addAction(QStringLiteral("Copy here (%1)").arg(drop_destination(session, found.path)));
	}
	copy->setObjectName(QStringLiteral("drop_copy"));

	QAction *paste = menu->addAction(count == 1 ? QStringLiteral("Paste the path")
	                                            : QStringLiteral("Paste the paths"));
	paste->setObjectName(QStringLiteral("drop_paste"));
	return menu;
}

void bssh_main_window::act_on_drop_menu(QMenu *menu, const QAction *chosen, bssh_session *session,
                                        const QStringList &paths, const QString &as_text,
                                        const bssh_working_directory_t &found) {
	if (!menu || !chosen || !session) return;
	if (chosen == menu->findChild<QAction *>(QStringLiteral("drop_paste"))) {
		paste_into_current(as_text);
		return;
	}
	if (chosen != menu->findChild<QAction *>(QStringLiteral("drop_copy")) || found.elsewhere) return;

	/*
	 * Said over the pane as it goes, and once it is done: where, and the
	 * names written -- which differ from the files dropped wherever one of
	 * that name was already there (bssh_free_name).
	 */
	const QPointer<bssh_session> held = session;
	const QString count = paths.size() == 1 ? QFileInfo(paths.first()).fileName()
	                                        : QStringLiteral("%1 items").arg(paths.size());
	auto progress = std::make_shared<QMetaObject::Connection>();
	auto finished = std::make_shared<QMetaObject::Connection>();
	*progress = connect(session, &bssh_session::upload_progress, this,
	                    [this, held, count, found](qint64 done, qint64 total) {
		if (!held) return;
		const int percent = total > 0 ? int(done * 100 / total) : 100;
		flash_session_notice(held, QStringLiteral("Copying %1 to %2 -- %3%")
		                                   .arg(count, drop_destination(held, found.path)).arg(percent));
	});
	*finished = connect(session, &bssh_session::upload_finished, this,
	                    [this, held, progress, finished](bool ok, const QString &directory,
	                                                     const QStringList &written, const QString &why) {
		disconnect(*progress);
		disconnect(*finished);
		if (!held) return;
		QString names = written.mid(0, 3).join(QStringLiteral(", "));
		if (written.size() > 3) names += QStringLiteral(" and %1 more").arg(written.size() - 3);
		flash_session_notice(held, ok ? QStringLiteral("Copied %1 to %2")
		                                        .arg(names, drop_destination(held, directory))
		                              : QStringLiteral("Copy failed: %1").arg(why));
	});
	session->upload(paths, found.path);
}

void bssh_main_window::offer_file_drop(const QStringList &paths, const QString &as_text,
                                       const QPoint &global) {
	bssh_session *session = m_current;
	if (!session) {
		paste_into_current(as_text);
		return;
	}

	/*
	 * Asked first, shown once answered: the menu names the destination,
	 * and finding it is a round trip on the connection. One answer, then
	 * the connection is let go.
	 */
	const QPointer<bssh_session> held = session;
	auto answered = std::make_shared<QMetaObject::Connection>();
	*answered = connect(session, &bssh_session::working_directory_found, this,
	                    [this, held, answered, paths, as_text, global](const bssh_working_directory_t &found) {
		disconnect(*answered);
		if (!held) return;
		QMenu *menu = build_drop_menu(this, held, found, int(paths.size()));
		const QAction *chosen = menu->exec(global);
		act_on_drop_menu(menu, chosen, held, paths, as_text, found);
		menu->deleteLater();
	});
	session->find_working_directory();
}

QMenu *bssh_main_window::build_terminal_menu(QWidget *parent, const QString &link) const {
	const bssh_app_settings::context_menu_t layout =
	        context_menu_layout(bssh_app_settings().context_menu(),
	                             bssh_platform::instance()
	                                     && bssh_platform::instance()->is_touch_primary());

	QMenu *menu = new QMenu(parent);
	menu->setObjectName(QStringLiteral("terminal_context_menu"));

	/*
	 * The chords come from the ROUTER, not from a literal. These two were
	 * written out as "Ctrl+Shift+C" and "Ctrl+Shift+V" while every other
	 * menu here asks binding_for() -- so a user who rebound copy got a
	 * context menu still advertising the chord they had just replaced.
	 *
	 * That is the failure the router's own table exists to prevent, in
	 * its words: "the list a user is shown and the list the router acts
	 * on cannot be two lists". A hardcoded label is a second list that
	 * happens to agree until somebody changes the first.
	 */
	/*
	 * First, and only when there is one. An entry that is always present
	 * and usually disabled teaches a user to ignore it; an entry that
	 * appears exactly when it will work is the one they read.
	 *
	 * The label says the address, elided, because "Open link" alone asks
	 * somebody to authorise something they cannot see -- and the whole
	 * reason this asks at all is that the URL came from the remote.
	 */
	if (!link.isEmpty()) {
		QAction *open_link = menu->addAction(
		        QStringLiteral("&Open %1")
		                .arg(bssh_uri_for_prompt(link, BSSH_URI_MENU_CHARS)));
		open_link->setObjectName(QStringLiteral("terminal_open_link"));
		menu->addSeparator();
	}

	QAction *copy = menu->addAction(QStringLiteral("&Copy"));
	copy->setObjectName(QStringLiteral("terminal_copy"));
	copy->setShortcut(
	        QKeySequence(bssh_input_router::binding_for(QStringLiteral("copy"))));
	copy->setEnabled(can_copy());

	QAction *paste = menu->addAction(QStringLiteral("&Paste"));
	paste->setObjectName(QStringLiteral("terminal_paste"));
	paste->setShortcut(
	        QKeySequence(bssh_input_router::binding_for(QStringLiteral("paste"))));
	paste->setEnabled(can_paste());

	menu->addSeparator();

	QAction *select_all = menu->addAction(QStringLiteral("Select &All"));
	select_all->setObjectName(QStringLiteral("terminal_select_all"));

	/*
	 * And the same verb where a phone can reach it. The chord is the
	 * only other way in, so before this a device with no keyboard could
	 * not search its scrollback at all -- the shape sec 8.7 was written
	 * about, on the feature sec 6.1 calls the point of keeping history.
	 *
	 * Beside Select All rather than in a group: it acts on the text in
	 * front of you, which is what the verbs above it do.
	 */
	QAction *find = menu->addAction(QStringLiteral("&Find..."));
	find->setObjectName(QStringLiteral("terminal_find"));
	find->setShortcut(
	        QKeySequence(bssh_input_router::binding_for(QStringLiteral("find"))));
	find->setEnabled(m_current != nullptr);

	/*
	 * Dropping the history, which only the REMOTE could ask for until
	 * now: ED 3 is what `clear` sends, and sec 6.4 records why somebody
	 * runs it -- "before pasting a password". A user who wanted their own
	 * scrollback gone had to hope the far end cooperated, which a
	 * disconnected session cannot do at all.
	 *
	 * No confirmation, which is konsole's behaviour and the honest one:
	 * the scrollback is not a document, the screen is untouched, and an
	 * "are you sure" on a privacy action is a prompt that makes people
	 * hesitate at exactly the wrong moment.
	 */
	/*
	 * Putting the terminal back in order: dropping the history, and
	 * clearing the MODES a dead program left behind.
	 *
	 * A group rather than two rows, and the reason is the measured one
	 * this menu keeps meeting: at 13 top-level rows it is 584px of a
	 * 640dp cover screen, and a fourteenth would not fit. One row that
	 * opens two is what does.
	 */
	QMenu *terminal_menu = menu->addMenu(QStringLiteral("&Terminal"));
	terminal_menu->setObjectName(QStringLiteral("terminal_state_menu"));

	QAction *clear_history = terminal_menu->addAction(QStringLiteral("Clear Scroll&back"));
	clear_history->setObjectName(QStringLiteral("terminal_clear_scrollback"));
	clear_history->setEnabled(m_current != nullptr);

	/*
	 * The soft reset a wedged terminal needs, and the one route to it
	 * that does not require a working shell. `cat` of a binary leaves
	 * mouse reporting on, the alternate screen up, or the keypad in
	 * application mode; the cure is `reset`, which has to be TYPED into
	 * the shell that is wedged -- and on a session that has dropped
	 * there is no shell to type it into at all.
	 *
	 * Soft, like the reconnect's: sec 5.3 argues it, and the hard branch
	 * homes the cursor and erases the screen, which is the history
	 * somebody reaching for this is trying to keep.
	 */
	QAction *reset_terminal = terminal_menu->addAction(QStringLiteral("&Reset Terminal"));
	reset_terminal->setObjectName(QStringLiteral("terminal_reset"));
	reset_terminal->setEnabled(m_current != nullptr);

	/* What the connection has carried, and what changes the keys (sec 5.3). */
	QAction *details = terminal_menu->addAction(QStringLiteral("Connection &Details"));
	details->setObjectName(QStringLiteral("terminal_details"));
	details->setEnabled(m_current != nullptr);

	/*
	 * The tmux and screen drawer back (sec 10.2a): listed now, and offered
	 * if anything is running. The way back after its x, and what to reach
	 * for with the setting off. In this group for the reason above: the
	 * menu has no row to spare.
	 */
	QAction *multiplexers = terminal_menu->addAction(QStringLiteral("tmux and screen &Sessions"));
	multiplexers->setObjectName(QStringLiteral("terminal_multiplexers"));
	multiplexers->setEnabled(m_current && m_current->can_offer_multiplexers());

	menu->addSeparator();

	/*
	 * Zoom (sec 9.5), and it is on this menu for the platform that has no
	 * keyboard to press the chords with and no wheel to turn. A phone's
	 * only other route to bigger text is the host's own font size, which
	 * is a settings page, a preset field and a decision about every
	 * future connection -- for something a user wants for the next two
	 * minutes because they are reading a log outdoors.
	 *
	 * Reset is offered whether or not it would do anything, unlike Copy
	 * and Split above: whether a session is zoomed is not visible from
	 * the menu, and an entry that vanishes when the answer is "it is
	 * already at the normal size" is one that cannot say so.
	 */
	/*
	 * A SUBMENU rather than three rows, and the reason is measured
	 * rather than tidy: on touch every row is sec 6.2's 48dp, and this
	 * menu wanted 776px of a 640dp cover screen with the three of them
	 * on it. See the_touch_terminal_menu_fits_a_phone, which is the
	 * assertion that now says so.
	 */
	QMenu *text_size = menu->addMenu(QStringLiteral("Text &Size"));
	text_size->setObjectName(QStringLiteral("terminal_text_size"));

	QAction *zoom_in = text_size->addAction(QStringLiteral("&Bigger"));
	zoom_in->setObjectName(QStringLiteral("terminal_zoom_in"));
	zoom_in->setShortcut(
	        QKeySequence(bssh_input_router::binding_for(QStringLiteral("zoom-in"))));

	QAction *zoom_out = text_size->addAction(QStringLiteral("&Smaller"));
	zoom_out->setObjectName(QStringLiteral("terminal_zoom_out"));
	zoom_out->setShortcut(
	        QKeySequence(bssh_input_router::binding_for(QStringLiteral("zoom-out"))));

	QAction *zoom_reset = text_size->addAction(QStringLiteral("&Normal"));
	zoom_reset->setObjectName(QStringLiteral("terminal_zoom_reset"));
	zoom_reset->setShortcut(
	        QKeySequence(bssh_input_router::binding_for(QStringLiteral("zoom-reset"))));

	menu->addSeparator();

	/*
	 * The session verbs, on the surface a pointer is already over.
	 *
	 * These existed only on the TAB bar's menu, which is a different
	 * target you have to know about -- and on a window with one session
	 * the tab bar is hidden entirely, so splitting was unreachable from
	 * anywhere except a chord nobody had been told. Reported as exactly
	 * that: cannot split a terminal from the right-click menu.
	 *
	 * After the edit verbs and a separator, which is the Windows
	 * Interface Guidelines order this menu already follows: the verbs
	 * everybody expects first, object-specific commands after a rule.
	 * Shown disabled rather than hidden for the same reason as the rest.
	 */
	/*
	 * The four arrangement verbs, grouped for the same measured reason
	 * as Text Size above: with them flat this menu is 776px on a 640dp
	 * screen, and with both groups it is comfortably inside it.
	 *
	 * Grouped rather than dropped, because sec 8.7 put Move Pane here
	 * precisely so touch could reach it -- a submenu is one more tap and
	 * still a route, where a removal is not. That trade is a judgement
	 * about somebody else's menu and is recorded in project.md as one.
	 */
	QMenu *pane_menu = menu->addMenu(QStringLiteral("&Pane"));
	pane_menu->setObjectName(QStringLiteral("terminal_pane_menu"));

	QAction *split = pane_menu->addAction(QStringLiteral("Split &Beside Current"));
	split->setObjectName(QStringLiteral("terminal_split"));
	split->setEnabled(m_current && m_layout
	                   && m_layout->room_to_split(m_current, bssh_layout_manager::EDGE_RIGHT));

	/*
	 * The un-split, and the reason it is a menu entry as well as a
	 * gesture: with two panes and nothing else, this window has no tab
	 * bar to drop a pane onto, so the gesture alone would leave the
	 * split with no way out. The same reasoning as Split's, which is on
	 * this menu because a one-session window has no tab bar either.
	 */
	QAction *as_tab = pane_menu->addAction(QStringLiteral("Show as &Tab"));
	as_tab->setObjectName(QStringLiteral("terminal_show_as_tab"));
	/*
	 * Offered where show_as_tab acts -- two panes or more, in a column as
	 * anywhere, now each pane has a bar (sec 8.3) -- and not otherwise:
	 * an entry that looked available and did nothing when chosen is what
	 * tear_out's own comment calls worse than a refusal.
	 */
	as_tab->setEnabled(m_current && m_layout && m_layout->visible_panes().size() > 1);

	/*
	 * The other way out of this window, and until now it had one route
	 * on one platform. The menu bar's own entry says why it exists --
	 * "the only route a PANE has out of a window. Dragging one out is
	 * Alt and a drag, which works and which nobody discovers" -- and a
	 * phone has neither the menu bar nor the Alt. Same shape as Close
	 * and Duplicate above: a verb sec 8.3 put on a desktop surface, left
	 * there when sec 8.7 took that surface away.
	 *
	 * Beside Split for the reason the menu bar keeps them together: they
	 * are the two halves of one question about where a session lives.
	 * Refused where tear_out would refuse, rather than offered and
	 * silently declining -- "a window will not empty itself" (sec 8.4).
	 */
	QAction *detach = pane_menu->addAction(QStringLiteral("&Detach to a Window"));
	detach->setObjectName(QStringLiteral("terminal_detach"));
	detach->setEnabled(m_current && m_shown.size() > 1);

	/*
	 * Reordering, which is the one arrangement a touch user could not
	 * reach: Split adds a pane and Show as Tab removes one, and neither
	 * moves a pane past its neighbour. The grip's drag does it with a
	 * pointer and cannot by touch -- see move_pane().
	 */
	const QList<bssh_session *> arrangement =
	        m_layout ? m_layout->intent() : QList<bssh_session *>();
	const int here = m_current ? arrangement.indexOf(m_current) : -1;

	QAction *move_left = pane_menu->addAction(QStringLiteral("Move Pane &Left"));
	move_left->setObjectName(QStringLiteral("terminal_move_pane_left"));
	move_left->setEnabled(here > 0);

	QAction *move_right = pane_menu->addAction(QStringLiteral("Move Pane &Right"));
	move_right->setObjectName(QStringLiteral("terminal_move_pane_right"));
	move_right->setEnabled(here >= 0 && here < arrangement.size() - 1);

	/*
	 * The session verbs, and two of them were unreachable on touch until
	 * now. Close and Duplicate lived on the TAB bar and its menu --
	 * sec 8.3 put them there -- and sec 8.7 then dissolved the tab bar on
	 * a phone, because the column holds every session and nothing is
	 * left to be a tab. That section accounted for losing the tab
	 * REORDER and replaced it with the column's Move state; these two
	 * went with them and were not replaced, so a phone could open
	 * sessions and never close one.
	 *
	 * Grouped rather than added flat, and the reason is the measured one
	 * from Text Size and Pane above: every row here is 48dp on touch, and
	 * this menu has less than one row of headroom against a 640dp cover
	 * screen. Four verbs as four rows would not fit; as one submenu they
	 * do, and the group is coherent -- all four are about the SESSION
	 * rather than about the text or the panes.
	 */
	QMenu *session_menu = menu->addMenu(QStringLiteral("&Session"));
	session_menu->setObjectName(QStringLiteral("terminal_session_menu"));

	/*
	 * Connect, first, and only for a session that is not live -- the tab
	 * menu's own rule and its own wording, because it is the same verb.
	 * Absent rather than disabled on a live session, since "connect" on
	 * something already connected is not a state a user should have to
	 * reason about.
	 *
	 * It is here for the reason the rest of this group is: sec 8.4's
	 * restore reopens every remembered host and connects none of them,
	 * and the tab menu that carried this is not reachable where there
	 * are no tabs. That made a restored session on a phone the dead end
	 * the tab menu's entry was written to prevent.
	 */
	if (m_current && !m_current->is_live()) {
		QAction *connect_now = session_menu->addAction(QStringLiteral("Co&nnect"));
		connect_now->setObjectName(QStringLiteral("terminal_connect"));
	}

	/*
	 * Not on touch: there New Tab opens the host list, which the menu's
	 * own Sessions and Hosts entry already does -- the same door twice
	 * (the holder, 2026-10-22).
	 */
	if (!m_host_list) {
		QAction *new_tab = session_menu->addAction(QStringLiteral("New &Tab"));
		new_tab->setObjectName(QStringLiteral("terminal_new_tab"));
		new_tab->setShortcut(
		        QKeySequence(bssh_input_router::binding_for(QStringLiteral("new-tab"))));
	}

	QAction *new_window = session_menu->addAction(QStringLiteral("New &Window"));
	new_window->setObjectName(QStringLiteral("terminal_new_window"));
	new_window->setShortcut(
	        QKeySequence(bssh_input_router::binding_for(QStringLiteral("new-window"))));

	/*
	 * Duplicate opens a new session on the same profile through the front
	 * door's own path (sec 8.3), so a host needing a password is asked for
	 * one: copying the session would mean copying a connection, which SSH
	 * cannot do.
	 */
	QAction *duplicate = session_menu->addAction(QStringLiteral("&Duplicate"));
	duplicate->setObjectName(QStringLiteral("terminal_duplicate"));
	duplicate->setEnabled(m_current != nullptr);

	/*
	 * Behind a separator, because Close is the only verb here that cannot
	 * be undone -- the same shape and the same argument as the tab menu's
	 * own ordering.
	 */
	session_menu->addSeparator();

	QAction *close_session = session_menu->addAction(QStringLiteral("&Close"));
	close_session->setObjectName(QStringLiteral("terminal_close_session"));
	close_session->setEnabled(m_current != nullptr);

	menu->addSeparator();

	/*
	 * The way back to the configuration, from the one surface both
	 * platforms already have (sec 6.4).
	 *
	 * It has to be here because the desktop now opens straight into a
	 * local shell (sec 2): without it, a window that starts in a terminal
	 * has no route to the host list or the settings at all -- the same
	 * one-way door that made the tab bar unreachable on Android, arriving
	 * on the other platform by the other route. Touch reaches the same
	 * page with Back (sec 8.3); this is what a pointer uses, and it works
	 * on both.
	 */
	/*
	 * Named for what that page is on this device. On touch it is the
	 * host list, which lists the open sessions above the hosts since
	 * 2026-10-22 -- and this entry is the way there besides Back, so a
	 * Session > Sessions entry added that day was the same door twice
	 * and was taken out again (the holder).
	 */
	/*
	 * The way back to a hidden menu bar from where the pointer already is
	 * (the holder, 2026-10-31): View holds Show Menu Bar, and View is in
	 * the bar that is hidden -- the corner's menus button reaches it, but
	 * the right button is where a desktop user looks first. A menu-local
	 * entry rather than View's action itself, because the touch layout
	 * rewrites the actions of the menu it is given.
	 */
	if (m_menu_bar && m_menu_bar->isHidden() && m_menu_bar_action) {
		QAction *show_bar = menu->addAction(QStringLiteral("Show &Menu Bar"));
		show_bar->setObjectName(QStringLiteral("terminal_show_menu_bar"));
	}

	QAction *front_door = menu->addAction(m_host_list ? QStringLiteral("&Sessions and Hosts")
	                                                  : QStringLiteral("&Hosts and Settings"));
	front_door->setObjectName(QStringLiteral("terminal_front_door"));

	apply_menu_layout(menu, layout);
	return menu;
}

/*
 * A thumb, not a pointer. sec 6.2's 48dp floor applies to a menu row exactly
 * as it does to a button, and a menu whose items are a pointer's height is
 * one that gets the wrong verb half the time.
 *
 * Shared rather than written into each menu builder, because the metrics are
 * the ONLY thing touch changes and every context menu in this app has to
 * change the same way. The tab menu opens from the same press-and-hold, on
 * the same phone, and had pointer-height rows until this was pulled out.
 */
void bssh_main_window::apply_menu_layout(QMenu *menu,
        bssh_app_settings::context_menu_t layout) {
	if (layout != bssh_app_settings::CONTEXT_MENU_TOUCH) return;

	for (QAction *action : menu->actions()) {
		if (action->isSeparator()) continue;

		/*
		 * A submenu's rows are a menu of their own, and Qt applies
		 * neither the stylesheet below nor this loop to them. Without
		 * recursing, the moment this menu grouped anything the group's
		 * contents went back to pointer-height rows with their chords
		 * still on -- which is the failure the comment on this function
		 * already names, arriving by the one route it did not cover.
		 */
		if (QMenu *child = action->menu()) {
			apply_menu_layout(child, layout);
			continue;
		}

		/*
		 * The shortcut text goes: it is telling somebody with no
		 * keyboard which chord to press, and it is what makes a touch
		 * row wide as well as short.
		 */
		action->setShortcut(QKeySequence());
	}
	/*
	 * The row height itself is touch_metrics', shared with the two menus
	 * on the touch front door which could not reach it here.
	 */
	bssh_apply_touch_menu(menu, true);
}

void bssh_main_window::open_remote_link(const QString &uri) {
	if (uri.isEmpty()) return;

	/*
	 * SHOWN elided, OPENED whole. The question is what the user is being
	 * asked to read, and a URI put in verbatim makes a dialog wider than
	 * the screen -- see bssh_uri_for_prompt. What openUrl gets is the
	 * address itself, because a shortened one is a different one.
	 */
	const QMessageBox::StandardButton answer = QMessageBox::question(
	      this, QStringLiteral("Open link"),
	      QStringLiteral("Open this address, sent by the remote host?\n\n%1")
	              .arg(bssh_uri_for_prompt(uri, BSSH_URI_PROMPT_CHARS)),
	      QMessageBox::Open | QMessageBox::Cancel, QMessageBox::Cancel);

	if (answer == QMessageBox::Open) QDesktopServices::openUrl(QUrl(uri));
}

void bssh_main_window::show_terminal_menu(const QPoint &at) {
	if (!m_terminal_view) return;

	/*
	 * A link under the press, which is the only route a touch user has.
	 * Ctrl+click is the pointer's, and a phone has no Ctrl -- so without
	 * this entry the whole hyperlink feature, marked and inferred alike,
	 * simply did not exist on the platform sec 14 says every milestone
	 * has to land on.
	 *
	 * Asked at the position the menu was opened AT rather than wherever
	 * the cursor now is: a long press has already moved on by the time
	 * the menu appears.
	 */
	const QString link = m_terminal_view->link_at(m_terminal_view->mapFromGlobal(at));

	QMenu *menu = build_terminal_menu(this, link);
	const QAction *chosen = menu->exec(at);
	menu->deleteLater();

	act_on_terminal_menu(menu, chosen, link);
}

/*
 * What a chosen entry DOES, separated from the menu being shown.
 *
 * It was inline after QMenu::exec(), which a headless suite cannot
 * reach: exec() runs its own event loop and returns only when somebody
 * clicks. So every test of this menu could assert that an entry EXISTS
 * and none could assert that choosing it does anything -- and this
 * project has spent the day finding entries that existed and reached
 * nothing. The seam is the remedy evidence.md names: move the
 * composition out of the UI shell into code the suite can call.
 *
 * The menu comes along because the dispatch identifies entries by
 * findChild rather than by pointer identity, which is what lets
 * build_terminal_menu decide what exists without this function knowing.
 */
void bssh_main_window::act_on_terminal_menu(QMenu *menu, const QAction *chosen,
        const QString &link) {
	if (!menu || !chosen) return;

	const QAction *open_link =
	        menu->findChild<QAction *>(QStringLiteral("terminal_open_link"));
	if (open_link && chosen == open_link) {
		open_remote_link(link);
		return;
	}

	const QAction *show_bar =
	        menu->findChild<QAction *>(QStringLiteral("terminal_show_menu_bar"));
	if (show_bar && chosen == show_bar) {
		if (m_menu_bar_action) m_menu_bar_action->setChecked(true);
		return;
	}

	const QAction *copy = menu->findChild<QAction *>(QStringLiteral("terminal_copy"));
	const QAction *paste = menu->findChild<QAction *>(QStringLiteral("terminal_paste"));
	const QAction *select_all =
	        menu->findChild<QAction *>(QStringLiteral("terminal_select_all"));

	if (chosen == copy) {
		const QString selected = m_terminal_view->selection_text();
		if (!selected.isEmpty()) QGuiApplication::clipboard()->setText(selected);
		return;
	}
	if (chosen == paste && m_current) {
		paste_into_current(QGuiApplication::clipboard()->text());
		return;
	}
	if (chosen == select_all) {
		m_terminal_view->select_all();
		return;
	}

	const QAction *reset_terminal =
	        menu->findChild<QAction *>(QStringLiteral("terminal_reset"));
	if (chosen == reset_terminal) {
		reset_terminal_state();
		return;
	}

	const QAction *details =
	        menu->findChild<QAction *>(QStringLiteral("terminal_details"));
	if (chosen == details && m_current) {
		QMessageBox box(this);
		box.setWindowTitle(QStringLiteral("Connection Details"));
		box.setText(m_current->profile().target());
		box.setInformativeText(m_current->details(QDateTime::currentDateTimeUtc()));
		box.exec();
		return;
	}

	const QAction *multiplexers =
	        menu->findChild<QAction *>(QStringLiteral("terminal_multiplexers"));
	if (chosen == multiplexers) {
		if (m_current) m_current->offer_multiplexers_again();
		return;
	}

	const QAction *clear_history =
	        menu->findChild<QAction *>(QStringLiteral("terminal_clear_scrollback"));
	if (chosen == clear_history) {
		clear_scrollback();
		return;
	}

	const QAction *connect_now =
	        menu->findChild<QAction *>(QStringLiteral("terminal_connect"));
	if (connect_now && chosen == connect_now) {
		if (m_current) m_current->connect_to_host();
		return;
	}

	const QAction *duplicate =
	        menu->findChild<QAction *>(QStringLiteral("terminal_duplicate"));
	if (chosen == duplicate) {
		if (m_current) open_profile(m_current->profile());
		return;
	}

	const QAction *close_session =
	        menu->findChild<QAction *>(QStringLiteral("terminal_close_session"));
	if (chosen == close_session) {
		/*
		 * Straight to the registry, which is what every other close
		 * route ends in: the tab bar's button, middle-click and the
		 * tab menu all turn an index into a session and call this.
		 * Here the session is the one in front of you.
		 */
		if (m_current && m_registry) m_registry->close(m_current);
		return;
	}

	const QAction *find = menu->findChild<QAction *>(QStringLiteral("terminal_find"));
	if (chosen == find) {
		open_find();
		return;
	}

	const QAction *zoom_in = menu->findChild<QAction *>(QStringLiteral("terminal_zoom_in"));
	if (chosen == zoom_in) {
		zoom_current(1);
		return;
	}

	const QAction *zoom_out =
	        menu->findChild<QAction *>(QStringLiteral("terminal_zoom_out"));
	if (chosen == zoom_out) {
		zoom_current(-1);
		return;
	}

	const QAction *zoom_reset =
	        menu->findChild<QAction *>(QStringLiteral("terminal_zoom_reset"));
	if (chosen == zoom_reset) {
		reset_zoom_current();
		return;
	}

	const QAction *split = menu->findChild<QAction *>(QStringLiteral("terminal_split"));
	if (chosen == split) {
		split_beside_current();
		return;
	}

	const QAction *move_left =
	        menu->findChild<QAction *>(QStringLiteral("terminal_move_pane_left"));
	if (chosen == move_left) {
		move_pane(m_current, -1);
		return;
	}

	const QAction *move_right =
	        menu->findChild<QAction *>(QStringLiteral("terminal_move_pane_right"));
	if (chosen == move_right) {
		move_pane(m_current, 1);
		return;
	}

	const QAction *as_tab =
	        menu->findChild<QAction *>(QStringLiteral("terminal_show_as_tab"));
	if (chosen == as_tab) {
		show_as_tab(m_current);
		return;
	}

	const QAction *detach = menu->findChild<QAction *>(QStringLiteral("terminal_detach"));
	if (chosen == detach) {
		if (m_current) tear_out(m_current);
		return;
	}

	/* Null-checked: absent on touch, and a dismissed menu answers null too. */
	const QAction *new_tab = menu->findChild<QAction *>(QStringLiteral("terminal_new_tab"));
	if (new_tab && chosen == new_tab) {
		open_another_session(false);
		return;
	}


	const QAction *new_window =
	        menu->findChild<QAction *>(QStringLiteral("terminal_new_window"));
	if (chosen == new_window) {
		open_another_session(true);
		return;
	}

	const QAction *front_door =
	        menu->findChild<QAction *>(QStringLiteral("terminal_front_door"));
	if (chosen == front_door) show_front_door();
}

void bssh_main_window::show_front_door() {
	/*
	 * Whichever one this device built (sec 8.5) -- only the chosen front
	 * door is constructed, so this asks rather than deciding again.
	 */
	if (m_host_list) {
		m_pages->setCurrentWidget(m_host_list);
		return;
	}
	if (!m_session_dialog) return;

	/*
	 * The intent is recorded whether or not this window is on screen yet,
	 * and the showing happens in showEvent(). A constructor that put a
	 * window on screen would be the same mistake open_default_session()
	 * and import_launch_document() were moved out of the constructor to
	 * avoid: building a widget tree is not running a program, and the
	 * fifteen windows the suite constructs have no business opening
	 * anything.
	 */
	m_front_door_up = true;
	if (!isVisible()) return;

	m_session_dialog->show();
	m_session_dialog->raise();
	m_session_dialog->activateWindow();
}

void bssh_main_window::hide_front_door() {
	m_front_door_up = false;
	if (m_session_dialog) m_session_dialog->hide();
}

/*
 * sec 8.3's tab context menu. "Show beside current" is how a split is
 * MADE -- until this existed the pane container could show four panes and
 * nothing in the app could ask it to show two.
 *
 * A method rather than a lambda on the right-click signal, because two
 * gestures reach it now and the alternative is the same menu built in two
 * places.
 */
QMenu *bssh_main_window::build_tab_menu(bssh_session *session, QWidget *parent) const {
	if (!session || !m_layout) return nullptr;

	/* Where it sits in its own pane's bar (sec 8.3's bar per pane). */
	const QList<bssh_session *> tabs = m_layout->tabs(session);
	const int index = tabs.indexOf(session);
	if (index < 0) return nullptr;

	QMenu *menu = new QMenu(parent);
	menu->setObjectName(QStringLiteral("tab_context_menu"));

	/*
	 * sec 8.3's three verbs. Duplicate first because it is the one people
	 * reach for most -- a second shell on the same host is the everyday
	 * action, and a split is not -- then the split, then a separator and
	 * the destructive one, which is the terminal menu's shape (sec 6.4)
	 * and the Windows guidelines' before that. Access keys on all three.
	 */
	/*
	 * Connect, first, and only for a session that is not live.
	 *
	 * A restore reopens every remembered host as a tab and connects none
	 * of them (sec 8.4), so without this a restored tab would be a dead
	 * end -- the same one-way door that made the whole tab bar
	 * unreachable on Android until sec 8.3's Back was added. Absent
	 * rather than disabled on a live session, because "connect" on
	 * something already connected is not a state a user should have to
	 * reason about.
	 */
	if (!session->is_live()) {
		QAction *connect_now = menu->addAction(QStringLiteral("Co&nnect"));
		connect_now->setObjectName(QStringLiteral("tab_connect"));
		connect(connect_now, &QAction::triggered, this, [session] {
			session->connect_to_host();
		});
	}

	QAction *duplicate = menu->addAction(QStringLiteral("&Duplicate"));
	duplicate->setObjectName(QStringLiteral("tab_duplicate"));

	QAction *split = menu->addAction(QStringLiteral("Show &beside current"));
	split->setObjectName(QStringLiteral("tab_split"));

	/*
	 * Offered only when there is room for another pane. A menu entry
	 * that does nothing on a phone is worse than one that is absent:
	 * it invites the user to conclude the feature is broken rather
	 * than that their window is narrow.
	 */
	split->setEnabled(m_layout->room_to_split(m_current, bssh_layout_manager::EDGE_RIGHT)
	                   && !m_layout->intent().contains(session));

	/*
	 * Reordering, because the DRAG that does it is desktop-only and
	 * says so above: the bar scrolls horizontally on a phone, and one
	 * that both scrolls and rearranges under the same drag picks
	 * between them by accident. So touch gets the same capability
	 * through a menu, exactly as panes do -- and the desktop keeps the
	 * drag, which is quicker where it works.
	 *
	 * Enabled on the BAR's order, which is what the move applies to.
	 * The registry is the wrong list twice over: it holds the sessions
	 * shown as panes, so a step past one of those moves nothing a user
	 * can see, and it is shared, so reordering it rearranges every other
	 * window's tabs (sec 8.4). Both were measured -- the first on a
	 * phone, where the item fired and the bar did not move.
	 */
	QAction *move_left = menu->addAction(QStringLiteral("Move &Left"));
	move_left->setObjectName(QStringLiteral("tab_move_left"));
	move_left->setEnabled(index > 0);

	QAction *move_right = menu->addAction(QStringLiteral("Move &Right"));
	move_right->setObjectName(QStringLiteral("tab_move_right"));
	move_right->setEnabled(index < tabs.size() - 1);

	menu->addSeparator();

	QAction *close = menu->addAction(QStringLiteral("&Close"));
	close->setObjectName(QStringLiteral("tab_close"));

	apply_menu_layout(menu, context_menu_layout(bssh_app_settings().context_menu(),
	                                             bssh_platform::instance()
	                                                     && bssh_platform::instance()
	                                                                ->is_touch_primary()));
	return menu;
}

void bssh_main_window::show_tab_menu(bssh_session *session, const QPoint &global) {
	if (!session) return;

	QMenu *menu = build_tab_menu(session, this);
	if (!menu) return;

	QPointer<bssh_session> held = session;
	const QAction *chosen = menu->exec(global);
	const QAction *duplicate = menu->findChild<QAction *>(QStringLiteral("tab_duplicate"));
	const QAction *split = menu->findChild<QAction *>(QStringLiteral("tab_split"));
	const QAction *close = menu->findChild<QAction *>(QStringLiteral("tab_close"));
	const QAction *move_left = menu->findChild<QAction *>(QStringLiteral("tab_move_left"));
	const QAction *move_right = menu->findChild<QAction *>(QStringLiteral("tab_move_right"));
	menu->deleteLater();

	/*
	 * Within its pane's bar, through the layout manager the desktop drag
	 * uses too, so the two cannot drift: one of them being wrong is how
	 * this arrived, the menu having moved the session in the registry
	 * while the drag moved it in the bar.
	 */
	if (!held) return;
	if (chosen == move_left || chosen == move_right) {
		move_tab(held, chosen == move_left ? -1 : 1);
		return;
	}

	/*
	 * Duplicate opens another session on the same host, through the same
	 * path the front door uses -- so a host that needs a password is
	 * asked for one, and a key-authenticated host simply opens. Copying
	 * the session would mean copying a connection, which SSH has no way
	 * to do.
	 */
	if (chosen == duplicate) open_profile(session->profile());
	if (chosen == split) split_with(session);
	if (chosen == close) m_registry->close(held);
}

bool bssh_main_window::paste_needs_confirming(const QString &text, bool warn_enabled) {
	if (!warn_enabled) return false;

	/*
	 * More than one line of CONTENT. A single trailing newline is how a
	 * command arrives when it is copied from a page, and asking about
	 * that is asking about nearly every paste anybody makes.
	 */
	const QString body = text.endsWith(QLatin1Char('\n'))
	        ? text.left(text.size() - 1)
	        : text;

	return body.contains(QLatin1Char('\n')) || body.contains(QLatin1Char('\r'));
}

void bssh_main_window::paste_into_current(const QString &text) {
	if (!m_current || text.isEmpty()) return;

	if (paste_needs_confirming(text, bssh_app_settings().warn_multiline_paste())) {
		/*
		 * sec 6.4: "a paste containing newlines executes commands".
		 * The count is in the question because it is the fact that
		 * decides the answer -- two lines is usually a mistake worth
		 * catching, forty is a script somebody meant to run.
		 */
		const int lines = text.count(QLatin1Char('\n'))
		        + (text.endsWith(QLatin1Char('\n')) ? 0 : 1);

		QMessageBox box(this);
		box.setWindowTitle(QStringLiteral("Paste %1 lines?").arg(lines));
		box.setIcon(QMessageBox::Warning);
		box.setText(QStringLiteral("This paste is %1 lines long.").arg(lines));
		box.setInformativeText(
		        QStringLiteral("Each line ends with a newline, which the shell reads "
		                        "as Enter -- so every line but the last will run "
		                        "before you can look at it."));

		QCheckBox *again = new QCheckBox(QStringLiteral("Do not ask again"), &box);
		box.setCheckBox(again);

		QPushButton *go = box.addButton(QStringLiteral("Paste"), QMessageBox::AcceptRole);
		box.addButton(QStringLiteral("Cancel"), QMessageBox::RejectRole);
		box.setDefaultButton(qobject_cast<QPushButton *>(box.buttons().last()));

		/*
		 * Cancel is the default button, so Return dismisses the dialog
		 * rather than running whatever was on the clipboard. The
		 * dangerous answer should never be the one a stray keystroke
		 * gives.
		 */
		box.exec();

		if (again->isChecked()) bssh_app_settings().set_warn_multiline_paste(false);
		if (box.clickedButton() != go) return;
	}

	m_current->input_router()->paste(text);
}

void bssh_main_window::update_window_title() {
	/*
	 * Whatever is on screen names the window, and it is recomputed HERE
	 * rather than at the one signal that used to drive it.
	 *
	 * That signal was the remote's OSC title, guarded on the session
	 * being the current one -- so a window showed "BeerSSH" until a
	 * remote spoke, and switching tabs kept the old tab's name until the
	 * new one happened to set a title of its own. Two defects with one
	 * cause: the title was a side effect of an event rather than a
	 * function of what is on screen.
	 */
	setWindowTitle(bssh_expand_window_title(m_window_title_format, m_current));
}

void bssh_main_window::update_status() {
	/*
	 * The title comes along, because this function already runs wherever
	 * what the window is showing changes -- a tab switch, a state change,
	 * a session going away. Hanging the title off its own set of signals
	 * is how it came to be updated by exactly one of them.
	 */
	update_window_title();

	const bssh_platform *platform = bssh_platform::instance();

	QString session_text = QStringLiteral("no session");
	if (m_current) {
		const QMetaEnum states = QMetaEnum::fromType<bssh_ssh_session::state_t>();
		session_text = QStringLiteral("%1 %2")
		                    .arg(m_current->profile().target(),
		                          QString::fromLatin1(states.valueToKey(m_current->state())).toLower());
	}
	if (!m_error.isEmpty()) session_text += QStringLiteral(" -- ") + m_error;

	/*
	 * And what is actually being drawn, when it is not what was asked
	 * for (sec 9.4). bssh_font_catalog writes the sentence -- "X is not
	 * installed on this device yet. Showing Y until it is." -- and
	 * bssh_terminal_view hands it on; its header has said since it was
	 * written that "the window shows it", and no window did. A host
	 * whose font is not on this device rendered in a substitute with no
	 * explanation, and the explanation existed the whole time.
	 *
	 * Recomputed only when the font id moves: the answer costs a
	 * QFontDatabase::families() scan, and this function runs far too
	 * often to pay that every time. Comparing one string is what makes
	 * asking here -- rather than at each of the three places a view can
	 * change -- affordable, and one place cannot have a call site
	 * somebody forgot.
	 */
	const QString font_id = m_terminal_view ? m_terminal_view->font_id() : QString();
	if (font_id != m_font_note_id) {
		m_font_note_id = font_id;
		m_font_note = m_terminal_view ? m_terminal_view->font_substitution_note()
		                              : QString();
	}
	if (!m_font_note.isEmpty()) session_text += QStringLiteral(" -- ") + m_font_note;

	/*
	 * Asked of the view rather than kept in a member of this window.
	 *
	 * The copy went stale: the window read the grid once while the pane
	 * was still being laid out and cached a one-column answer, and the
	 * later resize that made it 47 arrived through a connection that was
	 * not yet in place. The status line then reported "1x41 cells" on a
	 * screen plainly showing more, for the whole life of the session.
	 * Two places computing one thing, and only one of them right -- so
	 * there is now one place, read at the moment it is displayed.
	 */
	const int cols = m_terminal_view ? m_terminal_view->cols() : 0;
	const int rows = m_terminal_view ? m_terminal_view->rows() : 0;

	/*
	 * What the line says is the user's to decide (sec 8.4). It was
	 * unconditional, and its first field was the QPA platform name --
	 * which is a fact a developer wants and a user has no use for, on a
	 * row of screen taken from the terminal.
	 *
	 * The setting is read in apply_device_settings rather than here: this
	 * runs on every state change, every resize and every keystroke that
	 * scrolls, and opening an INI file that often to answer a question
	 * whose answer changes twice a year is a file read in the paint path.
	 */
	m_status_label->setVisible(m_status_line != bssh_app_settings::STATUS_LINE_OFF);

	m_status_text = m_status_line == bssh_app_settings::STATUS_LINE_FULL
	        ? QStringLiteral("%1  |  %2x%3 cells  |  %4")
	                  .arg(platform->name())
	                  .arg(cols)
	                  .arg(rows)
	                  .arg(session_text)
	        : session_text;

	elide_status();
}

void bssh_main_window::elide_status() {
	/*
	 * Shortened with an ellipsis rather than wrapped: a status line that
	 * grows to three lines when something goes wrong takes the space away
	 * from the terminal, which is where the user was looking. The full
	 * text stays reachable as the tooltip and as what a screen reader
	 * announces, so nothing is actually lost by making it fit.
	 */
	const int room = m_status_label->contentsRect().width();
	const QFontMetrics metrics(m_status_label->font());

	m_status_label->setText(room > 0
	                                ? metrics.elidedText(m_status_text, Qt::ElideRight, room)
	                                : m_status_text);
	m_status_label->setToolTip(m_status_text);
	m_status_label->setAccessibleName(m_status_text);
}

bssh_main_window::close_action_t bssh_main_window::close_action(bool sessions_live,
                                                                 bool remembered_background,
                                                                 bool way_back,
                                                                 bool keep_sessions) {
	/*
	 * Nothing live: an ordinary close, and the process should go. Keeping
	 * a terminal resident with no sessions would be a background process
	 * doing nothing, which is the kind of thing users hunt down and kill.
	 */
	if (!sessions_live) return CLOSE_AND_QUIT;

	/*
	 * Not keeping sessions past their window, the desktop default: the
	 * window closes what it shows, as a terminal's window does. Offering to
	 * keep them was a question with an answer most people never want, and
	 * the window it hid was one only a tray icon could bring back.
	 */
	if (!keep_sessions) return CLOSE_AND_QUIT;

	/*
	 * Live sessions and no answer on file: ASK. A terminal that vanishes
	 * into the tray unannounced is how someone loses work they believed
	 * they had closed -- and one that silently kills a running build is
	 * worse (sec 5.5).
	 */
	return remembered_background && way_back ? CLOSE_TO_BACKGROUND : ASK_FIRST;
}

void bssh_main_window::come_back() {
	if (!m_in_background) return;
	m_in_background = false;
	showNormal();
	raise();
	activateWindow();
	update_residency();
}

void bssh_main_window::update_residency() {
	bssh_platform *platform = bssh_platform::instance();
	if (!platform || !m_registry) return;

	bool hidden = false;
	for (QWidget *top : QApplication::topLevelWidgets()) {
		bssh_main_window *window = qobject_cast<bssh_main_window *>(top);
		if (window && window->m_in_background) hidden = true;
	}

	const int live = m_registry->live_count();
	platform->set_session_count(live);
	platform->set_keep_alive(live > 0 || hidden);
}

void bssh_main_window::closeEvent(QCloseEvent *event) {
	/*
	 * Back out of a session goes UP to the host list, not out of the app.
	 *
	 * On the touch front door the terminal is NOT the root: the host list
	 * is (sec 8.5), and every other page here already carries its own way
	 * back to it. The terminal did not, and Android has no other route --
	 * so opening a host was a one-way door. That made the second session
	 * unreachable, and with it the tab bar, its context menu, and every
	 * split (sec 8.3): the app could show four panes and a phone could
	 * never ask for two.
	 *
	 * Guarded on the host list EXISTING rather than on the platform, which
	 * is the same rule as everywhere else here -- only the chosen front
	 * door is constructed, so a desktop window has no m_host_list and
	 * closing it still means closing it.
	 */
	if (m_host_list && m_pages->currentWidget() == m_terminal_page) {
		m_pages->setCurrentWidget(m_host_list);
		event->ignore();
		return;
	}

	/*
	 * Asked about THIS window's sessions, not the process's (sec 8.4).
	 * A window is a view; what closing one ends is what it was showing,
	 * and a second window's live shell is no reason to interrogate
	 * somebody closing an idle one -- nor, worse, to offer them a
	 * "disconnect and quit" that would take it down.
	 */
	const int live_here = live_shown_count();

	const bool way_back = bssh_platform::instance()->has_way_back();
	close_action_t action = close_action(live_here > 0,
	                                      m_asked_about_closing && m_background_on_close,
	                                      way_back,
	                                      bssh_app_settings().keep_sessions_on_close());

	if (action == ASK_FIRST) {
		QMessageBox box(this);
		box.setWindowTitle(QStringLiteral("BeerSSH"));
		box.setText(live_here == 1
		                    ? QStringLiteral("A session is still connected.")
		                    : QStringLiteral("%1 sessions are still connected.")
		                              .arg(live_here));
		/*
		 * With no tray there is nothing to come back from, so the window
		 * is not offered out of sight: Cancel stands in for it.
		 */
		box.setInformativeText(way_back
		        ? QStringLiteral("Keep it running in the background, or disconnect and quit?")
		        : QStringLiteral("This desktop has no system tray to bring the window back "
		                         "from, so closing it disconnects."));

		QPushButton *background = way_back
		        ? box.addButton(QStringLiteral("Keep running"), QMessageBox::AcceptRole)
		        : nullptr;
		QPushButton *cancel = way_back ? nullptr : box.addButton(QMessageBox::Cancel);
		box.addButton(QStringLiteral("Disconnect and quit"), QMessageBox::DestructiveRole);
		box.setDefaultButton(background ? background : cancel);
		box.exec();

		if (cancel && box.clickedButton() == static_cast<QAbstractButton *>(cancel)) {
			event->ignore();
			return;
		}

		const bool chose_background =
		        background && box.clickedButton() == static_cast<QAbstractButton *>(background);
		if (way_back) {
			m_asked_about_closing = true;
			m_background_on_close = chose_background;
		}
		action = chose_background ? CLOSE_TO_BACKGROUND : CLOSE_AND_QUIT;
	}

	if (action == CLOSE_TO_BACKGROUND) {
		/*
		 * The session is untouched: it lives in the registry, not in this
		 * window (sec 3, core principle 2), so going out of sight is the
		 * whole operation.
		 *
		 * sec 5.1: "started when the first session connects and stopped
		 * when the last disconnects". On desktop that call means the tray
		 * icon and staying resident; on Android it means the foreground
		 * service. Neither is named here.
		 */
		/*
		 * And the window hides only if the platform did not already put
		 * the app away. On Android it did, and hiding as well is what
		 * left a blank Activity nothing could recover -- see
		 * send_to_background(). Residency is settled before the hide, so
		 * the last visible window going does not quit the app.
		 */
		const bool hide_here = !bssh_platform::instance()->send_to_background();
		m_in_background = hide_here;
		update_residency();
		if (hide_here) hide();

		event->ignore();
		return;
	}

	save_layout();

	/*
	 * ONLY what this window shows. This iterated the registry, so
	 * closing either of two windows disconnected every session in both
	 * -- the exact failure sec 8.4 names ("closing one anywhere would
	 * have closed it everywhere") arriving by the other half of the
	 * window, since the tab bar had been fixed and the close had not.
	 *
	 * Closed rather than merely disconnected, because a window is the
	 * only thing showing its sessions (sec 8.4: exactly one pane at a
	 * time). Disconnecting left them in the registry with no view
	 * anywhere -- counted by the tray, written into the saved layout,
	 * reachable from nothing.
	 *
	 * Over a copy: the registry's session_closing handler edits m_shown
	 * as each one goes.
	 */
	const QList<bssh_session *> mine = m_shown;
	for (bssh_session *session : mine) m_registry->close(session);

	/*
	 * And residency is the PROCESS's fact, so it is read back off the
	 * registry rather than asserted. Saying false here unconditionally
	 * is what would have let closing an idle window quit an app whose
	 * other window was hidden in the tray with a build running: Qt
	 * quits when the last VISIBLE window closes, and a backgrounded one
	 * is not visible.
	 */
	/*
	 * A window closed while out of sight -- its last shell ended there,
	 * and an ending closes the window -- is not waiting for the tray any
	 * more. Kept marked, it held the icon for a window that was gone.
	 * And Qt quits on the last VISIBLE window closing, which this one
	 * had already stopped being, so with nothing else on screen and
	 * nothing to stay for, the process is told to go.
	 */
	const bool was_away = m_in_background;
	m_in_background = false;
	update_residency();
	event->accept();

	if (was_away && QApplication::quitOnLastWindowClosed()) {
		bool shown = false;
		for (QWidget *top : QApplication::topLevelWidgets()) {
			if (top != this && top->isVisible()) shown = true;
		}
		if (!shown) QTimer::singleShot(0, qApp, &QCoreApplication::quit);
	}
}

int bssh_main_window::live_shown_count() const {
	int live = 0;
	for (bssh_session *session : m_shown) {
		if (session && session->is_live()) ++live;
	}
	return live;
}

void bssh_main_window::keep_the_minimum_to_the_shown_page() {
	if (!m_pages) return;

	const QWidget *shown = m_pages->currentWidget();

	for (int i = 0; i < m_pages->count(); ++i) {
		QWidget *page = m_pages->widget(i);
		if (!page) continue;

		if (!m_page_policies.contains(page)) m_page_policies.insert(page, page->sizePolicy());

		page->setSizePolicy(page == shown
		                            ? m_page_policies.value(page)
		                            : QSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored));
	}
}

QSize bssh_main_window::size_for_grid(int cols, int rows) {
	if (cols < 1 || rows < 1) return size();

	/*
	 * On a window that is up, measure. Whatever this one puts around its
	 * terminal is exactly the difference between the two, so a hidden tab
	 * bar costs nothing and a widget added later is counted without
	 * anyone remembering to.
	 *
	 * With the panes split this sizes the window so that the FOCUSED pane
	 * is the grid asked for, which is the honest reading of a request
	 * naming one size -- and the case a command line reaches, since a
	 * window opened from one has a single session in it.
	 */
	if (isVisible() && m_terminal_view && !m_terminal_view->size().isEmpty()) {
		if (QLayout *tree = layout()) tree->activate();

		return m_terminal_view->size_for_grid(cols, rows)
		       + (size() - m_terminal_view->size());
	}

	/*
	 * Before the window has ever been shown -- which is when main() asks,
	 * so that --geometry takes effect without the window appearing at one
	 * size and jumping to another -- there is nothing to measure. A
	 * hidden widget tree is not laid out, and activating the layout by
	 * hand reaches the top level and not the pages under it, so the view
	 * still answers with whatever size it was constructed at. The first
	 * version of this measured anyway and asked for a hundred columns of
	 * a window that came up with a hundred and eighty-eight.
	 *
	 * So the chrome is NAMED here. It is a short list and this window is
	 * the one place that knows it: the tab bar when it is showing, the
	 * status line, the key row on a touch device. Everything else in
	 * between -- the page stack, the pane container, its splitter -- has
	 * zero margins by construction. The test that caught the measuring
	 * asserts the grid this produces, both ways round.
	 */
	QSize wanted = m_terminal_view
	        ? m_terminal_view->size_for_grid(cols, rows)
	        : bssh_terminal_view().size_for_grid(cols, rows);

	if (wanted.isEmpty()) return size();

	/*
	 * A named list, and it has now been wrong once for the reason a named
	 * list is always wrong: the menu bar was added above and nothing here
	 * knew, so --geometry asked for a window one menu bar too short. The
	 * geometry test caught it, which is why that test names the grid both
	 * ways round rather than trusting the arithmetic.
	 *
	 * Left as a list rather than converted to a walk because the two are
	 * not equivalent here: a walk would have to know which widgets sit
	 * BESIDE the terminal and which sit above it, and getting that wrong
	 * is a silent wrong answer instead of a loud one.
	 */
	if (m_menu_bar && !m_menu_bar->isHidden()) {
		wanted.rheight() += m_menu_bar->sizeHint().height();
	}
	/* The focused pane's own bar, when it has one (sec 8.3's bar per pane). */
	if (bssh_pane_bar *bar = m_current && m_layout ? bar_for(m_layout->front_of(m_current))
	                                                : nullptr) {
		if (!bar->isHidden()) {
			if (m_tab_position == bssh_app_settings::TABS_LEFT
			    || m_tab_position == bssh_app_settings::TABS_RIGHT)
				wanted.rwidth() += bar->sizeHint().width();
			else
				wanted.rheight() += bar->sizeHint().height();
		}
	}
	/* Only when it shows: hidden by default since sec 8.7a, and counted
	 * anyway it made a window asked for 30 rows one row taller. */
	if (m_status_label && m_status_line != bssh_app_settings::STATUS_LINE_OFF)
		wanted.rheight() += m_status_label->sizeHint().height();
	if (m_key_row && !m_key_row->isHidden()) wanted.rheight() += m_key_row->sizeHint().height();
	if (m_keyboard && !m_keyboard->isHidden()) {
		wanted.rheight() += m_keyboard->sizeHint().height();
	}

	return wanted;
}

bool bssh_main_window::showing_front_door() const {
	/*
	 * Two shapes, one question. On touch the front door is a page in the
	 * stack; on desktop it is a window of its own, and a window that has
	 * been asked for but not yet shown -- this window not being on screen
	 * either -- still counts, because the question is what the user is
	 * being offered rather than what the compositor has drawn.
	 */
	if (m_session_dialog) return m_front_door_up;
	return m_pages->currentWidget() != m_terminal_page;
}

bssh_main_window::front_door_t bssh_main_window::front_door_for(bool touch_primary) {
	return touch_primary ? FRONT_DOOR_HOST_LIST : FRONT_DOOR_SESSION_DIALOG;
}

bool bssh_main_window::opens_a_local_shell(bool touch_primary) {
	/*
	 * The desktop opens a local shell; touch opens its configuration
	 * instead (sec 2).
	 *
	 * Not because Android cannot run a shell -- /system/bin/sh is there --
	 * but because an unprivileged app sandbox has no userland worth
	 * driving from one, which is the gap Termux exists to fill. Opening a
	 * shell that can do almost nothing would be worse than not offering
	 * it, so the phone opens the host list, which is what it is for.
	 *
	 * A capability question like front_door_for()'s, and asked the same
	 * way: a convertible in tablet mode gets the phone's answer and a
	 * docked tablet with a keyboard gets the desktop's.
	 */
	return !touch_primary;
}

void bssh_main_window::open_default_session() {
	if (!opens_a_local_shell(bssh_platform::instance()
	                          && bssh_platform::instance()->is_touch_primary())) {
		return;
	}

	/*
	 * Never on top of something this window is already showing -- a
	 * command-line target, or a session restored from a previous run.
	 * Asked of this window rather than of the registry: with more than
	 * one window, another window's session is not a reason for this one
	 * to open empty (sec 8.4).
	 */
	if (!m_shown.isEmpty()) return;

	open_profile(bssh_profile_t::local_shell());
}


QString bssh_main_window::stored_password_for(const bssh_profile_t &profile) {
	/*
	 * The profile's has_stored_password flag is what makes this cheap:
	 * without it, finding out whether a password exists would mean
	 * unlocking the store -- so every connection would ask for the
	 * store's passphrase, including the many that do not need one.
	 */
	if (profile.id == 0 || !profile.has_stored_password) return QString();

	bssh_secret_store *store = bssh_secret_store::instance();
	if (!store->is_unlocked() && !unlock_secret_store()) return QString();

	return store->load(bssh_profile_password_key(profile.id));
}

bool bssh_main_window::unlock_secret_store() {
	bssh_secret_store *store = bssh_secret_store::instance();
	if (store->is_unlocked()) {
		bssh_sweep_orphaned_secrets();
		return true;
	}
	if (!store->needs_passphrase()) return false;

	bool ok = false;
	const QString passphrase = QInputDialog::getText(
	        this, QStringLiteral("Unlock saved passwords"),
	        QStringLiteral("Passphrase for the credential store\n(%1):")
	                .arg(store->backend_name()),
	        QLineEdit::Password, QString(), &ok);
	if (!ok) return false;

	if (store->unlock(passphrase)) {
		/*
		 * sec 11: the moment a password left behind by a deleted host
		 * can finally be erased. It is swept HERE rather than asked for
		 * on its own, because this is the only place the store is
		 * unlocked and the user came to do something else -- they are
		 * releasing a password, not answering for a host they removed
		 * weeks ago.
		 */
		bssh_sweep_orphaned_secrets();
		return true;
	}

	/*
	 * Said plainly rather than folded into a generic failure: a wrong
	 * passphrase and a damaged store look the same to the user, and the
	 * difference matters to what they do next.
	 */
	QMessageBox::warning(this, QStringLiteral("BeerSSH"),
	                      QStringLiteral("That passphrase did not open the credential store.\n\n"
	                                      "Either it is wrong, or the store has been modified."));
	return false;
}

void bssh_main_window::offer_to_remember(bssh_session *session, const QString &password) {
	if (session->profile().id == 0 || password.isEmpty()) return;

	if (!unlock_secret_store()) return;

	/*
	 * The two writes and their ordering live in
	 * bssh_remember_profile_password(), because getting them right is not
	 * a widget's job and the rollback it does cannot be tested through a
	 * window. What is left here is the part that IS this window's: saying
	 * so when it did not work.
	 */
	const bssh_remember_result_t result = bssh_remember_profile_password(
	        session->profile().id, password, m_profile_store);

	/*
	 * Said rather than swallowed. The tick box is a promise, and the next
	 * connection asking for a password the user believes they saved is
	 * the moment they find out it was not kept -- by which time nothing
	 * on screen connects the two. The reason is named because what to do
	 * next differs: a full disk is fixable, a store that will not open is
	 * a different problem.
	 *
	 * Every case named and no default, so a result added later is a
	 * warning here rather than a warning box with a blank reason in it.
	 */
	QString reason;
	switch (result) {
	case BSSH_REMEMBER_SAVED:
		return;
	case BSSH_REMEMBER_NO_HOST:
		/*
		 * A host deleted while its session was still up (sec 8.4), or
		 * a session with no host behind it at all. Nothing was written
		 * and nothing needs saying: there is no longer anywhere to
		 * remember a password FOR, and the user deleted it themselves.
		 */
		return;
	case BSSH_REMEMBER_NO_STORE:
		reason = QStringLiteral("There is no credential store on this system.");
		break;
	case BSSH_REMEMBER_NOT_STORED:
		reason = QStringLiteral("The credential store could not be written.");
		break;
	case BSSH_REMEMBER_NOT_FLAGGED:
		reason = QStringLiteral("The host could not be updated, so the password "
		                         "was taken back out of the store.");
		break;
	case BSSH_REMEMBER_STRAY:
		reason = QStringLiteral("The host could not be updated, and the password "
		                         "could not be taken back out of the store -- it "
		                         "may still be there, unused.");
		break;
	}

	QMessageBox::warning(this, QStringLiteral("BeerSSH"),
	                      QStringLiteral("The password for %1 was not saved.\n\n%2")
	                              .arg(session->profile().target(), reason));
}

bool bssh_main_window::open_settings_section(const QString &name) {
	if (!m_settings_sections) return false;

	const QList<QListWidgetItem *> rows =
	        m_settings_sections->findItems(name, Qt::MatchExactly);
	if (rows.isEmpty()) return false;

	emit m_settings_sections->itemClicked(rows.first());
	return true;
}

void bssh_main_window::hide_keyboard() {
	if (!m_keyboard) return;
	m_keyboard_hidden_by_user = true;
	m_keyboard->hide();
}

bool bssh_main_window::restart_keeping_sessions(QString *why) {
	const bssh_handoff_point *point = bssh_handoff_point::instance();
	if (!point) {
		if (why) *why = QStringLiteral("this BeerSSH cannot hand its sessions over");
		return false;
	}
	return start_this_program({ QStringLiteral("--adopt-from"), point->path() }, why);
}

static bssh_main_window::process_starter_t &process_starter() {
	static bssh_main_window::process_starter_t starter;
	return starter;
}

void bssh_main_window::set_process_starter(process_starter_t starter) {
	process_starter() = std::move(starter);
}

bool bssh_main_window::start_this_program(const QStringList &arguments, QString *why) {
	/*
	 * The program as it is on disk now. An upgrade replaced the file this
	 * process was started from, and Linux names a running program whose
	 * file is gone with " (deleted)" after its path -- the new one is at
	 * the same path without it.
	 */
	QString program = QCoreApplication::applicationFilePath();
	const QString deleted = QStringLiteral(" (deleted)");
	if (program.endsWith(deleted)) program.chop(deleted.size());

	const bool started = process_starter() ? process_starter()(program, arguments)
	                                       : QProcess::startDetached(program, arguments);
	if (!started && why) *why = QStringLiteral("could not start %1").arg(program);
	return started;
}
