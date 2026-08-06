#ifndef BSSH_MAIN_WINDOW_H
#define BSSH_MAIN_WINDOW_H

#include <QString>
#include <QPointer>
#include <QWidget>

#include "model/app_settings.h"
#include "ui/long_press.h"

#include "input/input_router.h"
#include "ssh/ssh_session.h"

class QLabel;
class QTabBar;
class QStackedWidget;
class QDragEnterEvent;
class QDropEvent;
class QMimeData;
class bssh_host_editor;
class bssh_preset_editor;
class bssh_host_list;
class bssh_layout_manager;
class bssh_pane_container;
class bssh_key_row;
class bssh_profile_store;
class bssh_session_dialog;
class bssh_session;
class bssh_session_registry;
class bssh_terminal_view;

#include "model/profile.h"

/*
 * bssh_main_window -- the top-level window.
 *
 * A plain QWidget, deliberately, not a QMainWindow: the menubar/toolbar/
 * statusbar/dock scaffolding QMainWindow brings is desktop furniture that
 * has to be fought on a phone, and this window has to be the same widget
 * on both (project.md sec 1). The sibling fuzzypickles GUI made the same
 * call for the same reason.
 *
 * At M1 it holds one terminal view, one session and a status line. The
 * tab bar and the pane tree arrive at M7/M8, at which point sessions move
 * into bssh_session_registry and this window stops owning one -- the
 * pointer below is M1 scaffolding, not the design (sec 3, core principle
 * 2).
 */
class bssh_main_window : public QWidget {
	Q_OBJECT

public:
	/*
	 * target is an optional [user@]host[:port] from the command line.
	 * Empty means no connection: the app comes up showing an empty grid,
	 * which is all it could do before M1 anyway. Host profiles replace
	 * this at M6 (sec 10) -- a command-line target is a bring-up
	 * affordance, not the intended way in.
	 */
	explicit bssh_main_window(const QString &target = QString(), QWidget *parent = nullptr);

	/*
	 * The sessions this window is a view onto (sec 8.4). Process-wide and
	 * shared with every other window; a window never owns it.
	 *
	 * Exposed because a test drives sessions through the window it is
	 * asserting on, and because this is what a second window will be
	 * handed. Reaching for it with findChild() worked only while the
	 * window owned one, which is the arrangement being undone here.
	 */
	bssh_session_registry *registry() const { return m_registry; }

	/*
	 * What closing the window should do when sessions are still
	 * connected. A pure decision, separated from the dialog that asks it,
	 * so the policy can be tested without a message box on screen -- and
	 * so the Android back-button path can reuse it later.
	 */
	enum close_action_t {
		CLOSE_AND_QUIT,      /* nothing live; ordinary close */
		CLOSE_TO_BACKGROUND, /* keep sessions running, no window */
		ASK_FIRST            /* live sessions and no remembered answer */
	};

	static close_action_t close_action(bool sessions_live, bool remembered_background);

	/*
	 * Which front door to show (sec 8.5). A capability question, like the
	 * key row's: a convertible in tablet mode wants the touch list, and a
	 * phone docked with a keyboard and a monitor does not.
	 */
	enum front_door_t {
		FRONT_DOOR_SESSION_DIALOG, /* PuTTY-shaped, for pointer + keyboard */
		FRONT_DOOR_HOST_LIST       /* full-bleed list, for a thumb */
	};

	static front_door_t front_door_for(bool touch_primary);

	/*
	 * Whether this device opens a local shell at startup rather than its
	 * configuration (sec 2). Desktop does; touch does not, because an
	 * Android app sandbox has no userland worth driving a shell from.
	 *
	 * Static and beside front_door_for() for the same reason: it is a
	 * decision about the device, testable without building a window.
	 */
	static bool opens_a_local_shell(bool touch_primary);

	/*
	 * Whether the front door is what is on screen. Exists because its
	 * absence was not caught by anything: the widgets were tested and
	 * screenshotted on their own, so a window that never built them
	 * looked fine everywhere except on a device.
	 */
	bool showing_front_door() const;

	/*
	 * Imports the preset file this app was launched to open, if any
	 * (sec 10.5). Called by main() rather than from the constructor, and
	 * deliberately: it can put a modal dialog on screen, and --self-test
	 * runs with nobody to dismiss one. Constructing a window that may
	 * block forever is a window CI cannot check -- which is exactly what
	 * happened the first time this ran from the constructor.
	 */
	void import_launch_document();

	/*
	 * Opens what this device starts with: a local shell on desktop, and
	 * nothing on touch (sec 2).
	 *
	 * Called by main() rather than from the constructor, for the same
	 * reason import_launch_document() is -- and the reason was learned
	 * here the hard way. Opening it in the constructor forked a real
	 * shell for every bssh_main_window anything built, including the
	 * fifteen the test suite constructs and the one --self-test makes;
	 * and since a live session asks the platform to stay resident,
	 * quitOnLastWindowClosed went false and the smoke test never
	 * returned. A window is a widget tree, not a running program.
	 */
	void open_default_session();

protected:
	void changeEvent(QEvent *event) override;

