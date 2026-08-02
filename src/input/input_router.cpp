#include "input/input_router.h"

#include <QKeyEvent>

#include "term/emulator.h"

bssh_input_router::bssh_input_router(bssh_emulator *emulator, QObject *parent)
    : QObject(parent),
      m_emulator(emulator),
      m_sticky(bssh_emulator::MOD_NONE) {
}

quint8 bssh_input_router::modifiers_for(QKeyEvent *event) const {
	quint8 modifiers = m_sticky;

	if (event->modifiers() & Qt::ShiftModifier) modifiers |= bssh_emulator::MOD_SHIFT;
	if (event->modifiers() & Qt::ControlModifier) modifiers |= bssh_emulator::MOD_CTRL;
	if (event->modifiers() & Qt::AltModifier) modifiers |= bssh_emulator::MOD_ALT;

	return modifiers;
}

bool bssh_input_router::handle_key(QKeyEvent *event) {
	if (!m_emulator) return false;

	/*
	 * Application chords first, before any translation: whatever is
	 * stolen here never reaches the remote, so the check has to happen
	 * before the key becomes bytes (sec 6.1).
	 *
	 * Ctrl+Shift+C/V rather than Ctrl+C/V, because Ctrl-C is SIGINT and a
	 * terminal that swallows it to mean "copy" is a terminal that cannot
	 * interrupt a runaway process.
	 */
	if (event->modifiers().testFlag(Qt::ControlModifier)
	    && event->modifiers().testFlag(Qt::ShiftModifier)) {
		if (event->key() == Qt::Key_C) {
			emit shortcut_pressed(SHORTCUT_COPY);
			return false;
		}
		if (event->key() == Qt::Key_V) {
			emit shortcut_pressed(SHORTCUT_PASTE);
			return false;
		}
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
