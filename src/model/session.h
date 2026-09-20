#ifndef BSSH_SESSION_H
#define BSSH_SESSION_H

#include <QJsonObject>
#include <QObject>
#include <QString>

#include "model/profile.h"
#include "ssh/ssh_session.h"

class bssh_emulator;
class bssh_input_router;

/*
 * bssh_session -- one connection and everything that belongs to it
 * (project.md sec 3, core principle 2).
 *
 * The transport, the emulator, the screen, the scrollback, the images and
 * the input router all live here, and NOT in a widget. A view attaches to
 * a session and can be destroyed and rebuilt at will: that is what makes
 * tabs, split panes, Android activity recreation and background survival
 * bookkeeping rather than surgery.
 *
 * The rule that keeps it true: if a piece of state would be wrong after
 * the window closed and reopened, it belongs here.
 */
class bssh_session : public QObject {
	Q_OBJECT

public:
	explicit bssh_session(const bssh_profile_t &profile, QObject *parent = nullptr);
	~bssh_session() override;

	const bssh_profile_t &profile() const { return m_profile; }

	/*
	 * The host row was edited under a session that is already open.
	 *
	 * This copy is taken once, at construction, and used for everything
	 * the session does afterwards -- what a reconnect dials, which
	 * identity file and jump host it uses, what the tab is called, and
	 * which preset a propagation pass thinks it follows. Nothing used to
	 * refresh it, so an edit made while connected was inert until the tab
	 * was closed, and worse than inert in two places: a later preset edit
	 * re-resolved against the STALE overrides and undid the change for
	 * good, and storing a password wrote the whole stale row back over
	 * the edited one.
	 *
	 * The id is not allowed to change -- that would make this a different
	 * host rather than an edited one.
	 */
	void set_profile(const bssh_profile_t &profile);

	/*
	 * The resolved settings for this host: the preset's values with the
	 * host's own overrides applied (sec 10.1), as bssh_settings_resolver
	 * produced them.
	 *
	 * They live on the session rather than being looked up by whatever
	 * widget needs one, for the reason sec 3 puts all session state here:
	 * a pane can be destroyed and rebuilt by a layout change, and a view
	 * that resolved its own font would go back to the default every time
	 * the window was narrow enough to collapse the splits.
	 *
	 * Empty until someone sets them, which is honest -- a session created
	 * from a bare host:port has no preset behind it.
	 */
	const QJsonObject &settings() const { return m_settings; }
	void set_settings(const QJsonObject &settings);

	bssh_emulator *emulator() const { return m_emulator; }
	bssh_input_router *input_router() const { return m_input_router; }
	bssh_ssh_session *transport() const { return m_transport; }

	/*
	 * The tab label: the OSC title the remote set, falling back to the
	 * profile name (sec 8.3). Never the widget's idea of a title.
	 */
	QString title() const;

	bssh_ssh_session::state_t state() const;
	bool is_live() const;

	/*
	 * A bell that rang while this session was not the one on screen
	 * (sec 8.3). The whole value of a bell is telling you about the tab
	 * you are NOT looking at, so it is remembered rather than played and
	 * forgotten.
	 */
	bool has_unseen_bell() const { return m_unseen_bell; }
	void mark_seen();

	/*
	 * Whether a view is showing this session right now (sec 8.4: exactly
	 * one place, or none at all).
	 *
	 * Readable because the registry has to know it: a notification is for
	 * the session you are NOT looking at, and the window-level handlers
	 * cannot answer it -- each one knows only whether IT is showing the
	 * session, so with two windows open every one of them would have to
	 * agree, and none of them holds a pointer to another.
	 */
	bool is_on_screen() const { return m_on_screen; }

	/*
	 * True when the remote shell ended by itself rather than the
	 * connection dropping. Reconnecting one of these reopens a shell the
	 * user deliberately exited, which is the difference between a
	 * terminal that survives a flaky network and one that will not let
	 * you leave.
	 */
	bool exited_cleanly() const { return m_remote_exited; }
	int exit_status() const { return m_exit_status; }

