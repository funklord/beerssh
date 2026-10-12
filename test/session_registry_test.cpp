#include <QPointer>
#include <QSignalSpy>
#include <QtTest>

#include <QTemporaryDir>

#include "model/conn_manager.h"
#include "platform/platform.h"
#include "model/session.h"
#include "model/profile_store.h"
#include "model/session_registry.h"
#include "ui/main_window.h"
#include "model/window_title.h"
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

void session_registry_test::only_a_connected_keep_awake_host_keeps_the_computer_awake() {
	QList<bssh_fake_ssh_session *> fakes;
	bssh_ssh_session::set_factory([&fakes](QObject *parent) -> bssh_ssh_session * {
		bssh_fake_ssh_session *fake = new bssh_fake_ssh_session(parent);
		fakes.append(fake);
		return fake;
	});

	bssh_session_registry registry;

	bssh_profile_t awake = profile_for(QStringLiteral("build"), QStringLiteral("build.example"));
	awake.keep_awake = true;
	bssh_session *builder = registry.create(awake);

	bssh_session *plain = registry.create(profile_for(QStringLiteral("plain"),
	                                                   QStringLiteral("plain.example")));

	/* Asking is not enough: nothing is connected, so nothing is running. */
	QVERIFY(!registry.keeps_awake());

	/* A connected host that did not ask keeps nothing awake. */
	plain->connect_to_host();
	QCOMPARE(fakes.size(), 1);
	fakes.at(0)->simulate_key_auth_success();
	QVERIFY(plain->is_live());
	QVERIFY(!registry.keeps_awake());

	/* The one that asked, connected: now. */
	builder->connect_to_host();
	QCOMPARE(fakes.size(), 2);
	fakes.at(1)->simulate_key_auth_success();
	QVERIFY(builder->is_live());
	QVERIFY(registry.keeps_awake());

	/* And not once it is gone. */
	registry.close(builder);
	QVERIFY(!registry.keeps_awake());

	bssh_ssh_session::set_factory(nullptr);
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

	/*
	 * A remote that CLEARS its title -- OSC 0 with an empty string, which
	 * tmux and a bare shell both do on exit -- must fall back down the
	 * chain again rather than leaving a nameless tab. The fallback is
	 * evaluated on every read for this reason; caching the resolved title
	 * would strand the empty one.
	 */
	session->emulator()->feed(QByteArrayLiteral("\x1b]0;\x07"));
	QCOMPARE(session->title(), QStringLiteral("work bastion"));

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

void session_registry_test::a_shell_that_ends_closes_its_session() {
	/*
	 * sec 8.6: a terminal whose program has finished closes, which is
	 * what every terminal since xterm does and what this did NOT do --
	 * the pane stayed with "session ended" written under it, xterm's
	 * -hold behaviour applied to everybody whether they asked or not.
	 *
	 * Asserted on the REGISTRY rather than on a window, because that is
	 * where the rule lives: a tab torn into another window, and a session
	 * no view is showing at all, have to end the same way.
	 */
	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});

	bssh_session_registry registry;
	bssh_session *session = registry.create(profile_for(QStringLiteral("one"),
	                                                     QStringLiteral("a.example")));
	session->connect_to_host();
	QVERIFY(fake);

	QSignalSpy closing(&registry, &bssh_session_registry::session_closing);
	fake->simulate_remote_exit(0);

	/* Deferred by a turn of the event loop -- this arrives from inside
	 * the transport's own signal. */
	QTRY_COMPARE(registry.count(), 0);
	QCOMPARE(closing.count(), 1);

	bssh_ssh_session::set_factory(nullptr);
}

void session_registry_test::a_held_session_stays_when_its_shell_ends() {
	/*
	 * --hold, and the reason it exists: a command that fails prints why
	 * and exits, and closing the pane takes the message with it.
	 *
	 * The other half of the rule is asserted here too -- a session that
	 * DROPS is left alone whether it is held or not, because sec 5.3's
	 * reconnection machine owns that case and a registry that closed
	 * sessions on a lost link would undo it.
	 */
	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});

	bssh_profile_t profile = profile_for(QStringLiteral("held"), QStringLiteral("a.example"));
	profile.hold = true;

	bssh_session_registry registry;
	bssh_session *session = registry.create(profile);
	session->connect_to_host();
	QVERIFY(fake);

	fake->simulate_remote_exit(3);

	/*
	 * Waited out rather than checked once: the closing this asserts the
	 * absence of would arrive one event-loop turn later, so a test that
	 * looked immediately would pass even with --hold ignored entirely.
	 */
	QTest::qWait(50);
	QCOMPARE(registry.count(), 1);
	QVERIFY(session->exited_cleanly());

	bssh_ssh_session::set_factory(nullptr);
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

