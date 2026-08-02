#ifndef BSSH_SESSION_H
#define BSSH_SESSION_H

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

	void connect_to_host(const QString &password);

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

signals:
	void title_changed();
	void state_changed();
	void failed(const QString &reason);

private:
	bssh_profile_t m_profile;
	bssh_ssh_session *m_transport;
	bssh_emulator *m_emulator;
	bssh_input_router *m_input_router;
	QString m_remote_title;
	QString m_error;
	QString m_password;
	bool m_user_disconnected;
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
