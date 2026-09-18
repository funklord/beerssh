#include <QApplication>
#include "ui/touch_metrics.h"
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
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaEnum>
#include <QMouseEvent>
#include <QPointer>
#include <QFontMetrics>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QShowEvent>
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

#include "model/profile_store.h"
#include "model/session.h"
#include "model/conn_manager.h"
#include "model/session_registry.h"
#include "model/app_settings.h"
#include "model/command_line.h"
#include "model/window_title.h"
#include "model/settings_resolver.h"
#include "ui/layout_manager.h"
#include "ui/input_selector.h"
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
#include "keyboard/key_row.h"
#include "keyboard/soft_keyboard.h"
#include "ui/app_settings_view.h"
#include "ui/licences_view.h"
#include "model/preset.h"
#include "ui/preset_editor.h"
#include "term/colour_scheme.h"
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

bssh_main_window::bssh_main_window(const bssh_profile_t &launch, QWidget *parent)
    : QWidget(parent),
      m_pages(new QStackedWidget(this)),
      m_back_target(nullptr),
      m_front_door_up(false),
      m_menu_bar(nullptr),
      m_tab_row(nullptr),
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
      m_input_selector(nullptr),
      m_wired_view(nullptr),
      m_status_label(new QLabel(this)),
      m_registry(bssh_session_registry::shared()),
      m_layout(nullptr),
      m_current(nullptr),
      m_tabs(new QTabBar(m_terminal_page)),
      m_tab_long_press(nullptr),
      m_key_row(nullptr),
      m_keyboard(nullptr),
      m_keyboard_hidden_by_user(false),
      m_device_settings(nullptr),
      m_settings_sections(nullptr),
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
	if (front_door_for(bssh_platform::instance()->is_touch_primary())
	    == FRONT_DOOR_SESSION_DIALOG) {
		build_menu_bar();
		if (m_menu_bar) outer->addWidget(m_menu_bar, 0);
	}

	outer->addWidget(m_pages, 1);

	QVBoxLayout *layout = new QVBoxLayout(m_terminal_page);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	/*
	 * Tabs above the terminal (sec 8.3). One per session, labelled from
	 * the OSC title with the profile name as fallback, and hidden while
	 * there is only one -- a single tab is a strip of wasted rows on a
	 * phone.
	 */
	m_tabs->setExpanding(false);
	m_tabs->setTabsClosable(true);
	m_tabs->setDrawBase(false);

	/*
	 * ELIDED, because the label is a string the REMOTE writes. A title
	 * arrives by OSC (sec 8.3) and is capped at 1024 characters, which
	 * with setExpanding(false) is a tab about eleven thousand pixels
	 * wide: measured, and it puts every tab after it past the right-hand
	 * edge, reachable only through the scroll buttons. No hostility is
	 * needed for this -- a shell that puts the full command line or a
	 * deep path in its title gets there on an ordinary afternoon.
	 *
	 * In the MIDDLE, for the reason bssh_host_list gives about the same
	 * kind of string: the two ends are what identify a session and the
	 * middle of a long path is the part nobody needs. Several tabs on one
	 * host differ in their LEAF directory, so eliding at the end throws
	 * away the character that tells them apart.
	 */
	m_tabs->setElideMode(Qt::ElideMiddle);

	m_tabs->hide();

	/*
	 * A "+" at the end of the tab strip, which is where every browser and
	 * every terminal since the mid-2000s has put it -- so it needs no
	 * explaining, and it is the one affordance that is visible at the
	 * moment somebody wants it rather than filed under a menu.
	 *
	 * The menu bar carries the same action with its chord beside it; this
	 * is the shortcut for the hand already on the mouse. Both, not either:
	 * a menu teaches the chord and a button is quicker once you know.
	 *
	 * It lives in the tab bar's corner, so it appears and disappears with
	 * the strip -- a lone "+" floating above a single-session window
	 * would be chrome with nothing to sit beside.
	 */
	QToolButton *add_tab = new QToolButton(m_tabs);
	add_tab->setText(QStringLiteral("+"));
	add_tab->setAutoRaise(true);
	add_tab->setToolTip(QStringLiteral("Open another session in a tab"));
	add_tab->setAccessibleName(QStringLiteral("New tab"));
	connect(add_tab, &QToolButton::clicked, this, [this] { open_another_session(false); });
	m_tabs->setDrawBase(false);

	QHBoxLayout *tab_row = new QHBoxLayout();
	tab_row->setContentsMargins(0, 0, 0, 0);
	tab_row->setSpacing(0);
	tab_row->addWidget(m_tabs, 0);
	tab_row->addWidget(add_tab, 0);
	tab_row->addStretch(1);

	m_tab_row = new QWidget(m_terminal_page);
	m_tab_row->setLayout(tab_row);
	m_tab_row->hide();
	layout->addWidget(m_tab_row, 0);

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
	if (bssh_platform::instance() && bssh_platform::instance()->is_touch_primary())
		m_layout->set_arrangement(bssh_layout_manager::COLUMN);

	m_panes = new bssh_pane_container(m_layout, m_terminal_page);
	layout->addWidget(m_panes, 1);
	layout->addWidget(m_status_label, 0);

	/*
	 * The input selector (sec 8.7), at the bottom edge where a thumb
	 * reaches without stretching, and beneath the status line so it does
	 * not push the terminal around as the status text changes. Shown only
	 * where a thumb is the pointer: a desktop keeps Shift, the wheel and
	 * drag-select, so the control there would be chrome doing nothing.
	 *
	 * Where it finally lives is sec 8.7's open question, resolved here to
	 * the bottom bar on the holder's go-ahead 2026-09-18; it is one
	 * addWidget to move.
	 */
	m_input_selector = new bssh_input_selector(m_terminal_page);
	layout->addWidget(m_input_selector, 0);
	m_input_selector->setVisible(bssh_platform::instance()
	                              && bssh_platform::instance()->is_touch_primary());

	/*
	 * The Move state is offered only where there is a column to reorder.
	 * The arrangement is fixed at construction today (touch opens a
	 * column, a desktop opens tiles), so this is set once; when a runtime
	 * arrangement switch lands it calls the same setter.
	 */
	m_input_selector->set_column_available(
	        m_layout->arrangement() == bssh_layout_manager::COLUMN);

	connect(m_input_selector, &bssh_input_selector::mode_changed,
	         this, [this] { apply_input_mode(); });

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
	connect(m_panes, &bssh_pane_container::panes_rebuilt, this, [this] {
		on_pane_focus_changed(m_panes->focused_session());

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
	});

	connect(m_tabs, &QTabBar::currentChanged, this, &bssh_main_window::on_tab_changed);
	connect(m_tabs, &QTabBar::tabCloseRequested,
	         this, &bssh_main_window::on_tab_close_requested);

	/*
	 * sec 8.3's drag-to-reorder, and it is DESKTOP ONLY because that
	 * section says so: the tab bar scrolls horizontally on a phone, and a
	 * bar that both scrolls and rearranges under the same drag picks one
	 * of them by accident.
	 *
	 * The reorder is applied to the REGISTRY, not left in the bar. Tab
	 * index and registry index are one thing here -- every tab action
	 * turns an index into a session with at() -- and on_registry_changed
	 * rebuilds the bar from the registry, so a drag the registry never
	 * heard about is undone at the next repaint, having in the meantime
	 * pointed Close at whichever session used to be in that slot.
	 */
	const bool pointer = bssh_platform::instance()
	                      && !bssh_platform::instance()->is_touch_primary();
	m_tabs->setMovable(pointer);

	/*
	 * And the other end of that same gesture: a tab pulled clear of one
	 * window's bar can land on another's (sec 8.4). Gated on the same
	 * answer, because the drag it receives can only be started where
	 * there is a pointer to start it -- offering to receive one on a
	 * phone would be a capability nothing can exercise.
	 */
	setAcceptDrops(pointer);

	connect(m_tabs, &QTabBar::tabMoved, this, [this](int from, int to) {
		/*
		 * The window's own order, not the registry's. Which order a
		 * window shows its tabs in is that window's business; moving
		 * them in the registry would rearrange every other window's
		 * tabs as a side effect (sec 8.4).
		 */
		move_tab(from, to);
	});

	/*
	 * And sec 8.3's middle-click close, the other half of that sentence.
	 * Every tabbed program since the browsers settled it does this, and a
	 * user who has the habit tries it before they look for the small x.
	 *
	 * On RELEASE, like every other click: pressing over a tab and
	 * dragging away is a cancelled click everywhere else, and a close
	 * that fires under the finger is the one that cannot be taken back.
	 * Not gated on the platform -- a phone has no middle button to press,
	 * and a mouse plugged into one may as well work.
	 */
	m_tabs->installEventFilter(this);

	/*
	 * sec 8.3's tab context menu. "Show beside current" is how a split is
	 * MADE -- until now the pane container could show four panes and
	 * nothing in the app could ask it to show two.
	 */
	m_tabs->setContextMenuPolicy(Qt::CustomContextMenu);
	connect(m_tabs, &QWidget::customContextMenuRequested,
	         this, &bssh_main_window::show_tab_menu);

	/*
	 * And by press-and-hold, which is the other half of sec 8.3's
	 * "long-press (touch) or right-click (desktop)" and was the half with
	 * no implementation: Qt does not raise a context-menu event from a
	 * long press on Android. Same gesture as the host list and the
	 * terminal, and enabled the same way -- only where touch is the
	 * primary input, since with a pointer a held press on a tab means
	 * nothing and should keep meaning nothing.
	 */
	m_tab_long_press = new bssh_long_press(m_tabs, this);
	m_tab_long_press->set_enabled(bssh_platform::instance()
	                               && bssh_platform::instance()->is_touch_primary());

	connect(m_tab_long_press, &bssh_long_press::triggered, this, [this](const QPoint &at) {
		show_tab_menu(m_tabs->mapFromGlobal(at));
	});

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
		presets_row->setSizeHint(QSize(0, BSSH_TOUCH_ROW_DP));
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
			row->setSizeHint(QSize(0, BSSH_TOUCH_ROW_DP));
			section_pages.append(page);
		}

		/* The licences reach the same list, being the other thing a
		 * person looks for under settings. */
		QListWidgetItem *licences_row =
		        new QListWidgetItem(QStringLiteral("Licences"), sections);
		licences_row->setSizeHint(QSize(0, BSSH_TOUCH_ROW_DP));
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
	m_key_row = new bssh_key_row(nullptr, this);
	layout->insertWidget(2, m_key_row);

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
	m_keyboard = new bssh_soft_keyboard(nullptr, this);
	layout->insertWidget(3, m_keyboard);
	m_keyboard->hide();

	/*
	 * The keyboard's own two keys (sec 6.2). Hide is remembered rather
	 * than merely applied: apply_device_settings recomputes visibility on
	 * every session event and every settings change, so a keyboard that
	 * only called hide() would come back on the next one, which from the
	 * user's side is a key that does not work.
	 */
	connect(m_keyboard, &bssh_soft_keyboard::hide_requested, this, [this] {
		m_keyboard_hidden_by_user = true;
		m_keyboard->hide();
	});

	/*
	 * Cfg goes to the same page the front door's menu opens, rather than
	 * a second settings surface -- sec 14's M10 records what two front doors to
	 * one setting cost when only one of them was kept in step.
	 *
	 * On a desktop there is no touch front door and no page stack, so
	 * there is nothing to show; the button is only ever pressed on a
	 * device that has one.
	 */
	connect(m_keyboard, &bssh_soft_keyboard::settings_requested, this, [this] {
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
	if (m_layout->arrangement() == bssh_layout_manager::COLUMN) {
		/*
		 * A column holds every session the window shows (sec 8.7), in the
		 * order they arrived, so showing one shows the whole column and
		 * there are no tabs to leave behind. That is what dissolves the
		 * bar's off-by-one rather than fixing it: with nothing to convert
		 * to a tab, the miscount has nowhere to happen.
		 */
		sync_column_intent();
	} else if (!m_layout->intent().contains(session)) {
		/*
		 * Selecting a tab shows that session alone. Adding it BESIDE the
		 * others is a separate, deliberate act (the tab context menu's
		 * "Show beside current"), because a tab switch that silently
		 * split the window would be a click doing two things.
		 */
		m_layout->set_intent(QList<bssh_session *>{ session });
	}

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

	bssh_session *session = prepare_profile(bssh_profile_t::local_shell());
	if (!session) return;

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

	/*
	 * A column has no tabs (sec 8.7): it holds every session as a pane, so
	 * "show as a tab" would drop a session out of the intent with no bar
	 * to reach it by. Left where it is rather than hidden.
	 */
	if (m_layout->arrangement() == bssh_layout_manager::COLUMN) return;

	QList<bssh_session *> intent = m_layout->intent();
	if (!intent.contains(session)) return;

	/*
	 * The last pane cannot become a tab: a window with no pane is a
	 * window showing nothing, and there would be no way back to any of
	 * its terminals except the bar it just grew. Refused rather than
	 * silently ignored is the same answer tear_out gives ("a window will
	 * not empty itself", sec 8.4) applied one level in.
	 */
	if (intent.size() < 2) return;

	intent.removeAll(session);
	m_layout->set_intent(intent);

	m_panes->rebuild();

	/*
	 * Focus goes somewhere, and it has to be said explicitly: the pane
	 * that was focused is the one that just left, so leaving this out
	 * gives a window whose panes are all unfocused -- no border, no
	 * cursor filled, and keystrokes going to whatever Qt picked.
	 */
	if (m_current == session || !m_current) {
		const QList<bssh_session *> panes = m_layout->visible_panes();
		if (!panes.isEmpty()) m_panes->focus_session(panes.first());
	}

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

void bssh_main_window::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);

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
	 * The keyboard's own width question, which is not the layout
	 * manager's: it asks how many panes fit, this asks whether the
	 * full-size keyboard does (sec 6.2). Cheap on every resize, because
	 * set_style returns at once when the answer has not changed.
	 */
	apply_keyboard_style();
}

