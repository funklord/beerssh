#ifndef BSSH_STORE_VIEW_H
#define BSSH_STORE_VIEW_H

#include <QString>
#include <QStringList>

class bssh_main_window;

/*
 * The Play listing's screenshots, taken from the build being released
 * (the play-store skill; `make android-screenshots`). The target starts
 * the app with the string extra `store_view` naming one of these, and
 * waits for "store-view-ready <name>" in the log before capturing.
 *
 * What the pictures show is DEMO content written below, never the
 * user's: the listing publishes them. So the stores are moved out of
 * the way first (bssh_store_view_isolate) and nothing connects --
 * the sessions are ordinary ones that never dial, with their screens
 * filled in directly. No pretend transport, for the reason
 * test/fake_ssh_session.h gives: the shipping app must not carry an SSH
 * backend that talks to nobody.
 */

/* The views there are, in listing order. */
QStringList bssh_store_view_names();

/*
 * Before anything opens a store: Qt's test mode, which moves every
 * QStandardPaths location the stores use, and an empty one each run so
 * every capture starts from the same state. False -- and the caller
 * must not go on -- when the move could not be confirmed, because the
 * alternative is a screenshot of somebody's hosts.
 */
bool bssh_store_view_isolate();

/* Builds `name` in `window` and logs the ready line once it is drawn. */
void bssh_store_view_show(bssh_main_window *window, const QString &name);

#endif /* BSSH_STORE_VIEW_H */
