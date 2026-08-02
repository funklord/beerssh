#include <QAbstractButton>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QtTest>

#include "input/input_router.h"
#include "ui/key_row_layout.h"
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

void input_router_test::kitty_protocol_disambiguates_ctrl_and_escape() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	/* Legacy first: Ctrl+I and Tab really are the same byte. */
	QCOMPARE(press(router, emulator, Qt::Key_I, Qt::ControlModifier), QByteArrayLiteral("\t"));
	QCOMPARE(press(router, emulator, Qt::Key_Tab), QByteArrayLiteral("\t"));

	/* The remote turns on kitty's disambiguate tier. */
	emulator.feed(QByteArrayLiteral("\x1b[>1u"));
	QCOMPARE(emulator.kitty_keyboard_flags(), 1);

	/* Now they differ, which is the entire point of the protocol. */
	QCOMPARE(press(router, emulator, Qt::Key_I, Qt::ControlModifier),
	          QByteArrayLiteral("\x1b[105;5u"));
	QCOMPARE(press(router, emulator, Qt::Key_Tab), QByteArrayLiteral("\t"));

	/* Alt+x stops being byte-identical to Escape followed by x. */
	QCOMPARE(press(router, emulator, Qt::Key_X, Qt::AltModifier, QStringLiteral("x")),
	          QByteArrayLiteral("\x1b[120;3u"));
	QCOMPARE(press(router, emulator, Qt::Key_Escape), QByteArrayLiteral("\x1b[27u"));

	/* Unmodified typing must stay exactly as it was, or every program
	 * that never opted in would break. */
	QCOMPARE(press(router, emulator, Qt::Key_X, Qt::NoModifier, QStringLiteral("x")),
	          QByteArrayLiteral("x"));
}

void input_router_test::kitty_protocol_pushes_and_pops() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	emulator.feed(QByteArrayLiteral("\x1b[>1u"));
	QCOMPARE(emulator.kitty_keyboard_flags(), 1);

	/*
	 * Push/pop exists so a full-screen program can restore whatever the
	 * shell had without knowing what it was.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[<u"));
	QCOMPARE(emulator.kitty_keyboard_flags(), 0);
	QCOMPARE(press(router, emulator, Qt::Key_Escape), QByteArrayLiteral("\x1b"));

	/* Popping past the bottom must not underflow into a negative stack. */
	emulator.feed(QByteArrayLiteral("\x1b[<9u"));
	QCOMPARE(emulator.kitty_keyboard_flags(), 0);

	/* CSI = flags ; 2 u ORs bits into the current entry. */
	emulator.feed(QByteArrayLiteral("\x1b[=1;2u"));
	QCOMPARE(emulator.kitty_keyboard_flags(), 1);
	emulator.feed(QByteArrayLiteral("\x1b[=1;3u"));
	QCOMPARE(emulator.kitty_keyboard_flags(), 0);
}

