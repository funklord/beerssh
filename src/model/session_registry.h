#ifndef BSSH_SESSION_REGISTRY_H
#define BSSH_SESSION_REGISTRY_H

#include <QList>
#include <QObject>

#include "model/profile.h"

class bssh_session;

/*
 * bssh_session_registry -- where sessions live (project.md sec 3, core
 * principle 2).
 *
 * The registry owns every session; views borrow them. That single
 * arrangement is what makes four otherwise-painful things fall out:
 * switching tabs, collapsing split panes back to tabs on a small screen
 * (sec 8.2), surviving Android activity recreation, and staying connected
 * in the background with no window at all (sec 5).
 *
 * It is also what the notification and tray tooltip count (sec 5.2): "2
 * sessions connected" is a fact about the registry, never about how many
 * panes happen to be on screen.
 *
 * Process-wide and single, even when the desktop grows more than one
 * window (sec 8.4): a second window is another view onto these sessions,
 * not another set of them.
 */
class bssh_session_registry : public QObject {
	Q_OBJECT

public:
	explicit bssh_session_registry(QObject *parent = nullptr);

	bssh_session *create(const bssh_profile_t &profile);

	/* Destroys the session and its connection. */
	void close(bssh_session *session);

	const QList<bssh_session *> &sessions() const { return m_sessions; }
	int count() const { return m_sessions.size(); }
	int live_count() const;

	bssh_session *at(int index) const;
	int index_of(bssh_session *session) const;

signals:
	void session_added(bssh_session *session);

	/* Emitted before the session is destroyed, so views can detach. */
	void session_closing(bssh_session *session);

	/* Any add, close, title or state change -- for the tab bar and the
	 * platform's session count. */
	void changed();

private:
	QList<bssh_session *> m_sessions;
};

#endif /* BSSH_SESSION_REGISTRY_H */
