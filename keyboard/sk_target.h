#ifndef SK_TARGET_H
#define SK_TARGET_H

#include <QString>

class sk_modifiers;

/*
 * What an on-screen keyboard types into (softkeys' project.md sec 4).
 *
 * The keyboard decides WHICH key was pressed and with what armed, held or
 * locked; the target decides what that means to the application. A text
 * field wants ordinary key events; a terminal wants bytes on a wire, with
 * some chords kept by the application itself. Neither is the keyboard's
 * business, so neither is in it.
 *
 * Not a QObject, so an application object that already is one -- BeerSSH's
 * input router -- can be a target as well, rather than needing an adapter
 * that forwards every call.
 */
class sk_target {
public:
	virtual ~sk_target() = default;

	/*
	 * The modifier state the keyboard reads and changes. The target owns it
	 * so that whatever else types into the same place shares it -- a
	 * physical keyboard, a second on-screen one.
	 */
	virtual sk_modifiers &modifiers() = 0;

	/*
	 * A named key -- an arrow, Escape, Enter, a function key -- as a
	 * Qt::Key, with the modifiers the press carries beyond the keyboard's
	 * own state (a chord key's fixed Ctrl, say). The target adds the
	 * effective modifiers and spends the armed ones.
	 */
	virtual void key(int qt_key, quint8 modifiers) = 0;

	/*
	 * Characters, with Shift and AltGr already applied: the keyboard picked
	 * the character, so the target must not apply them again. Ctrl and Alt
	 * are the target's to apply from the state.
	 */
	virtual void text(const QString &text) = 0;

	/*
	 * Offered every letter before it is typed, so an application can keep a
	 * chord for itself (Ctrl+Shift+C as copy) or send something else in its
	 * place. True when the target dealt with it and the keyboard should type
	 * nothing. The default takes nothing.
	 */
	virtual bool claim_letter(char32_t letter) {
		Q_UNUSED(letter);
		return false;
	}
};

#endif /* SK_TARGET_H */
