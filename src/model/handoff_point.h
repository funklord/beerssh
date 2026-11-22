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

	/*
	 * A terminal this process would give, as another process's Terminals
	 * sidebar lists it: the handoff id to take it by, and its title.
	 */
	struct listed_t {
		QString id;
		QString title;
	};
	using cataloguer_t = std::function<QList<listed_t>()>;

	/* How this process has its terminals laid out, as
	 * bssh_arrangement_t::to_text() writes it, named by hand-off id. */
	using arranger_t = std::function<QString()>;

	explicit bssh_handoff_point(finder_t finder, QObject *parent = nullptr);
	~bssh_handoff_point() override;

	void set_lister(lister_t lister) { m_lister = std::move(lister); }
	void set_cataloguer(cataloguer_t cataloguer) { m_cataloguer = std::move(cataloguer); }
	void set_arranger(arranger_t arranger) { m_arranger = std::move(arranger); }

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
	 * Asks the process at path to restart into the new version installed
	 * over it, keeping its sessions -- as each window is a process, one
	 * Restart in the update bar is passed to the rest (the holder,
	 * 2026-11-21). A process whose program was not replaced does nothing;
	 * one from before the word ignores it and closes. Short, as the window
	 * asking waits on each.
	 */
	static bool request_restart(const QString &path, QString *why = nullptr, int timeout_ms = 250);

	/*
	 * The terminals the process at path would give, with their titles,
	 * for a Terminals sidebar (the holder, 2026-11-20). Its own request
	 * rather than a wider "list": a BeerSSH from before it ignores a word
	 * it does not know and closes, which reads here as an empty answer,
	 * and a new version taking everything over from an old one still asks
	 * the old "list". Bounded by timeout_ms, short, since a sidebar asks
	 * every process while somebody waits.
	 */
	static QList<listed_t> terminals(const QString &path, QString *why = nullptr,
	                                 int timeout_ms = 500);

	/*
	 * The arrangement of the process at path: its windows and the panes
	 * in them, for a new version to build the same (the holder,
	 * 2026-11-20). Empty from a BeerSSH from before it, which ignores the
	 * word -- the new version then shows what it takes as tabs.
	 */
	static QString arrangement(const QString &path, QString *why = nullptr);

	/* Where every OTHER BeerSSH of this user listens: directory()'s
	 * sockets less this process's own. A stale one simply refuses. */
	static QStringList peers();

	/*
	 * A drag from another BeerSSH is over the process at path, at global
	 * -- or, with active false, has left it or ended without a drop
	 * (sec 8.7a). The target draws where a drop would land; a drag shows
	 * its zones only in the process that started it otherwise.
	 *
	 * Asked every 30 ms of a drag, on the dragging window's thread, and
	 * only to light the target's zones: patient for a quarter of a second,
	 * not the hand-off's ten, which a process taking the question and
	 * sitting on it held the drag for, each time.
	 */
	static bool hover(const QString &path, const QPoint &global, bool active,
	                  QString *why = nullptr, int timeout_ms = 250);

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

	/* Another window's Restart, for this process too; see request_restart(). */
	void restart_requested();

private:
	void on_connection();
	void serve(int socket);

	finder_t m_finder;
	lister_t m_lister;
	cataloguer_t m_cataloguer;
	arranger_t m_arranger;
	QString m_path;
	int m_listener = -1;
	QSocketNotifier *m_notifier = nullptr;
};

/* How long the giving side waits for each message of an exchange. */
const int BSSH_HANDOFF_STEP_MS = 10000;

#endif /* BSSH_HANDOFF_POINT_H */
