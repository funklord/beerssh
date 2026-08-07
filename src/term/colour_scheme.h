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

/*
 * The minimum contrast a drawn colour is allowed to have against what it
 * is drawn on, as a WCAG ratio (project.md sec 9.3).
 *
 * 3:1 rather than 4.5:1, and the difference is arithmetic rather than
 * leniency: blue contributes 0.0722 of luminance, so against a near-black
 * background no recognisably saturated red or blue can reach 4.5 at all
 * -- pure red tops out at 4.26. A floor nothing can satisfy is not a
 * standard, it is a way of getting the rule switched off.
 */
const double BSSH_CONTRAST_FLOOR = 3.0;

/* WCAG relative luminance and contrast, exposed because three callers
 * need them and a fourth copy is how they stop agreeing. */
double bssh_relative_luminance(QRgb colour);
double bssh_contrast_ratio(QRgb a, QRgb b);

/*
 * The same colour, lightened or darkened only as far as it takes to clear
 * the floor against `against`, or returned untouched if it already does.
 *
 * Applied when DRAWING rather than stored in the palette, which is the
 * whole point: the scheme keeps saying what its author published -- so a
 * user can still check it against the source -- while what reaches the
 * screen stays legible. It also means a scheme added later is covered
 * without anybody editing it.
 *
 * Hue and saturation are preserved, so a red that has been lifted is
 * still recognisably the red the program asked for. The direction is
 * away from the background: lighter on a dark one, darker on a light one,
 * which is the only direction that can gain contrast.
 */
QRgb bssh_raise_contrast(QRgb colour, QRgb against);

/*
 * The same colour, scaled toward black (project.md sec 9.3).
 *
 * `percent` is 100 for untouched and 0 for black. Applied to whatever
 * DRAWS -- glyphs and the cursor -- and never to a background, so the
 * black stays pure black at every setting and only the light on it comes
 * down. Dimming the background too would grey the screen out, which is
 * the opposite of what a dark room wants.
 *
 * Applied AFTER the contrast floor, deliberately. The floor is a default
 * for legibility; this is the user saying they want it dimmer than that,
 * and a slider the floor could veto would be a slider that stops working
 * exactly where somebody reached for it. Order matters the other way too:
 * dimming first would simply be undone, since the floor lifts until it
 * clears.
 */
QRgb bssh_dim(QRgb colour, int percent);

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
