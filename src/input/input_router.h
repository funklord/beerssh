#ifndef BSSH_INPUT_ROUTER_H
#define BSSH_INPUT_ROUTER_H

#include <QObject>
#include <QHash>
#include <QKeyCombination>

#include <functional>

#include "term/emulator.h"
#include "keyboard/sk_modifiers.h"
#include "keyboard/sk_target.h"
#include <QList>
#include <QString>

class QKeyEvent;

/*
 * bssh_input_router -- every keystroke becomes bytes here and nowhere else
 * (project.md sec 6.3).
 *
 * Physical keyboards, the on-screen key row, and paste all go through this
 * one class, because the alternative -- each input source doing its own
 * translation -- guarantees they disagree. It is also what keeps the same
 * keystroke producing the same bytes on a phone and a workstation (sec 6).
 *
 * The translation tables themselves are libvterm's, reached through
 * bssh_emulator: the sequence a key produces depends on terminal modes
 * that only the emulator tracks, and hand-writing those tables is a large
 * amount of work to arrive at a worse version of what is already vendored.
 * What lives HERE is the part libvterm cannot know: which Qt key is which,
 * which chords the app keeps for itself, and the sticky modifiers the key
 * row needs.
 *
 * Focus-awareness (sec 6.3: exactly one pane receives input) arrives with
 * panes at M8. Today there is one terminal, so there is one router.
 */
/*
 * Also the on-screen keyboard's target (sk_target): the keyboard is
 * softkeys', generic, and this is what makes what it types reach the
 * terminal -- with the chords this app keeps, the remaps, and one modifier
 * state shared with the physical keyboard.
 */
class bssh_input_router : public QObject, public sk_target {
	Q_OBJECT

public:
	explicit bssh_input_router(bssh_emulator *emulator, QObject *parent = nullptr);

	/*
	 * Returns true if the event became terminal input. False means the
	 * app keeps it -- see shortcut_t below -- and the caller should let
	 * it propagate.
	 */
	bool handle_key(QKeyEvent *event);

	/*
	 * A key coming up, for the kitty protocol's event types (sec 7.1).
	 * Sent only for a key whose press went to the emulator by that path,
	 * with the modifiers the press carried: a sticky Ctrl spent on the
	 * press is still what the release belongs to, and a chord the app
	 * stole never had a press for a release to answer.
	 */
	bool handle_key_release(QKeyEvent *event);

	/*
	 * Committed text from an input method, which is NOT the same path as
	 * a paste: no bracketing, because the user typed it (sec 6.1). On
	 * Android some keys only ever arrive this way; on desktop this is
	 * where dead keys and compose sequences land.
	 */
	void send_text(const QString &text);

	void paste(const QString &text);

	/*
	 * Sticky modifiers, for the on-screen keyboards (sec 6.2): tapping
	 * Ctrl then C must produce Ctrl-C, because a screen that takes one
	 * touch at a time -- or a mouse -- cannot hold two keys at once.
	 *
	 * THREE states per modifier, not two, and the third is what a Treo
	 * had: OFF, armed for the next key only, and LOCKED until pressed
	 * again. One press arms, a second locks, a third releases.
	 *
	 * A one-shot on its own cannot express what a terminal constantly
	 * asks for. Ctrl-C is one key and is served by arming; but reading a
	 * long file with Ctrl-B, or walking a buffer with Ctrl-N, is the same
	 * modifier over and over, and re-arming it before every keystroke is
	 * the interaction a physical keyboard never demands of you. The Treo
	 * and the BlackBerry both answered it this way, Android's own sticky
	 * keys do, and so does macOS: the press count IS the state, so there
	 * is nothing extra to discover and nothing new to learn.
	 *
	 * `sticky_modifiers` is the ARMED set only, which is what it has
	 * always meant; `locked_modifiers` is the other half. What a key
	 * actually carries is effective_modifiers(), and a keystroke spends
	 * the armed half while leaving the locked half exactly where it is --
	 * that difference is the whole feature.
	 *
	 * HELD is the fourth, and it is the physical keyboard's: a finger
	 * resting on the modifier while another presses keys, on a screen
	 * that reports more than one touch. It applies to every key pressed
	 * while it lasts and leaves nothing behind when it is lifted. Both
	 * ways are live at once -- a press and release with no key between
	 * is a tap, and a tap cycles exactly as above -- so a user moving
	 * from a Treo-style habit to two thumbs does not have to switch
	 * anything off to practise the other.
	 */
	/*
	 * A modifier this app tracks and NEVER sends.
	 *
	 * AltGr is not a terminal modifier: libvterm has no encoding for it,
	 * and a remote program never hears about it. What it does is pick a
	 * different CHARACTER from the key -- level 3 of a layout, the same
	 * way Shift picks level 2 -- so by the time anything is sent, its
	 * whole effect is already in the text.
	 *
	 * It lives here rather than in bssh_emulator's modifier_t because
	 * that enum is the WIRE vocabulary, and a bit that never reaches the
	 * wire does not belong in it. It lives here rather than in the
	 * keyboard because the sticky/locked machinery -- tap once to arm,
	 * twice to lock, spent on the next key -- is this class's, and a
	 * second copy of it in a widget is the drift sec 6.3 exists to
	 * prevent.
	 *
	 * Masked out of everything sent, exactly as MOD_SHIFT is masked out
	 * of send_text and for the same reason: the character already IS the
	 * answer.
	 */
	static constexpr quint8 MOD_ALTGR = SK_MOD_ALTGR;

