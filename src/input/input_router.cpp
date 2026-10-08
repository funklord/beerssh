#include "input/input_router.h"

#include <QKeyCombination>
#include <QKeySequence>

#include <QKeyEvent>

#include <functional>

#include "term/emulator.h"

namespace {

/*
 * A Qt key to the emulator key that carries it, or KEY_NONE for one the
 * emulator has no named code for -- a letter, whose bytes come from
 * event->text() rather than from a code.
 *
 * Shared between handle_key(), which had this switch inline, and
 * steal_chord(): the chord table is written in Qt keys and the on-screen
 * keyboard sends emulator keys, so matching one against the other needs
 * exactly this translation. Factored rather than copied because the
 * stolen-chord list and the list the router acts on cannot be two lists
 * (sec 6.1), and a second copy of the mapping is a second way to disagree.
 */
int emulator_key_for(int qt_key) {
	switch (qt_key) {
	case Qt::Key_Return:
	case Qt::Key_Enter:
		return bssh_emulator::KEY_ENTER;
	case Qt::Key_Tab:
	case Qt::Key_Backtab:
		return bssh_emulator::KEY_TAB;
	case Qt::Key_Backspace:
		return bssh_emulator::KEY_BACKSPACE;
	case Qt::Key_Escape:
		return bssh_emulator::KEY_ESCAPE;
	case Qt::Key_Up:
		return bssh_emulator::KEY_UP;
	case Qt::Key_Down:
		return bssh_emulator::KEY_DOWN;
	case Qt::Key_Left:
		return bssh_emulator::KEY_LEFT;
	case Qt::Key_Right:
		return bssh_emulator::KEY_RIGHT;
	case Qt::Key_Insert:
		return bssh_emulator::KEY_INSERT;
	case Qt::Key_Delete:
		return bssh_emulator::KEY_DELETE;
	case Qt::Key_Home:
		return bssh_emulator::KEY_HOME;
	case Qt::Key_End:
		return bssh_emulator::KEY_END;
	case Qt::Key_PageUp:
		return bssh_emulator::KEY_PAGE_UP;
	case Qt::Key_PageDown:
		return bssh_emulator::KEY_PAGE_DOWN;
	default:
		break;
	}

	if (qt_key >= Qt::Key_F1 && qt_key <= Qt::Key_F35)
		return bssh_emulator::KEY_FUNCTION_BASE + 1 + (qt_key - Qt::Key_F1);

	return bssh_emulator::KEY_NONE;
}

/* The three modifiers the chord table names, in the emulator's bits. */
quint8 emulator_mods_for(Qt::KeyboardModifiers mods) {
	quint8 out = bssh_emulator::MOD_NONE;
	if (mods & Qt::ShiftModifier) out |= bssh_emulator::MOD_SHIFT;
	if (mods & Qt::ControlModifier) out |= bssh_emulator::MOD_CTRL;
	if (mods & Qt::AltModifier) out |= bssh_emulator::MOD_ALT;

	return out;
}

/* And back: the emulator's three in Qt's, for matching a letter's remap. */
Qt::KeyboardModifiers qt_modifiers_for(quint8 mods) {
	Qt::KeyboardModifiers out = Qt::NoModifier;
	if (mods & bssh_emulator::MOD_SHIFT) out |= Qt::ShiftModifier;
	if (mods & bssh_emulator::MOD_CTRL) out |= Qt::ControlModifier;
	if (mods & bssh_emulator::MOD_ALT) out |= Qt::AltModifier;

	return out;
}

} /* namespace */

bssh_input_router::bssh_input_router(bssh_emulator *emulator, QObject *parent)
    : QObject(parent),
      m_emulator(emulator),
      m_caps_as_ctrl(false),
      m_sticky(bssh_emulator::MOD_NONE),
      m_locked(bssh_emulator::MOD_NONE),
      m_held(bssh_emulator::MOD_NONE),
      m_held_used(bssh_emulator::MOD_NONE) {
}

void bssh_input_router::hold_modifier(quint8 modifier) {
	m_held_used = quint8(m_held_used & ~modifier);
	if (m_held & modifier) return;
	m_held = quint8(m_held | modifier);
	emit held_modifiers_changed(m_held);
}

bool bssh_input_router::release_modifier(quint8 modifier) {
	if (!(m_held & modifier)) return false;
	const bool tapped = !(m_held_used & modifier);
	m_held = quint8(m_held & ~modifier);
	m_held_used = quint8(m_held_used & ~modifier);
	emit held_modifiers_changed(m_held);
	return tapped;
}

void bssh_input_router::set_sticky_modifiers(quint8 modifiers) {
	if (m_sticky == modifiers) return;
	m_sticky = modifiers;
	emit sticky_modifiers_changed(m_sticky);
}

