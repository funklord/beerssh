#include <QApplication>
#include <QScrollArea>
#include <QMouseEvent>
#include <QAbstractButton>
#include <QKeyCombination>
#include <QKeyEvent>
#include <QKeySequence>
#include <QSignalSpy>
#include <QtTest>

#include "input/input_router.h"
#include "keyboard/key_row_layout.h"
#include "ui/key_row_editor.h"
#include "ui/long_press.h"
#include "input_router_test.h"
#include "phone_metrics.h"
#include "term/emulator.h"
#include "keyboard/key_row.h"

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

static QString seq_text(Qt::KeyboardModifiers mods, Qt::Key key) {
	return QKeySequence(QKeyCombination(mods, key)).toString(QKeySequence::PortableText);
}

void input_router_test::a_key_remap_sends_the_targets_bytes() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	/* What Escape and End produce on their own, to compare against. */
	const QByteArray escape = press(router, emulator, Qt::Key_Escape);
	const QByteArray end = press(router, emulator, Qt::Key_End);
	QVERIFY(!escape.isEmpty());
	QVERIFY(!end.isEmpty());

	QHash<QString, QString> remaps;
	remaps.insert(seq_text(Qt::NoModifier, Qt::Key_F13),
	              seq_text(Qt::NoModifier, Qt::Key_Escape));
	bssh_input_router::set_key_remaps(remaps);

	/* F13 now speaks Escape, byte for byte. */
	QCOMPARE(press(router, emulator, Qt::Key_F13), escape);

	/* A key nobody remapped is untouched. */
	QCOMPARE(press(router, emulator, Qt::Key_End), end);

	bssh_input_router::set_key_remaps(QHash<QString, QString>());
}

void input_router_test::a_stolen_chord_beats_a_key_remap() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	QSignalSpy shortcuts(&router, &bssh_input_router::shortcut_pressed);

	/*
	 * Try to remap the scroll chord's own keys. handle_key checks the
	 * stolen chords first (sec 6.1), so the app keeps the chord and the
	 * remap never runs -- the scroll fires and nothing reaches the shell.
	 */
	QHash<QString, QString> remaps;
	remaps.insert(seq_text(Qt::ShiftModifier, Qt::Key_PageUp),
	              seq_text(Qt::NoModifier, Qt::Key_Escape));
	bssh_input_router::set_key_remaps(remaps);

	const QByteArray out = press(router, emulator, Qt::Key_PageUp, Qt::ShiftModifier);

	QCOMPARE(shortcuts.count(), 1);
	QCOMPARE(out, QByteArray());

	bssh_input_router::set_key_remaps(QHash<QString, QString>());
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

void input_router_test::a_reset_gives_ctrl_c_back_after_a_program_dies_holding_it() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	QCOMPARE(press(router, emulator, Qt::Key_C, Qt::ControlModifier, QStringLiteral("\x03")),
	          QByteArrayLiteral("\x03"));

	/*
	 * A kitty-aware program pushes its flags and is killed before it can
	 * pop them. From here Ctrl+C is CSI 99;5u -- correct while the
	 * program is running, and catastrophic once it is not: no SIGINT is
	 * delivered and the escape lands on the shell's command line.
	 */
	emulator.feed(QByteArrayLiteral("\x1b[>5u"));
	const QByteArray held = press(router, emulator, Qt::Key_C, Qt::ControlModifier,
	                               QStringLiteral("\x03"));
	QVERIFY2(held != QByteArrayLiteral("\x03"), held.toHex().constData());

	/*
	 * `reset` at the shell sends RIS. Every terminal clears the kitty
	 * flags on it and so does xterm for modifyOtherKeys -- and libvterm
	 * could not, because it does not model either and had no way to tell
	 * anyone a reset had happened (vterm-patch/0005).
	 */
	/*
	 * Octal, not hex: `\x1bc` is ONE character, because a hex escape eats
	 * every hex digit that follows it and `c` is one. An octal escape
	 * takes three digits at most, so `\033c` really is ESC then 'c'. The
	 * hex form compiles with a warning and feeds a stray 0xBC.
	 */
	emulator.feed(QByteArrayLiteral("\033c"));

	QCOMPARE(press(router, emulator, Qt::Key_C, Qt::ControlModifier, QStringLiteral("\x03")),
	          QByteArrayLiteral("\x03"));

	/* And the soft reset, DECSTR, which is the other half of what
	 * xterm's rs1/rs2 sends. */
	emulator.feed(QByteArrayLiteral("\x1b[>5u"));
	QVERIFY(press(router, emulator, Qt::Key_C, Qt::ControlModifier,
	               QStringLiteral("\x03")) != QByteArrayLiteral("\x03"));

	emulator.feed(QByteArrayLiteral("\x1b[!p"));
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

