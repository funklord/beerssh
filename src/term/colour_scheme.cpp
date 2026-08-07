#include "term/colour_scheme.h"

#include <QColor>
#include <cmath>

namespace {

/*
 * The set. Four fixed schemes and a custom one, and the shape of the list
 * is the decision: a terminal is not a canvas.
 *
 * The palette argument is settled and has been for decades. What a program
 * means by \e[31m is "the user's red", and it is entitled to assume that
 * red is red -- so the sixteen are the CLASSIC ANSI set, the VGA text-mode
 * values every console, every DOS program and every ANSI art file was
 * written against. Normal 0-7 are the dimmed half and bright 8-15 the
 * saturated one, which is what makes bold mean something rather than
 * nothing.
 *
 * The designer palettes this list used to carry -- Solarized, Gruvbox,
 * Nord, Tango -- are gone. They were reproduced faithfully and that was
 * the problem: each one redefines what red is, so a program's colour
 * choices stop meaning what the program meant and start meaning what a
 * palette author preferred. Anyone who wants those numbers can still have
 * them through the custom scheme; they are no longer offered as though
 * choosing one were an ordinary thing to do.
 *
 * What a user actually wants to change is the colour of ORDINARY TEXT,
 * which is the one the remote never sets. Hence three on black -- white,
 * green, amber, the three phosphors terminals were actually built with --
 * and one inverse for e-paper, where a black background is a battery cost
 * and a readability loss rather than a preference.
 */
const QRgb BSSH_CLASSIC_ANSI[16] = {
	/* Normal: the dimmed half. Yellow is brown, as it has always been. */
	0x000000, 0xaa0000, 0x00aa00, 0xaa5500,
	0x0000aa, 0xaa00aa, 0x00aaaa, 0xaaaaaa,
	/* Bright: the saturated half, which is what bold selects. */
	0x555555, 0xff5555, 0x55ff55, 0xffff55,
	0x5555ff, 0xff55ff, 0x55ffff, 0xffffff
};

struct entry_t {
	const char *id;
	const char *display_name;
	const char *licence;
	bool dark;
	QRgb background;
	QRgb foreground;
	QRgb cursor;
};

/*
 * Backgrounds are PURE black or PURE white, not a near-black. #1c1c1c was
 * what this shipped with, and a background that is nearly black is the
 * worst of both: it costs an OLED panel its switched-off pixels and buys
 * no readability, since the contrast that matters is against the text.
 *
 * The id "default" is kept rather than renamed to what it now is, so that
 * every profile already saying "default" keeps working.
 */
const entry_t BSSH_SCHEMES[] = {
	{ "default", "White on black", "Classic ANSI", true,
		0x000000, 0xffffff, 0xffffff },

	{ "green-on-black", "Green on black", "Classic ANSI", true,
		0x000000, 0x00ff00, 0x00ff00 },

	{ "yellow-on-black", "Yellow on black", "Classic ANSI", true,
		0x000000, 0xffff00, 0xffff00 },

	/*
	 * Inverse, and named for who it is for. An e-paper display holds a
	 * light background for free and pays for a dark one, which is the
	 * reverse of every other panel -- so this is not "light mode", it is
	 * the mode that hardware wants.
	 */
	{ "black-on-white", "Black on white (e-paper)", "Classic ANSI", false,
		0xffffff, 0x000000, 0x000000 },

	/*
	 * The escape hatch, and the only place a user should have to think
	 * about sixteen numbers. It starts as a copy of the default and is
	 * overwritten from settings.
	 */
	{ "custom", "Custom", "This device", true,
		0x000000, 0xffffff, 0xffffff }
};

} /* namespace */

bssh_colour_scheme_catalog &bssh_colour_scheme_catalog::instance() {
	static bssh_colour_scheme_catalog catalog;
	return catalog;
}