/*
 * Both device-level appearance settings, applied together because they
 * arrive on the same signal and a caller that remembers one and forgets
 * the other is the bug this function exists to not have.
 */
void bssh_main_window::sync_column_intent() {
	/*
	 * In a column every session the window shows is a pane (sec 8.7), so
	 * the layout intent is exactly the shown list, in the same order. A
	 * no-op in every other arrangement, and when the two already agree, so
	 * a rebuild fires only on a real change.
	 */
	if (m_layout && m_layout->arrangement() == bssh_layout_manager::COLUMN
	    && m_layout->intent() != m_shown) {
		m_layout->set_intent(m_shown);
	}
}

void bssh_main_window::apply_input_mode() {
	if (!m_panes || !m_input_selector) return;

	const bssh_input_mode_t mode = m_input_selector->mode();
	for (bssh_terminal_view *view : m_panes->views()) {
		if (view) view->set_input_mode(mode);
	}
}

void bssh_main_window::apply_device_settings() {
	apply_key_row_visibility();

	const bssh_app_settings settings;
	const bool raise = settings.raise_contrast();
	const int brightness = settings.text_brightness();

	/*
	 * The custom palette, pushed into the catalog before anything reads a
	 * scheme out of it. term/ does not reach into the settings store --
	 * the dependency would run the wrong way -- so whoever owns the
	 * settings hands it over, and this is the one place that happens.
	 */
	bssh_colour_scheme_catalog::instance().set_custom(settings.custom_scheme());

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
	m_status_line = settings.status_line();
	m_window_title_format = settings.window_title();
	update_window_title();

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

	if ((bar_changed || ligatures_changed) && m_panes) m_panes->rebuild();

	/*
	 * Caps as Ctrl reaches every session's router, not just the focused
	 * one (sec 6.1). It is a fact about the keyboard in front of the
	 * user, so a setting that only took effect on the tab that happened
	 * to be open would be one they had to set again by switching tabs.
	 */
	const bool caps = settings.caps_as_ctrl();
	for (bssh_session *session : m_registry->sessions()) {
		if (session->input_router()) session->input_router()->set_caps_as_ctrl(caps);
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
		m_key_row->set_visibility_mode(bssh_key_row::ALWAYS);
		break;
	case bssh_app_settings::KEY_ROW_NEVER:
		m_key_row->set_visibility_mode(bssh_key_row::NEVER);
		break;
	case bssh_app_settings::KEY_ROW_AUTO:
		m_key_row->set_visibility_mode(bssh_key_row::AUTO);
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
	if (m_terminal_view) {
		m_terminal_view->setAttribute(Qt::WA_InputMethodEnabled, !built_in);
	}
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
	if (bssh_set_keyboard_layout(settings.keyboard_layout())) {
		m_keyboard->rebuild_keys();
	}

	const bssh_soft_keyboard_catalog::style_t wanted =
	        settings.keyboard() == bssh_app_settings::KEYBOARD_BUILT_IN_FULL
	                ? bssh_soft_keyboard_catalog::STYLE_FULL
	                : bssh_soft_keyboard_catalog::STYLE_COMPACT;

	m_keyboard->set_style(bssh_keyboard_style_for(wanted, width()));
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

		/*
		 * And a TAB dropped on this pane, which the view accepts only so
		 * that Qt delivers it at all -- it picks a target before
		 * delivery and never re-targets to an ancestor, so a view that
		 * refused meant the window was never offered the drop and the
		 * tab tore out into a new window instead of tiling (sec 8.4).
		 */
		connect(m_terminal_view, &bssh_terminal_view::session_dropped,
		         this, [this](const QMimeData *mime) {
			bssh_session *dropped = session_from_mime(mime);
			if (dropped) drop_session(dropped, true);
		});

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
		       this, [this](const QString &uri) {
			/*
			 * SHOWN elided, OPENED whole. The question is what the
			 * user is being asked to read, and a URI put in
			 * verbatim makes a dialog wider than the screen -- see
			 * bssh_uri_for_prompt. What openUrl gets is the address
			 * itself, because a shortened one is a different one.
			 */
			const QMessageBox::StandardButton answer = QMessageBox::question(
			      this, QStringLiteral("Open link"),
			      QStringLiteral("Open this address, sent by the remote host?\n\n%1")
			              .arg(bssh_uri_for_prompt(uri, BSSH_URI_PROMPT_CHARS)),
			      QMessageBox::Open | QMessageBox::Cancel, QMessageBox::Cancel);

			if (answer == QMessageBox::Open) QDesktopServices::openUrl(QUrl(uri));
		});

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

		if (m_keyboard) m_keyboard->set_router(session->input_router());
		if (m_key_row) {
			m_key_row->set_router(session->input_router());
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
	if (m_key_row) m_key_row->set_router(nullptr);
	if (m_keyboard) m_keyboard->set_router(nullptr);
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

	if (dialog.exec() != QDialog::Accepted) {
		session->disconnect_from_host();
		return;
	}

	const QString password = dialog.password();
	session->supply_password(password);

	if (dialog.remember()) offer_to_remember(session, password);
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
		if (session != m_current) return;
		on_failed(reason);
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
		const QString stored = stored_password_for(session->profile());
		if (!stored.isEmpty()) {
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
		if (session != m_current) return;

		m_error = status == 0
		                  ? QStringLiteral("session ended")
		                  : QStringLiteral("session ended (exit %1)").arg(status);
		update_status();
	});
	connect(session, &bssh_session::state_changed, this, [this, session] {
		/* A successful (re)connect clears whatever the last message was. */
		if (session == m_current && session->is_live()) {
			m_error.clear();
			update_status();
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
	 * Reconnection is only real to the user if they can see it. A
	 * terminal that has silently stopped receiving output looks
	 * identical to one whose remote is merely quiet (sec 5.3).
	 */
	if (bssh_conn_manager *manager = m_registry->conn_manager_for(session)) {
		connect(manager, &bssh_conn_manager::reconnect_scheduled,
		         this, [this, session](int delay_ms, int attempt) {
			if (session != m_current) return;
			m_error = QStringLiteral("reconnecting in %1s (attempt %2)")
			                  .arg((delay_ms + 999) / 1000)
			                  .arg(attempt);
			update_status();
		});
		connect(manager, &bssh_conn_manager::reconnect_started, this, [this, session](int) {
			if (session != m_current) return;
			m_error = QStringLiteral("reconnecting...");
			update_status();
		});
		connect(manager, &bssh_conn_manager::gave_up, this, [this, session] {
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

void bssh_main_window::drop_session(bssh_session *session, bool beside) {
	if (!session) return;

	/*
	 * Dropped back where it came from: nothing happens, which is what
	 * cancelling a drag should look like.
	 *
	 * Without this the drop still went through take-and-show below, and
	 * on a window with no room for another pane that means show_session()
	 * -- which replaces the intent with this session ALONE, so putting a
	 * pane back exactly where it was collapsed the split it was part of.
	 * A gesture whose undo is destructive is worse than one that cannot
	 * be undone.
	 */
	if (beside && m_layout && m_layout->intent().contains(session)) return;

	/*
	 * A pane of THIS window, dropped on THIS window's tab bar, is the
	 * un-split (sec 8.3). It has to be caught before the take-and-show
	 * below, which would put the session back on screen as the only pane
	 * -- the opposite of what aiming at the bar means, and the same shape
	 * of mistake as a drop that evicted the pane it was meant to join.
	 */
	if (!beside && m_layout && m_layout->intent().contains(session)) {
		show_as_tab(session);
		return;
	}

	/*
	 * Taken before it is shown, and those are two decisions rather than
	 * one. adopt_session() bundles them, and its "show" half replaces the
	 * intent with this session alone -- correct for a tab, and fatal for a
	 * tile: split_with() declines a session the intent already contains,
	 * so a drop onto the panes of a wide window produced ONE pane showing
	 * the dropped session, having just evicted the one it was meant to sit
	 * beside. It looked like a drop that had done nothing, when in fact it
	 * had done something worse.
	 */
	take_session(session);

	/*
	 * Beside, not instead: split_with() is what sec 8.3's "Show beside
	 * current" does, and a drop onto the panes means the same thing.
	 *
	 * Declined where the size class has no room for another pane
	 * (sec 8.2), and the fallback is to show it as a tab rather than to do
	 * nothing -- a drop onto a phone-width window still puts the session
	 * in front of the person who dropped it.
	 */
	const bool room = beside && m_layout
	                  && m_layout->intent().size() < m_layout->max_panes();

	if (room) {
		split_with(session);
	} else {
		show_session(session);
	}
}

QString bssh_main_window::session_mime_type() {
	/* One spelling, and it lives with the view because that is where a
	 * drop actually lands (sec 8.4). */
	return bssh_session_mime_type();
}

bssh_session *bssh_main_window::session_from_mime(const QMimeData *mime) const {
	if (!mime || !mime->hasFormat(session_mime_type())) return nullptr;

	bool ok = false;
	const int index = mime->data(session_mime_type()).toInt(&ok);
	if (!ok) return nullptr;

	/*
	 * Resolved out of the registry rather than dereferenced: the payload
	 * is an index, and an index that no longer names a session -- closed
	 * mid-drag, say -- must decline rather than land on whichever session
	 * moved into that slot.
	 */
	return m_registry ? m_registry->at(index) : nullptr;
}

void bssh_main_window::start_tab_drag(int index) {
	start_session_drag(session_at_tab(index));
}

/*
 * What the pointer carries during a session drag.
 *
 * Painted rather than loaded: this program ships no icon theme, and a
 * missing asset would leave the gesture looking like nothing again.
 */
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

void bssh_main_window::start_session_drag(bssh_session *session) {
	if (!session || !m_registry) return;

	const int in_registry = m_registry->index_of(session);
	if (in_registry < 0) return;

	QMimeData *mime = new QMimeData;
	mime->setData(session_mime_type(), QByteArray::number(in_registry));

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
	const Qt::DropAction landed = drag->exec(Qt::MoveAction);

	if (source) source->set_being_dragged(false);

	if (landed != Qt::IgnoreAction) return;

	/*
	 * Nobody wanted it, so the gesture means what it meant before there
	 * was anywhere to drop: a window of its own (sec 8.4). Dropping on
	 * the desktop and dropping on nothing are the same event to Qt, which
	 * is exactly the behaviour this needs.
	 */
	tear_out(session);
}

void bssh_main_window::dragEnterEvent(QDragEnterEvent *event) {
	if (session_from_mime(event->mimeData())) event->acceptProposedAction();
}

bool bssh_main_window::point_is_on_tab_row(const QPoint &at) const {
	if (!m_tabs || !m_tabs->isVisible()) return false;

	const QPoint in_bar = m_tabs->mapFrom(this, at);
	return in_bar.y() >= 0 && in_bar.y() < m_tabs->height();
}

void bssh_main_window::dropEvent(QDropEvent *event) {
	bssh_session *session = session_from_mime(event->mimeData());
	if (!session) return;

	/*
	 * Where it lands is what it means (sec 8.3): the tab bar makes a tab,
	 * and anywhere else -- the panes, which is most of the window -- makes
	 * a tile beside what is already showing.
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
	drop_session(session, !point_is_on_tab_row(event->position().toPoint()));
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
	 * A second window is a second VIEW, never a second registry
	 * (sec 8.4) -- which is what bssh_main_window already arranges, so
	 * this is the tear-out gesture's window without the tearing.
	 */
	bssh_main_window *window = new bssh_main_window;
	window->setAttribute(Qt::WA_DeleteOnClose);
	window->resize(size());
	window->show();
	window->open_profile(bssh_profile_t::local_shell());
}

bssh_main_window *bssh_main_window::tear_out_tab(int index) {
	return tear_out(session_at_tab(index));
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
 * Move a tab along this window's bar. See the header for why it is the
 * window's order rather than the registry's.
 *
 * Shared by the desktop DRAG and the menu items, which is the point:
 * the menu's first version moved the session in the registry instead,
 * and a registry step past an on-screen pane moved nothing the user
 * could see. One implementation, two callers.
 */
void bssh_main_window::move_tab(int from, int to) {
	if (from < 0 || from >= m_tab_order.size()) return;
	if (to < 0 || to >= m_tab_order.size()) return;

	/*
	 * The bar's indices are into the TAB list, which is m_shown without
	 * the sessions currently on screen as panes. So the move is applied
	 * there and written back through m_shown, leaving the panes where
	 * they are: a move in the bar must not reorder terminals the bar is
	 * not showing.
	 */
	m_tab_order.move(from, to);

	int next = 0;
	const QList<bssh_session *> panes =
	        m_layout ? m_layout->visible_panes() : QList<bssh_session *>();

	for (int i = 0; i < m_shown.size() && next < m_tab_order.size(); ++i) {
		if (panes.contains(m_shown.at(i))) continue;
		m_shown[i] = m_tab_order.at(next++);
	}

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

	/*
	 * In a column m_shown is the order of record -- sync_column_intent
	 * copies it into the intent -- so a reorder that touched only the
	 * intent would be undone the next time a session is opened or closed.
	 * Move it there too, by the same step, to make the change stick.
	 */
	if (m_layout->arrangement() == bssh_layout_manager::COLUMN) {
		const int sfrom = m_shown.indexOf(session);
		const int sto = sfrom + (direction < 0 ? -1 : 1);
		if (sfrom >= 0 && sto >= 0 && sto < m_shown.size())
			m_shown.move(sfrom, sto);
	}

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

bssh_main_window *bssh_main_window::tear_out(bssh_session *session) {
	if (!session || !m_shown.contains(session)) return nullptr;

	/*
	 * A window will not empty itself. Tearing out the last tab would
	 * leave a window with nothing in it and, on desktop, no front door
	 * showing either -- so the gesture does nothing rather than
	 * producing two windows one of which is useless.
	 */
	if (m_shown.size() < 2) return nullptr;

	bssh_main_window *window = new bssh_main_window;
	window->setAttribute(Qt::WA_DeleteOnClose);
	window->resize(size());
	window->show();
	window->adopt_session(session);

	return window;
}

bssh_session *bssh_main_window::session_at_tab(int index) const {
	/*
	 * A tab index means a position in THIS window's list, not in the
	 * registry (sec 8.4). They were the same thing while a window could
	 * only be one, and with two windows the registry's index is somebody
	 * else's tab.
	 */
	if (index < 0 || index >= m_tab_order.size()) return nullptr;
	return m_tab_order.at(index);
}

void bssh_main_window::on_tab_changed(int index) {
	bssh_session *session = session_at_tab(index);
	if (session && session != m_current) show_session(session);
}

void bssh_main_window::on_tab_close_requested(int index) {
	bssh_session *session = session_at_tab(index);
	if (session) m_registry->close(session);
}

/*
 * sec 8.3's middle-click close. QTabBar raises no signal for it --
 * tabCloseRequested comes from its own close button and from nothing else
 * -- so the button is read off the event here.
 */
bool bssh_main_window::eventFilter(QObject *watched, QEvent *event) {
	/*
	 * sec 8.3's drag, taken past the bar: a tab dragged well clear of the
	 * tab bar becomes a window of its own (sec 8.4).
	 *
	 * Distance rather than "left the widget", because a tab bar is thin
	 * and a pointer crosses its edge constantly while merely reordering
	 * -- tearing out on that would make reordering nearly impossible. The
	 * threshold is generous for the same reason.
	 */
	if (watched == m_tabs && event->type() == QEvent::MouseMove) {
		const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
		if (mouse->buttons().testFlag(Qt::LeftButton) && m_tabs->isMovable()) {
			const QPoint at = mouse->position().toPoint();

			/*
			 * Clear of the bar in ANY direction, not just below it.
			 *
			 * This measured `at.y() - height()` and nothing else, so
			 * a tab could only be torn out by dragging DOWNWARD.
			 * Dragging one up, or off either end, did nothing at all
			 * -- and "nothing at all" is indistinguishable from a
			 * feature that does not exist, which is how it was
			 * reported: tabs cannot be dragged freely. Konsole and
			 * every browser detach on distance, not on direction.
			 *
			 * Sideways is measured from the bar's full WIDTH rather
			 * than from the last tab, because dragging along a
			 * half-empty strip is still reordering. Measured from
			 * the tabs, a tab dragged to the end of a two-tab bar
			 * would tear itself out mid-reorder.
			 *
			 * And that is exactly what it did, because the bar has no
			 * half-empty strip to drag along: the row lays it out at
			 * stretch 0 with the stretch AFTER the "+" button, so
			 * QTabBar is exactly as wide as its tabs. Measuring
			 * against m_tabs->width() therefore measured distance
			 * past the LAST TAB -- the thing the comment says it
			 * avoided -- leaving about 40px of slack before a reorder
			 * turned into a tear-out. Dragging a tab to the last
			 * position starts within that margin, which is why moving
			 * tabs behaved differently depending on where you let go:
			 * over the panes it split the window, back on the bar it
			 * dropped the rest of the move, over nothing it detached.
			 *
			 * The ROW is the strip the comment meant. It spans the
			 * window because of that trailing stretch, so sideways
			 * travel stays a reorder however far along the bar it
			 * goes, and only leaving the row vertically -- or running
			 * off the window entirely -- is a tear-out.
			 */
			QWidget *strip = m_tab_row ? m_tab_row : static_cast<QWidget *>(m_tabs);
			const QPoint in_strip = m_tabs->mapTo(strip, at);

			const int above = -in_strip.y();
			const int below = in_strip.y() - strip->height();
			const int left = -in_strip.x();
			const int right = in_strip.x() - strip->width();

			const int clear = qMax(qMax(above, below), qMax(left, right));

			if (clear > BSSH_TEAR_OUT_DP && m_tabs->count() > 0) {
				/*
				 * Consumed, and the drag is over as far as QTabBar
				 * is concerned: it is still mid-move on a tab that
				 * may be about to leave this bar, so releasing the
				 * grab is what stops it moving one that no longer
				 * belongs here. Qt's drag takes the pointer from
				 * this point on.
				 */
				m_tabs->releaseMouse();
				start_tab_drag(m_tabs->currentIndex());
				return true;
			}
		}
	}

	if (watched == m_tabs && event->type() == QEvent::MouseButtonRelease) {
		const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
		if (mouse->button() == Qt::MiddleButton) {
			const int index = m_tabs->tabAt(mouse->position().toPoint());
			if (index >= 0) {
				on_tab_close_requested(index);
				return true;
			}
		}
	}

	return QWidget::eventFilter(watched, event);
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
 * is the only answer that is right from both doors.
 */
void bssh_main_window::show_page(QWidget *page) {
	if (!m_pages || !page) return;

	m_back_target = m_pages->currentWidget();
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

	QWidget *to = m_back_target;
	if (!to || to == here) to = m_host_list;
	m_back_target = nullptr;

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
 * Only accepted when there was something to leave. Back at the terminal
 * or at the host list is a request to leave the APP, and swallowing it
 * there would strand a user pressing a key that does nothing.
 */
void bssh_main_window::keyPressEvent(QKeyEvent *event) {
	if (event->key() == Qt::Key_Back && go_back()) {
		event->accept();
		return;
	}

	QWidget::keyPressEvent(event);
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
	if (!m_tabs) return;

	/*
	 * A tab is a terminal this window HOLDS and is not showing. A pane is
	 * one it is showing. They are different states, and the bar lists
	 * only the first (sec 8.3: "a session with a pane is on screen, one
	 * without is a tab").
	 *
	 * The bar used to be built from m_shown outright, so a session was a
	 * pane AND a tab at the same time -- split a window in two and both
	 * terminals appeared in the bar above themselves. Reported as exactly
	 * that: "they become tabs". The document had said otherwise since
	 * M8 and nothing had ever read it that way.
	 *
	 * visible_panes() rather than the intent, because a window narrowed
	 * past its size class is showing fewer panes than were asked for
	 * (sec 8.2) and the ones it is not showing are tabs again -- which is
	 * what "panes convert to tabs, preserving order" means, expressed
	 * once instead of by a second list somebody has to keep in step.
	 */
	const QList<bssh_session *> panes =
	        m_layout ? m_layout->visible_panes() : QList<bssh_session *>();

	m_tab_order.clear();
	for (bssh_session *session : m_shown) {
		if (!panes.contains(session)) m_tab_order.append(session);
	}

	const QSignalBlocker blocker(m_tabs);

	while (m_tabs->count() > m_tab_order.size()) {
		m_tabs->removeTab(m_tabs->count() - 1);
	}
	while (m_tabs->count() < m_tab_order.size()) {
		m_tabs->addTab(QString());
	}

	for (int i = 0; i < m_tab_order.size(); ++i) {
		bssh_session *session = m_tab_order.at(i);
		m_tabs->setTabText(i, session->title());
		m_tabs->setTabToolTip(i, session->profile().target());

		/*
		 * The status dot (sec 8.3). Painted rather than shipped as an
		 * icon file: four solid circles are cheaper to draw than to
		 * package, and they follow the theme's own sizing.
		 */
		const int size = m_tabs->fontMetrics().height() / 2;
		m_tabs->setTabIcon(i, QIcon(bssh_tab_status::dot(
		        size, bssh_tab_status::state_for(session))));
	}

	/*
	 * Shown whenever there is anything in it, which is a weaker test than
	 * the "more than one" it replaces -- and it has to be. One tab beside
	 * a pane is a real thing to show now: it is the terminal you are NOT
	 * looking at, and hiding it would leave no way back to it. What that
	 * old test was really guarding against is a bar listing the one
	 * terminal on screen, and that cannot arise any more: the thing on
	 * screen is a pane and panes are not in this list.
	 */
	m_tabs->setVisible(!m_tab_order.isEmpty());
	if (m_tab_row) m_tab_row->setVisible(!m_tab_order.isEmpty());
}

void bssh_main_window::on_registry_changed() {
	/*
	 * The tab bar is rebuilt from the registry rather than edited
	 * alongside it: two lists kept in step by hand drift, and the one
	 * that drifts here shows the wrong session's name over the wrong
	 * terminal.
	 */
	const QSignalBlocker blocker(m_tabs);

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
	sync_column_intent();

	refresh_tab_bar();

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
	const int live = m_registry->live_count();
	bssh_platform::instance()->set_session_count(live);
	bssh_platform::instance()->set_keep_alive(live > 0);

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
		bssh_session *successor = session_at_tab(m_tabs->currentIndex());
		show_session(successor ? successor : m_shown.first());
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
	const int panes = qMin(layout.pane_count, m_shown.size());
	for (int i = 1; i < panes; ++i) split_with(m_shown.at(i));

	/*
	 * Last, so it wins the focus: every prepare_profile above showed its
	 * own session as it arrived, which is the same one-pane-at-a-time
	 * path a tab switch takes.
	 */
	bssh_session *active = m_shown.value(layout.active_index, m_shown.first());
	if (active && panes <= 1) show_session(active);

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
	if (id == 0) return;

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

void bssh_main_window::on_failed(const QString &reason) {
	m_error = reason;
	update_status();
}

void bssh_main_window::on_grid_resized(int cols, int rows) {
	/*
	 * window-change for the session on screen only (sec 7). With panes at
	 * M8 each computes its own geometry, so this stays per-session rather
	 * than becoming a window-wide broadcast.
	 */
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
		const int index = m_tabs->currentIndex();
		if (index >= 0) on_tab_close_requested(index);
	});
	m_session_actions.append(close_tab);

	file->addSeparator();

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

	QMenu *view = m_menu_bar->addMenu(QStringLiteral("&View"));

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
	 * drop_session() asks -- so the menu says no in the case where the
	 * action would silently fall back to a tab.
	 */
	if (m_split_action) {
		const bool room = m_layout
		        && m_layout->intent().size() < m_layout->max_panes();
		m_split_action->setEnabled(has_session && room);
	}

	/* Detaching the only tab would leave an empty window behind. */
	if (m_detach_action) m_detach_action->setEnabled(m_shown.size() > 1);
}

QMenu *bssh_main_window::build_terminal_menu(QWidget *parent) const {
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
	QAction *split = menu->addAction(QStringLiteral("Split &Beside Current"));
	split->setObjectName(QStringLiteral("terminal_split"));
	split->setEnabled(m_current && m_layout
	                   && m_layout->intent().size() < m_layout->max_panes());

	/*
	 * The un-split, and the reason it is a menu entry as well as a
	 * gesture: with two panes and nothing else, this window has no tab
	 * bar to drop a pane onto, so the gesture alone would leave the
	 * split with no way out. The same reasoning as Split's, which is on
	 * this menu because a one-session window has no tab bar either.
	 */
	QAction *as_tab = menu->addAction(QStringLiteral("Show as &Tab"));
	as_tab->setObjectName(QStringLiteral("terminal_show_as_tab"));
	as_tab->setEnabled(m_current && m_layout && m_layout->intent().size() > 1);

	/*
	 * Reordering, which is the one arrangement a touch user could not
	 * reach: Split adds a pane and Show as Tab removes one, and neither
	 * moves a pane past its neighbour. The grip's drag does it with a
	 * pointer and cannot by touch -- see move_pane().
	 */
	const QList<bssh_session *> arrangement =
	        m_layout ? m_layout->intent() : QList<bssh_session *>();
	const int here = m_current ? arrangement.indexOf(m_current) : -1;

	QAction *move_left = menu->addAction(QStringLiteral("Move Pane &Left"));
	move_left->setObjectName(QStringLiteral("terminal_move_pane_left"));
	move_left->setEnabled(here > 0);

	QAction *move_right = menu->addAction(QStringLiteral("Move Pane &Right"));
	move_right->setObjectName(QStringLiteral("terminal_move_pane_right"));
	move_right->setEnabled(here >= 0 && here < arrangement.size() - 1);

	QAction *new_tab = menu->addAction(QStringLiteral("New &Tab"));
	new_tab->setObjectName(QStringLiteral("terminal_new_tab"));
	new_tab->setShortcut(
	        QKeySequence(bssh_input_router::binding_for(QStringLiteral("new-tab"))));

	QAction *new_window = menu->addAction(QStringLiteral("New &Window"));
	new_window->setObjectName(QStringLiteral("terminal_new_window"));
	new_window->setShortcut(
	        QKeySequence(bssh_input_router::binding_for(QStringLiteral("new-window"))));

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
	QAction *front_door = menu->addAction(QStringLiteral("&Hosts and Settings"));
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

void bssh_main_window::show_terminal_menu(const QPoint &at) {
	if (!m_terminal_view) return;

	QMenu *menu = build_terminal_menu(this);
	const QAction *chosen = menu->exec(at);
	menu->deleteLater();
	if (!chosen) return;

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

	const QAction *new_tab = menu->findChild<QAction *>(QStringLiteral("terminal_new_tab"));
	if (chosen == new_tab) {
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
QMenu *bssh_main_window::build_tab_menu(int index, QWidget *parent) const {
	bssh_session *session = session_at_tab(index);
	if (!session) return nullptr;

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
	const int fits = m_layout->max_panes();
	split->setEnabled(m_layout->intent().size() < fits &&
	                   !m_layout->intent().contains(session));

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
	move_right->setEnabled(index >= 0 && index < m_tab_order.size() - 1);

	menu->addSeparator();

	QAction *close = menu->addAction(QStringLiteral("&Close"));
	close->setObjectName(QStringLiteral("tab_close"));

	apply_menu_layout(menu, context_menu_layout(bssh_app_settings().context_menu(),
	                                             bssh_platform::instance()
	                                                     && bssh_platform::instance()
	                                                                ->is_touch_primary()));
	return menu;
}

void bssh_main_window::show_tab_menu(const QPoint &at) {
	const int index = m_tabs->tabAt(at);
	bssh_session *session = session_at_tab(index);
	if (!session) return;

	QMenu *menu = build_tab_menu(index, this);
	if (!menu) return;

	const QAction *chosen = menu->exec(m_tabs->mapToGlobal(at));
	const QAction *duplicate = menu->findChild<QAction *>(QStringLiteral("tab_duplicate"));
	const QAction *split = menu->findChild<QAction *>(QStringLiteral("tab_split"));
	const QAction *close = menu->findChild<QAction *>(QStringLiteral("tab_close"));
	const QAction *move_left = menu->findChild<QAction *>(QStringLiteral("tab_move_left"));
	const QAction *move_right = menu->findChild<QAction *>(QStringLiteral("tab_move_right"));
	menu->deleteLater();

	/*
	 * Through the same method the desktop drag uses, so the two cannot
	 * drift: one of them being wrong is how this arrived, the menu
	 * having moved the session in the registry while the drag moved it
	 * in the bar.
	 */
	if (chosen == move_left || chosen == move_right) {
		move_tab(index, index + (chosen == move_left ? -1 : 1));
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
	if (chosen == close) on_tab_close_requested(index);
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
                                                                 bool remembered_background) {
	/*
	 * Nothing live: an ordinary close, and the process should go. Keeping
	 * a terminal resident with no sessions would be a background process
	 * doing nothing, which is the kind of thing users hunt down and kill.
	 */
	if (!sessions_live) return CLOSE_AND_QUIT;

	/*
	 * Live sessions and no answer on file: ASK. A terminal that vanishes
	 * into the tray unannounced is how someone loses work they believed
	 * they had closed -- and one that silently kills a running build is
	 * worse (sec 5.5).
	 */
	return remembered_background ? CLOSE_TO_BACKGROUND : ASK_FIRST;
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

	close_action_t action = close_action(live_here > 0,
	                                      m_asked_about_closing && m_background_on_close);

	if (action == ASK_FIRST) {
		QMessageBox box(this);
		box.setWindowTitle(QStringLiteral("BeerSSH"));
		box.setText(live_here == 1
		                    ? QStringLiteral("A session is still connected.")
		                    : QStringLiteral("%1 sessions are still connected.")
		                              .arg(live_here));
		box.setInformativeText(QStringLiteral(
		        "Keep it running in the background, or disconnect and quit?"));

		QPushButton *background = box.addButton(QStringLiteral("Keep running"),
		                                         QMessageBox::AcceptRole);
		box.addButton(QStringLiteral("Disconnect and quit"), QMessageBox::DestructiveRole);
		box.setDefaultButton(background);
		box.exec();

		m_asked_about_closing = true;
		m_background_on_close = box.clickedButton() == static_cast<QAbstractButton *>(background);
		action = m_background_on_close ? CLOSE_TO_BACKGROUND : CLOSE_AND_QUIT;
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
		const int live = m_registry->live_count();
		bssh_platform::instance()->set_session_count(live);
		bssh_platform::instance()->set_keep_alive(live > 0);

		/*
		 * And the window hides only if the platform did not already put
		 * the app away. On Android it did, and hiding as well is what
		 * left a blank Activity nothing could recover -- see
		 * send_to_background().
		 */
		if (!bssh_platform::instance()->send_to_background()) hide();

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
	const int live = m_registry->live_count();
	bssh_platform::instance()->set_session_count(live);
	bssh_platform::instance()->set_keep_alive(live > 0);
	event->accept();
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
	if (m_tab_row && !m_tab_row->isHidden()) {
		wanted.rheight() += m_tab_row->sizeHint().height();
	} else if (m_tabs && !m_tabs->isHidden()) {
		wanted.rheight() += m_tabs->sizeHint().height();
	}
	if (m_status_label) wanted.rheight() += m_status_label->sizeHint().height();
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