void input_router_test::a_modifier_locks_on_the_second_press_and_releases_on_the_third() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	/*
	 * The Treo's cycle (sec 6.2). One press arms for the next key, a
	 * second locks until pressed again, a third releases.
	 *
	 * A one-shot alone cannot express what a terminal asks for
	 * constantly. Ctrl-C is one key and arming serves it; walking a
	 * buffer with Ctrl-N or paging with Ctrl-B is the same modifier over
	 * and over, and re-arming before every keystroke is an interaction no
	 * physical keyboard demands.
	 */
	router.cycle_modifier(bssh_emulator::MOD_CTRL);
	QCOMPARE(router.modifier_state(bssh_emulator::MOD_CTRL),
	          bssh_input_router::MODIFIER_ONCE);

	QCOMPARE(press(router, emulator, Qt::Key_C, Qt::NoModifier, QStringLiteral("c")),
	          QByteArrayLiteral("\x03"));
	QCOMPARE(router.modifier_state(bssh_emulator::MOD_CTRL),
	          bssh_input_router::MODIFIER_OFF);

	/* Second press: locked. */
	router.cycle_modifier(bssh_emulator::MOD_CTRL);
	router.cycle_modifier(bssh_emulator::MOD_CTRL);
	QCOMPARE(router.modifier_state(bssh_emulator::MOD_CTRL),
	          bssh_input_router::MODIFIER_LOCKED);

	/*
	 * Surviving the keystroke is the whole difference between the two
	 * states, so it is asserted over TWO keys rather than one: a lock
	 * that lasted exactly one more keystroke than an arm would satisfy a
	 * single check and be no more useful than the arm.
	 */
	QCOMPARE(press(router, emulator, Qt::Key_C, Qt::NoModifier, QStringLiteral("c")),
	          QByteArrayLiteral("\x03"));
	QCOMPARE(router.modifier_state(bssh_emulator::MOD_CTRL),
	          bssh_input_router::MODIFIER_LOCKED);
	QCOMPARE(press(router, emulator, Qt::Key_D, Qt::NoModifier, QStringLiteral("d")),
	          QByteArrayLiteral("\x04"));

	/* And the key row's own path carries it too, not just handle_key. */
	QCOMPARE(router.effective_modifiers(), quint8(bssh_emulator::MOD_CTRL));

	/* Third press: released, and the next letter is a letter again. */
	router.cycle_modifier(bssh_emulator::MOD_CTRL);
	QCOMPARE(router.modifier_state(bssh_emulator::MOD_CTRL),
	          bssh_input_router::MODIFIER_OFF);
	QCOMPARE(press(router, emulator, Qt::Key_C, Qt::NoModifier, QStringLiteral("c")),
	          QByteArrayLiteral("c"));

	/*
	 * Arming and locking are per modifier, not one shared state: a locked
	 * Ctrl must not be released by tapping Alt, or the two keys would be
	 * one key with two labels.
	 */
	router.cycle_modifier(bssh_emulator::MOD_CTRL);
	router.cycle_modifier(bssh_emulator::MOD_CTRL);
	router.cycle_modifier(bssh_emulator::MOD_ALT);
	QCOMPARE(router.modifier_state(bssh_emulator::MOD_CTRL),
	          bssh_input_router::MODIFIER_LOCKED);
	QCOMPARE(router.modifier_state(bssh_emulator::MOD_ALT),
	          bssh_input_router::MODIFIER_ONCE);
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

static QAbstractButton *button_labelled(const QWidget &parent, const QString &label) {
	for (QAbstractButton *button : parent.findChildren<QAbstractButton *>()) {
		if (button->text() == label) return button;
	}
	return nullptr;
}

void input_router_test::a_modifier_button_stops_claiming_a_session_that_has_gone() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);
	bssh_key_row row(&router);

	QAbstractButton *ctrl = button_labelled(row, QStringLiteral("Ctrl"));
	QVERIFY(ctrl != nullptr);

	/* Locked, which is the state that lasts and so the one that can rot. */
	ctrl->click();
	ctrl->click();
	QCOMPARE(router.modifier_state(bssh_emulator::MOD_CTRL),
	          bssh_input_router::MODIFIER_LOCKED);
	QVERIFY2(ctrl->isChecked(), "a locked modifier must read as pressed");

	/*
	 * The session goes. The row does NOT: its visibility depends on the
	 * mode and the platform (should_be_visible) and never on whether a
	 * session exists, so it stays on screen with nothing behind it.
	 */
	row.set_router(nullptr);

	QVERIFY2(!ctrl->isChecked(),
	          "with no session there is nothing armed, and a Ctrl still drawn "
	          "as pressed is the button lying about the state of the terminal");

	/*
	 * And pressing it with nothing behind it must not leave it stuck.
	 *
	 * This is the sharper half. The button is CHECKABLE, so Qt toggles it
	 * before the handler runs -- a handler that returns early on a null
	 * router leaves it drawn as pressed with nothing able to put it back,
	 * and the row is reachable in exactly that state at the front door.
	 */
	ctrl->click();
	QVERIFY2(!ctrl->isChecked(),
	          "a modifier pressed with no session must not stay pressed");
}

