#include <QSignalSpy>
#include <QtTest>

#include "model/session.h"
#include "model/session_registry.h"
#include "session_registry_test.h"
#include "fake_ssh_session.h"
#include "term/emulator.h"
#include "ui/terminal_view.h"

static bssh_profile_t profile_for(const QString &name, const QString &host) {
	bssh_profile_t profile;
	profile.name = name;
	profile.host = host;
	profile.user = QStringLiteral("nabbe");
	return profile;
}

void session_registry_test::creates_and_lists_sessions() {
	bssh_session_registry registry;
	QSignalSpy added(&registry, &bssh_session_registry::session_added);

	QCOMPARE(registry.count(), 0);

	bssh_session *first = registry.create(profile_for(QStringLiteral("one"),
	                                                   QStringLiteral("a.example")));
	bssh_session *second = registry.create(profile_for(QStringLiteral("two"),
	                                                    QStringLiteral("b.example")));

	QCOMPARE(registry.count(), 2);
	QCOMPARE(added.count(), 2);
	QCOMPARE(registry.at(0), first);
	QCOMPARE(registry.index_of(second), 1);
	QCOMPARE(registry.at(99), nullptr);
}

void session_registry_test::session_outlives_the_view_that_showed_it() {
	bssh_session_registry registry;
	bssh_session *session = registry.create(profile_for(QStringLiteral("keep"),
	                                                     QStringLiteral("host.example")));

	/*
	 * A view attaches, output arrives, the view is destroyed. This is
	 * exactly what a tab switch, a rotation and a collapsed split pane
	 * all do -- and what the background service leaves behind when the
	 * whole UI is gone (sec 5).
	 */
	{
		bssh_terminal_view view;
		view.show();
		view.resize(640, 320);
		view.attach_screen(&session->emulator()->screen());
		view.attach_emulator(session->emulator());

		session->emulator()->feed(QByteArrayLiteral("before the view went away\r\n"));
	}

	/* Output that arrives with NO view attached must still land. */
	session->emulator()->feed(QByteArrayLiteral("and after"));

	const QString text = session->emulator()->screen().to_text();
	QVERIFY(text.contains(QStringLiteral("before the view went away")));
	QVERIFY(text.contains(QStringLiteral("and after")));

	/* And a fresh view sees all of it, because it was never the view's. */
	bssh_terminal_view rebuilt;
	rebuilt.show();
	rebuilt.resize(640, 320);
	rebuilt.attach_screen(&session->emulator()->screen());
	QVERIFY(rebuilt.screen()->to_text().contains(QStringLiteral("and after")));
}

void session_registry_test::closing_announces_before_destroying() {
	bssh_session_registry registry;
	bssh_session *session = registry.create(profile_for(QStringLiteral("bye"),
	                                                     QStringLiteral("c.example")));

	QSignalSpy closing(&registry, &bssh_session_registry::session_closing);
	QSignalSpy changed(&registry, &bssh_session_registry::changed);

	/*
	 * Order matters and is not cosmetic: a view holds pointers into the
	 * session's screen, scrollback and images, so it has to be told to
	 * let go BEFORE the memory goes, or the next repaint reads freed
	 * cells.
	 */
	registry.close(session);

	QCOMPARE(closing.count(), 1);
	QVERIFY(changed.count() >= 1);
	QCOMPARE(registry.count(), 0);

	/* Closing something already gone is a no-op, not a crash. */
	registry.close(session);
	QCOMPARE(closing.count(), 1);
}

void session_registry_test::live_count_ignores_dead_sessions() {
	bssh_session_registry registry;
	registry.create(profile_for(QStringLiteral("a"), QStringLiteral("a.example")));
	registry.create(profile_for(QStringLiteral("b"), QStringLiteral("b.example")));

	/*
	 * Neither has connected, so both are DISCONNECTED. The notification
	 * and tray tooltip count LIVE sessions (sec 5.2) -- "2 sessions
	 * connected" over two dead ones would be a lie told by the one piece
	 * of UI a backgrounded app still has.
	 */
	QCOMPARE(registry.count(), 2);
	QCOMPARE(registry.live_count(), 0);
}

