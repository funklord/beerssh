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
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QShowEvent>
#include <QTimer>
#include <QWindow>
#include <QScreen>
#include <QLayout>
#include <QVBoxLayout>

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
#include "model/window_title.h"
#include "model/settings_resolver.h"
#include "ui/layout_manager.h"
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
#include "ui/key_row.h"
#include "ui/app_settings_view.h"
#include "ui/licences_view.h"
#include "model/preset.h"
#include "ui/preset_editor.h"
#include "ui/session_dialog.h"
#include "ui/password_prompt.h"
#include "ui/tab_status.h"
#include "ui/terminal_view.h"

/*
 * How far below the tab bar a drag goes before the tab becomes its own
 * window (sec 8.3, sec 8.4).
 *
 * Generous on purpose. A tab bar is thin and a pointer reordering tabs
 * crosses its lower edge constantly, so tearing out on "left the widget"
 * would make reordering nearly impossible to do without losing the tab.
 */
static const int BSSH_TEAR_OUT_DP = 40;

bssh_main_window::bssh_main_window(const bssh_profile_t &launch, QWidget *parent)
    : QWidget(parent),
      m_pages(new QStackedWidget(this)),
      m_terminal_page(new QWidget(this)),
      m_profile_store(new bssh_profile_store(QString(), this)),
      m_session_dialog(nullptr),
      m_host_list(nullptr),
      m_host_editor(nullptr),
      m_preset_editor(nullptr),
      m_terminal_view(nullptr),
      m_panes(nullptr),
      m_wired_view(nullptr),
      m_status_label(new QLabel(this)),
      m_registry(bssh_session_registry::shared()),
      m_layout(nullptr),
      m_current(nullptr),
      m_tabs(new QTabBar(m_terminal_page)),
      m_tab_long_press(nullptr),
      m_key_row(nullptr),
      m_status_line(bssh_app_settings::STATUS_LINE_SESSION),
      m_window_title_format(QStringLiteral("%t - BeerSSH")),
      m_asked_about_closing(false),
      m_background_on_close(false) {
	setWindowTitle(QStringLiteral("BeerSSH"));

	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);
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
	m_tabs->hide();
	layout->addWidget(m_tabs, 0);

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
	m_panes = new bssh_pane_container(m_layout, m_terminal_page);
	layout->addWidget(m_panes, 1);
	layout->addWidget(m_status_label, 0);

	connect(m_panes, &bssh_pane_container::focus_changed,
	         this, &bssh_main_window::on_pane_focus_changed);
	connect(m_panes, &bssh_pane_container::panes_rebuilt, this, [this] {
		on_pane_focus_changed(m_panes->focused_session());

		/*
		 * A pane built after the setting was changed starts from the
		 * view's own default, so the device settings are re-applied to
		 * every rebuild rather than only when the user edits them --
		 * otherwise opening a second host quietly ignores the choice.
		 */
		apply_device_settings();
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
		if (from < 0 || from >= m_shown.size()) return;
		if (to < 0 || to >= m_shown.size()) return;
		m_shown.move(from, to);
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
	 * A session about to be destroyed must be let go of first: the view
	 * holds pointers into its screen, scrollback and images, and the next
	 * repaint would read freed cells (sec 3, core principle 2).
	 */
	connect(m_registry, &bssh_session_registry::session_closing,
	         this, [this](bssh_session *session) {
		if (m_current == session) detach_session();

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
			m_pages->setCurrentWidget(m_host_editor);
		});

		connect(m_host_list, &bssh_host_list::delete_requested,
		         this, [this](const bssh_profile_t &profile) {
			if (!m_profile_store || profile.id == 0) return;
			m_profile_store->remove(profile.id);
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
			m_profile_store->save(profile);
			m_host_list->reload();
			m_pages->setCurrentWidget(m_host_list);
		});
		connect(m_host_editor, &bssh_host_editor::cancelled, this, [this] {
			m_pages->setCurrentWidget(m_host_list);
		});

		/*
		 * The same editor the desktop dialog embeds, as its own page
		 * (sec 8.5: same job, different shape). sec 14 is explicit that a
		 * milestone lands on both platforms before the next one starts,
		 * and a preset system editable only on a desktop would leave the
		 * phone -- the device this app is mostly for -- stuck with
		 * whatever was shipped.
		 */
		m_preset_editor = new bssh_preset_editor(m_profile_store, bssh_preset_editor::TOUCH, this);
		m_pages->addWidget(m_preset_editor);

		connect(m_host_list, &bssh_host_list::presets_requested, this, [this] {
			m_preset_editor->reload();
			m_pages->setCurrentWidget(m_preset_editor);
		});
		connect(m_preset_editor, &bssh_preset_editor::preset_saved,
		         this, &bssh_main_window::reresolve_sessions);
		connect(m_preset_editor, &bssh_preset_editor::preset_removed,
		         this, &bssh_main_window::reresolve_sessions);
		connect(m_preset_editor, &bssh_preset_editor::back_requested, this, [this] {
			m_pages->setCurrentWidget(m_host_list);
		});

		bssh_licences_view *licences = new bssh_licences_view(this);
		m_pages->addWidget(licences);

		connect(m_host_list, &bssh_host_list::licences_requested, this, [this, licences] {
			m_pages->setCurrentWidget(licences);
		});
		connect(licences, &bssh_licences_view::back_requested, this, [this] {
			m_pages->setCurrentWidget(m_host_list);
		});

		bssh_app_settings_view *device = new bssh_app_settings_view(this);
		m_pages->addWidget(device);

		connect(m_host_list, &bssh_host_list::settings_requested, this, [this, device] {
			m_pages->setCurrentWidget(device);
		});
		connect(device, &bssh_app_settings_view::changed,
		         this, &bssh_main_window::apply_device_settings);
		connect(device, &bssh_app_settings_view::back_requested, this, [this] {
			m_pages->setCurrentWidget(m_host_list);
		});
	} else {
		m_session_dialog = new bssh_session_dialog(m_profile_store, this);
		connect(m_session_dialog, &bssh_session_dialog::open_requested,
		         this, &bssh_main_window::open_profile);
		connect(m_session_dialog, &bssh_session_dialog::cancelled, this, &QWidget::close);

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
		m_pages->addWidget(m_session_dialog);
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
	auto handle_shortcut = [this](bssh_input_router::shortcut_t shortcut) {
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
	};
	connect(m_registry, &bssh_session_registry::session_added,
	         this, [this, handle_shortcut](bssh_session *session) {
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

		connect(session->input_router(), &bssh_input_router::shortcut_pressed,
		         this, handle_shortcut);

		/* OSC title also drives the window title on desktop (sec 8.3). */
		connect(session, &bssh_session::title_changed, this, [this, session] {
			if (session == m_current) update_window_title();
		});
	});



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
	}
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

	connect(session, &bssh_session::failed, this, &bssh_main_window::on_failed);

	/*
	 * Resolve the host's settings once, here, where the store is (sec
	 * 10.1). The session carries them from now on, so a pane rebuilt by a
	 * layout change does not have to find a preset table to know what font
	 * to draw in.
	 */
	session->set_settings(resolved_settings(profile));

	/*
	 * First contact with a host. The fingerprint is shown because it is
	 * the only thing the user can actually check -- against what the
	 * server's owner published, or against `ssh-keyscan` from somewhere
	 * they already trust.
	 */
	connect(session, &bssh_session::host_key_unknown, this, [this, session](const QString &fp) {
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
		/*
		 * A password already kept for this host is used without asking,
		 * which is the entire point of storing one -- and is what makes
		 * auto-reconnect work for password-authenticated hosts without
		 * holding the credential in memory all session (sec 5.3, 11).
		 */
		const QString stored = stored_password_for(session->profile());
		if (!stored.isEmpty()) {
			session->supply_password(stored);
			return;
		}

		bssh_password_prompt dialog(session->profile().target(),
		                             session->profile().id != 0, this);

		if (dialog.exec() != QDialog::Accepted) {
			session->disconnect_from_host();
			return;
		}

		const QString password = dialog.password();
		session->supply_password(password);

		if (dialog.remember()) offer_to_remember(session, password);
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
	m_pages->setCurrentWidget(m_host_editor);
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
	 * The window no longer attaches a view to a session: it says which
	 * sessions should be on screen, and the pane container builds the
	 * views for them. That indirection is the whole of M8 -- one pane or
	 * four, collapsing to tabs when the window narrows, is a question
	 * about the LIST rather than about any one view.
	 */
	QList<bssh_session *> intent = m_layout->intent();

	if (!intent.contains(session)) {
		/*
		 * Selecting a tab shows that session alone. Adding it BESIDE the
		 * others is a separate, deliberate act (the tab context menu's
		 * "Show beside current"), because a tab switch that silently
		 * split the window would be a click doing two things.
		 */
		intent = QList<bssh_session *>{ session };
		m_layout->set_intent(intent);
	}

	m_panes->rebuild();
	m_panes->focus_session(session);

	/*
	 * Per-session wiring that outlives any one view: the settings a host
	 * resolves to, and the geometry the remote is told about. Reconnected
	 * on every show because a pane rebuild replaces the view underneath.
	 */
	/*
	 * OSC 52 (sec 7.1): the remote asking to set the clipboard. Honoured
	 * only where the host's preset says so -- off by default -- and never
	 * silently: the status line says which host did it, because a
	 * clipboard that changed without the user acting is the whole hazard,
	 * and the useful case (yanking in vim over ssh) is one they just
	 * caused deliberately.
	 */
	connect(session->emulator(), &bssh_emulator::clipboard_write_requested,
	         this, [this, session](const QString &text) {
		if (!session->settings().value(QStringLiteral("allow_clipboard_write")).toBool()) {
			return;
		}

		QGuiApplication::clipboard()->setText(text);

		m_error = QStringLiteral("%1 set the clipboard (%2 characters)")
		                  .arg(session->title())
		                  .arg(text.size());
		update_status();
	}, Qt::UniqueConnection);

	connect(session, &bssh_session::settings_changed, this, [this, session] {
		if (m_current != session || !m_terminal_view) return;

		bssh_apply_appearance(m_terminal_view, session->settings());
		if (m_key_row) {
			m_key_row->set_layout(
			        session->settings().value(QStringLiteral("key_row_layout")).toString());
		}
		session->emulator()->set_cell_pixels(m_terminal_view->cell_width(),
		                                      m_terminal_view->cell_height());
		session->resize(m_terminal_view->cols(), m_terminal_view->rows());
		update_status();
	}, Qt::UniqueConnection);

	if (m_terminal_view) {
		bssh_apply_appearance(m_terminal_view, session->settings());
		m_terminal_view->setFocus();
	}

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
	if (m_layout) m_layout->set_width(width());
	if (m_panes) m_panes->rebuild();

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
	m_layout->set_width(width());

	if (m_panes) m_panes->rebuild();
}

/*
 * Both device-level appearance settings, applied together because they
 * arrive on the same signal and a caller that remembers one and forgets
 * the other is the bug this function exists to not have.
 */
void bssh_main_window::apply_device_settings() {
	apply_key_row_visibility();

	const bssh_app_settings settings;
	const bool raise = settings.raise_contrast();

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

	if (m_panes) {
		for (bssh_terminal_view *view : m_panes->views()) {
			if (!view) continue;
			view->set_raise_contrast(raise);
			view->set_long_press_menu(hold);
			view->set_focus_follows_mouse(follows);
		}
	}

	if (bar_changed && m_panes) m_panes->rebuild();

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
}

void bssh_main_window::on_pane_focus_changed(bssh_session *session) {
	m_terminal_view = m_panes ? m_panes->focused_view() : nullptr;

	if (!m_terminal_view) {
		m_current = nullptr;
		update_status();
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
		const QMessageBox::StandardButton answer = QMessageBox::question(
			      this, QStringLiteral("Open link"),
			      QStringLiteral("Open this address, sent by the remote host?\n\n%1").arg(uri),
			      QMessageBox::Open | QMessageBox::Cancel, QMessageBox::Cancel);

		if (answer == QMessageBox::Open) QDesktopServices::openUrl(QUrl(uri));
	});

	connect(m_terminal_view, &bssh_terminal_view::selection_changed, this, [this] {
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

		if (m_key_row) {
			m_key_row->set_router(session->input_router());
			m_key_row->set_layout(
			        session->settings().value(QStringLiteral("key_row_layout")).toString());
		}

		/* The tab bar follows the focused pane, not the other way round. */
		const int index = m_shown.indexOf(session);
		if (index >= 0 && m_tabs->currentIndex() != index) {
			const QSignalBlocker blocker(m_tabs);
			m_tabs->setCurrentIndex(index);
		}
	}

	update_status();
}

void bssh_main_window::detach_session() {
	m_current = nullptr;

	/*
	 * The panes own the views and attach them; letting go here is a
	 * matter of dropping the session from the layout and rebuilding, so
	 * that no view is left pointing into a session about to be freed
	 * (sec 3, core principle 2).
	 */
	if (m_layout) m_layout->forget(m_current);
	if (m_panes) m_panes->rebuild();

	m_terminal_view = m_panes ? m_panes->focused_view() : nullptr;
	if (m_key_row) m_key_row->set_router(nullptr);
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

void bssh_main_window::adopt_session(bssh_session *session) {
	if (!session || m_shown.contains(session)) return;

	take_session(session);
	show_session(session);
}

void bssh_main_window::drop_session(bssh_session *session, bool beside) {
	if (!session) return;

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
	                  && m_layout->intent().size()
	                             < bssh_layout_manager::max_panes_for(m_layout->size_class());

	if (room) {
		split_with(session);
	} else {
		show_session(session);
	}
}

QString bssh_main_window::session_mime_type() {
	return QStringLiteral("application/x-bssh-session");
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
	bssh_session *session = session_at_tab(index);
	if (!session || !m_registry) return;

	const int in_registry = m_registry->index_of(session);
	if (in_registry < 0) return;

	QMimeData *mime = new QMimeData;
	mime->setData(session_mime_type(), QByteArray::number(in_registry));

	QDrag *drag = new QDrag(this);
	drag->setMimeData(mime);

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
	if (drag->exec(Qt::MoveAction) != Qt::IgnoreAction) return;

	/*
	 * Nobody wanted it, so the gesture means what it meant before there
	 * was anywhere to drop: a window of its own (sec 8.4). Dropping on
	 * the desktop and dropping on nothing are the same event to Qt, which
	 * is exactly the behaviour this needs.
	 */
	tear_out_tab(index);
}

void bssh_main_window::dragEnterEvent(QDragEnterEvent *event) {
	if (session_from_mime(event->mimeData())) event->acceptProposedAction();
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
	 */
	const QPoint at = event->position().toPoint();
	const bool on_the_bar = m_tabs && m_tabs->isVisible()
	                        && m_tabs->rect().contains(m_tabs->mapFrom(this, at));

	drop_session(session, !on_the_bar);
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
	bssh_session *session = session_at_tab(index);
	if (!session) return nullptr;

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
	if (index < 0 || index >= m_shown.size()) return nullptr;
	return m_shown.at(index);
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
			const int below = at.y() - m_tabs->height();

			if (below > BSSH_TEAR_OUT_DP && m_tabs->count() > 0) {
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
void bssh_main_window::changeEvent(QEvent *event) {
	QWidget::changeEvent(event);
	if (event && event->type() == QEvent::PaletteChange) on_registry_changed();
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

	while (m_tabs->count() > m_shown.size()) {
		m_tabs->removeTab(m_tabs->count() - 1);
	}
	while (m_tabs->count() < m_shown.size()) {
		m_tabs->addTab(QString());
	}

	for (int i = 0; i < m_shown.size(); ++i) {
		bssh_session *session = m_shown.at(i);
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

		if (session == m_current) m_tabs->setCurrentIndex(i);
	}

	/* One tab is not worth a row of chrome, least of all on a phone. */
	m_tabs->setVisible(m_shown.size() > 1);

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
	bssh_preset_t imported = bssh_preset_t::read_ini(document, &ok);

	if (!ok) {
		/*
		 * Said out loud rather than ignored. Being handed a file and
		 * silently doing nothing with it is indistinguishable from the
		 * app being broken, and the user picked this app on purpose.
		 */
		QMessageBox::warning(this, QStringLiteral("Not a preset"),
		                      QStringLiteral("BeerSSH could not read a preset from "
		                                      "that file."));
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


/* sec 6.2's touch floor, the same one the key row and the editors use. */
static const int BSSH_TOUCH_TARGET_DP = 48;


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
QMenu *bssh_main_window::build_terminal_menu(QWidget *parent) const {
	const bssh_app_settings::context_menu_t layout =
	        context_menu_layout(bssh_app_settings().context_menu(),
	                             bssh_platform::instance()
	                                     && bssh_platform::instance()->is_touch_primary());

	QMenu *menu = new QMenu(parent);
	menu->setObjectName(QStringLiteral("terminal_context_menu"));

	QAction *copy = menu->addAction(QStringLiteral("&Copy"));
	copy->setObjectName(QStringLiteral("terminal_copy"));
	copy->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+C")));
	copy->setEnabled(m_terminal_view && m_terminal_view->has_selection());

	QAction *paste = menu->addAction(QStringLiteral("&Paste"));
	paste->setObjectName(QStringLiteral("terminal_paste"));
	paste->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+V")));
	paste->setEnabled(m_current && !QGuiApplication::clipboard()->text().isEmpty());

	menu->addSeparator();

	QAction *select_all = menu->addAction(QStringLiteral("Select &All"));
	select_all->setObjectName(QStringLiteral("terminal_select_all"));

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
	menu->setStyleSheet(QStringLiteral("QMenu::item { min-height: %1px; padding: 8px 24px; }")
	                            .arg(BSSH_TOUCH_TARGET_DP));
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
	if (m_session_dialog) m_pages->setCurrentWidget(m_session_dialog);
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
	const int fits = bssh_layout_manager::max_panes_for(m_layout->size_class());
	split->setEnabled(m_layout->intent().size() < fits &&
	                   !m_layout->intent().contains(session));

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
	menu->deleteLater();

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

	const bool sessions_live = m_registry->live_count() > 0;

	close_action_t action = close_action(sessions_live,
	                                      m_asked_about_closing && m_background_on_close);

	if (action == ASK_FIRST) {
		QMessageBox box(this);
		box.setWindowTitle(QStringLiteral("BeerSSH"));
		box.setText(m_registry->live_count() == 1
		                    ? QStringLiteral("A session is still connected.")
		                    : QStringLiteral("%1 sessions are still connected.")
		                              .arg(m_registry->live_count()));
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

	for (bssh_session *session : m_registry->sessions()) {
		session->disconnect_from_host();
	}
	bssh_platform::instance()->set_keep_alive(false);
	event->accept();
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

	if (m_tabs && !m_tabs->isHidden()) wanted.rheight() += m_tabs->sizeHint().height();
	if (m_status_label) wanted.rheight() += m_status_label->sizeHint().height();
	if (m_key_row && !m_key_row->isHidden()) wanted.rheight() += m_key_row->sizeHint().height();

	return wanted;
}

bool bssh_main_window::showing_front_door() const {
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

	return store->load(QStringLiteral("profile:%1:password").arg(profile.id));
}

bool bssh_main_window::unlock_secret_store() {
	bssh_secret_store *store = bssh_secret_store::instance();
	if (store->is_unlocked()) return true;
	if (!store->needs_passphrase()) return false;

	bool ok = false;
	const QString passphrase = QInputDialog::getText(
	        this, QStringLiteral("Unlock saved passwords"),
	        QStringLiteral("Passphrase for the credential store\n(%1):")
	                .arg(store->backend_name()),
	        QLineEdit::Password, QString(), &ok);
	if (!ok) return false;

	if (store->unlock(passphrase)) return true;

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

	bssh_secret_store *store = bssh_secret_store::instance();
	if (!store->store(QStringLiteral("profile:%1:password").arg(session->profile().id), password)) {
		return;
	}

	/*
	 * The flag goes in the profile row so the next run knows to ask for
	 * the store passphrase. Written only after the secret itself landed,
	 * or a failed write would leave the app hunting for a password that
	 * was never saved.
	 */
	bssh_profile_t updated = session->profile();
	updated.has_stored_password = true;
	m_profile_store->save(updated);
}
