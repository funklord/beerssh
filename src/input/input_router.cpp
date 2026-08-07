#include "input/input_router.h"

#include <QKeyCombination>
#include <QKeySequence>

#include <QKeyEvent>

#include "term/emulator.h"

bssh_input_router::bssh_input_router(bssh_emulator *emulator, QObject *parent)
    : QObject(parent),
      m_emulator(emulator),
      m_sticky(bssh_emulator::MOD_NONE),
      m_caps_as_ctrl(false) {
}

quint8 bssh_input_router::modifiers_for(QKeyEvent *event) const {
	quint8 modifiers = m_sticky;

	if (event->modifiers() & Qt::ShiftModifier) modifiers |= bssh_emulator::MOD_SHIFT;
	if (event->modifiers() & Qt::ControlModifier) modifiers |= bssh_emulator::MOD_CTRL;
	if (event->modifiers() & Qt::AltModifier) modifiers |= bssh_emulator::MOD_ALT;

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
	};
	return chords;
}

void bssh_input_router::set_bindings(const QHash<QString, QString> &bindings) {
	user_bindings = bindings;
	effective_current = false;
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

		set_sticky_modifiers(m_sticky ^ bssh_emulator::MOD_CTRL);
		emit sticky_modifiers_changed(m_sticky);
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

	const quint8 modifiers = modifiers_for(event);
	int key = bssh_emulator::KEY_NONE;

	switch (event->key()) {
	case Qt::Key_Return:
	case Qt::Key_Enter:
		key = bssh_emulator::KEY_ENTER;
		break;
	case Qt::Key_Tab:
	case Qt::Key_Backtab:
		key = bssh_emulator::KEY_TAB;
		break;
	case Qt::Key_Backspace:
		key = bssh_emulator::KEY_BACKSPACE;
		break;
	case Qt::Key_Escape:
		key = bssh_emulator::KEY_ESCAPE;
		break;
	case Qt::Key_Up:
		key = bssh_emulator::KEY_UP;
		break;
	case Qt::Key_Down:
		key = bssh_emulator::KEY_DOWN;
		break;
	case Qt::Key_Left:
		key = bssh_emulator::KEY_LEFT;
		break;
	case Qt::Key_Right:
		key = bssh_emulator::KEY_RIGHT;
		break;
	case Qt::Key_Insert:
		key = bssh_emulator::KEY_INSERT;
		break;
	case Qt::Key_Delete:
		key = bssh_emulator::KEY_DELETE;
		break;
	case Qt::Key_Home:
		key = bssh_emulator::KEY_HOME;
		break;
	case Qt::Key_End:
		key = bssh_emulator::KEY_END;
		break;
	case Qt::Key_PageUp:
		key = bssh_emulator::KEY_PAGE_UP;
		break;
	case Qt::Key_PageDown:
		key = bssh_emulator::KEY_PAGE_DOWN;
		break;
	default:
		break;
	}

	if (key == bssh_emulator::KEY_NONE
	    && event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F35) {
		key = bssh_emulator::KEY_FUNCTION_BASE + 1 + (event->key() - Qt::Key_F1);
	}

	if (key != bssh_emulator::KEY_NONE) {
		m_emulator->send_key(key, modifiers);
		set_sticky_modifiers(bssh_emulator::MOD_NONE);
		emit sticky_modifiers_changed(m_sticky);
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
			set_sticky_modifiers(bssh_emulator::MOD_NONE);
			emit sticky_modifiers_changed(m_sticky);
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

	set_sticky_modifiers(bssh_emulator::MOD_NONE);
	emit sticky_modifiers_changed(m_sticky);
	return true;
}

void bssh_input_router::send_key(int key, quint8 modifiers) {
	if (!m_emulator) return;

	m_emulator->send_key(key, modifiers | m_sticky);
	set_sticky_modifiers(bssh_emulator::MOD_NONE);
	emit sticky_modifiers_changed(m_sticky);
}

void bssh_input_router::send_text(const QString &text) {
	if (!m_emulator || text.isEmpty()) return;

	const QList<uint> codepoints = text.toUcs4();
	for (uint codepoint : codepoints) {
		m_emulator->send_char(codepoint, m_sticky);
	}

	set_sticky_modifiers(bssh_emulator::MOD_NONE);
	emit sticky_modifiers_changed(m_sticky);
}

void bssh_input_router::paste(const QString &text) {
	if (!m_emulator || text.isEmpty()) return;
	m_emulator->send_paste(text);
}