void session_registry_test::an_encrypted_key_is_offered_before_a_password() {
	bssh_fake_ssh_session transport;
	QSignalSpy passphrase(&transport, &bssh_ssh_session::key_passphrase_required);
	QSignalSpy password(&transport, &bssh_ssh_session::password_required);

	/*
	 * A key on disk that did not work unaided is most likely encrypted
	 * with no agent loaded. Asking for the passphrase first matters
	 * because for a server that only accepts keys, a password prompt is
	 * not a fallback -- it is a prompt that cannot succeed.
	 */
	transport.simulate_key_passphrase_required();
	QCOMPARE(passphrase.count(), 1);
	QCOMPARE(password.count(), 0);

	transport.supply_key_passphrase(QStringLiteral("hunter2"));
	QCOMPARE(transport.key_passphrase(), QStringLiteral("hunter2"));
}

void session_registry_test::a_hosts_keepalive_interval_reaches_its_transport() {
	/*
	 * sec 5.3 wants a keepalive so a dead socket is noticed rather than
	 * typed into. It is only worth anything if the host's interval
	 * actually arrives at the transport, and the two travel in opposite
	 * directions: settings are resolved when the session is created,
	 * while the transport is not built until someone connects.
	 */
	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});

	bssh_profile_t profile;
	profile.host = QStringLiteral("nat.example.com");

	bssh_session session(profile);

	QJsonObject settings;
	settings.insert(QStringLiteral("keepalive_seconds"), 45);
	session.set_settings(settings);

	/* Set before there was anything to set it on. */
	QVERIFY(fake == nullptr);

	session.connect_to_host();
	QVERIFY(fake != nullptr);

	/*
	 * The case this test exists for: applied at connect time from the
	 * settings the session was already carrying. Applying it only in
	 * set_settings would leave every FIRST connection without a
	 * keepalive, which is exactly the one most likely to be dropped.
	 */
	QCOMPARE(fake->keepalive_seconds(), 45);

	/* And a later preset edit retimes the live connection. */
	settings.insert(QStringLiteral("keepalive_seconds"), 15);
	session.set_settings(settings);
	QCOMPARE(fake->keepalive_seconds(), 15);

	bssh_ssh_session::set_factory(nullptr);
}

void session_registry_test::every_resolved_setting_reaches_something_that_uses_it() {
	/*
	 * The gap this test exists to close: bssh_preset_t had nine settable
	 * fields, a three-layer resolver, an editor and a config file, and
	 * NOTHING consumed any of them except the font. A setting a user can
	 * change and cannot observe is worse than one that is missing --
	 * missing looks unfinished, this looks broken.
	 */
	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});

	bssh_profile_t profile;
	profile.host = QStringLiteral("box.example.com");
	/*
	 * From the profile rather than the preset, because a key path names
	 * one machine's access and sec 10.3 keeps that off the shareable
	 * preset -- but it has to reach the transport by the same connect.
	 */
	profile.identity_file = QStringLiteral("~/.ssh/id_work");
	profile.proxy_jump = QStringLiteral("me@bastion.example.com");
	bssh_session session(profile);

	QJsonObject settings;
	settings.insert(QStringLiteral("term"), QStringLiteral("xterm-direct"));
	settings.insert(QStringLiteral("compression"), true);
	settings.insert(QStringLiteral("keepalive_seconds"), 45);
	settings.insert(QStringLiteral("scrollback_lines"), 12345);
	session.set_settings(settings);

	session.connect_to_host();
	QVERIFY(fake != nullptr);

	QCOMPARE(fake->term(), QStringLiteral("xterm-direct"));
	QCOMPARE(fake->compression(), true);
	QCOMPARE(fake->keepalive_seconds(), 45);
	QCOMPARE(fake->identity_file(), QStringLiteral("~/.ssh/id_work"));
	QCOMPARE(fake->proxy_jump(), QStringLiteral("me@bastion.example.com"));
	QCOMPARE(session.emulator()->scrollback().max_lines(), 12345);

	/*
	 * `term` has TWO consumers and this test used to check only one. The
	 * channel got the resolved TERM while the emulator's own copy stayed
	 * at its default, so XTGETTCAP answered a different name than the
	 * environment carried -- which sec 7.1 calls worse than declining to
	 * answer, and which a terminfo-querying client (ncurses 6.3+, tmux)
	 * sees as the terminal contradicting itself.
	 *
	 * Asked the way a client asks -- DCS + q <hex name> ST, "TN" being
	 * 544E -- rather than through a getter, so what is checked is the
	 * answer that actually goes on the wire.
	 */
	const int written_before = fake->written().size();
	session.emulator()->feed(QByteArrayLiteral("\x1bP+q544E\x1b\\"));

	/*
	 * Read off the TRANSPORT, not the emulator: a live session drains
	 * take_output() into the channel itself, so asking the emulator here
	 * returns nothing and would have passed against any answer at all.
	 */
	const QByteArray reply = fake->written().mid(written_before);
	QCOMPARE(reply, QByteArrayLiteral("\x1bP1+r544E=")
	                 + QByteArrayLiteral("xterm-direct").toHex().toUpper()
	                 + QByteArrayLiteral("\x1b\\"));

	bssh_ssh_session::set_factory(nullptr);
}

