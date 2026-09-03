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

	/* Nor does a new network make an exited shell worth reopening. */
	if (m_session->exited_cleanly()) return;

	/*
	 * The network moved, so whatever the backoff was waiting for has
	 * probably just changed. Reset rather than sitting out the remainder
	 * of a delay computed for a network that no longer exists -- this is
	 * the whole reason the platform reports the event at all (sec 5.3).
	 *
	 * SCHEDULED, not dialled. This used to call attempt_reconnect()
	 * directly, which is the one case sec 5.3 singles out by name:
	 * "with many sessions open, a network flap must not fire N
	 * simultaneous reconnects. Stagger with per-session jitter." Every
	 * manager in the registry hangs off the same platform signal, so a
	 * direct call had all of them dialling in the same millisecond, on
	 * the network that had just come back -- the storm, arriving by the
	 * one route that skipped the machinery built to prevent it.
	 *
	 * Going through the scheduler applies the same jitter the backoff
	 * uses, counts the attempt, and honours the cap. The reset is what
	 * keeps it prompt: the delay becomes the base second rather than
	 * whatever the last outage had climbed to, so "try again now" still
	 * means about a second instead of up to a minute.
	 */
	m_attempt = 0;
	m_timer->stop();
	schedule_reconnect();
}

void bssh_conn_manager::on_session_state_changed() {
	if (!m_session) return;

	/*
	 * ESTABLISHED, not merely alive.
	 *
	 * Back up: forget the backoff so the next unrelated drop starts from
	 * one second again rather than from wherever the last outage left
	 * off. That is right for a connection that actually came back, and it
	 * used to be asked as is_live() -- which is "not DISCONNECTED and not
	 * FAILED", so it is true of RESOLVING, CONNECTING and AUTHENTICATING
	 * as well.
	 *
	 * Those are the states a reconnect ATTEMPT passes through. So every
	 * attempt reset the counter that was measuring it: the delay never
	 * left its first step, BSSH_MAX_ATTEMPTS was unreachable and
	 * gave_up() could not fire. Against a host that is simply not there
	 * this dialled once a second for as long as the app was left running,
	 * which on a phone is the radio kept awake all night -- the storm
	 * sec 5.3 exists to prevent rather than a variation on it.
	 */
	if (m_session->state() == bssh_ssh_session::CONNECTED) {
		m_attempt = 0;
		m_reconnecting = false;
		m_timer->stop();
		return;
	}

	/*
	 * An attempt in flight is neither a success to reset on nor a failure
	 * to schedule against, and treating it as either is wrong in a
	 * different direction: as success it wipes the backoff, and as
	 * failure it would dial again while a connection is still being made.
	 */
	if (m_session->is_live()) return;

	/*
	 * A shell that ended by itself is not a connection that dropped.
	 * Retrying it reopens the shell the user just left -- reported from
	 * a real session as "when I exit it keeps restarting the terminal",
	 * which is exactly what this class did until the transport learned to
	 * say which kind of ending it was.
	 */
	if (m_session->exited_cleanly()) {
		cancel();
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
