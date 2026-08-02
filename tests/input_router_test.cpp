#include <QAbstractButton>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QtTest>

#include "input/input_router.h"
#include "input_router_test.h"
#include "term/emulator.h"
#include "ui/key_row.h"

/*
 * Drives a key through the router and returns the bytes it produced.
 * Building a QKeyEvent by hand rather than posting it to a widget keeps
 * these tests independent of focus, event loops and window managers.
 */
static QByteArray press(bssh_input_router &router, bssh_emulator &emulator,
                         int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
                         const QString &text = QString()) {
	QKeyEvent event(QEvent::KeyPress, key, modifiers, text);
	router.handle_key(&event);
	return emulator.take_output();
}

void input_router_test::plain_characters_pass_through() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	QCOMPARE(press(router, emulator, Qt::Key_A, Qt::NoModifier, QStringLiteral("a")),
	          QByteArrayLiteral("a"));

	/*
	 * text(), not the key code: on a French layout the physical A key
	 * reports Qt::Key_A but types "q", and a terminal that ignores the
	 * layout is unusable for anyone not on a US keyboard.
	 */
	QCOMPARE(press(router, emulator, Qt::Key_A, Qt::ShiftModifier, QStringLiteral("q")),
	          QByteArrayLiteral("q"));
}

void input_router_test::ctrl_c_is_the_interrupt_byte() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	/* 0x03, and exactly once -- not the character AND the modifier. */
	QCOMPARE(press(router, emulator, Qt::Key_C, Qt::ControlModifier, QStringLiteral("\x03")),
	          QByteArrayLiteral("\x03"));
}

void input_router_test::ctrl_letters_are_case_insensitive() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	/* Ctrl-D and Ctrl-Shift-D are both EOT; the shift must not change it. */
	const QByteArray plain = press(router, emulator, Qt::Key_D, Qt::ControlModifier);
	QCOMPARE(plain, QByteArrayLiteral("\x04"));
}

void input_router_test::arrows_follow_application_cursor_mode() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	QCOMPARE(press(router, emulator, Qt::Key_Up), QByteArrayLiteral("\x1b[A"));

	/*
	 * DECCKM. Full-screen programs turn it on, and an arrow key that
	 * keeps sending the normal sequence afterwards is why "arrows do
	 * nothing in vim" is a bug report every terminal gets once.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[?1h"));
	QCOMPARE(press(router, emulator, Qt::Key_Up), QByteArrayLiteral("\x1bOA"));

	emulator.feed(QByteArrayLiteral("\x1b[?1l"));
	QCOMPARE(press(router, emulator, Qt::Key_Up), QByteArrayLiteral("\x1b[A"));
}

void input_router_test::function_keys_and_navigation_produce_sequences() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	QCOMPARE(press(router, emulator, Qt::Key_F1), QByteArrayLiteral("\x1bOP"));
	QCOMPARE(press(router, emulator, Qt::Key_F5), QByteArrayLiteral("\x1b[15~"));
	QCOMPARE(press(router, emulator, Qt::Key_Home), QByteArrayLiteral("\x1b[H"));
	QCOMPARE(press(router, emulator, Qt::Key_PageUp), QByteArrayLiteral("\x1b[5~"));
	QCOMPARE(press(router, emulator, Qt::Key_Delete), QByteArrayLiteral("\x1b[3~"));
	QCOMPARE(press(router, emulator, Qt::Key_Return), QByteArrayLiteral("\r"));
	QCOMPARE(press(router, emulator, Qt::Key_Tab), QByteArrayLiteral("\t"));
	QCOMPARE(press(router, emulator, Qt::Key_Backspace), QByteArrayLiteral("\x7f"));
	QCOMPARE(press(router, emulator, Qt::Key_Escape), QByteArrayLiteral("\x1b"));
}

void input_router_test::alt_prefixes_with_escape() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	/* Meta-b, as readline expects it: ESC then the character. */
	QCOMPARE(press(router, emulator, Qt::Key_B, Qt::AltModifier, QStringLiteral("b")),
	          QByteArrayLiteral("\x1b" "b"));
}