	/*
	 * Watches the tab bar for sec 8.3's middle-click close, which QTabBar
	 * raises no signal for -- tabCloseRequested comes from its own close
	 * button and from nothing else.
	 */
	bool eventFilter(QObject *watched, QEvent *event) override;
	void closeEvent(QCloseEvent *event) override;

	/*
	 * The window's width in dp is what decides how many panes fit
	 * (sec 8.2), so a resize is a layout question rather than only a
	 * geometry one -- folding a foldable or tiling a window into a
	 * half-screen column collapses the splits to tabs and remembers them.
	 */
	void resizeEvent(QResizeEvent *event) override;

	/*
	 * A tab dragged out of another window (sec 8.3, sec 8.4).
	 *
	 * Qt's own drag-and-drop is what carries it, and that is the point
	 * rather than a convenience: the source window never touches this one.
	 * It offers a payload, this window decides whether it wants it, and
	 * the session is resolved out of the shared registry here -- which is
	 * the "windows behave as though they were separate processes" rule
	 * (sec 8.4) holding for the one gesture most likely to break it.
	 */
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dropEvent(QDropEvent *event) override;

private slots:
	void open_profile(const bssh_profile_t &profile);
	void on_add_host();
	void on_tab_changed(int index);
	void on_tab_close_requested(int index);
	void on_registry_changed();
	void on_grid_resized(int cols, int rows);
	void on_failed(const QString &reason);

private:
	/*
	 * A host's preset with its own overrides applied (sec 10.1). One place,
	 * because "which settings does this host actually have" is a question
	 * that must have exactly one answer -- two call sites resolving it
	 * their own way is how a host ends up with one font in a tab and
	 * another in a split.
	 */
	QJsonObject resolved_settings(const bssh_profile_t &profile) const;

	/* Re-applies a changed preset to the open sessions that follow it. */
	void reresolve_sessions(int preset_id);
	void on_pane_focus_changed(bssh_session *session);

	/* Reads sec 6.2's three-state setting and applies it to the key row. */
	void apply_key_row_visibility();
	void apply_device_settings();

	/*
	 * Adds a session to the visible panes rather than replacing them
	 * (sec 8.3's "move to split"). Separate from show_session because a
	 * tab click that silently split the window would be one click doing
	 * two things.
	 */
	void split_with(bssh_session *session);

	void connect_to(const QString &target);


	/*
	 * The credential flow (sec 11). Split out because it is a sequence of
	 * decisions -- is one stored, is the store unlocked, should this one
	 * be kept -- and burying that in a lambda is how it stops being
	 * reviewable.
	 */
	QString stored_password_for(const bssh_profile_t &profile);
	void offer_to_remember(bssh_session *session, const QString &password);
	bool unlock_secret_store();

	void save_layout();
	void restore_layout();
	void show_session(bssh_session *session);
	void detach_session();
	void update_status();

	/*
	 * The terminal's own context menu (sec 6.4). Public so a test can
	 * build it without synthesising a right-click, and static because it
	 * depends on nothing but the layout asked for.
	 */
public:
	static bssh_app_settings::context_menu_t context_menu_layout(
	        bssh_app_settings::context_menu_t setting, bool touch_primary);

private:
	void show_terminal_menu(const QPoint &at);

	/*
	 * Brings up whichever front door this device built (sec 8.5). The
	 * configuration must be reachable from a terminal on both platforms,
	 * and on desktop the terminal is now where the app starts.
	 */
	void show_front_door();
	void show_tab_menu(const QPoint &at);

	/* The session behind a tab of THIS window, or null (sec 8.4). */
	bssh_session *session_at_tab(int index) const;

public:
	/*
	 * Moves a session into this window, taking it out of whichever window
	 * was showing it (sec 8.4).
	 *
	 * The SESSION does not move: it stays in the registry, connected,
	 * with its scrollback and whatever is running. What moves is which
	 * view shows it -- which is why a torn-out tab is a new window and
	 * not a new process, and why this same call becomes a message when
	 * the client/server split arrives.
	 */
	void adopt_session(bssh_session *session);

	/*
	 * Takes a tab out into a window of its own (sec 8.3's drag, sec 8.4's
	 * multiple windows). Returns the new window, or null when there is
	 * nothing to tear out -- a window will not empty itself, because a
	 * window with no tabs and no front door showing is one nobody can do
	 * anything with.
	 */
	bssh_main_window *tear_out_tab(int index);

	/*
	 * Takes a session from another window and shows it here (sec 8.4).
	 *
	 * `beside` decides which: false makes it a tab, true puts it beside
	 * whatever this window is already showing -- a tile. That is the
	 * distinction a drop makes by where it lands, the tab bar or the
	 * panes, and it is expressed here as a plain command so the model can
	 * be driven and tested without synthesising a drag.
	 *
	 * Declines a tile where there is no room for another pane (sec 8.2's
	 * size classes) and falls back to a tab, because the alternative is a
	 * drop that appears to do nothing.
	 */
	void drop_session(bssh_session *session, bool beside);

private:
	/*
	 * The ownership half of adopting, without deciding how it is shown.
	 *
	 * adopt_session() is this plus show_session(), and show_session()
	 * REPLACES the intent with the one session -- right for a tab, wrong
	 * for a tile, because split_with() then finds the session already in
	 * the intent and declines. Separating the two is what lets a drop on
	 * the panes append instead.
	 */
	void take_session(bssh_session *session);