void session_registry_test::title_falls_back_from_osc_to_profile() {
	bssh_session_registry registry;
	bssh_session *session = registry.create(profile_for(QStringLiteral("work bastion"),
	                                                     QStringLiteral("bastion.example")));

	/* No OSC title yet: the profile name is the tab label. */
	QCOMPARE(session->title(), QStringLiteral("work bastion"));

	QSignalSpy titled(session, &bssh_session::title_changed);
	session->emulator()->feed(QByteArrayLiteral("\x1b]0;nabbe@bastion: ~\x07"));

	QCOMPARE(titled.count(), 1);
	QCOMPARE(session->title(), QStringLiteral("nabbe@bastion: ~"));

	/* A session with neither falls back to the target, never to blank. */
	bssh_profile_t bare;
	bare.host = QStringLiteral("plain.example");
	bare.user = QStringLiteral("root");
	QCOMPARE(registry.create(bare)->title(), QStringLiteral("root@plain.example"));
}

void session_registry_test::the_transport_reports_which_kind_of_ending_it_was() {
	bssh_fake_ssh_session transport;
	QSignalSpy exited(&transport, &bssh_ssh_session::shell_exited);
	QSignalSpy failed(&transport, &bssh_ssh_session::failed);

	/*
	 * The interface has to carry the distinction, or every implementation
	 * loses it the way the libssh one did: it knew the channel had closed
	 * cleanly and reported a plain disconnect, so nothing downstream could
	 * tell an exited shell from a dropped connection.
	 */
	transport.simulate_remote_exit(0);
	QCOMPARE(exited.count(), 1);
	QCOMPARE(exited.first().first().toInt(), 0);
	QCOMPARE(failed.count(), 0);
	QCOMPARE(transport.state(), bssh_ssh_session::DISCONNECTED);

	transport.simulate_connection_lost();
	QCOMPARE(failed.count(), 1);
	QCOMPARE(exited.count(), 1);
}

void session_registry_test::a_password_is_requested_only_when_keys_fail() {
	bssh_fake_ssh_session transport;
	QSignalSpy asked(&transport, &bssh_ssh_session::password_required);

	/*
	 * Key authentication succeeding must not ask for anything. Prompting
	 * before trying a key -- which is what this did until now -- asks for
	 * a credential the user usually does not need to give, and forces the
	 * password to be kept in memory so a reconnect can reuse it (sec 11).
	 */
	transport.simulate_key_auth_success();
	QCOMPARE(transport.state(), bssh_ssh_session::CONNECTED);
	QCOMPARE(asked.count(), 0);

	/* And when they fail, the request is explicit rather than assumed. */
	transport.simulate_password_required();
	QCOMPARE(asked.count(), 1);
}

void session_registry_test::a_host_key_is_never_trusted_without_an_answer() {
	bssh_fake_ssh_session transport;
	QSignalSpy unknown(&transport, &bssh_ssh_session::host_key_unknown);
	QSignalSpy changed(&transport, &bssh_ssh_session::host_key_changed);

	transport.simulate_host_key_unknown(QStringLiteral("SHA256:abc"));
	QCOMPARE(unknown.count(), 1);
	QCOMPARE(unknown.first().first().toString(), QStringLiteral("SHA256:abc"));

	/*
	 * Asking is not accepting. Nothing may be written to the known-hosts
	 * store until someone actually answers (sec 11) -- the failure mode
	 * being guarded against is a prompt that records the key while it is
	 * still on screen.
	 */
	QVERIFY(!transport.host_key_accepted());

	transport.accept_host_key();
	QVERIFY(transport.host_key_accepted());

	/* A changed key is reported as its own thing, not as a first sighting. */
	transport.simulate_host_key_changed(QStringLiteral("SHA256:xyz"));
	QCOMPARE(changed.count(), 1);
	QCOMPARE(unknown.count(), 1);
}
