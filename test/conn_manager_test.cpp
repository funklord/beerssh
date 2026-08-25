#include <QSet>
#include <QSignalSpy>
#include <QtTest>

#include "conn_manager_test.h"
#include "model/conn_manager.h"
#include "model/session.h"

static bssh_profile_t host(const QString &name) {
	bssh_profile_t profile;
	profile.name = name;
	profile.host = name + QStringLiteral(".example");
	return profile;
}

void conn_manager_test::backoff_grows_and_then_stops_growing() {
	/*
	 * 25 is the midpoint of the jitter input: the spread is
	 * (random % 51) - 25, so 25 maps to zero and the nominal schedule is
	 * what gets asserted here.
	 */
	const int no_jitter = 25;

	QCOMPARE(bssh_conn_manager::backoff_delay_ms(1, no_jitter),
	          bssh_conn_manager::base_delay_ms());
	QCOMPARE(bssh_conn_manager::backoff_delay_ms(2, no_jitter),
	          bssh_conn_manager::base_delay_ms() * 2);
	QCOMPARE(bssh_conn_manager::backoff_delay_ms(3, no_jitter),
	          bssh_conn_manager::base_delay_ms() * 4);

	/*
	 * Capped, and the cap has to hold at absurd attempt counts too: a
	 * session left retrying overnight must not shift its way into
	 * undefined behaviour or a delay measured in years.
	 */
	QCOMPARE(bssh_conn_manager::backoff_delay_ms(20, no_jitter),
	          bssh_conn_manager::max_delay_ms());
	QCOMPARE(bssh_conn_manager::backoff_delay_ms(4000, no_jitter),
	          bssh_conn_manager::max_delay_ms());

	/* Attempt 0 or negative is treated as the first attempt, not as 0ms. */
	QVERIFY(bssh_conn_manager::backoff_delay_ms(0, no_jitter) >= 100);
	QVERIFY(bssh_conn_manager::backoff_delay_ms(-5, no_jitter) >= 100);
}

void conn_manager_test::backoff_is_jittered_within_bounds() {
	const int nominal = bssh_conn_manager::base_delay_ms() * 4; /* attempt 3 */

	for (int random_percent = 0; random_percent < 100; ++random_percent) {
		const int delay = bssh_conn_manager::backoff_delay_ms(3, random_percent);

		/* Never zero, never wilder than a quarter either side. */
		QVERIFY(delay >= 100);
		QVERIFY(delay >= nominal * 3 / 4);
		QVERIFY(delay <= nominal * 5 / 4);
	}
}

void conn_manager_test::jitter_separates_sessions_dropped_together() {
	/*
	 * The storm sec 5.3 warns about: one network flap drops eight
	 * sessions, and an unjittered backoff reconnects all eight in the
	 * same millisecond. Different randomness must give different delays.
	 */
	QSet<int> delays;
	for (int i = 0; i < 8; ++i) {
		delays.insert(bssh_conn_manager::backoff_delay_ms(3, i * 12));
	}

	QVERIFY2(delays.size() > 1, "jitter collapsed: every session would retry at once");
}

void conn_manager_test::an_exited_shell_is_not_reopened() {
	bssh_session session(host(QStringLiteral("exiting")));
	bssh_conn_manager manager(&session);

	QSignalSpy scheduled(&manager, &bssh_conn_manager::reconnect_scheduled);

	/*
	 * The regression. Typing `exit` at the prompt ends the remote shell,
	 * and reconnecting reopens the shell that was just left -- which,
	 * reported from a real session, looked like "when I exit it keeps
	 * restarting the terminal".
	 *
	 * The transport knew this was a clean end and kept it to itself; the
	 * manager saw only a session that was no longer live.
	 */
	session.note_remote_exit(0);

	QVERIFY(session.exited_cleanly());
	QCOMPARE(session.exit_status(), 0);
	QCOMPARE(scheduled.count(), 0);
	QVERIFY(!manager.is_reconnecting());

	/* Nor does a network change make it worth reopening. */
	manager.note_network_changed();
	QCOMPARE(scheduled.count(), 0);

	/* A non-zero status is still an exit, not a failure to retry. */
	bssh_session failed_shell(host(QStringLiteral("failing")));
	bssh_conn_manager other(&failed_shell);
	QSignalSpy other_scheduled(&other, &bssh_conn_manager::reconnect_scheduled);

	failed_shell.note_remote_exit(130);
	QCOMPARE(failed_shell.exit_status(), 130);
	QCOMPARE(other_scheduled.count(), 0);
}

void conn_manager_test::a_dropped_connection_is_still_retried() {
	bssh_session session(host(QStringLiteral("dropping")));
	bssh_conn_manager manager(&session);

	QSignalSpy scheduled(&manager, &bssh_conn_manager::reconnect_scheduled);

	/*
	 * The other half of the same distinction, and the reason the fix is
	 * not simply "never reconnect": a connection that dropped underneath
	 * a live shell is exactly what auto-reconnect exists for.
	 */
	session.note_connection_lost();

	QVERIFY(!session.exited_cleanly());
	QCOMPARE(scheduled.count(), 1);
	QVERIFY(manager.is_reconnecting());
}

void conn_manager_test::a_user_disconnect_is_not_retried() {
	bssh_session session(host(QStringLiteral("a")));
	bssh_conn_manager manager(&session);

	QSignalSpy scheduled(&manager, &bssh_conn_manager::reconnect_scheduled);

	/*
	 * Retrying a connection someone just closed is the most irritating
	 * bug available in this area, and the state that prevents it has to
	 * survive the disconnect that follows it.
	 */
	manager.note_user_disconnect();
	session.disconnect_from_host();

	QCOMPARE(scheduled.count(), 0);
	QVERIFY(!manager.is_reconnecting());

	/* A network change must not resurrect it either. */
	manager.note_network_changed();
	QCOMPARE(scheduled.count(), 0);
}

void conn_manager_test::auto_reconnect_can_be_switched_off() {
	bssh_session session(host(QStringLiteral("b")));
	bssh_conn_manager manager(&session);

	QVERIFY(manager.auto_reconnect());

	manager.set_auto_reconnect(false);
	QVERIFY(!manager.auto_reconnect());

	QSignalSpy scheduled(&manager, &bssh_conn_manager::reconnect_scheduled);
	manager.note_network_changed();

	QCOMPARE(scheduled.count(), 0);
	QVERIFY(!manager.is_reconnecting());
}
