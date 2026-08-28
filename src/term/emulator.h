#ifndef BSSH_EMULATOR_H
#define BSSH_EMULATOR_H

#include <QByteArray>
#include <QObject>
#include <QString>

#include "term/term_features.h"

#include "term/image_store.h"
#include "term/screen.h"
#include "term/scrollback.h"

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
	/*
	 * Synchronised output, DEC 2026 (sec 7.1, tier 3). While a remote has
	 * it set, the screen is being rewritten and must not be painted
	 * half-done -- the tearing it removes is the flicker every full-screen
	 * app shows on a slow link.
	 */
	bool synchronised_output() const;

	/*
	 * The URI behind a cell's link id, or empty. Looked up rather than
	 * stored per cell, so a wrapped link costs two bytes a cell instead
	 * of a copy of the URL.
	 */
	QString link_uri(quint16 id) const;

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

	/*
	 * Mouse, forwarded to the remote when it has asked for it (sec 7.1,
	 * tier 1). The encoding -- X10, SGR 1006, which buttons, whether
	 * motion counts -- is libvterm's, and it tracks which modes the
	 * remote enabled, so this is a pass-through rather than a second
	 * implementation.
	 */
	bool mouse_reporting() const;
	void send_mouse_move(int row, int col, quint8 modifiers);
	void send_mouse_button(int button, bool pressed, quint8 modifiers);

	/*
	 * Whether the remote is on the ALTERNATE screen -- vim, less, htop
	 * (sec 7). Nothing scrolls off it, so while it is up the scrollback
	 * holds the SHELL's output rather than the program's, and a view
	 * that scrolls back into it draws two programs at once.
	 */
	bool alternate_screen() const;

	/*
	 * Focus reporting, DEC mode 1004 (sec 7.1, tier 1).
	 *
	 * The remote asks to be told when the terminal gains and loses focus,
	 * and an editor uses it to reload a file changed underneath it -- vim
	 * with autoread, and every editor with a "check for external changes"
	 * behaviour. libvterm parses the mode and emits the reports; nothing
	 * was telling it, so the mode could be enabled and no report ever
	 * followed, which looks to the remote exactly like a terminal that
	 * does not support it.
	 *
	 * Silent unless the remote asked: libvterm writes nothing when the
	 * mode is off, so callers say what happened and never check first.
	 */
	void set_focused(bool focused);

	/*
	 * Put every MODE back the way a terminal starts, and leave the screen
	 * and the scrollback alone (sec 5.3).
	 *
	 * A reconnect keeps this emulator on purpose, so the user keeps their
	 * history across a drop -- but the shell that comes back is genuinely
	 * new and set none of the modes the old one's programs did. Left
	 * alone they carry over: mouse reporting, so every click writes a
	 * report onto a command line that never asked; bracketed paste, so a
	 * paste arrives wrapped in markers the new shell prints; application
	 * cursor keys, so the arrows send bytes it does not know; and the
	 * alternate screen, so a link that dropped while an editor was up
	 * comes back with the shell drawing into the editor's buffer and the
	 * history hidden behind it.
	 */
	void reset_modes();

	void feed(const QByteArray &bytes);
	void resize(int cols, int rows);

	const bssh_screen &screen() const;

	/*
	 * Images placed on the grid (sec 7.1, tier 4). The store is the
	 * emulator's, like the screen: the view renders it and owns none of
	 * it, so a view can be destroyed and rebuilt without losing images.
	 */
	const bssh_image_store &images() const;

	/*
	 * Lines that have scrolled off the top (sec 7). The view reads this
	 * to render a scrolled-back position; nothing else writes it.
	 */
	const bssh_scrollback &scrollback() const;
	void set_scrollback_lines(int lines);

	/*
	 * Cell size in pixels, which the emulator cannot work out for itself
	 * and needs in order to turn an image's pixel dimensions into a cell
	 * footprint. The view is the only thing that knows it, so the view
	 * tells it -- and must, or images land with a zero-cell footprint.
	 */
	void set_cell_pixels(int width, int height);

	/*
	 * The TERM the app is telling the remote (sec 10.2). Mirrored here
	 * only so XTGETTCAP's TN answers with the same string: a terminal
	 * that names itself one thing in the environment and another when
	 * asked directly is worse than one that will not say.
	 */
	void set_term_name(const QString &term);

	/*
	 * Which of sec 7.1's extensions this terminal speaks. Everything on
	 * by default; this exists to turn things OFF, so that a TUI can be
	 * run against a terminal that really lacks a feature rather than
	 * against its own idea of one.
	 *
	 * Switching one off silences what the terminal ANSWERS about it as
	 * well as what it does -- an extension that is off but still
	 * advertised is the failure sec 7.1 records for DEC 2026, where a
	 * client was told the mode existed and the implementation behind it
	 * was never reached.
	 */
	void set_features(const bssh_term_features_t &features);
	const bssh_term_features_t &features() const;

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

	/*
	 * The remote asked to put text on the user's clipboard (OSC 52, sec
	 * 7.1). A REQUEST, not an instruction: whether it is honoured is the
	 * host's per-host setting, and the decision is made where the setting
	 * is known rather than here. The emulator's job is to parse the
	 * sequence and say what it asked for.
	 *
	 * There is deliberately no signal for the read half. A remote asking
	 * to be sent the local clipboard is an exfiltration channel for
	 * whatever the user last copied, and no setting makes that acceptable
	 * -- the risk is not theirs to accept on behalf of the password they
	 * copied ten minutes ago. The query is swallowed.
	 */
	void clipboard_write_requested(const QString &text);
	void output_ready();

	/* An image arrived or was removed; the view repaints. */
	void images_changed();

	/* A line entered or left the scrollback. */
	void scrollback_changed();

	/*
	 * The remote moved between the primary and the alternate screen.
	 * A signal rather than something to notice on the next damage: what
	 * depends on it is the view's scroll position, and a program that
	 * takes the screen and has not drawn yet gives no damage to notice
	 * it on.
	 */
	void alternate_screen_changed(bool alternate);

private:
	bssh_emulator_private *m_private;
};

#endif /* BSSH_EMULATOR_H */