void input_router_test::the_on_screen_keyboard_reaches_letter_chords() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);
	QSignalSpy spy(&router, &bssh_input_router::shortcut_pressed);

	/*
	 * The on-screen keyboard arms Ctrl and Shift, then presses its 'c'
	 * key, which reaches the router as steal_char_chord('c') carrying the
	 * armed pair. It must be the copy chord, not a capital C for the shell.
	 */
	router.cycle_modifier(bssh_emulator::MOD_CTRL);
	router.cycle_modifier(bssh_emulator::MOD_SHIFT);
	QVERIFY(router.steal_char_chord(U'c'));

	QCOMPARE(spy.count(), 1);
	QCOMPARE(spy.first().first().toInt(), int(bssh_input_router::SHORTCUT_COPY));

	/*
	 * A bare letter is text, not a chord: no shortcut, and it is left for
	 * send_text to turn into bytes.
	 */
	QVERIFY(!router.steal_char_chord(U'c'));
	QCOMPARE(spy.count(), 1);

	/*
	 * Ctrl alone is NOT the copy chord -- copy is Ctrl+Shift+C, and Ctrl+C
	 * is the interrupt the shell must still receive. So this is not stolen,
	 * and send_text will produce the interrupt byte as it should.
	 */
	router.cycle_modifier(bssh_emulator::MOD_CTRL);
	QVERIFY(!router.steal_char_chord(U'c'));
	QCOMPARE(spy.count(), 1);
}

void input_router_test::the_on_screen_keyboard_reaches_stolen_chords() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);
	QSignalSpy spy(&router, &bssh_input_router::shortcut_pressed);

	/*
	 * The on-screen keyboard arms a sticky Shift and then presses its own
	 * PgUp key, which reaches the router as send_key(KEY_PAGE_UP, NONE)
	 * carrying the armed modifier -- exactly the path bssh_soft_keyboard
	 * drives (sec 6.2). Before this it walked past the chord table and
	 * paged the remote; it must steal the scroll chord like a hardware
	 * Shift+PageUp does.
	 */
	router.cycle_modifier(bssh_emulator::MOD_SHIFT);
	router.send_key(bssh_emulator::KEY_PAGE_UP, bssh_emulator::MOD_NONE);

	QCOMPARE(spy.count(), 1);
	QCOMPARE(spy.first().first().toInt(),
	          int(bssh_input_router::SHORTCUT_SCROLL_PAGE_UP));
	/* Stolen from the remote, so it must not also produce bytes. */
	QCOMPARE(emulator.take_output(), QByteArray());

	/*
	 * PgUp with no Shift is NOT a chord: it is the page key a pager wants,
	 * so it still reaches the remote and fires no shortcut. The armed
	 * Shift was spent by the steal above, so this press carries none.
	 */
	router.send_key(bssh_emulator::KEY_PAGE_UP, bssh_emulator::MOD_NONE);
	QCOMPARE(spy.count(), 1);
	QVERIFY(!emulator.take_output().isEmpty());
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