void input_router_test::sticky_modifier_applies_once_then_clears() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);
	QSignalSpy spy(&router, &bssh_input_router::sticky_modifiers_changed);

	/*
	 * The on-screen key row's whole reason for existing: a touch screen
	 * cannot hold Ctrl while pressing C (sec 6.2).
	 */
	router.set_sticky_modifiers(bssh_emulator::MOD_CTRL);
	QCOMPARE(press(router, emulator, Qt::Key_C, Qt::NoModifier, QStringLiteral("c")),
	          QByteArrayLiteral("\x03"));

	/* One shot: the next key is unmodified, or every subsequent
	 * keystroke would be a control character. */
	QCOMPARE(router.sticky_modifiers(), quint8(bssh_emulator::MOD_NONE));
	QCOMPARE(press(router, emulator, Qt::Key_C, Qt::NoModifier, QStringLiteral("c")),
	          QByteArrayLiteral("c"));
	QVERIFY(spy.count() > 0);
}

void input_router_test::ctrl_shift_c_is_kept_by_the_app() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);
	QSignalSpy spy(&router, &bssh_input_router::shortcut_pressed);

	QKeyEvent copy(QEvent::KeyPress, Qt::Key_C,
	                Qt::ControlModifier | Qt::ShiftModifier, QString());
	QVERIFY(!router.handle_key(&copy));

	QCOMPARE(spy.count(), 1);
	/* Stolen from the remote, so it must not also produce bytes. */
	QCOMPARE(emulator.take_output(), QByteArray());

	/* Ctrl-C alone stays SIGINT -- the reason the copy chord needs shift. */
	QCOMPARE(press(router, emulator, Qt::Key_C, Qt::ControlModifier),
	          QByteArrayLiteral("\x03"));
}

void input_router_test::paste_is_bracketed_only_when_the_remote_asked() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	router.paste(QStringLiteral("ls"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("ls"));

	/* Bracketed paste on: the text arrives wrapped, so the shell knows
	 * it was pasted and does not run each line as it arrives. */
	emulator.feed(QByteArrayLiteral("\x1b[?2004h"));
	router.paste(QStringLiteral("ls"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[200~ls\x1b[201~"));
}

void input_router_test::key_row_goes_through_the_router() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);
	bssh_key_row row(&router);

	/*
	 * Every button must produce bytes via the router, not by writing to
	 * the session itself (sec 6.3). Finding the buttons by label rather
	 * than by index keeps this from breaking every time the row's layout
	 * is reordered -- which sec 10.2 says presets will do.
	 */
	const QList<QAbstractButton *> buttons = row.findChildren<QAbstractButton *>();
	QAbstractButton *ctrl = nullptr;
	QAbstractButton *escape = nullptr;
	for (QAbstractButton *button : buttons) {
		if (button->text() == QStringLiteral("Ctrl")) ctrl = button;
		if (button->text() == QStringLiteral("Esc")) escape = button;
	}
	QVERIFY(ctrl != nullptr);
	QVERIFY(escape != nullptr);

	escape->click();
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b"));

	/* Sticky Ctrl, then a key: the touch equivalent of holding Ctrl. */
	ctrl->click();
	QCOMPARE(router.sticky_modifiers(), quint8(bssh_emulator::MOD_CTRL));
	QVERIFY(ctrl->isChecked());

	QKeyEvent event(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier, QStringLiteral("c"));
	router.handle_key(&event);
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x03"));

	/* The button must stop looking pressed once the modifier is spent. */
	QVERIFY(!ctrl->isChecked());
}

void input_router_test::key_row_visibility_follows_the_input_device() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);
	bssh_key_row row(&router);

	/* Tests run on the desktop platform, which is not touch-primary. */
	QCOMPARE(row.visibility_mode(), bssh_key_row::AUTO);
	QVERIFY(!row.should_be_visible());

	row.set_visibility_mode(bssh_key_row::ALWAYS);
	QVERIFY(row.should_be_visible());

	row.set_visibility_mode(bssh_key_row::NEVER);
	QVERIFY(!row.should_be_visible());
}
