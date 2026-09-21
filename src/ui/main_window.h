#ifndef BSSH_MAIN_WINDOW_H
#define BSSH_MAIN_WINDOW_H

#include <QHash>
#include <QSizePolicy>
#include <QString>
#include <QPointer>
#include <QWidget>

#include "model/app_settings.h"
#include "ui/long_press.h"

#include "input/input_router.h"
#include "ssh/ssh_session.h"

class QAction;
class QLabel;
class QMenuBar;
class QTabBar;
class QListWidget;
class QStackedWidget;
class QKeyEvent;
class QDragEnterEvent;
class QShowEvent;
class QScreen;
class QDropEvent;
class QMimeData;
class bssh_host_editor;
class bssh_preset_editor;
class bssh_host_list;
class bssh_layout_manager;
class bssh_pane_container;
class bssh_input_selector;
class bssh_find_bar;
class bssh_key_row;
class bssh_soft_keyboard;
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
	 * launch is the session the command line asked for (sec 8.6), already
	 * assembled by main(): a destination, a saved session, or a local
	 * shell carrying a command, a working directory and a title.
	 *
	 * A default-constructed profile means it asked for none, and the
	 * window comes up on the front door (sec 8.5). It used to be a bare
	 * [user@]host[:port] string that this window parsed itself; a profile
	 * carries the other five things -e and --title and --hold have to say,
	 * and the parsing moved to bssh_profile_t::from_target where both
	 * readers of a destination can share it.
	 */
	explicit bssh_main_window(const bssh_profile_t &launch = bssh_profile_t(),
	                           QWidget *parent = nullptr);

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
	 * The host and preset rows this window reads and writes. Process-wide
	 * and shared with every other window, exactly like the registry above
	 * -- the window owns neither.
	 *
	 * It DID own one, and that was the bug: two stores over one hosts.ini
	 * are two full-file writers with no reader between them, so a host
	 * saved in one window was erased by the next save in another, and
	 * both windows minted the same id for a new host (sec 8.4).
	 *
	 * Exposed for the same reason: an edit to a row has to reach the
	 * sessions already open on it, and that wiring cannot be asserted on
	 * without the store the window actually listens to. A second store
	 * pointing at the same file is a different object and emits nothing
	 * this window hears, so the test would pass against no wiring at all.
	 */
	bssh_profile_store *profile_store() const { return m_profile_store; }

	/*
	 * The panes this window is showing. Exposed for the same reason the
	 * two above are: the invariant worth asserting after a session closes
	 * is that no VIEW is left attached to it, and "the app did not crash"
	 * cannot express that -- reading freed memory usually returns stale
	 * bytes rather than faulting, so a survival test passes against the
	 * bug it was written for.
	 */
	bssh_pane_container *panes() const { return m_panes; }

	/*
	 * The terminals this window HOLDS, in tab order -- panes and tabs
	 * together. Exposed for the same reason as the three above, and for
	 * one of its own: since a pane and a tab became different states
	 * (sec 8.3), counting the tab bar no longer answers "how many
	 * terminals does this window have", and several tests were asking
	 * it that way. A proxy that stops being a proxy is worse than no
	 * accessor, because the tests go on passing while measuring
	 * something else.
	 */
	QList<bssh_session *> shown_sessions() const { return m_shown; }

	/*
	 * The status line's text, before eliding.
	 *
	 * Exposed for the same reason as the four above, and it had none at
	 * all until a defect needed one: this line composes what it says from
	 * the CURRENT session and an error string set from anywhere, so
	 * whether a message belongs to the session it is printed beside is a
	 * question only the composed text can answer. The label is elided to
	 * the window width, so reading the widget would assert about layout
	 * rather than about attribution.
	 */
	QString status_text() const { return m_status_text; }

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
	 * Whether this paste should be asked about first (sec 6.4).
	 *
	 * A pure decision, separated from the dialog that asks it, the same
	 * way close_action() is -- so the rule can be tested without a
	 * message box on screen, and so there is one rule rather than one per
	 * place a paste can start.
	 *
	 * More than one LINE, not merely a trailing newline: copying a
	 * command with its line ending is the ordinary case, and a warning
	 * that fires on it is a warning people turn off -- after which it
	 * protects nobody from the case that matters, which is several
	 * commands arriving at once.
	 */
	static bool paste_needs_confirming(const QString &text, bool warn_enabled);

	/*
	 * Whether the front door is what is on screen. Exists because its
	 * absence was not caught by anything: the widgets were tested and
	 * screenshotted on their own, so a window that never built them
	 * looked fine everywhere except on a device.
	 */
	bool showing_front_door() const;

	/*
	 * Brings up whichever front door this device built (sec 8.5). The
	 * configuration must be reachable from a terminal on both platforms,
	 * and on desktop the terminal is now where the app starts.
	 *
	 * Public because it is a window ACTION rather than an internal step:
	 * the terminal's context menu raises it, and so must anything else
	 * that offers the settings.
	 */
	void show_front_door();

	/* Puts it away without touching whatever is behind it. */
	void hide_front_door();

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

	/*
	 * Reopens the hosts a previous run left open (sec 8.4), unconnected.
	 *
	 * Called by main() and not by the constructor, and it joins the other
	 * two above for the reason they give: it creates sessions, and the
	 * fifteen windows the test suite builds have no business reopening
	 * whatever the person running them last had on screen. main() also
	 * knows the one thing this cannot: whether the command line already
	 * named a session, in which case a restore would bury it under the
	 * tabs from last time.
	 */
	void restore_layout();

	/*
	 * How big this window has to be for a terminal of cols x rows
	 * (sec 8.6's --geometry).
	 *
	 * Cells, because that is what a terminal is measured in: xterm has
	 * counted its geometry this way since 1984, and a pixel size means a
	 * different grid on every font and every display scale. The cell
	 * comes from the focused view where there is one, so the answer
	 * follows the font that view actually resolved (sec 9.5), and from a
	 * throwaway view of the same class where there is not -- rather than
	 * from a second copy of the metrics arithmetic, which would be free
	 * to disagree with the one that draws.
	 *
	 * The chrome around the grid -- tab bar, status line, key row, the
	 * pane's own border -- is whatever this window is currently wrapping
	 * around its terminal, measured rather than enumerated: it is the
	 * difference between the window and the view inside it, which needs
	 * no list of the widgets in between and cannot go stale when one is
	 * added. Only what is SHOWING counts, which is the point -- a window
	 * with one tab hides its tab bar (sec 8.3), and reserving a strip for
	 * one would make every --geometry a row short.
	 *
	 * Not const: it activates the layout, because main() asks before the
	 * window has ever been shown and an unlaid-out window has no idea how
	 * big anything in it is.
	 */
	QSize size_for_grid(int cols, int rows);