	/*
	 * The states are softkeys' (sk_modifiers), named here as they always
	 * were so nothing that reads them had to change when the state machine
	 * moved out of this class.
	 */
	typedef sk_modifiers::state_t modifier_state_t;
	static constexpr modifier_state_t MODIFIER_OFF = sk_modifiers::OFF;
	static constexpr modifier_state_t MODIFIER_ONCE = sk_modifiers::ONCE;
	static constexpr modifier_state_t MODIFIER_LOCKED = sk_modifiers::LOCKED;
	static constexpr modifier_state_t MODIFIER_HELD = sk_modifiers::HELD;

	void set_sticky_modifiers(quint8 modifiers);
	quint8 sticky_modifiers() const { return m_modifiers.armed(); }

	void set_locked_modifiers(quint8 modifiers);
	quint8 locked_modifiers() const { return m_modifiers.locked(); }

	/* What the next keystroke will carry: armed, locked and held together. */
	quint8 effective_modifiers() const { return m_modifiers.effective(); }

	/*
	 * A finger has come down on a modifier key (see MODIFIER_HELD). It
	 * applies until release_modifier().
	 */
	void hold_modifier(quint8 modifier);

	/*
	 * The finger is lifted. Returns true when nothing was sent while it
	 * was down -- a TAP, which the caller answers with cycle_modifier(),
	 * so the press that did not chord anything still arms and locks.
	 * False for a hold that carried a key, and for a modifier that was
	 * not held at all.
	 */
	bool release_modifier(quint8 modifier);

	quint8 held_modifiers() const { return m_modifiers.held(); }

	/* sk_target: what the on-screen keyboard types arrives here. */
	sk_modifiers &modifiers() override { return m_modifiers; }
	void key(int qt_key, quint8 modifiers) override;
	void text(const QString &text) override;
	bool claim_letter(char32_t letter) override;

	modifier_state_t modifier_state(quint8 modifier) const;

	/*
	 * One press of a modifier key, wherever it came from. OFF to ONCE to
	 * LOCKED and back to OFF.
	 *
	 * Here rather than in the widget because both on-screen keyboards
	 * need it: a cycle written per caller is a cycle that goes round
	 * differently in two places.
	 *
	 * Caps-as-Ctrl (sec 6.1) deliberately does NOT use this. It toggles,
	 * for the reason recorded at that key: the second press is a
	 * correction, and answering a correction with a lock is the worst
	 * available reading of it.
	 */
	void cycle_modifier(quint8 modifier);

	/* Feeds a key from the on-screen row through the same translation. */
	void send_key(int key, quint8 modifiers);

	/*
	 * The on-screen keyboard's LETTER path: returns true and fires the
	 * shortcut when the armed modifiers make this letter a stolen chord
	 * (Ctrl+Shift+C and the like), so the caller sends no text. send_key
	 * cannot cover it -- a letter's bytes are a character, not a named key
	 * with an emulator code -- so it is checked here against the chord
	 * table in Qt keys, before Shift is folded into the character (sec 6.1).
	 */
	bool steal_char_chord(char32_t ch);

	/*
	 * The letter path's share of the general remaps (sec 6.1): sends the
	 * remap target and returns true when this letter, with the modifiers
	 * armed, held or locked, is a remapped key -- so the caller sends no
	 * text. The same table handle_key and send_key read.
	 */
	bool remap_char(char32_t ch);