void bssh_input_router::set_locked_modifiers(quint8 modifiers) {
	if (m_locked == modifiers) return;
	m_locked = modifiers;
	emit locked_modifiers_changed(m_locked);
}

/*
 * A keystroke has just gone out carrying effective_modifiers(). The armed
 * half is spent; the locked half is not.
 *
 * One function rather than the clear-and-emit that was written out at
 * eight call sites. Every one of them was the same two lines, and the
 * eighth is the one that would eventually be a clear without an emit --
 * which is a Ctrl button left drawn as pressed over a terminal that has
 * already forgotten it.
 */
void bssh_input_router::spend_sticky_modifiers() {
	/*
	 * Every keystroke comes through here, so this is also where a held
	 * modifier learns it carried one -- which is what makes its release
	 * a chord ending rather than a tap.
	 */
	m_held_used = quint8(m_held_used | m_held);
	set_sticky_modifiers(bssh_emulator::MOD_NONE);
}

bssh_input_router::modifier_state_t
bssh_input_router::modifier_state(quint8 modifier) const {
	if (m_locked & modifier) return MODIFIER_LOCKED;
	if (m_held & modifier) return MODIFIER_HELD;
	if (m_sticky & modifier) return MODIFIER_ONCE;
	return MODIFIER_OFF;
}

/*
 * OFF -> ONCE -> LOCKED -> OFF, which is the order a Treo goes round and
 * the reason the header calls the press count the state.
 *
 * Locking from ARMED rather than from OFF is what makes the common case
 * cheap: one tap for the Ctrl-C everybody sends, and a second only when
 * you already know you are sending more. Going OFF -> LOCKED first would
 * make the rare case free and the common one dangerous, since a lock
 * nobody noticed turns every following letter into a control byte.
 */
void bssh_input_router::cycle_modifier(quint8 modifier) {
	/*
	 * Read from the two latched sets rather than from modifier_state(),
	 * which would answer HELD for a modifier somebody is also resting a
	 * finger on: the cycle is about what stays once the finger is gone.
	 */
	if (m_locked & modifier) {
		set_locked_modifiers(quint8(m_locked & ~modifier));
	} else if (m_sticky & modifier) {
		set_sticky_modifiers(quint8(m_sticky & ~modifier));
		set_locked_modifiers(quint8(m_locked | modifier));
	} else {
		set_sticky_modifiers(quint8(m_sticky | modifier));
	}
}

quint8 bssh_input_router::modifiers_for(QKeyEvent *event) const {
	/* Armed AND locked: what a key carries is both (see the header). */
	quint8 modifiers = effective_modifiers();

	if (event->modifiers() & Qt::ShiftModifier) modifiers |= bssh_emulator::MOD_SHIFT;
	if (event->modifiers() & Qt::ControlModifier) modifiers |= bssh_emulator::MOD_CTRL;
	if (event->modifiers() & Qt::AltModifier) modifiers |= bssh_emulator::MOD_ALT;

	/*
	 * Super, which only the kitty protocol can say; everything else masks
	 * it off (see MOD_SUPER).
	 */
	if (event->modifiers() & Qt::MetaModifier) modifiers |= bssh_emulator::MOD_SUPER;

	return modifiers;
}

/*
 * The stolen set, and the only place it is written down (sec 6.1).
 *
 * Minimal, and each entry earns its place against the same test: a chord
 * taken here is one the remote shell can never receive again.
 *
 * Ctrl+Shift+C/V rather than Ctrl+C/V, because Ctrl-C is SIGINT and a
 * terminal that swallows it to mean "copy" cannot interrupt a runaway
 * process. Shift+PageUp/PageDown is every terminal's convention, and
 * plain PageUp deliberately still goes through because a pager needs it.
 * Ctrl+Alt+Arrow for pane focus because Ctrl+Arrow and Alt+Arrow are both
 * in daily use by readline and by editors, so the pair together is the
 * cheapest chord available.
 */
namespace {

/* How a chord is written for a reader, from what it actually matches. */
QString chord_label(Qt::KeyboardModifiers modifiers, int key) {
	const QKeySequence sequence(QKeyCombination(modifiers, Qt::Key(key)));

	return sequence.toString(QKeySequence::NativeText);
}

/*
 * The bindings somebody has set, by action name. Empty for a fresh
 * process, which is every process until the window reads app.ini.
 */
QHash<QString, QString> user_bindings;

/*
 * The general key remaps (sec 6.1): a portable "from" key sequence to the
 * "to" one the shell should get instead. Process-wide like the bindings,
 * because it is a fact about the keyboard in front of the user rather than
 * about any one session, and empty until the window reads app.ini.
 */
QHash<QString, QString> user_remaps;

/* Rebuilt whenever the bindings change; see stolen_chords(). */
QList<bssh_input_router::chord_t> effective_chords;
bool effective_current = false;

} /* namespace */

