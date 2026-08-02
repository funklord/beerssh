#ifndef BSSH_INPUT_ROUTER_H
#define BSSH_INPUT_ROUTER_H

#include <QObject>
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
	 * Chords the application keeps rather than sending to the remote.
	 * Deliberately a short list: every chord stolen here is one the
	 * remote shell can never receive again (sec 6.1), so this stays
	 * minimal until it is user-remappable.
	 */
	enum shortcut_t {
		SHORTCUT_NONE,
		SHORTCUT_COPY,
		SHORTCUT_PASTE
	};

signals:
	void shortcut_pressed(bssh_input_router::shortcut_t shortcut);
	void sticky_modifiers_changed(quint8 modifiers);

private:
	quint8 modifiers_for(QKeyEvent *event) const;

	bssh_emulator *m_emulator;
	quint8 m_sticky;
};

#endif /* BSSH_INPUT_ROUTER_H */
