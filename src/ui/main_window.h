#ifndef BSSH_MAIN_WINDOW_H
#define BSSH_MAIN_WINDOW_H

#include <QString>
#include <QWidget>

#include "input/input_router.h"
#include "ssh/ssh_session.h"

class QLabel;
class QTabBar;
class QStackedWidget;
class bssh_host_editor;
class bssh_preset_editor;
class bssh_host_list;
class bssh_layout_manager;
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
	 * Whether the front door is what is on screen. Exists because its
	 * absence was not caught by anything: the widgets were tested and
	 * screenshotted on their own, so a window that never built them
	 * looked fine everywhere except on a device.
	 */
	bool showing_front_door() const;

protected:
	void closeEvent(QCloseEvent *event) override;

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

	QStackedWidget *m_pages;
	QWidget *m_terminal_page;
	bssh_profile_store *m_profile_store;
	bssh_session_dialog *m_session_dialog;
	bssh_host_list *m_host_list;
	bssh_host_editor *m_host_editor;
	bssh_preset_editor *m_preset_editor;
	bssh_terminal_view *m_terminal_view;
	QLabel *m_status_label;
	bssh_session_registry *m_registry;
	bssh_layout_manager *m_layout;
	bssh_session *m_current;
	QTabBar *m_tabs;
	bssh_key_row *m_key_row;
	QString m_error;
	int m_cols;
	int m_rows;

	/* Remembered answer to the close prompt; unset until asked once. */
	bool m_asked_about_closing;
	bool m_background_on_close;
};

#endif /* BSSH_MAIN_WINDOW_H */