	/* How the transport reports each kind of ending. */
	void note_remote_exit(int exit_status);
	void note_connection_lost();

	/*
	 * Connects. No password up front: keys are tried first, and one is
	 * requested through needs_password() only if they fail.
	 */
	void connect_to_host();

	void supply_password(const QString &password);

	/* Trust the host key that is being asked about, and carry on. */
	void accept_host_key();

	/*
	 * Unlocks the private key. Not retained: unlike the password, this
	 * never needs replaying -- a reconnect re-runs key auth, and holding
	 * a key passphrase for the life of a session would be strictly worse
	 * than the password case sec 11 already dislikes.
	 */
	void supply_key_passphrase(const QString &passphrase);

	/* True when this session got in without a password at all. */
	bool used_key_auth() const { return m_used_key_auth; }

	/*
	 * Reconnect after a drop. A NEW remote shell: the running program and
	 * the working directory are gone, and only the local scrollback
	 * survives (sec 5.3). Callers must not present this as resumption.
	 */
	void reconnect();

	void disconnect_from_host();

	/* True when the last disconnect was asked for rather than suffered. */
	bool disconnect_was_requested() const { return m_user_disconnected; }

	/* Grid size for this session, from whichever view is showing it. */
	void resize(int cols, int rows);

	/* Shown or hidden -- drives whether a bell counts as unseen. */
	void set_on_screen(bool on_screen);

signals:
	/*
	 * A bell rang while nothing was showing this session (sec 5.5).
	 *
	 * Separate from state_changed, which fires for everything and would
	 * make a listener re-derive the one fact it wants. The condition is
	 * evaluated once, where bell_enabled and the on-screen flag are
	 * already in hand, so a listener cannot get it subtly different.
	 */
	void bell_while_away();

	void title_changed();
	void state_changed();

	/*
	 * The resolved settings changed -- a preset was edited, or this host
	 * overrode a field. Whatever is showing the session re-reads them;
	 * the font and size are the ones that change the grid (sec 9.5), so
	 * this ends in a window-change rather than only a repaint.
	 */
	void settings_changed();

	/*
	 * Somebody closed this session on purpose (sec 5.3).
	 *
	 * Emitted BEFORE the transport is told, so that whatever is watching
	 * has the fact before the DISCONNECTED state arrives -- the reconnect
	 * manager decides from the state change, and learning afterwards
	 * would be learning one scheduled attempt too late.
	 */
	void user_disconnected();

	/* The shell ended: the window says so instead of silently retrying. */
	void shell_exited(int exit_status);

	/* Keys did not get in; the window should ask. */
	void needs_password();
	void needs_key_passphrase();

	/* A host key needs a person's answer -- see bssh_ssh_session. */
	void host_key_unknown(const QString &fingerprint);
	void host_key_changed(const QString &fingerprint);
	void host_key_revoked(const QString &fingerprint);
	void failed(const QString &reason);

private:
	bssh_profile_t m_profile;
	QJsonObject m_settings;
	bssh_ssh_session *m_transport;
	bssh_emulator *m_emulator;
	bssh_input_router *m_input_router;
	QString m_remote_title;
	QString m_error;
	QString m_password;
	bool m_user_disconnected;
	bool m_unseen_bell;
	bool m_remote_exited;
	int m_exit_status;
	bool m_used_key_auth;
	bool m_password_requested;
	bool m_on_screen;
	int m_cols;
	int m_rows;
};

/*
 * Registered because signals carry it -- bssh_session_registry's
 * session_added/session_closing and the pane container's focus_changed.
 * Without this a queued connection cannot deliver it and QSignalSpy
 * cannot record it, and both fail at RUNTIME with a warning rather than
 * at compile time. It surfaced as a test that could not see a signal it
 * was watching.
 */
Q_DECLARE_METATYPE(bssh_session *)

#endif /* BSSH_SESSION_H */
