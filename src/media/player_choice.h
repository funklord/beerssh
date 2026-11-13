#ifndef BSSH_PLAYER_CHOICE_H
#define BSSH_PLAYER_CHOICE_H

#include <QList>
#include <QString>
#include <QStringList>

/*
 * Which media player Play hands a file to (project.md sec 6.4), and how
 * it is started -- the holder, 2026-11-13: "a complete configuration of
 * media player to use and possibly custom command, sync this with hydra
 * which has a similar thing". hydra's player_launcher is the model: the
 * same ids, labels, settings keys, %U template and terminal rule, so the
 * two programs read alike. Two ids are BeerSSH's own: builtin, its player
 * (snurra), and system, the desktop's or the phone's choice -- hydra has
 * that one on Android only.
 *
 * A file here, not a stream: the target is a local path, as a file URL
 * where a command line asks for %U.
 */
struct bssh_player_entry_t {
	QString id;          /* "mpv" */
	QString label;       /* "mpv" */
	QString path;        /* resolved program; empty for builtin, system, or not installed */
	bool installed = false;
};

/* builtin, system, then mpv, VLC, SMPlayer, ffplay, mplayer2, mplayer,
 * then Custom -- each found on PATH now, the missing ones there to be
 * shown greyed out (hydra's sec 11.3). builtin only where it is built. */
QList<bssh_player_entry_t> bssh_players(const QString &custom_command);

/* The player in use: the setting, or the default -- builtin where it is
 * built in, system where not. A chosen external player that is not
 * installed is still returned; Play says so rather than guessing another. */
QString bssh_player_in_use();
QString bssh_player_default();

/* A program's arguments for `target` (hydra's player_launcher::arguments,
 * without the stream pairs): mpv opens a window, ffplay exits at the end,
 * mplayer caches; Custom substitutes %U, or appends the target when the
 * command names none. */
QStringList bssh_player_arguments(const QString &id, const QString &custom, const QString &target);

/* The players controlled from their terminal, which are started in one:
 * mplayer reads its keys from where it was started (hydra, 2026-10-22). */
bool bssh_player_wants_terminal(const QString &id);

/* The terminal as program and leading arguments: the configured one when
 * found, else the first known terminal installed, x-terminal-emulator -e
 * first. Empty for none. */
QStringList bssh_player_terminal(const QString &configured);

/*
 * The whole command line Play starts for the file at `path`, external
 * players only (not builtin or system); empty, with `error` filled, when
 * there is none to start -- the player not installed, Custom with no
 * command.
 */
QStringList bssh_player_command(const QString &id, const QString &path, QString *error);

/*
 * Plays the local file at `path` in the chosen player, unless that is
 * builtin, which the file panel plays itself: system through the
 * desktop's own opener, the rest started detached. False, with `error`
 * filled, when nothing was started.
 */
bool bssh_play_externally(const QString &path, QString *error);

#endif
