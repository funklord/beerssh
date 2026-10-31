#ifndef BSSH_WORKING_DIRECTORY_H
#define BSSH_WORKING_DIRECTORY_H

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <functional>

/*
 * Where "here" is, for a file dropped on a terminal (sec 6.4): the
 * directory the shell on the other end is in, found without the shell's
 * help, so that it works whatever shell and prompt are there.
 *
 * Over SSH a short POSIX script runs on a second channel of the same
 * connection. Its own environment carries the connection's
 * SSH_CONNECTION -- the same for every channel of it, through a jump host
 * as well -- so among the processes in /proc it finds the one leading a
 * terminal with that value: our shell. It reports that shell's directory
 * and the program in the terminal's foreground, which is what a nested
 * `ssh` or a `cd` inside a sub-shell changes. A remote with no /proc says
 * only its host name, and the shell's own report (OSC 7) or the home
 * directory stands in.
 */
struct bssh_working_directory_t {
	QString host;            /* the remote's own name, uname -n */
	bool proc = false;       /* it has a /proc to look in at all */
	QString path;            /* the directory, empty when unknown */
	QString foreground;      /* the program in the terminal's foreground */

	/*
	 * The foreground program is a connection onward -- ssh, mosh, telnet:
	 * "here" is another machine, which this connection cannot write to.
	 */
	bool elsewhere = false;
};

/* The script, as one line for the remote's login shell to run. */
QString bssh_working_directory_probe();

/* What the script printed, read into the above. */
bssh_working_directory_t bssh_parse_working_directory(const QByteArray &output);

/*
 * The same answer for a shell on this machine, read straight from /proc:
 * `shell` is its pid. Empty where there is no /proc.
 */
bssh_working_directory_t bssh_local_working_directory(qint64 shell);

/*
 * Copies `paths` into the local `directory` under free names, calling
 * `progress(done, total)` as it goes; what it wrote, or `why` it stopped.
 * Run on a thread of its own by the local transport.
 */
bool bssh_copy_into(const QStringList &paths, const QString &directory, QStringList &written,
                    QString &why, const std::function<void(qint64, qint64)> &progress,
                    const std::function<bool()> &stopped = {});

/* What a copy says when it was stopped on purpose, not by a fault. */
extern const char *const BSSH_COPY_STOPPED;

/* A program that connects onward to another machine. */
bool bssh_is_onward_program(const QString &name);

/*
 * The directory a shell reported with OSC 7 -- "file://host/path" --
 * when it names this machine: the host empty, "localhost", or `host`.
 * Empty otherwise, since a report from a host further on is about a
 * directory this connection cannot reach.
 */
QString bssh_reported_directory(const QString &report, const QString &host);

/*
 * `name`, or the first of "name (1).ext", "name (2).ext" ... that `taken`
 * says is free: a dropped file never replaces one already there (the
 * holder, 2026-10-31). The extension is what follows the last dot, so
 * "notes.tar.gz" becomes "notes.tar (1).gz" -- a rule a reader can
 * predict -- and a leading dot is no extension: ".bashrc (1)".
 */
QString bssh_free_name(const QString &name, const std::function<bool(const QString &)> &taken);

#endif /* BSSH_WORKING_DIRECTORY_H */
