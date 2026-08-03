#ifndef BSSH_THEME_H
#define BSSH_THEME_H

#include <QObject>
#include <QPalette>

#include "platform/platform.h"

/*
 * bssh_theme -- the app's light/dark appearance (project.md sec 6.2).
 *
 * Three states rather than a checkbox, the same shape as the on-screen
 * key row's: **System**, which follows the desktop and is the default,
 * plus Light and Dark for the person whose terminal should not match the
 * rest of their screen.
 *
 * The palette is built and applied HERE rather than asked of Qt.
 * QStyleHints::setColorScheme() looks like the right call and does
 * nothing on Linux -- measured on this tree's Qt 6.8.2 under offscreen
 * and xcb, with and without a platform theme: no signal, no palette
 * change. A setting that silently does nothing is worse than one that is
 * missing, so the app paints its own.
 *
 * What this does NOT touch is the terminal's colour scheme. That is a
 * per-host setting (sec 10.2) describing what the remote's output should
 * look like, and a dark terminal in a light window is a deliberate and
 * extremely common choice -- following the desktop there would override
 * a decision the user has already made per host.
 */
class bssh_theme : public QObject {
	Q_OBJECT

public:
	enum mode_t {
		MODE_SYSTEM,
		MODE_LIGHT,
		MODE_DARK
	};

	/*
	 * Applies a mode now, and keeps following the system while the mode
	 * is MODE_SYSTEM. Safe to call again to change mode.
	 */
	static void apply(mode_t mode);

	/* What MODE_SYSTEM currently resolves to; never Unknown. */
	static bssh_appearance_t resolved_system_appearance();

	/* What is actually being shown, whatever the mode asked for. */
	static bssh_appearance_t effective_appearance(mode_t mode);

	static QPalette palette_for(bssh_appearance_t appearance);

	static QString to_text(mode_t mode);
	static mode_t from_text(const QString &text);
};

#endif /* BSSH_THEME_H */
