#include <QtTest>

#include "model/session.h"
#include <QApplication>
#include <QImage>
#include <cmath>
#include "ui/theme.h"
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

namespace {

/* WCAG relative luminance. */
double channel(int value) {
	const double c = value / 255.0;
	return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(const QColor &colour) {
	return 0.2126 * channel(colour.red()) + 0.7152 * channel(colour.green()) +
	        0.0722 * channel(colour.blue());
}

double contrast(const QColor &a, const QColor &b) {
	const double la = luminance(a);
	const double lb = luminance(b);
	return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

/* The dot's own colour, read back from the pixmap it draws. */
QColor dot_colour(bssh_tab_status::state_t state) {
	const QImage image = bssh_tab_status::dot(16, state).toImage();
	return image.pixelColor(image.width() / 2, image.height() / 2);
}

} /* namespace */

void tab_status_test::every_dot_is_visible_in_both_themes() {
	/*
	 * These four colours are the only thing on a tab that says whether a
	 * session is alive, and they were chosen against a dark bar. When
	 * sec 6.2's light theme arrived every one of them failed WCAG's 3:1
	 * for a graphical indicator against the light window colour -- green
	 * at 1.83:1, grey at 2.22:1, and "connecting" yellow at 1.44:1, which
	 * is as good as invisible.
	 *
	 * Asserted against the palette actually in force, so this checks the
	 * colours the app would really draw rather than a table of constants.
	 */
	const bssh_tab_status::state_t states[] = {
		bssh_tab_status::CONNECTED, bssh_tab_status::CONNECTING,
		bssh_tab_status::BELL, bssh_tab_status::DISCONNECTED
	};

	for (bssh_theme::mode_t mode : { bssh_theme::MODE_DARK, bssh_theme::MODE_LIGHT }) {
		bssh_theme::apply(mode);
		const QColor window = QApplication::palette().color(QPalette::Window);

		for (bssh_tab_status::state_t state : states) {
			const double ratio = contrast(dot_colour(state), window);
			QVERIFY2(ratio >= 3.0,
			          qPrintable(QStringLiteral("%1 dot on the %2 theme is only %3:1")
			                             .arg(int(state))
			                             .arg(bssh_theme::to_text(mode))
			                             .arg(ratio, 0, 'f', 2)));
		}
	}
}