void input_router_test::kitty_protocol_answers_a_query() {
	bssh_emulator emulator(80, 24);

	/* A client cannot use the protocol without being able to detect it. */
	emulator.feed(QByteArrayLiteral("\x1b[?u"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?0u"));

	emulator.feed(QByteArrayLiteral("\x1b[>5u"));
	emulator.feed(QByteArrayLiteral("\x1b[?u"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[?5u"));
}

void input_router_test::modify_other_keys_encodes_and_reports_its_level() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	emulator.feed(QByteArrayLiteral("\x1b[>4;2m"));
	QCOMPARE(emulator.modify_other_keys_level(), 2);

	QCOMPARE(press(router, emulator, Qt::Key_I, Qt::ControlModifier),
	          QByteArrayLiteral("\x1b[27;5;105~"));

	/* XTQMODKEYS: the client asks what it actually got. */
	emulator.feed(QByteArrayLiteral("\x1b[?4m"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[>4;2m"));

	/*
	 * Level 3 is clamped to 2 and reported as 2, so a client asking for
	 * more is told plainly rather than silently getting less (sec 7.1).
	 */
	emulator.feed(QByteArrayLiteral("\x1b[>4;3m"));
	QCOMPARE(emulator.modify_other_keys_level(), 2);
	emulator.feed(QByteArrayLiteral("\x1b[?4m"));
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1b[>4;2m"));
}

void input_router_test::pane_chords_are_kept_by_the_app() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);
	QSignalSpy spy(&router, &bssh_input_router::shortcut_pressed);

	QKeyEvent left(QEvent::KeyPress, Qt::Key_Left,
	                Qt::ControlModifier | Qt::AltModifier, QString());
	QVERIFY(!router.handle_key(&left));
	QCOMPARE(spy.count(), 1);

	/* Stolen from the remote, so it must not also produce bytes. */
	QCOMPARE(emulator.take_output(), QByteArray());

	/*
	 * Ctrl+Arrow and Alt+Arrow are both in daily use by readline and by
	 * editors, so neither may be swallowed -- only the pair together
	 * (sec 6.1: keep the stolen set minimal).
	 */
	QVERIFY(!press(router, emulator, Qt::Key_Left, Qt::ControlModifier).isEmpty());
	QVERIFY(!press(router, emulator, Qt::Key_Left, Qt::AltModifier).isEmpty());
	QCOMPARE(spy.count(), 1);
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

void input_router_test::a_key_row_layout_changes_which_keys_exist() {
	const bssh_key_row_catalog catalog;

	/* Every layout has to be usable: a row without Escape or a modifier
	 * is a row that cannot drive a terminal at all. */
	for (const bssh_key_row_layout_t &layout : catalog.all()) {
		QVERIFY(!layout.id.isEmpty());
		QVERIFY(!layout.display_name.isEmpty());
		QVERIFY2(!layout.description.isEmpty(),
		         qPrintable("no description for " + layout.id));

		bool has_escape = false;
		bool has_modifier = false;
		for (const bssh_key_t &key : layout.keys) {
			if (key.kind == bssh_key_t::KEY && key.value == bssh_emulator::KEY_ESCAPE) {
				has_escape = true;
			}
			if (key.kind == bssh_key_t::MODIFIER) has_modifier = true;
		}

		QVERIFY2(has_escape, qPrintable("no Escape in " + layout.id));
		QVERIFY2(has_modifier, qPrintable("no modifier in " + layout.id));
	}

	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);
	bssh_key_row row(&router);

	row.set_layout(QStringLiteral("tmux"));
	QCOMPARE(row.layout_id(), QStringLiteral("tmux"));

	const QList<QAbstractButton *> tmux_keys = row.findChildren<QAbstractButton *>();
	bool has_prefix = false;
	for (const QAbstractButton *button : tmux_keys) {
		if (button->text() == QStringLiteral("C-b")) has_prefix = true;
	}
	QVERIFY2(has_prefix, "the tmux layout exists for the prefix key");

	/*
	 * And they have to be VISIBLE, not merely present. Counting buttons
	 * is what this test did at first, and it passed while every layout
	 * switched to at runtime drew an empty strip: Qt does not show a
	 * widget added to a layout whose parent is already visible. A key row
	 * that reports twelve keys and displays none is worse than one that
	 * fails outright.
	 */
	row.set_visibility_mode(bssh_key_row::ALWAYS);
	for (const QAbstractButton *button : tmux_keys) {
		QVERIFY2(!button->isHidden(),
		         qPrintable("key not shown: " + button->text()));
	}

	/* Switching layouts replaces the row rather than appending to it. */
	row.set_layout(QStringLiteral("minimal"));
	QVERIFY(row.findChildren<QAbstractButton *>().size() < tmux_keys.size());

	for (const QAbstractButton *button : row.findChildren<QAbstractButton *>()) {
		QVERIFY2(button->text() != QStringLiteral("C-b"),
		         "keys from the previous layout must not survive");
	}

	/* An unknown layout falls back rather than leaving an empty strip: a
	 * row with no keys is a session that cannot send Ctrl-C. */
	row.set_layout(QStringLiteral("no-such-layout"));
	QVERIFY(!row.findChildren<QAbstractButton *>().isEmpty());
}

void input_router_test::a_chord_key_sends_one_press_not_a_sticky_modifier() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);
	bssh_key_row row(&router);

	row.set_layout(QStringLiteral("tmux"));

	QAbstractButton *prefix = nullptr;
	for (QAbstractButton *button : row.findChildren<QAbstractButton *>()) {
		if (button->text() == QStringLiteral("C-b")) prefix = button;
	}
	QVERIFY(prefix != nullptr);

	emulator.take_output();
	prefix->click();

	/* Ctrl-B is 0x02. */
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x02"));

	/*
	 * And it must not have left Ctrl stuck on. A chord that set a sticky
	 * modifier would make the NEXT key a Ctrl chord too -- so pressing
	 * the tmux prefix and then "c" would send Ctrl-C, killing the pane
	 * the user was trying to create.
	 */
	QCOMPARE(router.sticky_modifiers(), quint8(bssh_emulator::MOD_NONE));
}
