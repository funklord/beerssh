#ifndef BSSH_DOCUMENT_PATH_H
#define BSSH_DOCUMENT_PATH_H

#include <QString>

/*
 * Turning what a platform hands over into something QFile can open
 * (project.md sec 10.5).
 *
 * Three things arrive at an import and only one of them is a path: a
 * plain filename, a file:// URL (whenever something hands the app a
 * document by URL rather than by name), and Android's content:// URI.
 *
 * Here rather than beside either caller because getting it wrong is
 * silent in its cause and confusing in its effect -- QFile takes a PATH,
 * so it opens "file:///sdcard/x.ini" as a relative filename that does not
 * exist, and the import reports "not a preset" about a perfectly good
 * preset. Two copies of that rule are two chances to fix it once.
 */

/* file:// to a path; anything else unchanged. */
QString bssh_normalise_document_path(const QString &path);

/*
 * Whether QFile-and-friends can treat this as an ordinary path. content://
 * is the only thing that genuinely cannot: QFile opens one, but APIs that
 * take a filename (QSettings) cannot, so those callers bridge the bytes
 * through a temporary file instead.
 */
bool bssh_is_local_document_path(const QString &path);

#endif /* BSSH_DOCUMENT_PATH_H */
