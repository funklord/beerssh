#ifndef BSSH_TERM_FEATURES_H
#define BSSH_TERM_FEATURES_H

#include <QByteArray>
#include <QString>
#include <QStringList>

/*
 * bssh_term_features_t -- which of project.md sec 7.1's extensions this
 * terminal speaks.
 *
 * Everything is on by default and this type exists to turn things OFF.
 * That is not a preference knob: it is a test instrument. A TUI library
 * has to work against terminals that lack each of these, and the only
 * honest way to find out is to run it against a terminal that really
 * lacks them -- not one that has been asked to pretend in the library's
 * own test double, which tests the double.
 *
 * The names are sec 7.1's own, tier by tier, so that a spec string and
 * the document read the same way.
 *
 * THE RULE THAT MAKES THIS WORTH BUILDING: switching a feature off must
 * change what the terminal ANSWERS about it, not only what it does.
 * sec 7.1 already decided this for XTGETTCAP -- "a capability database
 * is a promise: a client told `bce` will use it" -- and it is the same
 * promise every probe makes. A feature that is off but still advertised
 * is worse than one that is on, because the client is told to go ahead
 * and then nothing happens; that is the exact failure sec 7.1 records
 * for DEC 2026 answering "not recognised" while the implementation
 * behind it was never reached.
 */
struct bssh_term_features_t {
	enum feature_t {
		/* Tier 1: a terminal is broken without these. */
		BRACKETED_PASTE,     /* DEC 2004 */
		SGR_MOUSE,           /* DEC 1006 and the modes that feed it */
		FOCUS_REPORTING,     /* DEC 1004 */

		/* Tier 2: keyboard disambiguation. */
		KITTY_KEYBOARD,      /* CSI > flags u, and the query */
		MODIFY_OTHER_KEYS,   /* CSI > 4 ; n m, and XTQMODKEYS */

		/* Tier 3. */
		SYNCHRONISED_OUTPUT, /* DEC 2026 */
		HYPERLINKS,          /* OSC 8 */
		CLIPBOARD_WRITE,     /* OSC 52, write half only */
		XTGETTCAP,           /* DCS + q */
		COLOUR_QUERY,        /* OSC 10 and OSC 11 with "?" */
		WINDOW_SIZE,         /* CSI 14 t and CSI 16 t */

		/* Tier 4: images. */
		SIXEL,               /* DCS q */
		KITTY_GRAPHICS,      /* APC G */
		ITERM2_IMAGES,       /* OSC 1337 */

		FEATURE_COUNT
	};

	/*
	 * The spec name, kebab-case, stable. It reaches a command line and a
	 * stored preset, so it is not something to rename casually -- see
	 * code-style.md on names that escape into something you cannot
	 * rename later.
	 */
	static const char *name(feature_t feature);

	/* The one-line description the help text prints. */
	static const char *description(feature_t feature);

	static bool from_name(const QString &name, feature_t &feature);


	bool enabled(feature_t feature) const { return m_on[feature]; }
	void set(feature_t feature, bool on) { m_on[feature] = on; }

	bool operator==(const bssh_term_features_t &other) const;
	bool operator!=(const bssh_term_features_t &other) const { return !(*this == other); }

	/*
	 * A spec is a comma-separated list, read left to right, starting
	 * from everything on:
	 *
	 *   none                  turn everything off
	 *   all                   turn everything on
	 *   -sixel                turn one off
	 *   +sixel                turn one on
	 *   none,+bracketed-paste everything off except one
	 *
	 * Left to right rather than as a set, because that is the only order
	 * in which "none,+x" and "all,-x" both mean what they look like.
	 *
	 * An unknown name is an ERROR rather than something ignored. A
	 * misspelled feature in a test harness would otherwise run the whole
	 * suite against a terminal with that feature still on, and report a
	 * pass for a configuration nobody tested.
	 */
	static bssh_term_features_t from_spec(const QString &spec, QString &error);

	/* Everything that is off, as `-name` -- empty when all are on. */
	QString to_spec() const;

	/*
	 * What --term-features said, for this whole process, or nothing.
	 *
	 * Process-wide and outranking the preset, and both halves of that
	 * are deliberate. A reduced terminal is an instruction about THIS
	 * RUN, typed by whoever is running the test -- so it applies to
	 * every session the run opens, including tabs opened later, and a
	 * stored preset must not quietly restore a feature the tester asked
	 * to be without. It is the one place in this project where the
	 * command line beats a preset, which is why it says so here rather
	 * than being inferred from where the call happens to sit.
	 *
	 * Set once from main() and never again; unset in every other
	 * process, which is why the getter reports whether there was one at
	 * all rather than returning a default-constructed set that would be
	 * indistinguishable from "all on, deliberately".
	 */
	static void set_process_override(const bssh_term_features_t &features);
	static bool process_override(bssh_term_features_t &features);

	/* Every name, for the help text and for a test to iterate. */
	static QStringList every_name();

	bssh_term_features_t() {
		for (int i = 0; i < FEATURE_COUNT; ++i) m_on[i] = true;
	}

private:
	bool m_on[FEATURE_COUNT];
};

#endif /* BSSH_TERM_FEATURES_H */
