#include "term/colour_scheme.h"

#include <QColor>
#include <cmath>

namespace {

/*
 * The set. Six schemes, chosen the way sec 9.2 chose the fonts: a small
 * curated list rather than everything that exists, each one widely used
 * enough that someone arriving from another terminal finds what they
 * already know, and each one under a licence that permits redistribution.
 *
 * The default is deliberately NOT one of the famous ones. It is the
 * xterm-ish palette this renderer already used, so that the day this
 * landed nobody's terminal changed colour without being asked.
 */
struct entry_t {
	const char *id;
	const char *display_name;
	const char *licence;
	bool dark;
	QRgb ansi[16];
	QRgb background;
	QRgb foreground;
	QRgb cursor;
};

const entry_t BSSH_SCHEMES[] = {
	{
		"default", "Default", "BeerSSH", true,
		/*
		 * xterm's palette, reproduced exactly -- including the blue at
		 * 0x0000ee that measures 1.81:1 against this background and is
		 * the most notorious readability problem in terminal history.
		 *
		 * It is NOT corrected here, and that is the design. A scheme is
		 * a record of what somebody published; editing the numbers makes
		 * every scheme in this list a thing the user cannot check
		 * against its source, and the argument that protects Solarized
		 * from us protects xterm too. Legibility is fixed at DRAW time
		 * instead, by the contrast floor in sec 9.3, which is a property
		 * of the reader rather than of the palette -- and which works
		 * for schemes added later without anybody editing them.
		 */
		{
			0x000000, 0xcd0000, 0x00cd00, 0xcdcd00,
			0x0000ee, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
			0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00,
			0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff
		},
		0x1c1c1c, 0xd0d0d0, 0xd0d0d0
	},

	/*
	 * The sets below are OTHER PEOPLE'S DESIGNS and are reproduced
	 * exactly, contrast measurements notwithstanding. A "Solarized Dark"
	 * whose values are not Solarized's is a mislabelled scheme, and the
	 * licences recorded above exist precisely because these came from
	 * somewhere. Several of them do have entries below the contrast floor
	 * -- Solarized's bright black IS its background, by design -- and
	 * that is theirs to decide, not this project's to correct.
	 *
	 * Solarized's whole point is that the two variants share one palette
	 * and differ only in which end of it is the background, so they are
	 * two entries rather than one with a flag.
	 */
	{
		"solarized-dark", "Solarized Dark", "MIT (Ethan Schoonover)", true,
		{
			0x073642, 0xdc322f, 0x859900, 0xb58900,
			0x268bd2, 0xd33682, 0x2aa198, 0xeee8d5,
			0x002b36, 0xcb4b16, 0x586e75, 0x657b83,
			0x839496, 0x6c71c4, 0x93a1a1, 0xfdf6e3
		},
		0x002b36, 0x839496, 0x93a1a1
	},
	{
		"solarized-light", "Solarized Light", "MIT (Ethan Schoonover)", false,
		{
			0x073642, 0xdc322f, 0x859900, 0xb58900,
			0x268bd2, 0xd33682, 0x2aa198, 0xeee8d5,
			0x002b36, 0xcb4b16, 0x586e75, 0x657b83,
			0x839496, 0x6c71c4, 0x93a1a1, 0xfdf6e3
		},
		0xfdf6e3, 0x657b83, 0x586e75
	},

	{
		"gruvbox-dark", "Gruvbox Dark", "MIT (Pavel Pertsev)", true,
		{
			0x282828, 0xcc241d, 0x98971a, 0xd79921,
			0x458588, 0xb16286, 0x689d6a, 0xa89984,
			0x928374, 0xfb4934, 0xb8bb26, 0xfabd2f,
			0x83a598, 0xd3869b, 0x8ec07c, 0xebdbb2
		},
		0x282828, 0xebdbb2, 0xebdbb2
	},

	{
		"nord", "Nord", "MIT (Arctic Ice Studio)", true,
		{
			0x3b4252, 0xbf616a, 0xa3be8c, 0xebcb8b,
			0x81a1c1, 0xb48ead, 0x88c0d0, 0xe5e9f0,
			0x4c566a, 0xbf616a, 0xa3be8c, 0xebcb8b,
			0x81a1c1, 0xb48ead, 0x8fbcbb, 0xeceff4
		},
		0x2e3440, 0xd8dee9, 0xd8dee9
	},

	/*
	 * Tango is the palette GNOME Terminal shipped for years, so it is the
	 * one a Linux user is most likely to recognise without knowing its
	 * name.
	 */
	{
		"tango-dark", "Tango Dark", "Public domain (Tango Desktop Project)", true,
		{
			0x2e3436, 0xcc0000, 0x4e9a06, 0xc4a000,
			0x3465a4, 0x75507b, 0x06989a, 0xd3d7cf,
			0x555753, 0xef2929, 0x8ae234, 0xfce94f,
			0x729fcf, 0xad7fa8, 0x34e2e2, 0xeeeeec
		},
		0x2e3436, 0xd3d7cf, 0xd3d7cf
	}
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

		for (int i = 0; i < 16; ++i) {
			/* The table is written as 0xRRGGBB for readability; QRgb wants
			 * the alpha byte set or every colour is transparent. */
			scheme.ansi[i] = 0xff000000u | entry.ansi[i];
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
