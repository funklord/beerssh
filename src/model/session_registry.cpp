#include "model/session_registry.h"

#include <QMetaType>

#include "model/conn_manager.h"
#include "model/session.h"
#include "platform/platform.h"

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

bssh_session *bssh_session_registry::create(const bssh_profile_t &profile) {
	bssh_session *session = new bssh_session(profile, this);
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

	emit session_added(session);
	emit changed();
	return session;
}

void bssh_session_registry::close(bssh_session *session) {
	if (!session || !m_sessions.contains(session)) return;

	/*
	 * Announced before it goes: a view holding pointers into this
	 * session's screen and scrollback has to drop them before the memory
	 * does, or the next repaint reads freed cells.
	 */
	emit session_closing(session);

	m_sessions.removeAll(session);

	if (bssh_conn_manager *manager = m_managers.take(session)) {
		manager->note_user_disconnect();
		manager->deleteLater();
	}
	session->disconnect_from_host();
	session->deleteLater();

	emit changed();
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
