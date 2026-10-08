#ifndef SK_MODIFIERS_H
#define SK_MODIFIERS_H

#include <QObject>

/*
 * The modifier bits an on-screen keyboard reasons in.
 *
 * The values are BeerSSH's emulator bits, unchanged, plus its AltGr, so the
 * terminal that the keyboard came from can assert the two agree rather than
 * translate: Shift 0x01, Alt 0x02, Ctrl 0x04. AltGr picks a character
 * rather than modifying a key, so a target never sends it.
 */
enum : quint8 {
	SK_MOD_NONE  = 0x00,
	SK_MOD_SHIFT = 0x01,
	SK_MOD_ALT   = 0x02,
	SK_MOD_CTRL  = 0x04,
	SK_MOD_ALTGR = 0x08
};

/*
 * The state of a keyboard's modifiers (softkeys' project.md sec 4).
 *
 * FOUR states per modifier, and every one of them is a way somebody types:
 *
 *   OFF     nothing.
 *   ONCE    armed for the next key only: a tap on the modifier. A Treo's
 *           one-shot, and what Ctrl-C needs.
 *   LOCKED  until pressed again: a second tap. What Ctrl-B over and over
 *           needs, without re-arming before every key.
 *   HELD    a finger resting on the modifier while another presses keys,
 *           on a screen that reports more than one touch: every key under
 *           it carries it, and lifting leaves nothing behind.
 *
 * A press and release with nothing sent between is a TAP, and a tap cycles
 * OFF -> ONCE -> LOCKED -> OFF, so the one-shot habit and the two-thumb one
 * live on the same key without a setting between them.
 *
 * Its own object rather than a widget's members because more than one
 * keyboard can share it -- BeerSSH's physical keyboard and its on-screen
 * ones read one state, so a Ctrl armed on screen applies to the next key
 * typed on glass or on plastic.
 */
class sk_modifiers : public QObject {
	Q_OBJECT

public:
	enum state_t {
		OFF,
		ONCE,
		LOCKED,
		HELD
	};

	explicit sk_modifiers(QObject *parent = nullptr);

	quint8 armed() const { return m_armed; }
	quint8 locked() const { return m_locked; }
	quint8 held() const { return m_held; }

	/* What the next keystroke carries: armed, locked and held together. */
	quint8 effective() const { return quint8(m_armed | m_locked | m_held); }

	void set_armed(quint8 modifiers);
	void set_locked(quint8 modifiers);

	/* LOCKED wins over HELD, and HELD over ONCE. */
	state_t state(quint8 modifier) const;

	/*
	 * One tap. OFF to ONCE to LOCKED and back to OFF, read from the armed
	 * and locked sets only: the cycle is about what stays once a finger
	 * resting on the same modifier is gone.
	 *
	 * Locking from ONCE rather than from OFF makes the common case cheap:
	 * one tap for the Ctrl-C everybody sends, a second only when you know
	 * you are sending more. The other order makes the rare case free and
	 * the common one dangerous, since an unnoticed lock turns every
	 * following letter into a control character.
	 */
	void cycle(quint8 modifier);

	/* A finger has come down on a modifier key. */
	void hold(quint8 modifier);

	/*
	 * The finger is lifted. True when nothing was sent while it was down --
	 * a TAP, which the keyboard answers with cycle(). False for a hold that
	 * carried a key, and for a modifier that was not held at all.
	 */
	bool release(quint8 modifier);

	/*
	 * A keystroke has gone out carrying effective(). The armed set is
	 * spent; the locked set is not, which is the whole difference between
	 * the two; and the held set learns it carried a key, which is what
	 * makes its release the end of a chord rather than a tap.
	 */
	void spend();

signals:
	void armed_changed(quint8 modifiers);
	void locked_changed(quint8 modifiers);
	void held_changed(quint8 modifiers);

private:
	quint8 m_armed = SK_MOD_NONE;
	quint8 m_locked = SK_MOD_NONE;
	quint8 m_held = SK_MOD_NONE;

	/* The held modifiers a keystroke has gone out under since they came down. */
	quint8 m_held_used = SK_MOD_NONE;
};

#endif /* SK_MODIFIERS_H */