const QList<bssh_input_router::chord_t> &bssh_input_router::default_chords() {
	static const QList<chord_t> chords = {
		{ SHORTCUT_COPY, Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_C,
			chord_label(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_C),
			"copy", "Copy the selection" },
		{ SHORTCUT_PASTE, Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_V,
			chord_label(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_V),
			"paste", "Paste" },
		{ SHORTCUT_SCROLL_PAGE_UP, Qt::ShiftModifier, Qt::Key_PageUp,
			chord_label(Qt::ShiftModifier, Qt::Key_PageUp),
			"scroll-page-up", "Scroll back a page" },
		{ SHORTCUT_SCROLL_PAGE_DOWN, Qt::ShiftModifier, Qt::Key_PageDown,
			chord_label(Qt::ShiftModifier, Qt::Key_PageDown),
			"scroll-page-down", "Scroll forward a page" },
		{ SHORTCUT_FOCUS_PANE_LEFT, Qt::ControlModifier | Qt::AltModifier, Qt::Key_Left,
			chord_label(Qt::ControlModifier | Qt::AltModifier, Qt::Key_Left),
			"focus-pane-left", "Focus the pane to the left" },
		{ SHORTCUT_FOCUS_PANE_RIGHT, Qt::ControlModifier | Qt::AltModifier, Qt::Key_Right,
			chord_label(Qt::ControlModifier | Qt::AltModifier, Qt::Key_Right),
			"focus-pane-right", "Focus the pane to the right" },
		{ SHORTCUT_NEW_TAB, Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_T,
			chord_label(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_T),
			"new-tab", "Open another session in a tab" },
		{ SHORTCUT_NEW_WINDOW, Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_N,
			chord_label(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_N),
			"new-window", "Open another session in a window" },
		{ SHORTCUT_FIND, Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_F,
			chord_label(Qt::ControlModifier | Qt::ShiftModifier, Qt::Key_F),
			"find", "Find in the scrollback" },
		{ SHORTCUT_ZOOM_IN, Qt::ControlModifier, Qt::Key_Equal,
			chord_label(Qt::ControlModifier, Qt::Key_Equal),
			"zoom-in", "Make this session's text bigger" },
		{ SHORTCUT_ZOOM_OUT, Qt::ControlModifier, Qt::Key_Minus,
			chord_label(Qt::ControlModifier, Qt::Key_Minus),
			"zoom-out", "Make this session's text smaller" },
		{ SHORTCUT_ZOOM_RESET, Qt::ControlModifier, Qt::Key_0,
			chord_label(Qt::ControlModifier, Qt::Key_0),
			"zoom-reset", "Back to the size this host resolves to" },
	};
	return chords;
}

void bssh_input_router::set_bindings(const QHash<QString, QString> &bindings) {
	user_bindings = bindings;
	effective_current = false;
}

void bssh_input_router::set_key_remaps(const QHash<QString, QString> &remaps) {
	user_remaps = remaps;
}

QString bssh_input_router::binding_for(const QString &action_name) {
	for (const chord_t &chord : stolen_chords()) {
		if (QLatin1String(chord.name) == action_name) return chord.label;
	}
	return QString();
}

const QList<bssh_input_router::chord_t> &bssh_input_router::stolen_chords() {
	if (effective_current) return effective_chords;

	effective_chords.clear();

	for (const chord_t &chord : default_chords()) {
		const QString bound = user_bindings.value(QString::fromLatin1(chord.name));

		if (!user_bindings.contains(QString::fromLatin1(chord.name))) {
			effective_chords.append(chord);
			continue;
		}

		/*
		 * Bound to nothing: the chord goes back to the remote shell.
		 * Dropped from the list rather than kept with a null key,
		 * because this list is both what handle_key matches against and
		 * what "Keys this app keeps" shows -- and a chord this app no
		 * longer keeps has no business in either.
		 */
		if (bound.trimmed().isEmpty()) continue;

		const QKeySequence sequence(bound, QKeySequence::PortableText);
		if (sequence.isEmpty()) {
			effective_chords.append(chord);
			continue;
		}

		const QKeyCombination combination = sequence[0];
		if (combination.key() == Qt::Key_unknown) {
			effective_chords.append(chord);
			continue;
		}

		chord_t rebound = chord;
		rebound.modifiers = combination.keyboardModifiers();
		rebound.key = combination.key();
		rebound.label = chord_label(rebound.modifiers, rebound.key);
		effective_chords.append(rebound);
	}

	effective_current = true;
	return effective_chords;
}