	/*
	 * Caps Lock arms Ctrl for the next key, rather than sending itself
	 * (sec 6.1). Off by default.
	 *
	 * STICKY rather than held-down, and that is forced rather than
	 * chosen: only key PRESSES reach this router -- the terminal view
	 * implements keyPressEvent and not keyReleaseEvent -- so a
	 * hold-to-Ctrl reading would set a modifier with nothing able to
	 * clear it, and every key after it would become a control character.
	 * Sticky is also what the on-screen row already does for Ctrl and
	 * Alt (sec 6.2), so a user meets one behaviour rather than two.
	 *
	 * What this CANNOT do is stop the operating system toggling its own
	 * caps state: the key reaches the compositor before it reaches any
	 * application, so the lock still flips and still affects other
	 * programs. A real remap belongs in the OS keyboard layout; this is
	 * the terminal doing what it can within its own window, and the
	 * setting says so.
	 */
	void set_caps_as_ctrl(bool caps_as_ctrl) { m_caps_as_ctrl = caps_as_ctrl; }
	bool caps_as_ctrl() const { return m_caps_as_ctrl; }

	/*
	 * Chords the application keeps rather than sending to the remote.
	 * Deliberately a short list: every chord stolen here is one the
	 * remote shell can never receive again (sec 6.1), so this stays
	 * minimal until it is user-remappable.
	 */
	enum shortcut_t {
		SHORTCUT_NONE,
		SHORTCUT_COPY,
		SHORTCUT_PASTE,
		SHORTCUT_SCROLL_PAGE_UP,
		SHORTCUT_SCROLL_PAGE_DOWN,

		/*
		 * Pane focus (sec 6.1). Ctrl+Alt+Arrow, because every chord
		 * stolen here is one the remote shell can never receive again --
		 * and Ctrl+Arrow and Alt+Arrow are both in daily use by readline
		 * and by editors, so the pair together is the cheapest chord
		 * available.
		 */
		SHORTCUT_FOCUS_PANE_LEFT,
		SHORTCUT_FOCUS_PANE_RIGHT,

		/*
		 * Konsole's Ctrl+Shift+T and Ctrl+Shift+N, which sec 6.1 named
		 * as chords to steal and nothing ever took. Without them a
		 * desktop user's only way to a second session was the context
		 * menu, the front door and a dialog -- three steps for the
		 * commonest thing a terminal does.
		 */
		SHORTCUT_NEW_TAB,
		SHORTCUT_NEW_WINDOW,

		/*
		 * Find in the scrollback (sec 6.4). Ctrl+Shift+F, konsole's own
		 * chord for it, and stolen for the same reason as the rest: a
		 * remote shell almost never wants Ctrl+Shift+F, and a terminal
		 * that cannot search its history is missing the thing a long
		 * scrollback is FOR.
		 */
		SHORTCUT_FIND,

		/*
		 * Zoom (sec 9.5). Ctrl+= , Ctrl+- and Ctrl+0 -- the spelling
		 * browsers and editors settled on, and chosen over the
		 * Ctrl+Shift+plus a terminal might use because the modifiers
		 * here must match EXACTLY: "Ctrl and plus" is physically
		 * Ctrl+Shift+equal on a US keyboard and plain Ctrl+plus on a
		 * keypad, so one declaration cannot cover both while
		 * Ctrl+equal covers everybody.
		 *
		 * What is being given up is small and worth saying, and it is
		 * NOT a control byte: the ASCII control set runs over @ A-Z
		 * [ \ ] ^ _ and ? and holds no digit, equals or minus, so
		 * bssh_emulator::send_char hands all three straight to
		 * libvterm. What a remote loses is therefore a CSI-u report --
		 * the one it asked for if it enabled sec 7.1's disambiguation,
		 * and one libvterm emits unasked-for if it did not. A user who
		 * wants a chord back unbinds it by name, like any other.
		 */
		SHORTCUT_ZOOM_IN,
		SHORTCUT_ZOOM_OUT,
		SHORTCUT_ZOOM_RESET
	};

	/*
	 * One stolen chord: what it takes, what it does, and how to write it
	 * down.
	 *
	 * Sec 6.1 asks for three things about this set -- keep it minimal,
	 * make it user-remappable, and DOCUMENT it -- and the third was the
	 * one nothing served. The list existed only as an if-ladder in
	 * handle_key(), so the only way to find out which chords never reach
	 * your shell was to press them and notice. Any list written anywhere
	 * else would be a second copy free to drift from the behaviour, which
	 * is the failure this tree keeps unpicking, so the ladder is gone and
	 * the table below is what the router actually matches against.
	 *
	 * `label` is the chord as a person writes it, not as Qt spells it: it
	 * is shown to somebody who wants to know what they cannot send.
	 */
	struct chord_t {
		shortcut_t action;
		Qt::KeyboardModifiers modifiers;
		int key;

