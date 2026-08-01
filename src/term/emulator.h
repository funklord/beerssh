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