bool bssh_input_router::handle_key(QKeyEvent *event) {
	if (!m_emulator) return false;

	/*
	 * Caps Lock as Ctrl (sec 6.1), before anything else looks at the key.
	 *
	 * It arms the same sticky modifier the on-screen row uses, so the
	 * next key is Ctrl-something and the button on the row -- if there is
	 * one -- lights up to say so. Consumed either way: Caps Lock has no
	 * escape sequence of its own, so passing it on would send nothing
	 * while leaving the modifier armed.
	 *
	 * Toggling rather than setting, so a second press disarms it. The
	 * lock the operating system is keeping is beyond reach here (see the
	 * header), and pressing Caps twice to undo a mistake is what a user
	 * will try.
	 */
	if (event->key() == Qt::Key_CapsLock) {
		if (!m_caps_as_ctrl) return false;

		/*
		 * A TOGGLE, and deliberately NOT the Treo cycle the on-screen
		 * keyboards use. Sec 6.1 settled this for the physical key and
		 * the reason still holds: the operating system's own caps state
		 * is beyond reach here, so pressing Caps again to undo a mistake
		 * is what a user will try -- and a second press that LOCKED Ctrl
		 * instead would answer that correction by turning every
		 * following letter into a control byte.
		 *
		 * The cycle also wants something the physical key does not have.
		 * It takes three presses to return to off, which is workable for
		 * an on-screen button whose state you can see and wrong for a
		 * key whose own light is busy telling you something else.
		 */
		if (modifier_state(bssh_emulator::MOD_CTRL) == MODIFIER_OFF) {
			set_sticky_modifiers(quint8(m_sticky | bssh_emulator::MOD_CTRL));
		} else {
			set_sticky_modifiers(quint8(m_sticky & ~bssh_emulator::MOD_CTRL));
			set_locked_modifiers(quint8(m_locked & ~bssh_emulator::MOD_CTRL));
		}
		return true;
	}

	/*
	 * Application chords first, before any translation: whatever is
	 * stolen here never reaches the remote, so the check has to happen
	 * before the key becomes bytes (sec 6.1).
	 *
	 * Walked from the table rather than written out as an if-ladder, so
	 * the list a user is shown and the list the router acts on cannot be
	 * two lists.
	 *
	 * The modifiers must match EXACTLY -- Ctrl+Alt+Shift+Left is not
	 * Ctrl+Alt+Left -- because matching loosely steals chords nobody
	 * declared and leaves the shown list quietly wrong. But exactly
	 * against the four that MEAN something, not against the raw set: Qt
	 * reports Qt::KeypadModifier for the keypad's own PageUp and
	 * Qt::GroupSwitchModifier on X11 with a third level engaged, and
	 * neither says anything about which chord was pressed. Comparing the
	 * whole set would take Shift+PageUp on the main block and let it
	 * through on the keypad, on the same keyboard.
	 */
	const Qt::KeyboardModifiers meaningful = Qt::ControlModifier | Qt::ShiftModifier
	                                          | Qt::AltModifier | Qt::MetaModifier;

	for (const chord_t &chord : stolen_chords()) {
		if (event->key() != chord.key) continue;
		if ((event->modifiers() & meaningful) != chord.modifiers) continue;

		emit shortcut_pressed(chord.action);
		return false;
	}

	/*
	 * General key remaps (sec 6.1): send a different key's bytes for this
	 * one. After the stolen chords, so a chord the app keeps still wins,
	 * and before the translation below, so the shell hears the mapped key
	 * rather than the pressed one.
	 *
	 * Key-to-key only: the target is translated through emulator_key_for
	 * like any other key, so a remap to a character -- which has no
	 * emulator key and would go through send_text -- is left alone and the
	 * pressed key takes its normal path. That covers the remaps a person
	 * actually keeps (an F-key to a navigation key, a stray key to Escape)
	 * without a second, text-sending path to reason about.
	 */
	const Qt::KeyboardModifiers held = event->modifiers() & meaningful;
	if (send_remap([event, held](const QKeyCombination &from) {
		    return from.key() == event->key() && from.keyboardModifiers() == held;
	    })) {
		return true;
	}

	/*
	 * The kitty modes above disambiguate want the whole event -- repeat
	 * or press, the unshifted key, the text -- so it goes to the emulator
	 * whole, and anything it leaves to legacy falls through below.
	 */
	/*
	 * And the keypad under any kitty mode at all: disambiguate alone
	 * already reports its non-text keys as keys of their own.
	 */
	const bool keypad_kitty = (event->modifiers() & Qt::KeypadModifier)
	                          && m_emulator->kitty_keyboard_flags() != 0;
	if (m_emulator->wants_key_events() || keypad_kitty) {
		bssh_emulator::key_event_t kitty;
		if (kitty_event_for(event, kitty)) {
			if (m_emulator->send_key_event(kitty)) {
				m_kitty_pressed.insert(event->key(), kitty);
				spend_sticky_modifiers();
				return true;
			}
		}
	}

	const quint8 modifiers = modifiers_for(event);
	const int key = emulator_key_for(event->key());

	if (key != bssh_emulator::KEY_NONE) {
		m_emulator->send_key(key, modifiers);
		spend_sticky_modifiers();
		return true;
	}

	/*
	 * Ordinary characters. event->text() is used rather than the key
	 * code, because it is the only one that accounts for the keyboard
	 * layout and for dead keys -- a French layout's key code for the
	 * physical A key says "A", and what the user typed is "q".
	 *
	 * With Ctrl held, text() is already the control character, so the
	 * codepoint is taken from the key instead and libvterm applies the
	 * modifier itself. Passing 0x03 AND ctrl would encode it twice.
	 */
	if (modifiers & bssh_emulator::MOD_CTRL) {
		const int code = event->key();
		if (code >= 0x20 && code < 0x7f) {
			m_emulator->send_char(static_cast<char32_t>(QChar(code).toLower().unicode()),
			                       modifiers);
			spend_sticky_modifiers();
			return true;
		}
	}

	const QString text = event->text();
	if (text.isEmpty()) return false;

	const QList<uint> codepoints = text.toUcs4();
	for (uint codepoint : codepoints) {
		/*
		 * The ALT modifier is passed through, since libvterm turns it
		 * into the ESC prefix a shell expects for Meta. SHIFT is not:
		 * text() already reflects it, and passing both would ask for a
		 * shifted version of an already-shifted character.
		 */
		m_emulator->send_char(codepoint, modifiers & bssh_emulator::MOD_ALT);
	}

	spend_sticky_modifiers();
	return true;
}

