#ifndef BSSH_KEPT_SCROLLBACK_H
#define BSSH_KEPT_SCROLLBACK_H

#include <QByteArray>
#include <QString>

class bssh_scrollback;

/*
 * A host's scrollback kept across restarts, for hosts that ask for it
 * (bssh_profile_t::keep_scrollback). Opt-in per host on the holder's
 * answer of 2026-09-26; off by default because scrollback holds whatever
 * was on screen, a password echoed by mistake and secrets in output
 * included, and this puts it on disk.
 *
 * One file per host, beside the app's other configuration, readable by
 * its owner alone. Cells rather than text, so the history comes back in
 * its colours; hyperlinks are not kept, since a link id means something
 * only inside the session that numbered it.
 */

/*
 * The history as bytes and back. Compressed, with a magic and a version
 * in front, so a file from something else -- or a later format -- is
 * refused rather than read as cells. `decode` fills `into` only on
 * success, and appends to what it already holds.
 */
QByteArray bssh_scrollback_encode(const bssh_scrollback &history);
bool bssh_scrollback_decode(const QByteArray &bytes, bssh_scrollback *into);

/*
 * Where a host's history lives, and the three things done with it.
 * `directory` is the configuration directory; empty means the app's own.
 * save() writes the whole file or nothing (QSaveFile), load() reports
 * whether there was one to read, and erase() is quiet about one that was
 * never there.
 */
QString bssh_kept_scrollback_path(int profile_id, const QString &directory = QString());
bool bssh_save_kept_scrollback(int profile_id, const bssh_scrollback &history,
                               const QString &directory = QString());
bool bssh_load_kept_scrollback(int profile_id, bssh_scrollback *into,
                               const QString &directory = QString());
void bssh_erase_kept_scrollback(int profile_id, const QString &directory = QString());

#endif /* BSSH_KEPT_SCROLLBACK_H */