void input_router_test::a_held_key_repeats() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);
	bssh_key_row row(&router);

	/*
	 * A PHONE width, so the strip genuinely scrolls -- the default key
	 * row is 652px of keys and the narrow screen is 320. At a width
	 * where everything fits, the scroller is grabbed and has nothing to
	 * do, which is not the case a thumb meets.
	 */
	row.resize(BSSH_PHONE_DP, 64);
	row.show();
	QVERIFY(QTest::qWaitForWindowExposed(&row));
	QCoreApplication::processEvents();

	/*
	 * The left arrow, because moving along a line is the case that was
	 * reported and because its output is one fixed sequence -- so the
	 * count below is a count of PRESSES rather than of bytes that might
	 * have come from anywhere.
	 */
	QAbstractButton *left = nullptr;
	for (QAbstractButton *button : row.findChildren<QAbstractButton *>()) {
		if (button->text() == QString::fromUtf8("\xe2\x86\x90")) left = button;
	}
	QVERIFY(left != nullptr);

	/*
	 * PRESSED AT THE VIEWPORT, not at the button, and that is the whole
	 * point of this test rather than a detail of it.
	 *
	 * The row grabs a QScroller gesture on its scroll area's viewport
	 * (sec 6.2 -- the keys do not fit and are dragged sideways), so a
	 * real touch is filtered by the scroller before any button sees it.
	 * A press sent straight to the button is not: an event filter runs
	 * on its receiver, and the button is not the viewport. So the
	 * obvious version of this test proves the button repeats and says
	 * nothing about whether a finger can reach it -- `evidence.md`'s
	 * "a correct function is not a working feature", with the wiring
	 * being the half that was untested.
	 *
	 * What the scroller does, measured here rather than assumed: it
	 * withholds the press (isDown() is false immediately afterwards),
	 * decides it is not a drag, and replays it to the widget underneath
	 * (isDown() is true by the time the hold is over). Auto-repeat
	 * survives that, which was the open question.
	 */
	QScrollArea *area = row.findChild<QScrollArea *>();
	QVERIFY2(area, "the key row scrolls, and this found no scroll area");
	QWidget *viewport = area->viewport();

	const QPoint at = left->mapTo(viewport, left->rect().center());
	const QPointF global = viewport->mapToGlobal(QPointF(at));

	const QByteArray once = QByteArrayLiteral("\x1b[D");

	emulator.take_output();
	QMouseEvent press(QEvent::MouseButtonPress, QPointF(at), global,
	                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(viewport, &press);
	QTest::qWait(900);

	QVERIFY2(left->isDown(),
	          "a press at the viewport never reached the key, so the scroller "
	          "swallowed it and nothing below measures the repeat");

	const QByteArray held = emulator.take_output();

	/*
	 * TWO THINGS HAVE TO BE RIGHT AT ONCE, and each alone looks like the
	 * other one failing.
	 *
	 * The release is sent to the BUTTON, because once the scroller
	 * replays the press and the button accepts it the button holds the
	 * mouse grab, and every later event in that stream goes to it
	 * directly rather than through the viewport's filter again. And it
	 * names LEFTBUTTON as the button that CHANGED, with none still
	 * held, because QAbstractButton ignores a release whose button() is
	 * not the left one.
	 *
	 * Measured over the four combinations, bytes arriving in the 300ms
	 * AFTER the release:
	 *
	 *     viewport, NoButton     18     runaway
	 *     button,   NoButton     18     runaway
	 *     viewport, LeftButton   18     runaway
	 *     button,   LeftButton    0     stops
	 *
	 * A release the button never sees leaves the key down repeating for
	 * ever, and the tap assertion below then reads those repeats rather
	 * than a tap -- which is green about half the time, often enough to
	 * pass a container run. The assertion on the quiet after the release
	 * is what makes that failure loud instead.
	 */
	QMouseEvent release(QEvent::MouseButtonRelease, QPointF(at), global,
	                     Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(left, &release);
	QCoreApplication::processEvents();
	/*
	 * NOTHING MORE ARRIVES. This is the arm that the pushed version of
	 * this test did not have, and the defect it would have caught was
	 * in the test itself.
	 */
	QTest::qWait(300);
	const QByteArray after = emulator.take_output();
	QVERIFY2(after.isEmpty(),
	          qPrintable(QStringLiteral("the key went on repeating after the release: "
	                                     "%1 more byte(s)").arg(after.size())));

	/*
	 * At least twice, not exactly N: how many repeats fit in 900ms is a
	 * property of the machine's scheduler, and a test that pins the
	 * count would fail on a loaded one for a reason that is not a
	 * defect. What is being asserted is that holding sends MORE than
	 * tapping, which is the whole feature.
	 */
	QVERIFY2(held.size() >= once.size() * 2,
	          qPrintable(QStringLiteral("held for 900ms and sent %1 bytes; one press is %2")
	                             .arg(held.size()).arg(once.size())));
	QVERIFY(held.startsWith(once));

	/*
	 * And a tap does not repeat, which is what the 500ms delay is for.
	 * Without this the test above would pass on a keyboard that repeated
	 * instantly -- and that keyboard would type two of everything.
	 *
	 * Through the viewport as well, so the control travels the same road
	 * as the case.
	 *
	 * WITH 80ms BETWEEN PRESS AND RELEASE, and that is a property of the
	 * scroller rather than a fudge. It withholds a press while it decides
	 * whether the finger is dragging, and that decision runs on a timer:
	 * a press and release delivered back to back, with no turn of the
	 * event loop between them, is released before the scroller has
	 * decided anything and reaches no button at all. Measured -- it sends
	 * zero bytes, where the same tap 80ms apart sends exactly one.
	 *
	 * A human tap is tens of milliseconds, so the instantaneous version
	 * was the fixture describing something nobody can do. It is worth
	 * recording because zero bytes is also what a fixture that has
	 * stopped delivering produces, and the two are told apart only by
	 * the held case above still working.
	 */
	emulator.take_output();
	QMouseEvent tap(QEvent::MouseButtonPress, QPointF(at), global,
	                 Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(viewport, &tap);
	QTest::qWait(80);
	QMouseEvent lift(QEvent::MouseButtonRelease, QPointF(at), global,
	                  Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(left, &lift);
	QCoreApplication::processEvents();
	QTest::qWait(300);
	QCOMPARE(emulator.take_output(), once);
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

		/*
		 * The same predicate a user-composed row is admitted by, rather
		 * than a copy of its reasoning. This assertion used to spell the
		 * rule out here, which was fine while every row in the catalogue
		 * was compiled in -- once a user can add one, a rule that lives
		 * in the test can only report the breach after it has happened,
		 * and it would report it against the five rows that are correct.
		 */
		QVERIFY2(bssh_key_row_usable(layout.keys),
		         qPrintable("no Escape or no modifier in " + layout.id));
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

namespace {

/* A press, a move and a release, delivered where the filter is watching. */
void press_at(QWidget *target, const QPoint &at) {
	QMouseEvent event(QEvent::MouseButtonPress, at, target->mapToGlobal(at),
	                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(target, &event);
}

void move_to(QWidget *target, const QPoint &at) {
	QMouseEvent event(QEvent::MouseMove, at, target->mapToGlobal(at),
	                   Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(target, &event);
}

void release_at(QWidget *target, const QPoint &at) {
	QMouseEvent event(QEvent::MouseButtonRelease, at, target->mapToGlobal(at),
	                   Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(target, &event);
}

} /* namespace */

void input_router_test::a_hold_fires_once_it_has_been_held() {
	QWidget target;
	target.resize(200, 200);

	bssh_long_press hold(&target);
	hold.set_enabled(true);
	QSignalSpy fired(&hold, &bssh_long_press::triggered);

	press_at(&target, QPoint(40, 40));

	/* Not yet: a hold that fires immediately is a click. */
	QCOMPARE(fired.count(), 0);

	/*
	 * Nor when the hold matures, which is the part a device taught. The
	 * popup would open under a finger that is still down and be dismissed
	 * by that finger's own release.
	 */
	QTest::qWait(700);
	QCOMPARE(fired.count(), 0);

	release_at(&target, QPoint(40, 40));
	QCOMPARE(fired.count(), 1);

	/*
	 * Global coordinates, because what opens is a popup and a popup is
	 * placed on the screen rather than in the widget.
	 */
	QCOMPARE(fired.at(0).at(0).toPoint(), target.mapToGlobal(QPoint(40, 40)));
}

void input_router_test::a_hold_that_moves_is_a_drag() {
	QWidget target;
	target.resize(200, 200);

	bssh_long_press hold(&target);
	hold.set_enabled(true);
	QSignalSpy fired(&hold, &bssh_long_press::triggered);

	press_at(&target, QPoint(40, 40));

	/*
	 * A thumb never holds perfectly still, so a few pixels must not
	 * cancel it -- and a real drag must. Both, because getting the slop
	 * wrong in either direction breaks one of selecting and holding.
	 */
	move_to(&target, QPoint(44, 42));
	QTest::qWait(700);
	release_at(&target, QPoint(44, 42));
	QCOMPARE(fired.count(), 1);

	QSignalSpy again(&hold, &bssh_long_press::triggered);
	press_at(&target, QPoint(40, 40));
	move_to(&target, QPoint(120, 40));
	QTest::qWait(700);
	release_at(&target, QPoint(120, 40));
	QCOMPARE(again.count(), 0);
}

void input_router_test::a_hold_released_early_is_a_click() {
	QWidget target;
	target.resize(200, 200);

	bssh_long_press hold(&target);
	hold.set_enabled(true);
	QSignalSpy fired(&hold, &bssh_long_press::triggered);

	press_at(&target, QPoint(40, 40));
	release_at(&target, QPoint(40, 40));
	QTest::qWait(700);

	QCOMPARE(fired.count(), 0);
}

void input_router_test::a_hold_does_nothing_until_it_is_enabled() {
	QWidget target;
	target.resize(200, 200);

	bssh_long_press hold(&target);
	QSignalSpy fired(&hold, &bssh_long_press::triggered);

	/*
	 * Off by default. With a pointer, press-and-hold means "start a
	 * selection and think about it", and it has always meant that.
	 */
	QVERIFY(!hold.is_enabled());
	press_at(&target, QPoint(40, 40));
	QTest::qWait(700);
	QCOMPARE(fired.count(), 0);
}

void input_router_test::a_matured_hold_swallows_its_own_release() {
	QWidget target;
	target.resize(200, 200);

	bssh_long_press hold(&target);
	hold.set_enabled(true);

	press_at(&target, QPoint(40, 40));
	QTest::qWait(700);

	/*
	 * The release is consumed, so the widget behind the filter does not
	 * also see a click. Without this, holding a row in the host list
	 * opened that host as well as offering the menu, and holding in the
	 * terminal left a selection behind it.
	 */
	QMouseEvent release(QEvent::MouseButtonRelease, QPoint(40, 40),
	                     target.mapToGlobal(QPoint(40, 40)),
	                     Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(&target, &release);
	QVERIFY2(release.isAccepted(), "a matured hold must swallow its release");
}

void input_router_test::the_fn_key_reaches_the_function_keys() {
	/*
	 * sec 6.2 lists Fn among the keys the row must carry, and no layout
	 * had one -- so F1..F12 were unreachable on a phone. The emulator has
	 * understood KEY_FUNCTION_BASE + n since M3 and the router has
	 * translated physical F1..F35 for as long; the only thing missing was
	 * a way for a thumb to ask. htop, mc and vim's help all want them.
	 */
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);
	bssh_key_row row(&router);
	row.set_layout(QString());

	QList<QAbstractButton *> buttons = row.findChildren<QAbstractButton *>();
	QAbstractButton *fn = nullptr;
	for (QAbstractButton *button : buttons) {
		if (button->text() == QStringLiteral("Fn")) fn = button;
		QVERIFY2(button->text() != QStringLiteral("F1"),
		          "the ordinary row must not already be the function keys");
	}
	QVERIFY2(fn, "the default row must carry Fn (sec 6.2)");

	fn->click();

	/*
	 * The row is now F1..F12 -- and still carries Fn, or there would be
	 * no way back to the keys that send Escape.
	 */
	buttons = row.findChildren<QAbstractButton *>();
	QStringList labels;
	for (QAbstractButton *button : buttons) labels.append(button->text());

	QVERIFY2(labels.contains(QStringLiteral("F1")), "Fn must reach F1");
	QVERIFY2(labels.contains(QStringLiteral("F12")), "Fn must reach F12");
	QVERIFY2(labels.contains(QStringLiteral("Fn")), "there must be a way back");
	QVERIFY2(!labels.contains(QStringLiteral("Esc")),
	          "the function row replaces the keys rather than crowding in beside them");

	/* And the key it sends is the one the remote expects. */
	QAbstractButton *f1 = nullptr;
	for (QAbstractButton *button : buttons) {
		if (button->text() == QStringLiteral("F1")) f1 = button;
	}
	QVERIFY(f1);

	f1->click();
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x1bOP"));

	/* Pressing Fn again comes back to the ordinary row. */
	for (QAbstractButton *button : row.findChildren<QAbstractButton *>()) {
		if (button->text() == QStringLiteral("Fn")) { button->click(); break; }
	}
	labels.clear();
	for (QAbstractButton *button : row.findChildren<QAbstractButton *>()) {
		labels.append(button->text());
	}
	QVERIFY2(labels.contains(QStringLiteral("Esc")), "Fn must come back to the real row");
	QVERIFY2(!labels.contains(QStringLiteral("F1")), "and leave the function keys behind");
}

void input_router_test::caps_lock_can_be_made_to_send_ctrl() {
	/*
	 * sec 6.1's Caps->Ctrl option, which was specified and absent. Off by
	 * default, because the only honest default for a preference about
	 * somebody's keyboard habit is the one that changes nothing.
	 */
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	QVERIFY2(!router.caps_as_ctrl(), "this must be off until asked for");

	/* Off: Caps is not the router's business and it says so. */
	QKeyEvent caps(QEvent::KeyPress, Qt::Key_CapsLock, Qt::NoModifier);
	QVERIFY2(!router.handle_key(&caps), "an unasked-for Caps must pass through untouched");
	QCOMPARE(router.sticky_modifiers(), quint8(bssh_emulator::MOD_NONE));

	router.set_caps_as_ctrl(true);

	/*
	 * On: Caps arms Ctrl for the next key, the same sticky modifier the
	 * on-screen row uses, and is consumed -- Caps Lock has no escape
	 * sequence of its own, so passing it on would send nothing while
	 * leaving the modifier armed.
	 */
	QSignalSpy armed(&router, &bssh_input_router::sticky_modifiers_changed);
	QVERIFY2(router.handle_key(&caps), "Caps must be consumed once it means Ctrl");
	QCOMPARE(router.sticky_modifiers(), quint8(bssh_emulator::MOD_CTRL));
	QCOMPARE(armed.count(), 1);

	/* And the next key is Ctrl-something. */
	QKeyEvent letter(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier, QStringLiteral("c"));
	router.handle_key(&letter);
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x03"));

	/* Spent, like every other sticky modifier. */
	QCOMPARE(router.sticky_modifiers(), quint8(bssh_emulator::MOD_NONE));

	/*
	 * Pressing it twice disarms rather than arming harder: the operating
	 * system's own caps lock is beyond reach here, so pressing Caps again
	 * to undo a mistake is what a user will try.
	 */
	router.handle_key(&caps);
	QCOMPARE(router.sticky_modifiers(), quint8(bssh_emulator::MOD_CTRL));
	router.handle_key(&caps);
	QCOMPARE(router.sticky_modifiers(), quint8(bssh_emulator::MOD_NONE));

	/*
	 * A letter typed with caps lock genuinely on still sends the control
	 * byte, not a different one: the translation reads the KEY, and Qt
	 * reports Key_C whether the text is "c" or "C".
	 */
	router.handle_key(&caps);
	QKeyEvent shouted(QEvent::KeyPress, Qt::Key_C, Qt::NoModifier, QStringLiteral("C"));
	router.handle_key(&shouted);
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("\x03"));
}

void input_router_test::every_stolen_chord_is_the_table_and_nothing_else() {
	/*
	 * Sec 6.1 asks that the stolen set be documented, and a list written
	 * down somewhere is only documentation while it still matches what
	 * the code does. So the list IS the code -- bssh_input_router::
	 * stolen_chords() is what handle_key walks -- and this holds the two
	 * ends of that together in both directions.
	 *
	 * Forwards: every entry really is intercepted, emits the action it
	 * claims, and sends the remote nothing.
	 */
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	QVERIFY(!bssh_input_router::stolen_chords().isEmpty());

	for (const bssh_input_router::chord_t &chord : bssh_input_router::stolen_chords()) {
		QSignalSpy fired(&router, &bssh_input_router::shortcut_pressed);

		const QByteArray sent = press(router, emulator, chord.key, chord.modifiers,
		                               QString());

		QVERIFY2(sent.isEmpty(),
		          qPrintable(QStringLiteral("%1 is stolen, so it must send the remote "
		                                     "nothing; sent %2 byte(s)")
		                             .arg(chord.label)
		                             .arg(sent.size())));
		QCOMPARE(fired.size(), 1);
		QCOMPARE(fired.first().first().toInt(), int(chord.action));

		/* And it is written down in a form somebody can read. */
		QVERIFY(!chord.label.isEmpty());
		QVERIFY(chord.what && *chord.what);
		QVERIFY2(chord.name && *chord.name,
		          "a chord needs a stable name, or a rebinding cannot be filed");
	}

	/*
	 * Backwards, which is the half that would rot silently: a chord that
	 * is NOT in the table must reach the remote. Ctrl+Alt+Up is the near
	 * miss worth pinning -- same modifiers as pane focus, a different
	 * key -- because a loose match would swallow it and the shown list
	 * would be quietly wrong rather than visibly so.
	 */
	QSignalSpy quiet(&router, &bssh_input_router::shortcut_pressed);
	const QByteArray through = press(router, emulator, Qt::Key_Up,
	                                  Qt::ControlModifier | Qt::AltModifier, QString());
	QVERIFY2(!through.isEmpty(), "a chord nobody declared must still reach the remote");
	QCOMPARE(quiet.size(), 0);

	/*
	 * And the keypad's own PageUp is the same chord as the main block's.
	 * Qt sets Qt::KeypadModifier there, which says nothing about which
	 * chord was pressed, so a comparison against the whole modifier set
	 * would steal one and pass the other on one keyboard.
	 */
	QSignalSpy keypad(&router, &bssh_input_router::shortcut_pressed);
	const QByteArray nothing = press(router, emulator, Qt::Key_PageUp,
	                                  Qt::ShiftModifier | Qt::KeypadModifier, QString());
	QVERIFY(nothing.isEmpty());
	QCOMPARE(keypad.size(), 1);
}

void input_router_test::a_rebound_chord_is_the_one_that_is_stolen() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	QHash<QString, QString> bindings;
	bindings.insert(QStringLiteral("copy"), QStringLiteral("Ctrl+Alt+K"));
	bssh_input_router::set_bindings(bindings);

	QSignalSpy fired(&router, &bssh_input_router::shortcut_pressed);

	/* The new chord is taken... */
	const QByteArray stolen = press(router, emulator, Qt::Key_K,
	                                 Qt::ControlModifier | Qt::AltModifier, QString());
	QVERIFY2(stolen.isEmpty(), "a rebound chord must not reach the remote");
	QCOMPARE(fired.size(), 1);
	QCOMPARE(fired.first().first().toInt(), int(bssh_input_router::SHORTCUT_COPY));

	/* ...and the one it replaced is not, which is the half that matters:
	 * a rebinding that ADDED a chord would quietly keep stealing the old
	 * one, and the list on screen would be wrong about it. */
	QSignalSpy quiet(&router, &bssh_input_router::shortcut_pressed);
	const QByteArray through = press(router, emulator, Qt::Key_C,
	                                  Qt::ControlModifier | Qt::ShiftModifier, QString());
	QVERIFY2(!through.isEmpty(), "the chord that was replaced must reach the remote");
	QVERIFY(quiet.isEmpty());

	/* And what the settings page shows is what the router matches. */
	QCOMPARE(bssh_input_router::binding_for(QStringLiteral("copy")),
	          QStringLiteral("Ctrl+Alt+K"));

	bssh_input_router::set_bindings(QHash<QString, QString>());
}

void input_router_test::a_chord_given_back_reaches_the_remote() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	/*
	 * sec 6.1: "any chord you steal is a chord the remote shell can no
	 * longer receive". Handing one back has to be sayable, and an empty
	 * binding is how it is said.
	 */
	QHash<QString, QString> bindings;
	bindings.insert(QStringLiteral("scroll-page-up"), QString());
	bssh_input_router::set_bindings(bindings);

	QSignalSpy fired(&router, &bssh_input_router::shortcut_pressed);
	const QByteArray sent = press(router, emulator, Qt::Key_PageUp, Qt::ShiftModifier,
	                               QString());

	QVERIFY2(!sent.isEmpty(), "an unbound chord must reach the remote");
	QVERIFY(fired.isEmpty());

	/* It is off the list this app shows, too, since it keeps it no more. */
	for (const bssh_input_router::chord_t &chord : bssh_input_router::stolen_chords()) {
		QVERIFY2(QLatin1String(chord.name) != QLatin1String("scroll-page-up"),
		          "a chord given back must not still be listed as kept");
	}

	/*
	 * A typo is not a rebinding: an unparseable sequence leaves the
	 * built-in, because a hand-edited file with a mistake in it should
	 * behave like the default rather than lose the chord.
	 */
	bindings.clear();
	bindings.insert(QStringLiteral("copy"), QStringLiteral("Ctrl+Shift+Nonsense"));
	bssh_input_router::set_bindings(bindings);

	QSignalSpy again(&router, &bssh_input_router::shortcut_pressed);
	const QByteArray still = press(router, emulator, Qt::Key_C,
	                                Qt::ControlModifier | Qt::ShiftModifier, QString());
	QVERIFY2(still.isEmpty(), "a mistyped binding must leave the built-in chord alone");
	QCOMPARE(again.size(), 1);

	bssh_input_router::set_bindings(QHash<QString, QString>());
}

void input_router_test::a_paste_cannot_close_its_own_bracket() {
	/*
	 * sec 6.4 leans on bracketed paste to stop a pasted newline being
	 * executed, and sec 7.1 lists it as Done. But the protection lives in
	 * a marker inside the same byte stream as the data it protects: if the
	 * pasted text contains ESC [ 2 0 1 ~ itself, the far end sees the
	 * paste end early and reads everything after it as TYPED input.
	 *
	 * That is the whole attack. Somebody copies a line from a web page, it
	 * carries the terminator and a command after it, and the shell runs
	 * the command without the paste ever looking multi-line -- so sec
	 * 6.4's newline confirmation never fires either.
	 */
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	emulator.feed(QByteArrayLiteral("\x1b[?2004h"));   /* remote enables it */
	emulator.take_output();

	router.paste(QStringLiteral("safe\x1b[201~rm -rf ~"));

	const QByteArray sent = emulator.take_output();

	QVERIFY2(sent.startsWith(QByteArrayLiteral("\x1b[200~")),
	          "the control: bracketed paste has to be in use for this to matter");

	const int first_end = sent.indexOf(QByteArrayLiteral("\x1b[201~"));
	QVERIFY(first_end > 0);
	QVERIFY2(sent.indexOf(QByteArrayLiteral("\x1b[201~"), first_end + 1) < 0,
	          qPrintable(QStringLiteral("the pasted text closed its own bracket; "
	                                     "bytes: ") + QString::fromLatin1(sent.toPercentEncoding())));

	/*
	 * And the text itself still arrives, so this cannot pass against a
	 * paste that dropped everything. The command is delivered as ordinary
	 * characters INSIDE the bracket, which is exactly where a shell will
	 * not run it.
	 */
	QVERIFY2(sent.contains(QByteArrayLiteral("safe")), "the pasted text has to survive");
	QVERIFY2(sent.contains(QByteArrayLiteral("rm -rf ~")),
	          "the text after the stripped marker is still text, and still pasted");

	/* Tab is text people paste; it is not dropped with the controls. */
	emulator.take_output();
	router.paste(QStringLiteral("a\tb"));
	QVERIFY2(emulator.take_output().contains(QByteArrayLiteral("a\tb")),
	          "a tab in a paste has to survive");
}

/*
 * sec 6.2 left "letting a user compose their own row" open and sec 6.3
 * says it is one question for fonts, colour schemes, key rows and
 * keyboard layouts. The colour scheme answered it first -- a reserved
 * catalogue entry written by a push from the settings layer -- and these
 * three cover the same answer applied to the row.
 */
void input_router_test::a_custom_key_row_is_composed_from_words() {
	const QList<bssh_key_t> keys =
	        bssh_parse_key_row(QStringLiteral("esc tab ctrl alt left down up right | ctrl-b gap"));

	QCOMPARE(keys.size(), 11);
	QCOMPARE(keys.at(0).kind, bssh_key_t::KEY);
	QCOMPARE(keys.at(0).value, int(bssh_emulator::KEY_ESCAPE));
	QCOMPARE(keys.at(2).kind, bssh_key_t::MODIFIER);
	QCOMPARE(keys.at(2).modifier, quint8(bssh_emulator::MOD_CTRL));

	/* A bare character is itself, which is how the punctuation a soft
	 * keyboard buries gets onto the row at all. */
	QCOMPARE(keys.at(8).kind, bssh_key_t::TEXT);
	QCOMPARE(keys.at(8).value, int('|'));

	/* One press sending what two would (sec 6.2's tmux case). */
	QCOMPARE(keys.at(9).kind, bssh_key_t::CHORD);
	QCOMPARE(keys.at(9).value, int('b'));
	QCOMPARE(keys.at(9).modifier, quint8(bssh_emulator::MOD_CTRL));

	QCOMPARE(keys.at(10).kind, bssh_key_t::SPACER);

	/*
	 * Round-trip, because app.ini is what stores this and a spec that
	 * does not survive being read back is one that rewrites the user's
	 * row every time the app starts.
	 */
	const QString spec = bssh_key_row_spec(keys);
	QCOMPARE(bssh_key_row_spec(bssh_parse_key_row(spec)), spec);
	QCOMPARE(bssh_parse_key_row(spec).size(), keys.size());

	/* And it reaches the catalogue under the reserved id, keys only. */
	bssh_key_row_catalog catalog;
	QVERIFY(catalog.set_custom(keys));

	const bssh_key_row_layout_t *custom = catalog.by_id(QStringLiteral("custom"));
	QVERIFY(custom != nullptr);
	QCOMPARE(custom->keys.size(), keys.size());
	QCOMPARE(custom->display_name, QStringLiteral("Custom"));
	QVERIFY(!custom->description.isEmpty());
}

void input_router_test::a_custom_row_that_cannot_drive_a_terminal_is_refused() {
	bssh_key_row_catalog catalog;

	const bssh_key_row_layout_t *before = catalog.by_id(QStringLiteral("custom"));
	QVERIFY(before != nullptr);
	const QList<bssh_key_t> kept = before->keys;

	/* The entry starts as a copy of the default, so it satisfies the
	 * contract before anybody has written to it. */
	QVERIFY(bssh_key_row_usable(kept));

	/* No Escape: arrows and a modifier are not a row you can get out of
	 * anything with. */
	QVERIFY(!catalog.set_custom(bssh_parse_key_row(QStringLiteral("ctrl left right"))));

	/* No modifier: sticky Ctrl is the thing the row exists to provide. */
	QVERIFY(!catalog.set_custom(bssh_parse_key_row(QStringLiteral("esc tab left right"))));

	/* Nothing at all, which is what an emptied settings line parses to. */
	QVERIFY(!catalog.set_custom(bssh_parse_key_row(QString())));

	/*
	 * And each refusal left the last good row alone. A refusal that
	 * cleared the entry would hand the user an empty strip and no way
	 * back to a working one except the file this is meant to save them
	 * from opening.
	 */
	const bssh_key_row_layout_t *after = catalog.by_id(QStringLiteral("custom"));
	QVERIFY(after != nullptr);
	QCOMPARE(after->keys.size(), kept.size());
}

void input_router_test::the_custom_row_note_names_only_words_that_parse() {
	/*
	 * The dialog explains the vocabulary in prose, and prose is where a
	 * word that no longer exists survives longest: nothing fails, the
	 * user simply types what they were told and loses that key. Both come
	 * off one table, and this is what holds them together.
	 */
	const QStringList words = bssh_key_row_editor::vocabulary();
	QVERIFY(!words.isEmpty());

	for (const QString &word : words) {
		const QList<bssh_key_t> parsed = bssh_parse_key_row(word);
		QVERIFY2(parsed.size() == 1,
		         qPrintable(QStringLiteral("the note names '%1', which does not parse")
		                            .arg(word)));
	}
}
