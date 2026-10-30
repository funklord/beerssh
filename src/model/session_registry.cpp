#include "model/session_registry.h"

#include <QMetaType>
#include <QPointer>
#include <QTimer>

#include "model/conn_manager.h"
#include "model/kept_scrollback.h"
#include "model/profile_store.h"
#include "model/session.h"
#include "platform/platform.h"
#include "term/emulator.h"
#include "term/scrollback.h"

#include <QDateTime>
#include <QFileInfo>

bssh_session_registry::bssh_session_registry(QObject *parent)
    : QObject(parent) {
	/*
	 * Declaring the metatype is a compile-time fact; REGISTERING it is
	 * the runtime one, and signals carrying bssh_session * need both.
	 * Without this a queued connection cannot deliver the pointer and
	 * QSignalSpy cannot record it -- neither fails to compile, both warn
	 * at runtime, and the visible symptom is a signal that appears never
	 * to have been emitted.
	 */
	qRegisterMetaType<bssh_session *>();
}

namespace {

/* Where kept scrollback lives: beside the hosts it belongs to. */
QString kept_directory() {
	const bssh_profile_store *store = bssh_profile_store::shared();
	return store ? store->directory() : QString();
}

/* One line of plain text as cells, for the marker between old and new. */
QList<bssh_cell_t> text_line(const QString &text) {
	QList<bssh_cell_t> line;
	for (const QChar ch : text) {
		bssh_cell_t cell;
		cell.ch = char32_t(ch.unicode());
		cell.attrs = bssh_cell_t::BOLD;
		line.append(cell);
	}
	return line;
}

/*
 * The history as it stands, and the screen below it: what is on screen
 * when a session closes has not scrolled into the history yet, and is
 * the part most likely to be wanted. Not the alternate screen, which is a
 * program's display (vim's, less's) rather than output, and trailing
 * blank rows are left where they are.
 */
bssh_scrollback history_with_screen(const bssh_emulator *emulator) {
	bssh_scrollback history = emulator->scrollback();
	if (emulator->alternate_screen()) return history;

	const bssh_screen &screen = emulator->screen();
	QList<QList<bssh_cell_t>> rows;
	for (int row = 0; row < screen.rows(); ++row) {
		QList<bssh_cell_t> line;
		for (int col = 0; col < screen.cols(); ++col) line.append(screen.cell(row, col));
		while (!line.isEmpty() && line.last().ch == U' '
		       && line.last().bg.kind == bssh_color_t::DEFAULT)
			line.removeLast();
		rows.append(line);
	}
	while (!rows.isEmpty() && rows.last().isEmpty()) rows.removeLast();

	for (const QList<bssh_cell_t> &line : rows) history.push(line);
	return history;
}

void keep(bssh_session *session) {
	const bssh_profile_t &profile = session->profile();
	if (!profile.keep_scrollback || profile.id == 0 || !session->emulator()) return;
	bssh_save_kept_scrollback(profile.id, history_with_screen(session->emulator()),
	                          kept_directory());
}

} /* namespace */

bssh_session *bssh_session_registry::create(const bssh_profile_t &profile) {
	bssh_session *session = new bssh_session(profile, this);

	/*
	 * A host that keeps its scrollback gets it back before anything else
	 * can arrive, with a line saying where the old ends -- so output from
	 * yesterday is not read as output from now.
	 */
	if (profile.keep_scrollback && profile.id != 0) {
		bssh_scrollback kept(session->emulator()->scrollback().max_lines());
		if (bssh_load_kept_scrollback(profile.id, &kept, kept_directory()) && kept.count() > 0) {
			const QDateTime when = QFileInfo(
			        bssh_kept_scrollback_path(profile.id, kept_directory())).lastModified();
			kept.push(text_line(QStringLiteral("-- kept from %1 --")
			                            .arg(when.toString(QStringLiteral("yyyy-MM-dd HH:mm")))));
			session->emulator()->restore_history(kept);
		}
	}

	attach(session);
	return session;
}