	/*
	 * Starts the drag a tab pulled clear of the bar becomes (sec 8.3).
	 *
	 * Blocks in Qt's nested drag loop until the pointer is released, and
	 * tears the tab out into its own window only when nobody accepted it
	 * -- so the gesture keeps its old meaning where there is nothing to
	 * drop onto, and gains a second one where there is.
	 */
	void start_tab_drag(int index);

public:
	/*
	 * The drag payload's format, and the session it names.
	 *
	 * A session has no id of its own, so what travels is its index in the
	 * process-wide registry, resolved at drop time rather than trusted: a
	 * stale index names nothing and is declined. Exposed so a test can
	 * build a drop without a window manager, since QDrag::exec() needs a
	 * real pointer grab and the offscreen platform has none.
	 */
	static QString session_mime_type();
	bssh_session *session_from_mime(const QMimeData *mime) const;

public:
	/*
	 * The menu, built but not shown. Separated from showing it because
	 * exec() blocks until somebody clicks, which no test can do -- and
	 * the part worth testing is the contents: which verbs, in what order,
	 * which of them are available, and how big the rows are.
	 */
	QMenu *build_terminal_menu(QWidget *parent) const;

	/*
	 * The tab menu, likewise built but not shown, and for the same reason
	 * -- it was a menu nothing could inspect, so which verbs it carried
	 * and which of them were available went untested while the terminal's
	 * next door had both pinned. Returns null for an index with no
	 * session behind it.
	 */
	QMenu *build_tab_menu(int index, QWidget *parent) const;

	/*
	 * The touch metrics, applied to any of this window's context menus.
	 * Static and public for the same reason as context_menu_layout(): it
	 * depends on nothing but the menu and the layout asked for, so a test
	 * can hand it a menu of its own.
	 */
	static void apply_menu_layout(QMenu *menu, bssh_app_settings::context_menu_t layout);

private:
	void elide_status();

	QStackedWidget *m_pages;
	QWidget *m_terminal_page;
	bssh_profile_store *m_profile_store;
	bssh_session_dialog *m_session_dialog;
	bssh_host_list *m_host_list;
	bssh_host_editor *m_host_editor;
	bssh_preset_editor *m_preset_editor;
	/*
	 * The FOCUSED pane's view, owned by the container rather than by this
	 * window, and null while no pane exists. Everything acting on "the
	 * terminal" means the focused one.
	 */
	bssh_terminal_view *m_terminal_view;
	bssh_pane_container *m_panes;

	/*
	 * The view whose signals are already connected. A plain pointer
	 * compared for identity, never dereferenced: it may be dangling after
	 * a pane rebuild, and comparing a dangling pointer against a fresh
	 * one is exactly the test wanted -- they differ.
	 */
	/*
	 * Which view's signals are already connected.
	 *
	 * A QPointer, not a raw one, and that is the whole fix for a bug this
	 * found the hard way: panes are destroyed and rebuilt, the allocator
	 * hands the replacement the address the old one had, and a raw
	 * comparison then says "already wired" about an object that has never
	 * been connected to anything. Every signal in that block -- the grid
	 * size, the link prompt, the primary-selection copy, the context menu
	 * -- is silently dead on that view.
	 *
	 * A QPointer is cleared when its object is destroyed, so the
	 * replacement compares unequal however the allocator behaves.
	 */
	QPointer<bssh_terminal_view> m_wired_view;
	QLabel *m_status_label;
	bssh_session_registry *m_registry;

	/*
	 * The sessions THIS window shows, in tab order (sec 8.4).
	 *
	 * The registry holds every session in the process; a window is a view
	 * onto some of them, and with more than one window a session appears
	 * in exactly one. The tab bar was built from the whole registry,
	 * which was indistinguishable from correct while there could only be
	 * one window and wrong the moment there could be two -- both would
	 * have shown every tab, and closing one anywhere would have closed it
	 * everywhere.
	 *
	 * Tab ORDER lives here too, for the same reason: which order a window
	 * shows its tabs in is that window's business, not a fact about the
	 * process. Dragging one to reorder used to move it in the registry,
	 * which was right while a window was the only view and would have
	 * rearranged the other window's tabs as a side effect.
	 */
	QList<bssh_session *> m_shown;
	bssh_layout_manager *m_layout;
	bssh_session *m_current;
	QTabBar *m_tabs;
	bssh_long_press *m_tab_long_press;
	bssh_key_row *m_key_row;
	QString m_error;

	/*
	 * The status line's text before it is shortened to fit. Kept because
	 * the shortening depends on the width, so it has to be redone on
	 * every resize from the original rather than from what is on screen.
	 */
	QString m_status_text;

	/* Remembered answer to the close prompt; unset until asked once. */
	bool m_asked_about_closing;
	bool m_background_on_close;
};

#endif /* BSSH_MAIN_WINDOW_H */
