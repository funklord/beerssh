#include "model/conn_manager.h"

#include <QRandomGenerator>
#include <QTimer>

#include "model/session.h"

/*
 * Jitter spread, as a percentage either side of the computed delay.
 * A quarter is wide enough to separate a handful of sessions that dropped
 * together and narrow enough that the backoff still means something.
 */
static const int BSSH_JITTER_PERCENT = 25;

/*
 * How many times to try before giving up and waiting for the user or a
 * network change. Not infinite: a session retrying forever against a host
 * that is genuinely gone is a radio waking a phone every minute for
 * nothing (sec 5.4).
 */
static const int BSSH_MAX_ATTEMPTS = 10;

bssh_conn_manager::bssh_conn_manager(bssh_session *session, QObject *parent)
    : QObject(parent),
      m_session(session),
      m_timer(new QTimer(this)),
      m_auto_reconnect(true),
      m_reconnecting(false),
      m_user_disconnected(false),
      m_attempt(0) {
	m_timer->setSingleShot(true);
	connect(m_timer, &QTimer::timeout, this, &bssh_conn_manager::attempt_reconnect);

	if (m_session) {
		connect(m_session, &bssh_session::state_changed,
		         this, &bssh_conn_manager::on_session_state_changed);
	}
}

int bssh_conn_manager::backoff_delay_ms(int attempt, int random_percent) {
	if (attempt < 1) attempt = 1;

	/*
	 * Doubling, but computed with a shift guarded against overflow rather
	 * than a loop: attempt 40 would otherwise be undefined behaviour
	 * reached by a session left retrying overnight.
	 */
	qint64 delay = base_delay_ms();
	for (int i = 1; i < attempt && delay < max_delay_ms(); ++i) {
		delay *= 2;
	}
	if (delay > max_delay_ms()) delay = max_delay_ms();

	/* random_percent is 0..99; map it to [-JITTER, +JITTER]. */
	const int spread = (random_percent % (2 * BSSH_JITTER_PERCENT + 1)) - BSSH_JITTER_PERCENT;
	delay += delay * spread / 100;

	return int(qMax<qint64>(100, delay));
}

void bssh_conn_manager::set_auto_reconnect(bool enabled) {
	m_auto_reconnect = enabled;
	if (!enabled) cancel();
}

void bssh_conn_manager::note_user_disconnect() {
	m_user_disconnected = true;
	cancel();
}

void bssh_conn_manager::note_network_changed() {
	if (!m_auto_reconnect || m_user_disconnected) return;
	if (!m_session || m_session->is_live()) return;

	/*
	 * The network moved, so whatever the backoff was waiting for has
	 * probably just changed. Reset and try immediately rather than
	 * sitting out the remainder of a delay that was computed for a
	 * network that no longer exists -- this is the whole reason the
	 * platform reports the event at all (sec 5.3).
	 */
	m_attempt = 0;
	m_timer->stop();
	attempt_reconnect();
}

void bssh_conn_manager::on_session_state_changed() {
	if (!m_session) return;

	if (m_session->is_live()) {
		/*
		 * Back up: forget the backoff so the next unrelated drop starts
		 * from one second again rather than from wherever the last
		 * outage left off.
		 */
		m_attempt = 0;
		m_reconnecting = false;
		m_timer->stop();
		return;
	}

	schedule_reconnect();
}

void bssh_conn_manager::schedule_reconnect() {
	if (!m_auto_reconnect || m_user_disconnected || !m_session) return;
	if (m_timer->isActive()) return;

	if (m_attempt >= BSSH_MAX_ATTEMPTS) {
		m_reconnecting = false;
		emit gave_up();
		return;
	}

	++m_attempt;
	m_reconnecting = true;

	const int delay = backoff_delay_ms(m_attempt, int(QRandomGenerator::global()->bounded(100)));
	m_timer->start(delay);

	emit reconnect_scheduled(delay, m_attempt);
}

void bssh_conn_manager::attempt_reconnect() {
	if (!m_auto_reconnect || m_user_disconnected || !m_session) return;

	m_reconnecting = true;
	emit reconnect_started(m_attempt);

	/*
	 * The session reconnects itself; this class only decides when. It
	 * cannot supply a password -- that was consumed at first connect and
	 * deliberately not kept (sec 11) -- so a password-authenticated
	 * session reconnects only as far as authentication and then reports
	 * what it needs. Key auth at M6 is what makes this seamless.
	 */
	m_session->reconnect();
}

void bssh_conn_manager::cancel() {
	m_timer->stop();
	m_reconnecting = false;
}
