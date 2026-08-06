#ifndef BSSH_SESSION_REGISTRY_H
#define BSSH_SESSION_REGISTRY_H

#include <QHash>
#include <QList>
#include <QObject>

#include "model/profile.h"

class bssh_conn_manager;
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

	/*
	 * The one registry this process has (sec 3, core principle 2; sec
	 * 8.4).
	 *
	 * A window is a VIEW onto it, never an owner of one. That was
	 * already the design and the window owned one anyway, which made a
	 * second top-level window impossible to add correctly: it would have
	 * brought a second registry with it, and sec 8.4 says in as many
	 * words not to let it.
	 *
	 * It is also the seam a client/server split would cut along. The
	 * intention is that windows behave as though they were separate
	 * processes talking to a session server -- so a window asks this
	 * object for sessions and never reaches into another window. Today
	 * the call is a pointer dereference; later it can be a message,
	 * without every caller changing.
	 *
	 * Tests construct their own instead of using this, which is why it
	 * is a shared default rather than the only way to get one.
	 */
	static bssh_session_registry *shared();

	bssh_session *create(const bssh_profile_t &profile);

	/* Destroys the session and its connection. */
	void close(bssh_session *session);

	const QList<bssh_session *> &sessions() const { return m_sessions; }
	int count() const { return m_sessions.size(); }
	int live_count() const;

	bssh_conn_manager *conn_manager_for(bssh_session *session) const;

	bssh_session *at(int index) const;
	int index_of(bssh_session *session) const;

	/*
	 * Reorders a session, for sec 8.3's drag-to-reorder.
	 *
	 * The ORDER lives here rather than in the tab bar, and that is the
	 * whole point of the method. Everything that acts on a tab -- close,
	 * the context menu, switching -- turns a tab index into a session
	 * with at(), so tab order and registry order are one thing. Let a
	 * dragged tab change only the bar and the two disagree until the next
	 * rebuild, which reads the registry and silently puts the tabs back;
	 * in between, closing a tab closes whichever session used to be
	 * there. Out of range or a no-op move does nothing and says nothing.
	 */
	void move(int from, int to);

signals:
	void session_added(bssh_session *session);

	/* Emitted before the session is destroyed, so views can detach. */
	void session_closing(bssh_session *session);

	/*
	 * A view has taken this session over, and any other view showing it
	 * must let go (sec 8.4: shown in exactly one place).
	 *
	 * Broadcast through the registry rather than passed between windows,
	 * because windows are meant to behave as though they were separate
	 * processes -- no window holds a pointer to another. The adopter
	 * emits this and then takes the session, so it is unaffected by its
	 * own announcement; everyone else drops it.
	 *
	 * The session is NOT closing and nothing about it changes. Only which
	 * view shows it does, which is why this is separate from
	 * session_closing: a view that treated a move as a close would tear
	 * down the very session being handed to somebody else.
	 */
	void session_taken(bssh_session *session);

	/* Any add, close, title or state change -- for the tab bar and the
	 * platform's session count. */
	void changed();

private:
	QList<bssh_session *> m_sessions;
	QHash<bssh_session *, bssh_conn_manager *> m_managers;
};

#endif /* BSSH_SESSION_REGISTRY_H */
