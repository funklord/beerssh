#ifndef BSSH_HEADLESS_H
#define BSSH_HEADLESS_H

#include <QByteArray>
#include <QtGlobal>

/*
 * Drops QT_QPA_PLATFORMTHEME when the platform is offscreen. Call before the
 * QApplication is made.
 *
 * The theme plugin opens a display of its own whatever the platform is: with
 * QT_QPA_PLATFORMTHEME=gtk3 exported -- the default on a GNOME or XFCE
 * desktop -- and no display, the suite died on "Gtk-WARNING: cannot open
 * display" before running a test, a message naming GTK rather than the
 * environment. Measured here 2026-10-08 with both plugins installed (gtk3,
 * lxqt), reported from hembygd. Latent on this desktop, which exports
 * neither, which is why nobody had met it.
 *
 * In the program rather than in the Makefile, because somebody chasing a
 * failure runs the binary directly. An offscreen run has nothing to theme.
 */
inline void bssh_drop_theme_when_offscreen() {
	if (qgetenv("QT_QPA_PLATFORM") == QByteArrayLiteral("offscreen")) {
		qunsetenv("QT_QPA_PLATFORMTHEME");
	}
}

#endif /* BSSH_HEADLESS_H */