protected:
	void changeEvent(QEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;

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
	 * Where the screen watch is armed (sec 9.5). The window handle does
	 * not exist until the window is shown, and QWindow::screenChanged is
	 * the only public signal that says a window has moved to a different
	 * display -- QEvent::DevicePixelRatioChange would be tidier and
	 * arrived in Qt 6.6, above the 6.4 the CI runner carries (sec 12.4).
	 */
	void showEvent(QShowEvent *event) override;

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

	/*
	 * Every live session takes its host row from the store again, and
	 * re-resolves against it. Wired once to the store's own `changed()`
	 * rather than to each of the four places that write a row -- the
	 * desktop dialog, the touch editor, the ssh_config import and the
	 * password write-back -- because four call sites are four chances to
	 * forget, and sec 6.4 already records what that costs.
	 */
	void refresh_sessions_from_store();

	/*
	 * Everything this window listens to on one session. Called when a
	 * session is created and again when one is ADOPTED from another
	 * window, which is the road that used to wire nothing at all.
	 */
	/*
	 * The typed-password prompt. Two roads reach it: a host with nothing
	 * stored, and one whose stored password was not unlocked (sec 11).
	 */
	void ask_for_password(bssh_session *session);

	void wire_session(bssh_session *session);

	/* The chords, acting on whichever session this window is showing. */
	void handle_shortcut(bssh_input_router::shortcut_t shortcut);
	void on_pane_focus_changed(bssh_session *session);

	/*
	 * Lets only the page on screen set this window's minimum size
	 * (sec 8.5, sec 8.6).
	 *
	 * A QStackedWidget asks EVERY page how small it may be and takes the
	 * largest answer, so the front door's category tree and form were
	 * setting a floor of 520x460 on a window showing a terminal --
	 * which meant a desktop user could not drag a terminal window small,
	 * and --geometry below about 57 by 21 came out bigger than it was
	 * asked for. The pages that are not on screen have nothing to say
	 * about it: their size policy goes to Ignored, which is exactly the
	 * "this widget's hint does not count" that qSmartMinSize reads, and
	 * comes back when the page is shown again.
	 *
	 * Kept per page rather than assumed, because a page that had a policy
	 * of its own would otherwise silently acquire the default on its
	 * first hiding.
	 */
	void keep_the_minimum_to_the_shown_page();
	QHash<QWidget *, QSizePolicy> m_page_policies;

	/* Reads sec 6.2's three-state setting and applies it to the key row. */
	void apply_key_row_visibility();

	/*
	 * Which on-screen keyboard is showing (sec 6.2). Exactly one of them,
	 * or the modifiers appear twice and the two copies disagree about
	 * what is armed.
	 */
	void apply_keyboard_choice();
	void apply_keyboard_style();

	/*
	 * Set by the keyboard's own Hide key and cleared by a tap in the
	 * terminal. Not persisted: it is about this session's screen, not a
	 * preference, and a keyboard that stayed hidden across a restart
	 * would look like the setting had changed itself.
	 */
	bool m_keyboard_hidden_by_user;
	QWidget *m_device_settings;

	/*
	 * The settings index's rows, kept so their separator can be redrawn
	 * when the palette changes (sec 8.5). A fixed grey that reads as a
	 * hairline on the light theme is invisible on the dark one, which is
	 * the reason the host list rebuilds its own.
	 */
	QListWidget *m_settings_sections;
	void style_settings_index();

	/*
	 * Pushes the selector's mode to every pane (sec 8.7). Called when the
	 * user changes it and after a rebuild, because a freshly built view
	 * starts from its own default and would otherwise ignore a mode set
	 * before it existed -- the same shape as the device settings above.
	 */
	void apply_input_mode();

	/*
	 * Find in the scrollback (sec 6.4): open the bar and focus it, run the
	 * focused pane's search as the query changes or a step is asked for,
	 * and put the count back on the bar. The search lives in the view; the
	 * window only routes the bar to whichever pane has focus.
	 */
	/*
	 * Zoom the focused session's text (sec 9.5). The window names the
	 * session; the session owns the number; the pane redraws.
	 */
	void zoom_current(int steps);
	void reset_zoom_current();

	/* Drop the focused session's history (sec 6.4). */
	void clear_scrollback();

	/* Clear the modes a dead program left behind (sec 6.4, sec 5.3). */
	void reset_terminal_state();

	void open_find();
	void run_find(const QString &query);
	void step_find(int direction);
	void close_find();
	void update_find_status();

	/*
	 * Keeps the column's layout intent equal to the shown list (sec 8.7),
	 * so the column holds every session the window holds and the tab bar
	 * has nothing left to convert. Called wherever the shown list changes;
	 * a no-op outside the column arrangement.
	 */
	void sync_column_intent();

	/*
	 * Adds a session to the visible panes rather than replacing them
	 * (sec 8.3's "move to split"). Separate from show_session because a
	 * tab click that silently split the window would be one click doing
	 * two things.
	 */
	void split_with(bssh_session *session);


	/*
	 * The credential flow (sec 11). Split out because it is a sequence of
	 * decisions -- is one stored, is the store unlocked, should this one
	 * be kept -- and burying that in a lambda is how it stops being
	 * reviewable.
	 */
	QString stored_password_for(const bssh_profile_t &profile);
	void offer_to_remember(bssh_session *session, const QString &password);
	bool unlock_secret_store();

	/*
	 * What this window is called (sec 8.3), from the format the device
	 * setting carries and whatever session is on screen.
	 */
	void update_window_title();

	/*
	 * Another session, in a tab or in a window of its own (sec 6.1's
	 * new-tab and new-window chords).
	 */
	void open_another_session(bool in_a_window);

	/*
	 * Every paste goes through here: the shortcut, the context menu and
	 * the middle button. Three call sites asking the router directly is
	 * three places for sec 6.4's warning to be forgotten, and it was
	 * missing from all three.
	 */
	void paste_into_current(const QString &text);

	void save_layout();

	/*
	 * How many of the sessions THIS window shows are live. Closing a
	 * window is a question about the window, not about the process
	 * (sec 8.4), and the registry's own live_count() answers the other
	 * one.
	 */
	int live_shown_count() const;

	/*
	 * Rebuilds the tab bar from m_shown minus whatever is on screen as a
	 * pane. A tab and a pane are different states (sec 8.3), and this is
	 * the one place that knows which sessions are in which.
	 */
	void refresh_tab_bar();

	/*
	 * The sessions this window holds and is NOT showing, in tab order.
	 * Derived from m_shown and the pane list rather than maintained
	 * beside them -- two lists kept in step by hand drift, and the tab
	 * bar's indices are read back out of this one.
	 */
	QList<bssh_session *> m_tab_order;
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
	 * Open a URL the remote chose, after asking. One method because there
	 * are two ways in -- Ctrl+click and the long-press menu -- and a
	 * second copy of the prompt is a second place for it to be forgotten.
	 */
	void open_remote_link(const QString &uri);
	void show_tab_menu(const QPoint &at);

	/*
	 * The desktop menu bar (sec 8.3). Built once and kept, so that
	 * refresh_menu_bar() can re-label the shortcuts when a chord is
	 * rebound and enable or disable the items as the session changes.
	 */
	void build_menu_bar();
	void refresh_menu_bar();

	/*
	 * When each edit verb is available. ONE answer each, because this
	 * window offers both of them from two menus -- the menu bar and the
	 * right-click menu -- and the two disagreed: the bar enabled Paste
	 * whenever a session existed, while the context menu also required
	 * something on the clipboard. Same verb, same instant, greyed in one
	 * place and offered in the other.
	 */
	bool can_copy() const;
	bool can_paste() const;

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

	/*
	 * Whether a point in this window's coordinates is on the tab ROW.
	 *
	 * The row, not the widget, and that distinction was a bug: a QTabBar
	 * is only as wide as its tabs, so with one tab open it covers about
	 * 123 pixels of a 1000-pixel window. Asking the widget meant a drop
	 * at x=60 un-split correctly while the identical drop at x=340 --
	 * the same row, visually the same strip -- did nothing at all.
	 * Nobody aiming at "the tab bar" is aiming at a widget.
	 *
	 * Public because it is the decision, and a synthesised QDropEvent
	 * does not reach dropEvent() -- Qt's drop delivery wants a real
	 * drag -- so the geometry is what a test can hold.
	 */
	bool point_is_on_tab_row(const QPoint &at) const;

	/* Whether any other window of ours is still up. */
	bool other_windows_exist() const;

	/*
	 * What the pointer carries during a session drag.
	 *
	 * Public so it can be RENDERED. The drag itself runs QDrag::exec(),
	 * a nested loop waiting on a real pointer, so nothing headless can
	 * drive it -- and a pixmap nobody has looked at is the same kind of
	 * claim as a keycap nobody has looked at.
	 */
	QPixmap session_drag_pixmap(bssh_session *session) const;

	/*
	 * What the Split menu items mean: a NEW terminal beside the current
	 * one. They used to pass m_current to split_with(), which always
	 * declines it, so the action did nothing at all (sec 8.3).
	 *
	 * Reachable from a test for the same reason drop_session is: the
	 * defect was the ARGUMENT the menus passed, so a test that called
	 * split_with() with something sensible would have passed throughout.
	 */
	void split_beside_current();

	/*
	 * Takes a session off the screen and leaves it in the bar -- the
	 * un-split, and the other direction of "Show beside current"
	 * (sec 8.3). Public because it is a plain command with two front
	 * doors: the terminal menu's verb, and a pane dropped on the tab
	 * bar.
	 */
	void show_as_tab(bssh_session *session);

	/*
	 * Picks a session up as a drag, wherever the gesture came from -- a
	 * tab pulled clear of the bar, or Alt+drag on a pane. One function,
	 * because what a drop MEANS must not depend on which half of the
	 * window the drag started in.
	 */
	void start_session_drag(bssh_session *session);


	/* The session into a window of its own; nullptr if it will not go. */
	bssh_main_window *tear_out(bssh_session *session);

	/*
	 * Move a pane one place earlier or later in the arrangement.
	 *
	 * The touch route to REORDERING, and the reason it is a method
	 * rather than a drag: dragging a pane by its grip works with a
	 * pointer and cannot work by touch. QDrag hands the gesture to the
	 * platform, Android makes the whole window the payload, and this
	 * device's window manager claims it as drag-to-split-screen -- the
	 * app is halved, an app picker appears, and dropEvent never runs.
	 * Measured on a Fold at two drag distances and confirmed by hand.
	 *
	 * So the arrangement is reordered through the menu, which nothing
	 * can intercept, and the desktop keeps the drag it already had.
	 * Direction is negative for earlier and positive for later; a move
	 * off either end does nothing rather than wrapping, since a wrap
	 * makes "left" mean "far right" at exactly the moment a user is
	 * looking at the leftmost pane.
	 */
	void move_pane(bssh_session *session, int direction);

	/*
	 * Move a tab one place along THIS window's bar.
	 *
	 * The window's own order, not the registry's, for the reason the
	 * drag handler gives: which order a window shows its tabs in is that
	 * window's business, and moving them in the registry rearranges
	 * every other window's tabs as a side effect (sec 8.4).
	 *
	 * It also has to be the tab order rather than the registry because a
	 * registry step can be INVISIBLE: the sessions on screen as panes sit
	 * in the same registry and are not in the bar, so moving a tab past
	 * one of those changes nothing a user can see. Measured on a phone --
	 * the menu item fired, the session moved from registry index 0 to 1,
	 * and the bar did not move.
	 */
	void move_tab(int from, int to);

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
	/*
	 * Opens a host as far as showing it, and stops short of connecting.
	 * open_profile() is this plus connect_to_host(); a restore is this
	 * alone (sec 8.4).
	 */
	bssh_session *prepare_profile(const bssh_profile_t &profile);

	void take_session(bssh_session *session);

	/*
	 * The window moved to another display, so every pane re-measures its
	 * cell against the new one (sec 9.5). Connected once, on first show.
	 */
	void on_screen_changed();
	bool m_screen_watched = false;

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
	QMenu *build_terminal_menu(QWidget *parent, const QString &link = QString()) const;

	/*
	 * What choosing one of that menu's entries does. Public and separate
	 * from show_terminal_menu so the suite can drive it: QMenu::exec runs
	 * its own event loop, so a headless test can build the menu and find
	 * an entry but can never choose one.
	 */
	void act_on_terminal_menu(QMenu *menu, const QAction *chosen,
	                           const QString &link = QString());

	/*
	 * Re-read the device settings and apply what changed. Public for the
	 * reason above in its own form: the only callers are signals from the
	 * settings surfaces, so a suite could reach this code only by
	 * building one of those pages and driving it -- and a setting that
	 * takes effect at the next start but not in the window somebody
	 * changed it in is a defect no startup test can see.
	 */
	void apply_device_settings();

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

	/*
	 * Where Back goes from the page now showing, and nullptr on the
	 * pages Back does not leave.
	 *
	 * Every sub-page used to return to the host list whatever it was
	 * opened from, so reaching the settings from a live session's
	 * keyboard and pressing Back put the session out of sight -- the
	 * page it came from is the only answer that is right from both
	 * doors.
	 */
	QWidget *m_back_target;

	/* Show a sub-page, remembering what to return to. */
	void show_page(QWidget *page);

	/* Leave the page now showing. False when there is nothing to leave. */
	bool go_back();
	bool m_front_door_up;
	QMenuBar *m_menu_bar;
	QWidget *m_tab_row;
	QList<QAction *> m_session_actions;
	QAction *m_copy_action;
	QAction *m_paste_action;
	QAction *m_split_action;
	QAction *m_detach_action;
	QWidget *m_terminal_page;
	bssh_profile_store *m_profile_store;

	/*
	 * The sessions this window has already wired. Qt::UniqueConnection
	 * cannot do this job -- it is refused with a lambda, which sec 6.4
	 * records costing this project a feature twice.
	 */
	QSet<bssh_session *> m_wired_sessions;
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
	 * The one input selector for this window (sec 8.7): the always-visible
	 * control that says what a touch body-drag does, and the window pushes
	 * its state to every pane. Present only where a thumb is the pointer.
	 */
	bssh_input_selector *m_input_selector;
	bssh_find_bar *m_find_bar;

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
	bssh_soft_keyboard *m_keyboard;
	QString m_error;

	/*
	 * The status line's text before it is shortened to fit. Kept because
	 * the shortening depends on the width, so it has to be redone on
	 * every resize from the original rather than from what is on screen.
	 */
	QString m_status_text;

	/*
	 * sec 9.4's substitution note, and the font id it was computed for.
	 *
	 * Cached against the id because answering it costs a
	 * QFontDatabase::families() scan, and update_status() runs on every
	 * state change, every resize and every keystroke that scrolls -- the
	 * same argument the status-line setting is read elsewhere for, and
	 * the same paint path.
	 */
	QString m_font_note;
	QString m_font_note_id;

	/* Remembered answer to the close prompt; unset until asked once. */
	/*
	 * Read once by apply_device_settings and kept: update_status runs on
	 * every state change and every resize, and an INI read there is a
	 * file opened in the paint path.
	 */
	bssh_app_settings::status_line_t m_status_line;
	QString m_window_title_format;

	bool m_asked_about_closing;
	bool m_background_on_close;
};

#endif /* BSSH_MAIN_WINDOW_H */
