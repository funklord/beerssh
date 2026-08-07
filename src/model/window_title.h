#ifndef BSSH_WINDOW_TITLE_H
#define BSSH_WINDOW_TITLE_H

#include <QString>

class bssh_session;

/*
 * Expands the window-title format (project.md sec 8.3).
 *
 * A format rather than a choice from a list, which is where Konsole,
 * xterm and tmux all arrived: everybody wants a different one of the same
 * four facts, and a list long enough to cover them is longer than the
 * format it replaces.
 *
 *   %t  what the session calls itself -- the OSC title the remote set, or
 *       the saved name until it does. The one most people want.
 *   %n  the saved name, never the remote's
 *   %u  user            %h  host            %p  port
 *   %a  this app's name
 *   %%  a literal per cent
 *
 * A free function over a session rather than a method on the window,
 * because it is a pure mapping and the window is the one place it cannot
 * be tested without building a widget tree.
 *
 * A null session expands to the app name alone: a window with no session
 * still needs something in the task switcher, and "%t - BeerSSH" with an
 * empty %t would put a stray dash there.
 */
QString bssh_expand_window_title(const QString &format, const bssh_session *session);

#endif /* BSSH_WINDOW_TITLE_H */
