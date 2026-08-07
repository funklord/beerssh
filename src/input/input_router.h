#ifndef BSSH_INPUT_ROUTER_H
#define BSSH_INPUT_ROUTER_H

#include <QObject>
#include <QHash>
#include <QList>
#include <QString>

class QKeyEvent;
class bssh_emulator;

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
class bssh_input_router : public QObject {
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
	 * Committed text from an input method, which is NOT the same path as
	 * a paste: no bracketing, because the user typed it (sec 6.1). On
	 * Android some keys only ever arrive this way; on desktop this is
	 * where dead keys and compose sequences land.
	 */
	void send_text(const QString &text);

	void paste(const QString &text);

	/*
	 * Sticky modifiers, for the on-screen key row (sec 6.2): tapping Ctrl
	 * then C must produce Ctrl-C, because a touch screen cannot hold two
	 * keys at once. Cleared as soon as they are applied.
	 */
	void set_sticky_modifiers(quint8 modifiers) { m_sticky = modifiers; }
	quint8 sticky_modifiers() const { return m_sticky; }

	/* Feeds a key from the on-screen row through the same translation. */
	void send_key(int key, quint8 modifiers);

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
		SHORTCUT_NEW_WINDOW
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

	/* What a chord is bound to now, as text -- empty when unbound. */
	static QString binding_for(const QString &action_name);

signals:
	void shortcut_pressed(bssh_input_router::shortcut_t shortcut);
	void sticky_modifiers_changed(quint8 modifiers);

private:
	quint8 modifiers_for(QKeyEvent *event) const;

	bssh_emulator *m_emulator;
	bool m_caps_as_ctrl;
	quint8 m_sticky;
};

#endif /* BSSH_INPUT_ROUTER_H */