void bssh_input_router::send_key(int key, quint8 modifiers) {
	if (!m_emulator) return;

	/*
	 * ALTGR masked off: it selects a character rather than modifying a
	 * key, and libvterm has no encoding for it (see MOD_ALTGR). Passing
	 * it would ask for a modified version of a key that was never
	 * modified.
	 */
	const quint8 sending = quint8((modifiers | effective_modifiers()) & ~MOD_ALTGR);

	/*
	 * Stolen chords first, exactly as handle_key does for a hardware key
	 * (sec 6.1): a chord the app keeps -- scroll, copy, a pane move -- is
	 * one the remote must never receive. This path is the on-screen
	 * keyboard's, and until now it walked straight past the table, so
	 * Shift+PgUp on the soft keyboard PAGED THE REMOTE instead of
	 * scrolling the scrollback, and none of the app chords could be
	 * reached from the one keyboard a phone has. That is the "cannot be
	 * two lists" rule broken by a second entry point; this is the second
	 * entry point learning the list.
	 */
	if (steal_chord(key, sending)) return;

	/*
	 * The general remaps too, for the same reason as the chords above:
	 * they were applied in handle_key alone, so a remap made in the
	 * settings changed the physical keyboard and did nothing to the keys
	 * on screen -- one setting, two keyboards, two answers. Matched in
	 * the emulator's terms, as steal_chord matches the chord table.
	 */
	const quint8 meaningful = bssh_emulator::MOD_SHIFT | bssh_emulator::MOD_CTRL
	                          | bssh_emulator::MOD_ALT;
	if (key != bssh_emulator::KEY_NONE
	     && send_remap([key, sending, meaningful](const QKeyCombination &from) {
		        return emulator_key_for(from.key()) == key
		               && emulator_mods_for(from.keyboardModifiers()) == (sending & meaningful);
	        })) {
		return;
	}

	/*
	 * Under the kitty protocol's key events this is a key like any other,
	 * press and release, as handle_key makes it for a physical one. It
	 * went straight to libvterm, so on the Fold the keyboard's Enter, Tab
	 * and Backspace arrived as the legacy 0d, 09 and 7f to a program that
	 * had asked for every key as an escape code.
	 */
	if (m_emulator->wants_key_events()) {
		bssh_emulator::key_event_t press;
		press.key = key;
		press.modifiers = sending;
		if (m_emulator->send_key_event(press)) {
			bssh_emulator::key_event_t release = press;
			release.type = bssh_emulator::EVENT_RELEASE;
			m_emulator->send_key_event(release);
			spend_sticky_modifiers();
			return;
		}
	}

	m_emulator->send_key(key, sending);
	spend_sticky_modifiers();
}

/*
 * The one place a remap is sent from, whichever keyboard asked: the
 * physical one matches in Qt keys, the on-screen ones in emulator keys or
 * a letter's Qt key, and all three send the same target the same way.
 *
 * Key-to-key only, as sec 6.1 records: a target with no emulator key is
 * left alone and the pressed key takes its normal path.
 */
