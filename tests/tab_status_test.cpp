#include <QtTest>

#include "model/session.h"
#include "tab_status_test.h"
#include "term/emulator.h"
#include "ui/tab_status.h"

static bssh_profile_t host_named(const QString &name) {
	bssh_profile_t profile;
	profile.name = name;
	profile.host = name + QStringLiteral(".example");
	return profile;
}

void tab_status_test::reports_the_connection_state() {
	bssh_session session(host_named(QStringLiteral("a")));

	/* Never connected: grey, not "connected but quiet". */
	QCOMPARE(bssh_tab_status::state_for(&session), bssh_tab_status::DISCONNECTED);
	QCOMPARE(bssh_tab_status::state_for(nullptr), bssh_tab_status::DISCONNECTED);

	/* Every state has a distinct colour, or the dot says nothing. */
	QVERIFY(bssh_tab_status::colour_for(bssh_tab_status::CONNECTED)
	         != bssh_tab_status::colour_for(bssh_tab_status::CONNECTING));
	QVERIFY(bssh_tab_status::colour_for(bssh_tab_status::BELL)
	         != bssh_tab_status::colour_for(bssh_tab_status::CONNECTED));
	QVERIFY(bssh_tab_status::colour_for(bssh_tab_status::DISCONNECTED)
	         != bssh_tab_status::colour_for(bssh_tab_status::CONNECTED));
}

void tab_status_test::a_bell_is_remembered_only_while_you_are_elsewhere() {
	bssh_session session(host_named(QStringLiteral("b")));

	/*
	 * The entire value of a bell is telling you about the tab you are NOT
	 * looking at. Rung while on screen, it has already done its job and
	 * must not leave a marker behind.
	 */
	session.set_on_screen(true);
	session.emulator()->feed(QByteArrayLiteral("\a"));
	QVERIFY(!session.has_unseen_bell());

	session.set_on_screen(false);
	session.emulator()->feed(QByteArrayLiteral("\a"));
	QVERIFY(session.has_unseen_bell());

	/* Switching to it is what clears the marker. */
	session.set_on_screen(true);
	QVERIFY(!session.has_unseen_bell());
}

void tab_status_test::a_dead_session_outranks_its_own_bell() {
	bssh_session session(host_named(QStringLiteral("c")));

	session.set_on_screen(false);
	session.emulator()->feed(QByteArrayLiteral("\a"));
	QVERIFY(session.has_unseen_bell());

	/*
	 * A bell on a tab whose connection has dropped would invite the user
	 * to go and read output that is no longer arriving. Dead wins.
	 */
	QCOMPARE(bssh_tab_status::state_for(&session), bssh_tab_status::DISCONNECTED);
}
