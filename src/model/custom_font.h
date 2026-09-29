#ifndef BSSH_CUSTOM_FONT_H
#define BSSH_CUSTOM_FONT_H

#include <QString>

/*
 * The user's own font file (project.md sec 9.4 -- the reserved "custom"
 * entry, the holder's answer of 2026-09-29).
 *
 * One at a time, as the custom colour scheme and keyboard layout are one
 * at a time: importing another replaces it. The file is copied into app
 * storage under a fixed name, so the original can move or go and the
 * font stays, and it is loaded at every start after the set.
 *
 * Refused unless it is monospace by measurement, not by its own flag:
 * sec 9.1 excludes proportional fonts because a character grid cannot
 * hold one, and a font's fixed-pitch bit is its author's claim. Its
 * licence is the user's business; it is their file.
 */

/* Where the imported file lives: app storage, beside the downloads. */
QString bssh_custom_font_directory();

/*
 * Points it somewhere else, or back with an empty string -- for the
 * tests, which would otherwise write into the real app storage.
 */
void bssh_set_custom_font_directory(const QString &directory);

/*
 * Copies `source` -- a path, or a content:// URL on Android -- into the
 * custom font directory, registers it, and tells the catalog. The family
 * it registered comes back through `family`; a refusal leaves whatever
 * was there before in place and says why through `error`.
 */
bool bssh_import_custom_font(const QString &source, QString *family, QString *error);

/*
 * Registers the imported file, if there is one, and tells the catalog.
 * Called once at start, after the bundled fonts. Returns the family, or
 * an empty string.
 */
QString bssh_load_custom_font();

/* Whether every glyph of `family` a terminal cell holds has one width. */
bool bssh_family_is_monospace(const QString &family);

#endif /* BSSH_CUSTOM_FONT_H */
