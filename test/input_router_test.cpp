#include <QAbstractButton>
#include <QKeyEvent>
#include <QSignalSpy>
#include <QtTest>

#include "input/input_router.h"
#include "ui/key_row_layout.h"
#include "ui/long_press.h"
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