void session_registry_test::the_on_connect_command_runs_on_every_connect() {
	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});

	bssh_profile_t profile;
	profile.host = QStringLiteral("box.example.com");
	bssh_session session(profile);

	QJsonObject settings;
	settings.insert(QStringLiteral("on_connect_command"),
	                 QStringLiteral("tmux attach || tmux new"));
	session.set_settings(settings);

	session.connect_to_host();
	fake->simulate_key_auth_success();

	/*
	 * sec 5.3: a reconnect is a NEW shell, and attaching to tmux is the
	 * available answer. That only works if the command runs on the
	 * reconnect too -- which is precisely the connection where the user's
	 * work is sitting in a detached session waiting for it.
	 */
	QVERIFY(fake->written().contains("tmux attach || tmux new\n"));

	const qsizetype first = fake->written().size();
	fake->simulate_connection_lost();
	fake->simulate_key_auth_success();
	QVERIFY2(fake->written().size() > first, "it must run again on a reconnect");

	bssh_ssh_session::set_factory(nullptr);
}

namespace {

/* Everything on the emulator's screen, row by row. */
QString screen_text(const bssh_emulator *emulator) {
	QString text;
	const bssh_screen &screen = emulator->screen();
	for (int row = 0; row < screen.rows(); ++row) {
		for (int col = 0; col < screen.cols(); ++col) {
			const char32_t ch = screen.cell(row, col).ch;
			text += ch ? QString::fromUcs4(&ch, 1) : QStringLiteral(" ");
		}
		text += QLatin1Char('\n');
	}
	return text;
}

} /* namespace */

void session_registry_test::a_reconnected_session_shows_and_takes_the_new_shell() {
	/*
	 * Reported from the phone 2026-10-12: a session that dropped and
	 * reconnected said "connected" and then showed nothing of the new
	 * shell, not even its prompt, while the old screen stayed up. A
	 * reconnect is a NEW transport (sec 5.3), and nothing checked that
	 * the new one is wired both ways: its output onto the same screen,
	 * and the keys typed afterwards out through it rather than the one
	 * that died.
	 */
	QList<QPointer<bssh_fake_ssh_session>> made;
	bssh_ssh_session::set_factory([&made](QObject *parent) -> bssh_ssh_session * {
		bssh_fake_ssh_session *fake = new bssh_fake_ssh_session(parent);
		made.append(fake);
		return fake;
	});

	bssh_profile_t profile;
	profile.host = QStringLiteral("box.example.com");
	bssh_session session(profile);
	session.connect_to_host();
	QCOMPARE(made.size(), 1);
	made.at(0)->simulate_key_auth_success();
	made.at(0)->emit_chunk("old shell$ ");
	QVERIFY(screen_text(session.emulator()).contains(QStringLiteral("old shell$")));

	made.at(0)->simulate_connection_lost();
	session.reconnect();
	QCOMPARE(made.size(), 2);
	QPointer<bssh_fake_ssh_session> fresh = made.at(1);
	QVERIFY(fresh && fresh != made.at(0));
	fresh->simulate_key_auth_success();
	QCoreApplication::processEvents();
	QVERIFY(session.is_live());

	fresh->emit_chunk("\r\nnew shell$ ");
	QVERIFY2(screen_text(session.emulator()).contains(QStringLiteral("new shell$")),
	          "the reconnected shell's output never reached the screen");

	fresh->clear_written();
	session.input_router()->send_text(QStringLiteral("x"));
	QCoreApplication::processEvents();
	QVERIFY2(fresh->written().contains('x'),
	          "a key typed after the reconnect did not go out on the new connection");

	bssh_ssh_session::set_factory(nullptr);
}