		/*
		 * How it is written for a reader -- and COMPUTED, not stored,
		 * once a chord can be rebound: a label kept beside a binding is
		 * a label that describes the old one.
		 */
		QString label;

		/*
		 * The name this chord has in app.ini, and it never changes.
		 * Bindings are filed under it rather than under a position in
		 * the list, so a hand-edited file survives the list being
		 * reordered or added to.
		 */
		const char *name;

		const char *what;
	};

	/*
	 * Every chord this application takes for itself, in the order a
	 * reader should meet them, with whatever the user has rebound
	 * applied. Nothing else is intercepted.
	 */
	static const QList<chord_t> &stolen_chords();

	/* The built-in bindings, for showing a default and offering a reset. */
	static const QList<chord_t> &default_chords();

	/*
	 * Rebinds the chords (sec 6.1), by action name to key sequence.
	 *
	 * A name with an EMPTY sequence is not stolen at all, and that is the
	 * feature rather than an edge case: the section this implements says
	 * "any chord you steal is a chord the remote shell can no longer
	 * receive", so giving one back has to be sayable. An unknown name and
	 * an unparseable sequence are ignored, leaving the built-in -- a
	 * hand-edited file with a typo in it should behave like the default
	 * rather than lose the binding.
	 *
	 * Process-wide, because it is a fact about the keyboard in front of
	 * the user rather than about one session. Tests set it back to an
	 * empty map in cleanup, the same way they clear the transport
	 * factory.
	 */
	static void set_bindings(const QHash<QString, QString> &bindings);

	/*
	 * General key remaps (sec 6.1): a portable "from" key sequence to the
	 * "to" one the shell receives instead. Process-wide, like the
	 * bindings, and for the same reason -- it is a fact about the keyboard
	 * rather than about a session. Applied in handle_key after the stolen
	 * chords, so a chord the app keeps still wins, and only for a target
	 * that is a translatable key rather than a character.
	 */
	static void set_key_remaps(const QHash<QString, QString> &remaps);

	/* What a chord is bound to now, as text -- empty when unbound. */
	static QString binding_for(const QString &action_name);

signals:
	void shortcut_pressed(bssh_input_router::shortcut_t shortcut);

	/*
	 * The ARMED set changed. Keeps its name and its meaning: a keystroke
	 * spends it, so a button drawn from it must be redrawn straight after
	 * one, or it is a Ctrl key claiming to be held when it is not.
	 */
	void sticky_modifiers_changed(quint8 modifiers);

	/* The LOCKED set changed. A separate signal because it survives a
	 * keystroke and the armed set does not, so a listener that cares
	 * about only one of them can say so. */
	void locked_modifiers_changed(quint8 modifiers);

	/* The HELD set changed: a finger came down on a modifier or left it. */
	void held_modifiers_changed(quint8 modifiers);

private:
	quint8 modifiers_for(QKeyEvent *event) const;

	/*
	 * Steals a chord arriving through send_key -- the on-screen keyboard's
	 * path -- the way handle_key steals one from a hardware key: emits
	 * shortcut_pressed and returns true when the emulator key and
	 * modifiers name a stolen chord, so the caller sends nothing to the
	 * remote (sec 6.1).
	 */
	bool steal_chord(int key, quint8 modifiers);

	/* Sends the target of the first remap whose "from" `matches`. */
	bool send_remap(const std::function<bool(const QKeyCombination &)> &matches);


	/* Applied to the next key and then gone; see modifier_state_t. */
	void spend_sticky_modifiers();

	/*
	 * The whole event for the kitty modes above disambiguate, or false
	 * when this key is not one they describe.
	 */
	bool kitty_event_for(QKeyEvent *event, bssh_emulator::key_event_t &out) const;

	/* Presses sent as whole events, by Qt key, awaiting their release. */
	QHash<int, bssh_emulator::key_event_t> m_kitty_pressed;

	bssh_emulator *m_emulator;
	bool m_caps_as_ctrl;
	/* Armed, locked and held: one state, shared with the on-screen keyboards. */
	sk_modifiers m_modifiers;
};

#endif /* BSSH_INPUT_ROUTER_H */
