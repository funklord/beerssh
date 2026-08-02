#ifndef BSSH_KEY_ROW_LAYOUT_H
#define BSSH_KEY_ROW_LAYOUT_H

#include <QList>
#include <QString>

/*
 * bssh_key_row_layout -- what the on-screen key row holds (project.md
 * sec 6.2, sec 10.2).
 *
 * sec 6.2 says the row's layout is a preset-owned setting "so different
 * server types can get different key rows", and the starter presets are
 * named for exactly those types: a network switch, an OpenWrt box, a
 * tmux-first workflow. Until now the row was one hardcoded strip and the
 * preset field selecting between layouts had nothing to select from.
 *
 * The keys that matter genuinely differ by what is on the other end. A
 * Cisco-style console is driven by `?` and Tab far more than by arrows; a
 * tmux user needs one chord more than anything else, and it is a chord no
 * soft keyboard can type; a BusyBox box has no readline, so history keys
 * are wasted space on it.
 *
 * Same shape as the font and colour-scheme sets (sec 9.2, sec 10.2):
 * curated, compiled in, addressed by a stable id, identical on both
 * platforms so a preset means the same thing wherever it is opened.
 */

struct bssh_key_t {
	enum kind_t {
		/* A named key: Esc, Tab, an arrow. Goes through the router so a
		 * sticky modifier applies to it. */
		KEY,

		/* Ctrl or Alt: sticky, applying to the NEXT key pressed. */
		MODIFIER,

		/* A character the soft keyboard buries two layers deep. */
		TEXT,

		/*
		 * A modifier and a key in one press -- tmux's Ctrl-B, screen's
		 * Ctrl-A. This exists because those are the chords a touch user
		 * cannot type at all: two sticky presses to reach one prefix,
		 * dozens of times an hour, is the difference between usable and
		 * not.
		 */
		CHORD
	};

	QString label;
	kind_t kind;

	/* A bssh_emulator key code, a modifier mask, or a codepoint,
	 * depending on kind. CHORD uses both value and modifier. */
	int value;
	quint8 modifier;

	bssh_key_t() : kind(KEY), value(0), modifier(0) {}
};

struct bssh_key_row_layout_t {
	QString id;
	QString display_name;

	/* Shown under the picker: which end of a connection this suits. */
	QString description;

	QList<bssh_key_t> keys;
};

class bssh_key_row_catalog {
public:
	static bssh_key_row_catalog &instance();

	bssh_key_row_catalog();

	const QList<bssh_key_row_layout_t> &all() const { return m_layouts; }
	const bssh_key_row_layout_t *by_id(const QString &id) const;

	/* Empty means the default; an unknown id passes through unchanged, so
	 * a preset from a newer build still shows what it asked for. */
	QString effective_id(const QString &stored) const;
	QString default_id() const;

private:
	QList<bssh_key_row_layout_t> m_layouts;
};

#endif /* BSSH_KEY_ROW_LAYOUT_H */
