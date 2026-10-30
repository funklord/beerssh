#ifndef BSSH_HANDOFF_POINT_H
#define BSSH_HANDOFF_POINT_H

#include <QObject>
#include <QPoint>
#include <QString>
#include <QStringList>

#include <functional>

class QSocketNotifier;
class bssh_session;

/*
 * Where another BeerSSH process asks this one for a live session (project.md
 * sec 8.4): a unix socket in a directory only this user can enter, one per
 * process, named for the process.
 *
 * The exchange, each message bounded in time (bssh_handoff_wire):
 *
 *   taker  -> giver   "take" and the session's handoff id
 *   giver  -> taker   the session's state, with its descriptors -- or a
 *                     refusal saying why
 *   taker  -> giver   "taken", or "refused" and why
 *   giver  -> taker   "released", after which the session is the taker's
 *
 * The giver commits only on "taken": until then its session is stopped but
 * still its own, and anything else -- a refusal, a timeout, a taker that
 * vanished -- resumes it as if nothing had been asked. A peer of another
 * uid is refused before it is read: the state carries the session's keys.
 *
 * Desktop only. Android runs one process per app, and an update or a
 * process death takes all of them at once.
 */
class bssh_handoff_point : public QObject {
	Q_OBJECT

public:
	/*
	 * Finds the session a taker names, or null. The callee decides what
	 * may be given: a session that is not connected answers a refusal.
	 */
	using finder_t = std::function<bssh_session *(const QString &handoff_id)>;

	/* The handoff ids of every session this process would give, for a new
	 * version taking them all. */
	using lister_t = std::function<QStringList()>;

	explicit bssh_handoff_point(finder_t finder, QObject *parent = nullptr);
	~bssh_handoff_point() override;

	void set_lister(lister_t lister) { m_lister = std::move(lister); }

	/* Starts listening. False, with why, when the socket cannot be made. */
	bool listen(QString *why = nullptr);

	/* The one this process listens with, or null: what a drag payload
	 * names, so the process it is dropped on can come and take it. */
	static bssh_handoff_point *instance();

	/* Where the BeerSSH process with that pid would listen. */
	static QString path_for(qint64 pid);

	/* Where this process listens, for a drag payload or a new version to
	 * find it. Empty until listen() succeeds. */
	QString path() const { return m_path; }

	/* The directory every BeerSSH of this user listens in. */
	static QString directory();

	/*
	 * Removes the sockets in directory() that nobody listens on -- left by
	 * processes ended by a signal, which run no destructor -- and returns
	 * how many. Refusing a connection is the proof: a live process's
	 * socket accepts, so it is never touched (the holder asked what the
	 * processes share, 2026-10-30, and 66 of 67 found were such leftovers).
	 */
	static int sweep_stale();

	/*
	 * The taking side, run in the process that wants the session: asks the
	 * process listening at path for the session with that handoff id, and
	 * returns it imported and CONNECTED, or null with why. Blocks for at
	 * most timeout_ms.
	 */
	static bssh_session *take(const QString &path, const QString &handoff_id, QObject *parent,
	                          QString *why, int timeout_ms = 10000);

	/*
	 * Tells the process at target that a session of the process at source
	 * was let go over it, at global (X11's own drag: the source knows where
	 * it was released, the target does not). The target answers at once
	 * and takes the session from source after, so the source must return
	 * to its event loop to serve that.
	 */
	static bool offer(const QString &target, const QString &source, const QString &handoff_id,
	                  const QPoint &global, QString *why = nullptr);

	/* The ids the process at path would give a new version, and the
	 * request, once it has given them, that it quit. */
	static QStringList list(const QString &path, QString *why = nullptr);
	static bool request_quit(const QString &path, QString *why = nullptr);

	/*
	 * A drag from another BeerSSH is over the process at path, at global
	 * -- or, with active false, has left it or ended without a drop
	 * (sec 8.7a). The target draws where a drop would land; a drag shows
	 * its zones only in the process that started it otherwise.
	 */
	static bool hover(const QString &path, const QPoint &global, bool active,
	                  QString *why = nullptr);

signals:
	/*
	 * A session has been given away and is inert: the caller removes it
	 * from whatever shows it and deletes it. Emitted after the taker has
	 * confirmed, never before.
	 */
	void session_given(bssh_session *session);

	/* Another process let a session go over this one; see offer(). */
	void offered(const QString &source, const QString &handoff_id, const QPoint &global);

	/* Another process's drag is over this one, or no longer; see hover(). */
	void hovered(const QPoint &global, bool active);

	/* A new version has taken everything it could, and asks this to go. */
	void quit_requested();

private:
	void on_connection();
	void serve(int socket);

	finder_t m_finder;
	lister_t m_lister;
	QString m_path;
	int m_listener = -1;
	QSocketNotifier *m_notifier = nullptr;
};

/* How long the giving side waits for each message of an exchange. */
const int BSSH_HANDOFF_STEP_MS = 10000;

#endif /* BSSH_HANDOFF_POINT_H */