bssh_colour_scheme_catalog::bssh_colour_scheme_catalog() {
	for (const entry_t &entry : BSSH_SCHEMES) {
		bssh_colour_scheme_t scheme;
		scheme.id = QString::fromLatin1(entry.id);
		scheme.display_name = QString::fromLatin1(entry.display_name);
		scheme.licence = QString::fromLatin1(entry.licence);
		scheme.dark = entry.dark;

		/*
		 * One palette, shared by every scheme. A scheme chooses the
		 * background and the colour of ordinary text; it does not get
		 * to redefine what the program meant by red.
		 */
		for (int i = 0; i < 16; ++i) {
			/* The table is written as 0xRRGGBB for readability; QRgb wants
			 * the alpha byte set or every colour is transparent. */
			scheme.ansi[i] = 0xff000000u | BSSH_CLASSIC_ANSI[i];
		}

		scheme.background = 0xff000000u | entry.background;
		scheme.foreground = 0xff000000u | entry.foreground;
		scheme.cursor = 0xff000000u | entry.cursor;

		m_schemes.append(scheme);
	}
}

const bssh_colour_scheme_t *bssh_colour_scheme_catalog::by_id(const QString &id) const {
	for (const bssh_colour_scheme_t &scheme : m_schemes) {
		if (scheme.id == id) return &scheme;
	}
	return nullptr;
}

QString bssh_colour_scheme_catalog::effective_id(const QString &stored) const {
	if (stored.isEmpty()) return default_id();
	if (by_id(stored)) return stored;

	for (const bssh_colour_scheme_t &scheme : m_schemes) {
		if (scheme.display_name.compare(stored, Qt::CaseInsensitive) == 0) return scheme.id;
	}

	/*
	 * Unknown names pass through rather than being guessed at, so a
	 * profile from a newer build keeps saying what it asked for. The view
	 * falls back to the default to draw with, and the picker shows the
	 * name so the user can see what is missing.
	 */
	return stored;
}

QString bssh_colour_scheme_catalog::default_id() const {
	return QStringLiteral("default");
}

namespace {

double srgb_channel(int value) {
	const double c = value / 255.0;
	return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

} /* namespace */

double bssh_relative_luminance(QRgb colour) {
	const QColor c(colour);
	return 0.2126 * srgb_channel(c.red()) + 0.7152 * srgb_channel(c.green()) +
	        0.0722 * srgb_channel(c.blue());
}

double bssh_contrast_ratio(QRgb a, QRgb b) {
	const double la = bssh_relative_luminance(a);
	const double lb = bssh_relative_luminance(b);
	return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

QRgb bssh_raise_contrast(QRgb colour, QRgb against) {
	if (bssh_contrast_ratio(colour, against) >= BSSH_CONTRAST_FLOOR) return colour;

	/*
	 * Away from the background, because that is the only direction that
	 * gains anything: a dark colour on a dark background has to get
	 * lighter, and no amount of darkening will help it.
	 */
	const bool lighten = bssh_relative_luminance(against) < 0.5;

	QColor start(colour);
	float h = 0.0f;
	float sat = 0.0f;
	float l = 0.0f;
	start.getHslF(&h, &sat, &l);

	/*
	 * Walked in small steps rather than solved, because the target is a
	 * ratio in linear light and the knob is HSL lightness -- and because
	 * stepping stops at the FIRST value that clears the floor, which is
	 * the least the colour can be changed. A solved answer would be no
	 * more accurate and would overshoot on saturated hues.
	 */
	for (int step = 1; step <= 100; ++step) {
		const float moved = lighten ? l + (1.0f - l) * (step / 100.0f)
		                             : l * (1.0f - step / 100.0f);

		QColor candidate = QColor::fromHslF(h < 0.0f ? 0.0f : h, sat,
		                                     qBound(0.0f, moved, 1.0f));
		if (bssh_contrast_ratio(candidate.rgb(), against) >= BSSH_CONTRAST_FLOOR) {
			return candidate.rgb();
		}
	}

	/*
	 * Nothing in the hue reached the floor -- a fully saturated blue on
	 * mid grey cannot. White or black then, whichever side the background
	 * is not: an ugly answer beats an unreadable one, and this is the
	 * case the user turned the setting on for.
	 */
	return lighten ? qRgb(255, 255, 255) : qRgb(0, 0, 0);
}
