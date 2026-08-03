#ifndef BSSH_PRESET_H
#define BSSH_PRESET_H

#include <QJsonObject>
#include <QString>

/*
 * bssh_preset_t -- a reusable named template attached to many hosts
 * (project.md sec 10).
 *
 * Editing one changes every host using it; that is the entire point, and
 * why a preset is a separate thing rather than a pile of duplicated
 * per-host settings.
 *
 * **This struct is structurally incapable of holding a secret, and that is
 * the design (sec 10.3).** There is no field for a password, a key, a key
 * reference, a hostname, a username or a known-hosts entry -- so an export
 * path cannot serialise one by accident, no matter how it is written. That
 * is what makes a preset safe to share: it describes how a terminal should
 * behave, and reveals nothing about whose infrastructure it belongs to.
 *
 * Anything added here in future has to pass the same test: would sending
 * this file to a stranger tell them something about the sender's network
 * or credentials? If yes, it belongs on the host profile instead.
 */
struct bssh_preset_t {
	int id;
	QString name;

	/* Terminal behaviour (sec 10.2). */
	QString term;               /* TERM string */
	int scrollback_lines;
	bool bell_enabled;
	QString cursor_style;       /* block | underline | bar */
	bool cursor_blink;

	/* Appearance -- the font a host's output is read in (sec 9). */
	QString font_family;
	int font_size;
	QString colour_scheme;

	/*
	 * Whether this host may put things on the local clipboard (OSC 52,
	 * sec 7.1). Off by default and per host, because it is a question of
	 * trust rather than of taste: a build box of one's own is not a jump
	 * host shared with strangers, and a compromised remote that can
	 * rewrite the clipboard can swap a copied command for a destructive
	 * one and wait for it to be pasted.
	 */
	bool allow_clipboard_write;

	/* Connection behaviour. */
	int keepalive_seconds;
	bool compression;

	/*
	 * Run on connect -- the natural place to attach tmux, which is the
	 * available answer to sec 5.3's "a reconnect is a new shell".
	 */
	QString on_connect_command;

	/* The on-screen key row for this kind of host (sec 6.2). */
	QString key_row_layout;

	/*
	 * Shipped presets are cloned, not edited (sec 10.4), so a user cannot
	 * damage the starting points everyone else's clones came from.
	 */
	bool is_builtin;

	bssh_preset_t()
	    : id(0),
	      term(QStringLiteral("xterm-256color")),
	      scrollback_lines(5000),
	      bell_enabled(true),
	      cursor_style(QStringLiteral("block")),
	      cursor_blink(true),
	      font_size(11),
	      allow_clipboard_write(false),
	      keepalive_seconds(60),
	      compression(false),
	      is_builtin(false) {}

	/* JSON, human-readable and diffable, with a schema version (sec 10.5). */
	QJsonObject to_json() const;
	static bssh_preset_t from_json(const QJsonObject &object);

	/*
	 * Export and import, in the INI the store itself uses (sec 10.5, sec
	 * 10.6). One format rather than two: a second serialiser for sharing
	 * is a second serialiser to keep in step, and the first divergence
	 * shows up as a preset that imports subtly wrong.
	 *
	 * The path may be a content:// URI on Android, which QSettings cannot
	 * open and QFile can -- so the bytes are bridged through a temporary
	 * file rather than by hand-rolling an INI parser for one platform.
	 *
	 * id and is_builtin never travel: they are facts about THIS
	 * installation's store, and importing someone else's "id 4, shipped"
	 * would claim a slot and a status that are not theirs to give.
	 */
	bool write_ini(const QString &raw_path) const;
	static bssh_preset_t read_ini(const QString &raw_path, bool *ok = nullptr);

	/* The starter set (sec 10.4). Read-only; users clone them. */
	static QList<bssh_preset_t> builtins();
};

#endif /* BSSH_PRESET_H */