void session_registry_test::editing_a_host_reaches_the_session_already_open_on_it() {
	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});

	bssh_main_window window;

	/*
	 * The window's OWN store, not a second one pointing at the same file:
	 * a different object emits nothing this window hears, and the test
	 * would then pass against no wiring at all.
	 */
	bssh_profile_store *store = window.profile_store();
	QVERIFY(store);

	bssh_profile_t host;
	host.name = QStringLiteral("prod");
	host.host = QStringLiteral("wrong.example");
	host.user = QStringLiteral("nabbe");
	QVERIFY(store->save(host));

	int id = 0;
	for (const bssh_profile_t &row : store->profiles()) {
		if (row.name == QStringLiteral("prod")) id = row.id;
	}
	QVERIFY(id > 0);

	/*
	 * Through the registry the WINDOW watches, which is process-wide, so
	 * the refresh reaches it exactly as it would for a session the window
	 * opened itself.
	 */
	bssh_session *session = window.registry()->create(store->profile(id));
	QVERIFY(session);
	QCOMPARE(session->profile().host, QStringLiteral("wrong.example"));

	/*
	 * The hostname is corrected while the session is open. Nothing used
	 * to carry that across: the session's profile is a copy taken at
	 * construction, and the store's changed() signal had no listener
	 * anywhere in src/. So a reconnect kept dialling the old name, a
	 * later preset edit re-resolved against the stale overrides and undid
	 * the correction for good, and storing a password wrote the whole
	 * stale row back over it.
	 */
	bssh_profile_t edited = store->profile(id);
	edited.host = QStringLiteral("right.example");
	QVERIFY(store->save(edited));

	QCOMPARE(session->profile().host, QStringLiteral("right.example"));

	/* The registry is shared, so this session must not outlive the test. */
	window.registry()->close(session);
	bssh_ssh_session::set_factory(nullptr);
}

void session_registry_test::a_reconnect_hands_the_new_shell_a_clean_terminal() {
	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});

	bssh_session_registry registry;
	bssh_session *session = registry.create(profile_for(QStringLiteral("drops"),
	                                                     QStringLiteral("a.example")));
	session->connect_to_host();
	QVERIFY(fake);

	bssh_emulator *emulator = session->emulator();
	QVERIFY(emulator);

	emulator->feed(QByteArrayLiteral("history worth keeping\r\n"));

	/*
	 * What a full-screen program leaves behind when the link dies under
	 * it -- it never got to disable any of this.
	 */
	emulator->feed(QByteArrayLiteral("\x1b[?1049h\x1b[?1000h\x1b[?1006h\x1b[?2004h"));
	QVERIFY(emulator->mouse_reporting());
	QVERIFY(emulator->alternate_screen());

	/*
	 * sec 5.3 keeps the emulator across a reconnect so the user keeps
	 * their history, and the shell that comes back is genuinely new. This
	 * asserts on the SESSION rather than on the emulator, because
	 * emulator::reset_modes() being correct says nothing about whether
	 * anything calls it -- and a reset nothing invokes reads as fixed in
	 * every review. The sabotage runner reported exactly that: the three
	 * edits inside the reset were caught and removing the call site was
	 * MISSED, by a test that only ever asked the emulator directly.
	 */
	session->reconnect();

	QVERIFY(!emulator->mouse_reporting());
	QVERIFY(!emulator->alternate_screen());
	QVERIFY2(emulator->screen().to_text().contains(QStringLiteral("history worth keeping")),
	          qPrintable(emulator->screen().to_text()));

	bssh_ssh_session::set_factory(nullptr);
}

