#ifndef BSSH_COLOUR_SCHEME_H
#define BSSH_COLOUR_SCHEME_H

#include <QList>
#include <QRgb>
#include <QString>

/*
 * bssh_colour_scheme -- the sixteen colours a terminal argues about
 * (project.md sec 10.2).
 *
 * sec 10.2 gives a preset a colour_scheme field and says nothing about what
 * one is, so this defines it, in the shape sec 9.2 already uses for fonts:
 * a curated set, compiled in, addressed by a stable id, identical on both
 * platforms so a profile means the same thing wherever it is opened.
 *
 * Only the low sixteen entries are a scheme's business. The 6x6x6 cube and
 * the grey ramp above them are exact formulas that every terminal computes
 * the same way, and a program asking for colour 137 has asked for a
 * specific RGB value rather than for "the user's fifth colour". Rewriting
 * those would make this terminal render 256-colour output differently from
 * every other one, which is a bug wearing a feature's clothes.
 *
 * A scheme also carries its own default foreground and background, because
 * those are the two the remote never sets and the user looks at all day.
 */

struct bssh_colour_scheme_t {
	QString id;
	QString display_name;

	/*
	 * Recorded for the same reason the fonts' licences are (sec 9.4):
	 * these are other people's designs, and a set that cannot say where
	 * each entry came from is a set nobody can check before shipping.
	 */
	QString licence;

	/* Whether it is meant to be read light-on-dark. Drives which of the
	 * two defaults a form factor starts from, and nothing else. */
	bool dark;

	/* ANSI 0-7 then the bright 8-15, in the usual order: black, red,
	 * green, yellow, blue, magenta, cyan, white. */
	QRgb ansi[16];

	QRgb background;
	QRgb foreground;

	/*
	 * The cursor, which is not always the foreground: a scheme with a low
	 * contrast body often wants a cursor that stands out from it.
	 */
	QRgb cursor;

	bssh_colour_scheme_t() : dark(true), ansi(), background(0), foreground(0), cursor(0) {}
};

class bssh_colour_scheme_catalog {
public:
	static bssh_colour_scheme_catalog &instance();

	bssh_colour_scheme_catalog();

	const QList<bssh_colour_scheme_t> &all() const { return m_schemes; }

	/* Null for an unknown id, which is a caller error worth seeing. */
	const bssh_colour_scheme_t *by_id(const QString &id) const;

	/*
	 * What a stored value means, with the same rule as fonts: empty is the
	 * default, a display name resolves to its id, and anything else is
	 * returned unchanged so an unknown scheme stays visible rather than
	 * being silently rewritten (sec 10.1).
	 */
	QString effective_id(const QString &stored) const;

	QString default_id() const;

private:
	QList<bssh_colour_scheme_t> m_schemes;
};

#endif /* BSSH_COLOUR_SCHEME_H */
