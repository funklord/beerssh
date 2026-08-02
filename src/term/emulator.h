#ifndef BSSH_EMULATOR_H
#define BSSH_EMULATOR_H

#include <QByteArray>
#include <QObject>
#include <QString>

#include "term/screen.h"

struct bssh_emulator_private;

/*
 * bssh_emulator -- bytes from the SSH channel in, a screen out
 * (project.md sec 7).
 *
 * The parser is vendored libvterm, and no libvterm type appears in this
 * header -- the implementation is behind an opaque pointer for the same
 * reason bssh_ssh_session hides libssh: sec 16 lists the emulator choice
 * as an open decision, and it stays reversible only while nothing outside
 * emulator_vterm.cpp includes vterm.h.
 *
 * There is no PTY under this. The byte stream comes from the SSH channel
 * and nowhere else, on both platforms (sec 3, core principle 1).
 */
class bssh_emulator : public QObject {
	Q_OBJECT

public:
	bssh_emulator(int cols, int rows, QObject *parent = nullptr);
	~bssh_emulator() override;

	/*
	 * Key entry points. The escape sequences a key produces depend on the
	 * terminal's current modes -- application cursor keys, application
	 * keypad, bracketed paste -- which only the emulator knows, so the
	 * translation belongs here and the bytes come out through the same
	 * output path as the emulator's own replies (sec 6.3).
	 *
	 * The tables behind this are libvterm's, not ours. Hand-writing an
	 * escape-sequence table for every key x modifier x mode combination
	 * is exactly the kind of thing the vendored library already gets
	 * right, and getting it subtly wrong is invisible until someone
	 * presses Shift-F7 in a full-screen editor.
	 */
	enum key_t {
		KEY_NONE,
		KEY_ENTER,
		KEY_TAB,
		KEY_BACKSPACE,
		KEY_ESCAPE,
		KEY_UP,
		KEY_DOWN,
		KEY_LEFT,
		KEY_RIGHT,
		KEY_INSERT,
		KEY_DELETE,
		KEY_HOME,
		KEY_END,
		KEY_PAGE_UP,
		KEY_PAGE_DOWN,
		/* Function keys are KEY_FUNCTION_BASE + n, for F1..Fn. */
		KEY_FUNCTION_BASE = 256
	};

	enum modifier_t {
		MOD_NONE  = 0x00,
		MOD_SHIFT = 0x01,
		MOD_ALT   = 0x02,
		MOD_CTRL  = 0x04
	};

	/*
	 * Keyboard-protocol state (sec 7.1, tier 2). Legacy terminal input
	 * cannot distinguish Ctrl+I from Tab, or Alt+x from Escape then x --
	 * these are the two protocols that fix that, and a remote program
	 * turns on whichever it prefers.
	 */
	int kitty_keyboard_flags() const;
	int modify_other_keys_level() const;

	void send_key(int key, quint8 modifiers);
	void send_char(char32_t ch, quint8 modifiers);

	/*
	 * Wraps a paste in the bracketed-paste markers when the remote has
	 * asked for them. Bypassing this and writing the text straight to the
	 * channel is what makes a paste into vim indent itself to death.
	 */
	void send_paste(const QString &text);

	void feed(const QByteArray &bytes);
	void resize(int cols, int rows);

	const bssh_screen &screen() const;

	/*
	 * Bytes the terminal owes the remote: answers to device-status and
	 * primary-attribute queries, and later the mouse and bracketed-paste
	 * reports. A caller that never drains this hangs any program that
	 * asks the terminal a question and waits for the reply -- which is
	 * most full-screen editors, at startup.
	 */
	QByteArray take_output();

signals:
	/* The screen changed; repaint. Coalesced by the view, not here. */
	void damaged();

	/* OSC 0/2. Feeds the tab label and, on desktop, the window title. */
	void title_changed(const QString &title);

	void bell();
	void output_ready();

private:
	bssh_emulator_private *m_private;
};

#endif /* BSSH_EMULATOR_H */