void session_registry_test::turning_scrollback_off_actually_turns_it_off() {
	/*
	 * Sec 10.2's scrollback size, at the one value the spin box offers
	 * that the session discarded.
	 *
	 * BSSH_PRESET_MIN_SCROLLBACK is 0 and the preset editor's range
	 * starts there, so "0 lines" is a value the UI invites. The session
	 * applied it behind `if (lines > 0)`, so the emulator kept its 5000
	 * default and the app went on retaining history the user had just
	 * asked it not to keep. For a terminal that is a privacy setting --
	 * somebody who turns scrollback off is asking for output not to be
	 * held -- and it silently did nothing.
	 *
	 * The guard was reading an ABSENT key, which QJsonValue::toInt()
	 * also reports as 0. Absent and zero are different, and every caller
	 * passes a RESOLVED object, where preset_t::to_json() always inserts
	 * the key -- so the case it protected against cannot occur and the
	 * case it discarded is the one a user can reach.
	 */
	bssh_profile_t profile = profile_for(QStringLiteral("quiet"),
	                                      QStringLiteral("quiet.example"));
	bssh_session session(profile);

	QJsonObject settings;
	settings.insert(QStringLiteral("scrollback_lines"), 120);
	session.set_settings(settings);
	QCOMPARE(session.emulator()->scrollback().max_lines(), 120);

	/*
	 * Asserted after a non-zero value, so this cannot pass by the setting
	 * never being applied at all: the ring has to be seen changing before
	 * it is seen going to zero.
	 */
	settings.insert(QStringLiteral("scrollback_lines"), 0);
	session.set_settings(settings);
	QCOMPARE(session.emulator()->scrollback().max_lines(), 0);

	/* And nothing is retained once it is off. */
	const qint64 before = session.emulator()->scrollback().base();

	/*
	 * Enough to SCROLL, which three lines are not: libvterm pushes to the
	 * scrollback only when a line leaves the top of the screen, so a
	 * short feed reaches none of this and would let the assertions below
	 * pass without the ring ever being asked to drop anything.
	 */
	QByteArray output;
	for (int i = 0; i < 60; ++i) {
		output += QByteArrayLiteral("line ") + QByteArray::number(i)
		        + QByteArrayLiteral("\r\n");
	}
	session.emulator()->feed(output);

	QCOMPARE(session.emulator()->scrollback().count(), 0);

	/*
	 * And the lines that went past are still COUNTED, which is what
	 * base() promises: "how many lines this ring has dropped off the
	 * front". At capacity nought every pushed line is dropped the instant
	 * it arrives, so the count has to move or `base() + count()` stops
	 * being the number of lines that have gone by -- and the emulator
	 * measures a resize with exactly that difference to keep images with
	 * their text (sec 7.1).
	 *
	 * Reachable only now: the session used to discard a scrollback size
	 * of 0 before it could ever get here.
	 */
	QVERIFY2(session.emulator()->scrollback().base() > before,
	          "a line dropped by a zero-length ring is still a line that "
	          "has gone past, and base() is what counts those");
}

void session_registry_test::a_silenced_bell_does_not_mark_the_tab() {
	bssh_profile_t profile;
	profile.host = QStringLiteral("noisy.example.com");
	bssh_session session(profile);

	QJsonObject settings;
	settings.insert(QStringLiteral("bell_enabled"), false);
	session.set_settings(settings);

	session.set_on_screen(false);
	session.emulator()->feed(QByteArrayLiteral("\a"));

	/*
	 * "Do not tell me about the bell" has to mean the tab dot as well as
	 * the sound. A build machine that rings every few minutes would
	 * otherwise keep its tab permanently marked, which trains the user to
	 * ignore the one marker that means something.
	 */
	QVERIFY2(!session.has_unseen_bell(), "a silenced bell must not mark the tab");

	settings.insert(QStringLiteral("bell_enabled"), true);
	session.set_settings(settings);
	session.emulator()->feed(QByteArrayLiteral("\a"));
	QVERIFY(session.has_unseen_bell());
}

