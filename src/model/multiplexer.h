#ifndef BSSH_MULTIPLEXER_H
#define BSSH_MULTIPLEXER_H

#include <QByteArray>
#include <QList>
#include <QMetaType>
#include <QString>

/*
 * tmux and screen sessions offered on connect (project.md sec 10.2a).
 *
 * When a shell connects, the sessions already running there are listed on
 * the side -- a second SSH channel, or a local process -- and if there are
 * any, a menu asks which to open. Nothing is shown when there are none.
 * The choice is typed into the shell, as the on-connect command is, so it
 * is the same locally and over SSH, a reconnect types it again, and
 * detaching leaves a shell rather than closing the tab.
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

/* How the menu names it: "tmux  work  (3 windows, attached)". */
QString bssh_mux_label(const bssh_mux_session_t &session);

#endif /* BSSH_MULTIPLEXER_H */