/*
 * A session that arrived from another process (sec 8.4), already connected:
 * the same wiring a new one gets, and no kept scrollback -- it brought its
 * own history with it.
 */
void bssh_session_registry::adopt(bssh_session *session) {
	if (!session || m_sessions.contains(session)) return;
	session->setParent(this);
	attach(session);
}

/*
 * A session given to another process (sec 8.4): gone from here, and alive
 * there. Not close(): nothing is disconnected -- the transport already let
 * go without a word -- and the kept scrollback is not written, since the
 * history it would record has moved on with the session.
 */
void bssh_session_registry::give_away(bssh_session *session) {
	if (!session || !m_sessions.contains(session)) return;
	emit session_given_away(session);
	release(session);
}

void bssh_session_registry::release(bssh_session *session) {
	if (!session || !m_sessions.contains(session)) return;
	emit session_closing(session);
	m_sessions.removeAll(session);
	if (bssh_conn_manager *manager = m_managers.take(session)) manager->deleteLater();
	session->deleteLater();
	emit changed();
}

void bssh_session_registry::attach(bssh_session *session) {
	m_sessions.append(session);

	/*
	 * A title or state change is a registry change, because both are
	 * things the tab bar and the notification text are showing. Wiring it
	 * here rather than in each view means a session that no view is
	 * currently displaying still keeps them correct.
	 */
	connect(session, &bssh_session::title_changed, this, &bssh_session_registry::changed);
	connect(session, &bssh_session::state_changed, this, &bssh_session_registry::changed);

	/*
	 * Every session gets its own reconnection machine (sec 5.3), so a
	 * split view can show one pane healthy while another retries. The
	 * platform's network_changed drives them all -- until now that signal
	 * had no listener at all.
	 */
	bssh_conn_manager *manager = new bssh_conn_manager(session, this);
	m_managers.insert(session, manager);

	connect(bssh_platform::instance(), &bssh_platform::network_changed,
	         manager, &bssh_conn_manager::note_network_changed);

	/*
	 * The three things sec 5.5 says a desktop notification carries:
	 * reconnecting, reconnect failed, and a bell in a pane nobody is
	 * looking at. bssh_platform::notify() was declared with exactly that
	 * list beside it and implemented on both platforms, and until now
	 * **nothing called it** -- an interface complete enough that reading
	 * it found nothing wrong, which is why reading the callers is the
	 * only thing that could have.
	 *
	 * Here rather than in a window, for the reason the shell-ended
	 * handler below gives: it is a fact about a session, not about a view
	 * of one. A window-level handler cannot even ask the question -- each
	 * knows only whether IT shows the session, so with two windows open
	 * the answer needs agreement between objects that deliberately hold
	 * no pointer to each other, and a session no view is showing at all
	 * is the case that matters most.
	 *
	 * The guard is the INVERSE of the status line's. Every window handler
	 * for these signals starts `if (session != m_current) return' and
	 * writes into the status area, so the focused session is already
	 * covered and a notification for it would be a popup about something
	 * on screen. What had no signal at all was the session reconnecting
	 * behind another tab, which is the one a person needs told about.
	 */
	connect(manager, &bssh_conn_manager::reconnect_started,
	         this, [session](int attempt) {
		if (session->is_on_screen()) return;

		bssh_platform::instance()->notify(
		        QStringLiteral("Reconnecting"),
		        QStringLiteral("%1 -- attempt %2")
		                .arg(session->title())
		                .arg(attempt));
	});

	connect(manager, &bssh_conn_manager::gave_up, this, [session] {
		/*
		 * Not guarded on the screen, unlike the other two. Giving up is
		 * terminal: the shell is gone and will not come back on its own,
		 * and that is worth saying whether or not the pane is visible --
		 * a person watching a pane that has quietly stopped retrying has
		 * no way to tell it from one still trying.
		 */
		bssh_platform::instance()->notify(
		        QStringLiteral("Reconnect failed"),
		        QStringLiteral("%1 -- gave up; the shell is gone, not suspended")
		                .arg(session->title()));
	});

	connect(session, &bssh_session::bell_while_away, this, [session] {
		bssh_platform::instance()->notify(
		        QStringLiteral("Bell"),
		        QStringLiteral("%1 rang while you were elsewhere")
		                .arg(session->title()));
	});

	/*
	 * A session whose shell ended closes itself, unless it was opened to
	 * hold (sec 8.6). This is what every terminal since xterm does when
	 * the thing it was running finishes, and until now nothing did it:
	 * the pane stayed with "session ended" written under it, which is
	 * xterm's -hold behaviour applied to everybody whether they asked for
	 * it or not.
	 *
	 * Here rather than in the window, because it is a fact about a
	 * session rather than about a view of one -- a tab torn into another
	 * window, or one no view is showing at all, must end the same way.
	 * Only an ENDING closes: a dropped connection is sec 5.3's business
	 * and stays for the reconnect, and a failure stays so its reason can
	 * be read.
	 *
	 * Deferred by a turn of the event loop, and through a QPointer,
	 * because this arrives from inside the transport's own signal and the
	 * session is what owns that transport.
	 */
	connect(session, &bssh_session::shell_exited, this, [this, session](int) {
		if (session->profile().hold) return;

		QPointer<bssh_session> alive(session);
		QTimer::singleShot(0, this, [this, alive] {
			if (alive) close(alive);
		});
	});

	emit session_added(session);
	emit changed();
}