bool bssh_input_router::send_remap(const std::function<bool(const QKeyCombination &)> &matches) {
	if (user_remaps.isEmpty() || !m_emulator) return false;

	for (auto it = user_remaps.cbegin(); it != user_remaps.cend(); ++it) {
		const QKeySequence from(it.key(), QKeySequence::PortableText);
		if (from.count() != 1 || !matches(from[0])) continue;

		const QKeySequence target(it.value(), QKeySequence::PortableText);
		if (target.count() != 1) return false;
		const QKeyCombination combo = target[0];
		const int mapped = emulator_key_for(combo.key());
		if (mapped == bssh_emulator::KEY_NONE) return false;

		m_emulator->send_key(mapped, emulator_mods_for(combo.keyboardModifiers()));
		spend_sticky_modifiers();
		return true;
	}
	return false;
}

bool bssh_input_router::remap_char(char32_t ch) {
	/* A letter's Qt key, as steal_char_chord derives it. */
	int qt_key = 0;
	if (ch >= U'a' && ch <= U'z') qt_key = Qt::Key_A + int(ch - U'a');
	else if (ch >= U'A' && ch <= U'Z') qt_key = Qt::Key_A + int(ch - U'A');
	else return false;

	/*
	 * The board's Shift is folded into the character, so a capital is
	 * Shift held -- which is how the physical keyboard reports one too.
	 */
	Qt::KeyboardModifiers held = qt_modifiers_for(effective_modifiers());
	if (ch >= U'A' && ch <= U'Z') held |= Qt::ShiftModifier;

	return send_remap([qt_key, held](const QKeyCombination &from) {
		return from.key() == qt_key && from.keyboardModifiers() == held;
	});
}

bool bssh_input_router::steal_chord(int key, quint8 modifiers) {
	/*
	 * Only the modifiers the table means, so an armed keypad or group
	 * bit does not stop a match -- the same exact-but-only-the-meaningful
	 * comparison handle_key makes, in the emulator's bits.
	 */
	const quint8 meaningful = bssh_emulator::MOD_SHIFT | bssh_emulator::MOD_CTRL
	                          | bssh_emulator::MOD_ALT;

	/*
	 * A letter chord translates to KEY_NONE and its bytes come through
	 * send_text, not here; guarding keeps a KEY_NONE from matching one.
	 */
	if (key == bssh_emulator::KEY_NONE) return false;

	for (const chord_t &chord : stolen_chords()) {
		if (emulator_key_for(chord.key) != key) continue;
		if ((modifiers & meaningful) != emulator_mods_for(chord.modifiers)) continue;

		emit shortcut_pressed(chord.action);
		spend_sticky_modifiers();

		return true;
	}

	return false;
}

bool bssh_input_router::steal_char_chord(char32_t ch) {
	/*
	 * The letter's Qt key, which for a-z and A-Z is the uppercase letter's
	 * code -- Qt::Key_A is 'A'. Anything else carries no chord and is left
	 * to send_text.
	 */
	int qt_key = 0;
	if (ch >= U'a' && ch <= U'z') qt_key = Qt::Key_A + int(ch - U'a');
	else if (ch >= U'A' && ch <= U'Z') qt_key = Qt::Key_A + int(ch - U'A');
	else return false;

	/*
	 * Matched against the ARMED modifiers, before the board folds Shift
	 * into the character: a chord's Shift is a modifier here, not a
	 * request for a capital. So only Ctrl+Shift+C and its siblings match;
	 * a bare letter, a capital, or Ctrl+C alone carries no chord and falls
	 * through to the text it is.
	 */
	const quint8 mods = effective_modifiers();
	const quint8 meaningful = bssh_emulator::MOD_SHIFT | bssh_emulator::MOD_CTRL
	                          | bssh_emulator::MOD_ALT;

	for (const chord_t &chord : stolen_chords()) {
		if (chord.key != qt_key) continue;
		if ((mods & meaningful) != emulator_mods_for(chord.modifiers)) continue;

		emit shortcut_pressed(chord.action);
		spend_sticky_modifiers();

		return true;
	}

	return false;
}

