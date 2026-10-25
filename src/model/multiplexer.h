#ifndef BSSH_MULTIPLEXER_H
#define BSSH_MULTIPLEXER_H

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>

/*
 * tmux and screen sessions offered on connect (project.md sec 10.2a).
 *
 * When a shell connects, the sessions already running there are listed on
 * the side -- a second SSH channel, or a local process -- and if there are
 * any, a drawer offers them. Nothing is shown when there are none.
 *
 * Choosing one replaces the tab's shell with a command that attaches to it
 * and leaves a login shell behind when detached (restart_shell() on the
 * transport). Nothing is typed into the terminal: once attached, whatever
 * runs there -- vim inside tmux -- would get the keystrokes instead. The
 * same command is the shell of every reconnect, and choosing another
 * replaces it again: the old client's terminal hangs up and it detaches.
 */

struct bssh_mux_session_t {
	enum kind_t { TMUX, SCREEN };

	kind_t kind = TMUX;

	/* tmux's session name, or screen's "pid.name" -- what attaching names. */
	QString name;

	/* "3 windows", or screen's own word for its state. For the menu. */
	QString detail;

	/* Somebody is attached to it already, here or elsewhere. */
	bool attached = false;

	/* The one this tab is showing; set by the session, not the listing. */
	bool current = false;

	bool operator==(const bssh_mux_session_t &other) const {
		return kind == other.kind && name == other.name;
	}
};

/* Carried by bssh_session::multiplexers_found(). */
Q_DECLARE_METATYPE(bssh_mux_session_t)

/*
 * The shell line that lists both, for an SSH exec request or a local
 * /bin/sh -c. Wrapped in sh -c for SSH, where the far end hands the line
 * to the account's own shell and that may be fish or csh: the listing
 * itself is POSIX, and only the wrapper has to read in every shell.
 */
QString bssh_mux_listing_script();
QString bssh_mux_listing_command();

/* What that line printed, as sessions: tmux's first, in the order listed. */
QList<bssh_mux_session_t> bssh_mux_parse(const QByteArray &output);

/*
 * The line typed into the shell to attach to it. tmux's name is matched
 * exactly ("=name"), so "work" does not attach to "workshop".
 */
QString bssh_mux_attach_command(const bssh_mux_session_t &session);

/* How the drawer names it: "tmux  work  (3 windows, attached)". */
QString bssh_mux_label(const bssh_mux_session_t &session);

/*
 * The tab's command for showing it: attach, and once detached a login
 * shell, so detaching leaves a prompt rather than closing the tab. Local
 * runs it through /bin/sh; over SSH it is one string for the account's
 * shell, wrapped in sh -c like the listing.
 */
QString bssh_mux_switch_script(const bssh_mux_session_t &session);
QStringList bssh_mux_switch_command(const bssh_mux_session_t &session, bool local);

#endif /* BSSH_MULTIPLEXER_H */