void bssh_session_registry::close(bssh_session *session) {
	if (!session || !m_sessions.contains(session)) return;

	/*
	 * Announced before it goes: a view holding pointers into this
	 * session's screen and scrollback has to drop them before the memory
	 * does, or the next repaint reads freed cells.
	 */
	emit session_closing(session);

	/* Written down before it goes, for a host that keeps its scrollback. */
	keep(session);

	m_sessions.removeAll(session);

	if (bssh_conn_manager *manager = m_managers.take(session)) {
		manager->note_user_disconnect();
		manager->deleteLater();
	}
	session->disconnect_from_host();
	session->deleteLater();

	emit changed();
}

bool bssh_session_registry::keeps_awake() const {
	for (const bssh_session *session : m_sessions) {
		if (session->profile().keep_awake && session->is_live()) return true;
	}
	return false;
}

void bssh_session_registry::save_kept_scrollback() {
	for (bssh_session *session : m_sessions) keep(session);
}

bssh_conn_manager *bssh_session_registry::conn_manager_for(bssh_session *session) const {
	return m_managers.value(session, nullptr);
}

int bssh_session_registry::live_count() const {
	int live = 0;
	for (bssh_session *session : m_sessions) {
		if (session->is_live()) ++live;
	}
	return live;
}

bssh_session *bssh_session_registry::at(int index) const {
	if (index < 0 || index >= m_sessions.size()) return nullptr;
	return m_sessions.at(index);
}

int bssh_session_registry::index_of(bssh_session *session) const {
	return m_sessions.indexOf(session);
}

void bssh_session_registry::move(int from, int to) {
	if (from == to) return;
	if (from < 0 || from >= m_sessions.size()) return;
	if (to < 0 || to >= m_sessions.size()) return;

	m_sessions.move(from, to);

	/*
	 * changed() rather than a signal of its own: every listener rebuilds
	 * from the list anyway, and the count is the same, so a second signal
	 * would only be a second thing to forget to connect.
	 */
	emit changed();
}

bssh_session_registry *bssh_session_registry::shared() {
	/*
	 * Constructed on first use and never destroyed, deliberately: the
	 * sessions in it outlive every window (sec 3), and tearing it down at
	 * static-destruction time would race the widgets still holding
	 * pointers into their screens.
	 */
	static bssh_session_registry *registry = new bssh_session_registry;
	return registry;
}