bool bssh_input_router::kitty_event_for(QKeyEvent *event,
                                        bssh_emulator::key_event_t &out) const {
	out = bssh_emulator::key_event_t();
	out.type = event->isAutoRepeat() ? bssh_emulator::EVENT_REPEAT : bssh_emulator::EVENT_PRESS;
	out.modifiers = modifiers_for(event);

	/*
	 * A modifier on its own. Its own bit is SET on the press, as the
	 * protocol requires -- Qt reports the state from before the key on
	 * some platforms and after it on others, so it is set here rather
	 * than trusted. Left keys only: Qt does not say which side.
	 */
	switch (event->key()) {
	case Qt::Key_Shift:
		out.modifier_key = bssh_emulator::KITTY_LEFT_SHIFT;
		out.modifiers |= bssh_emulator::MOD_SHIFT;
		return true;
	case Qt::Key_Control:
		out.modifier_key = bssh_emulator::KITTY_LEFT_CONTROL;
		out.modifiers |= bssh_emulator::MOD_CTRL;
		return true;
	case Qt::Key_Alt:
		out.modifier_key = bssh_emulator::KITTY_LEFT_ALT;
		out.modifiers |= bssh_emulator::MOD_ALT;
		return true;
	case Qt::Key_Meta:
		out.modifier_key = bssh_emulator::KITTY_LEFT_SUPER;
		out.modifiers |= bssh_emulator::MOD_SUPER;
		return true;
	default:
		break;
	}

	/*
	 * The keypad, told from the main block by Qt's KeypadModifier -- which
	 * is also why it is not a modifier the remote hears about. Digits and
	 * operators type text; Enter and, with Num Lock off, the navigation
	 * keys do not. Numbers from the protocol's table: KP_0 is 57399 and
	 * KP_BEGIN 57427.
	 */
	if (event->modifiers() & Qt::KeypadModifier) {
		const int qt_key = event->key();
		int keypad = 0;
		bool typed = false;
		if (qt_key >= Qt::Key_0 && qt_key <= Qt::Key_9) {
			keypad = 57399 + (qt_key - Qt::Key_0);
			typed = true;
		} else {
			switch (qt_key) {
			case Qt::Key_Period:   keypad = 57409; typed = true; break;
			case Qt::Key_Comma:    keypad = 57416; typed = true; break;
			case Qt::Key_Slash:    keypad = 57410; typed = true; break;
			case Qt::Key_Asterisk: keypad = 57411; typed = true; break;
			case Qt::Key_Minus:    keypad = 57412; typed = true; break;
			case Qt::Key_Plus:     keypad = 57413; typed = true; break;
			case Qt::Key_Equal:    keypad = 57415; typed = true; break;
			case Qt::Key_Enter:    keypad = 57414; break;
			case Qt::Key_Left:     keypad = 57417; break;
			case Qt::Key_Right:    keypad = 57418; break;
			case Qt::Key_Up:       keypad = 57419; break;
			case Qt::Key_Down:     keypad = 57420; break;
			case Qt::Key_PageUp:   keypad = 57421; break;
			case Qt::Key_PageDown: keypad = 57422; break;
			case Qt::Key_Home:     keypad = 57423; break;
			case Qt::Key_End:      keypad = 57424; break;
			case Qt::Key_Insert:   keypad = 57425; break;
			case Qt::Key_Delete:   keypad = 57426; break;
			case Qt::Key_Clear:    keypad = 57427; break;
			default: break;
			}
		}

		if (keypad) {
			out.keypad = keypad;
			out.keypad_text = typed;
			if (typed) {
				for (uint c : event->text().toUcs4()) out.text.append(c);
				out.modifiers &= quint8(~bssh_emulator::MOD_SHIFT);
			}
			return true;
		}
	}

	out.key = emulator_key_for(event->key());
	if (out.key != bssh_emulator::KEY_NONE) return true;

	/*
	 * A text key: the UNSHIFTED key is what the protocol numbers it by.
	 * For a letter that is its lower case whatever Shift did, and the
	 * shifted key is the upper case. For anything else Qt gives only the
	 * character Shift already made -- '!' rather than the 1 key under it
	 * -- so that is reported as the key and Shift is dropped, since it has
	 * been spent: the one place this falls short of kitty, which knows
	 * the physical key.
	 */
	for (uint c : event->text().toUcs4()) out.text.append(c);

	const int qt_key = event->key();
	if (qt_key >= Qt::Key_A && qt_key <= Qt::Key_Z) {
		out.code = char32_t(qt_key - Qt::Key_A + 'a');
		if (out.modifiers & bssh_emulator::MOD_SHIFT) out.shifted = char32_t(qt_key - Qt::Key_A + 'A');
		return true;
	}

	char32_t produced = 0;
	for (uint c : out.text) {
		if (c >= 0x20 && c != 0x7f) {
			produced = char32_t(c);
			break;
		}
	}
	if (!produced && qt_key >= 0x20 && qt_key < 0x7f) produced = char32_t(qt_key);
	if (!produced) return false;

	const QString as_string = QString::fromUcs4(&produced, 1);
	const QString lower = as_string.toLower();
	out.code = lower.toUcs4().value(0, produced);
	if (out.code != produced) {
		out.shifted = produced;
	} else {
		out.modifiers &= quint8(~bssh_emulator::MOD_SHIFT);
	}
	return true;
}