void session_registry_test::the_window_title_expands_what_the_session_knows() {
	bssh_session_registry registry;

	bssh_profile_t profile = profile_for(QStringLiteral("build box"),
	                                      QStringLiteral("build.example"));
	profile.port = 2222;

	bssh_session *session = registry.create(profile);

	/*
	 * %t is what the session calls itself, which until the remote sets an
	 * OSC title is the saved name -- and that is the whole fix: a window
	 * showed "BeerSSH" and nothing else until a remote spoke up.
	 */
	QCOMPARE(bssh_expand_window_title(QStringLiteral("%t - BeerSSH"), session),
	          QStringLiteral("build box - BeerSSH"));

	QCOMPARE(bssh_expand_window_title(QStringLiteral("%u@%h:%p"), session),
	          QStringLiteral("nabbe@build.example:2222"));

	QCOMPARE(bssh_expand_window_title(QStringLiteral("%n on %a"), session),
	          QStringLiteral("build box on BeerSSH"));

	/* A literal per cent, and an unknown placeholder left as written --
	 * a title that silently loses characters cannot be debugged from
	 * outside the program. */
	QCOMPARE(bssh_expand_window_title(QStringLiteral("100%% %z"), session),
	          QStringLiteral("100% %z"));

	/*
	 * No session at all is the app's own name, not a format with holes in
	 * it: "%t - BeerSSH" with an empty %t leaves a stray dash in the task
	 * switcher.
	 */
	QCOMPARE(bssh_expand_window_title(QStringLiteral("%t - BeerSSH"), nullptr),
	          QStringLiteral("BeerSSH"));
}

/*
 * sec 5.5 says a desktop notification carries three things: reconnecting,
 * reconnect failed, and a bell in a pane nobody is looking at.
 * bssh_platform::notify() was declared with that list written beside it,
 * implemented on both platforms, and called by nothing at all.
 *
 * Reading the interface could not find that, because the interface was
 * complete -- the declarations are exactly the part that is not missing.
 * Only asking each method who calls it does, which is sec 11's erase()
 * again: a method with no caller anywhere hides better than a signal with
 * no listener, because a dangling signal at least looks unfinished.
 */
void session_registry_test::the_three_events_sec_5_5_names_reach_the_platform() {
	QStringList titles;
	QObject sink;
	QObject::connect(bssh_platform::instance(), &bssh_platform::notified, &sink,
	                  [&titles](const QString &title, const QString &) {
		titles.append(title);
	});

	bssh_session_registry registry;
	bssh_profile_t profile;
	profile.host = QStringLiteral("box.example.com");
	bssh_session *session = registry.create(profile);

	bssh_conn_manager *manager = registry.conn_manager_for(session);
	QVERIFY(manager != nullptr);

	/*
	 * On screen first, and nothing is announced. A notification for the
	 * pane you are looking at is a popup about something already visible,
	 * and the window's own status line covers that case -- every one of
	 * its handlers for these signals opens `if (session != m_current)'.
	 */
	session->set_on_screen(true);
	emit manager->reconnect_started(1);
	QVERIFY2(titles.isEmpty(), "announced a reconnect the user was watching");

	/* Out of sight, and it is. */
	session->set_on_screen(false);
	emit manager->reconnect_started(2);
	QCOMPARE(titles.size(), 1);
	QCOMPARE(titles.first(), QStringLiteral("Reconnecting"));

	/*
	 * Giving up is announced either way, deliberately: it is terminal,
	 * and a pane that has quietly stopped retrying looks exactly like one
	 * still trying.
	 */
	session->set_on_screen(true);
	emit manager->gave_up();
	QCOMPARE(titles.size(), 2);
	QCOMPARE(titles.at(1), QStringLiteral("Reconnect failed"));

	/*
	 * And the bell, fed as a BEL byte rather than by emitting the signal.
	 *
	 * The first version of this did `emit session->bell_while_away()',
	 * and the sabotage that DELETES that emit from the session stayed
	 * green -- the test was raising the signal itself, so removing the
	 * only thing that legitimately raises it changed nothing it could
	 * see. A test that calls the helper it is verifying cannot see a
	 * wrong caller. Driving it from the wire exercises the two
	 * conditions the session evaluates (bell_enabled, and nothing
	 * showing it) instead of assuming their result.
	 */
	QVERIFY(session->emulator() != nullptr);
	session->set_on_screen(false);
	session->emulator()->feed(QByteArrayLiteral("\a"));
	QCOMPARE(titles.size(), 3);
	QCOMPARE(titles.at(2), QStringLiteral("Bell"));

	/* And on screen it is remembered rather than announced (sec 8.3). */
	session->set_on_screen(true);
	session->emulator()->feed(QByteArrayLiteral("\a"));
	QCOMPARE(titles.size(), 3);
}