bool bssh_input_router::handle_key_release(QKeyEvent *event) {
	if (!m_emulator) return false;

	/*
	 * An autorepeat release is half of a repeat, not the key coming up:
	 * X11 sends a release before each repeated press. The press that
	 * follows is marked repeat, and the real release comes once.
	 */
	if (event->isAutoRepeat()) return false;

	const auto found = m_kitty_pressed.constFind(event->key());
	if (found == m_kitty_pressed.constEnd()) return false;

	bssh_emulator::key_event_t release = found.value();
	m_kitty_pressed.erase(found);

	release.type = bssh_emulator::EVENT_RELEASE;
	release.text.clear();

	/* A modifier's own bit is RESET on its release, as its press set it. */
	if (release.modifier_key == bssh_emulator::KITTY_LEFT_SHIFT)
		release.modifiers &= quint8(~bssh_emulator::MOD_SHIFT);
	if (release.modifier_key == bssh_emulator::KITTY_LEFT_CONTROL)
		release.modifiers &= quint8(~bssh_emulator::MOD_CTRL);
	if (release.modifier_key == bssh_emulator::KITTY_LEFT_ALT)
		release.modifiers &= quint8(~bssh_emulator::MOD_ALT);
	if (release.modifier_key == bssh_emulator::KITTY_LEFT_SUPER)
		release.modifiers &= quint8(~bssh_emulator::MOD_SUPER);

	return m_emulator->send_key_event(release);
}

void bssh_input_router::send_text(const QString &text) {
	if (!m_emulator || text.isEmpty()) return;

	/*
	 * Under every-key-as-escape the text has to arrive as key events too,
	 * or it is the one thing typed that the program cannot see. An input
	 * method says nothing of keys, so each character is its own key: the
	 * lower case as the key and Shift for an upper-case letter, which is
	 * what a keyboard would have said -- and a release straight after it
	 * when releases are wanted, since no real one is coming.
	 */
	if (m_emulator->wants_key_events()) {
		const QList<uint> codepoints = text.toUcs4();
		bool all_sent = true;
		for (uint codepoint : codepoints) {
			const char32_t produced = char32_t(codepoint);
			bssh_emulator::key_event_t press;
			press.modifiers = quint8(effective_modifiers() & ~MOD_ALTGR);

			/*
			 * No text under Ctrl: a Ctrl chord types nothing, and the
			 * spec's associated text is what a key typed. A hardware
			 * Ctrl+C says so by carrying 0x03, which is dropped; this
			 * path carried the plain letter, and the Fold sent "c" with
			 * Ctrl+C.
			 */
			if (!(press.modifiers & bssh_emulator::MOD_CTRL)) press.text.append(codepoint);

			const QString lower = QString::fromUcs4(&produced, 1).toLower();
			press.code = lower.toUcs4().value(0, produced);
			if (press.code != produced) {
				press.shifted = produced;
				press.modifiers |= bssh_emulator::MOD_SHIFT;
			} else {
				press.modifiers &= quint8(~bssh_emulator::MOD_SHIFT);
			}

			if (!m_emulator->send_key_event(press)) {
				all_sent = false;
				break;
			}
			bssh_emulator::key_event_t release = press;
			release.type = bssh_emulator::EVENT_RELEASE;
			release.text.clear();
			m_emulator->send_key_event(release);
		}
		if (all_sent) {
			spend_sticky_modifiers();
			return;
		}
	}

	/*
	 * SHIFT is masked off, for the reason handle_key states thirty lines
	 * up: the TEXT is already what the shift produced, and passing the
	 * modifier as well asks libvterm for a shifted version of an
	 * already-shifted character.
	 *
	 * It mattered the moment a keyboard of our own had a Shift key on it
	 * (sec 6.2). Before that the only way to reach here with SHIFT set
	 * was a sticky Shift armed for the mouse override, so the defect
	 * existed and had almost no way to be met. ALT is passed, because
	 * libvterm turns it into the ESC prefix a shell expects for Meta.
	 */
	const quint8 modifiers = quint8(effective_modifiers()
	                                 & ~quint8(bssh_emulator::MOD_SHIFT)
	                                 & ~MOD_ALTGR);

	const QList<uint> codepoints = text.toUcs4();
	for (uint codepoint : codepoints) {
		m_emulator->send_char(codepoint, modifiers);
	}

	spend_sticky_modifiers();
}

void bssh_input_router::paste(const QString &text) {
	if (!m_emulator || text.isEmpty()) return;
	m_emulator->send_paste(text);
}
