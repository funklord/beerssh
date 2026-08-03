#include <QtTest>
#include <QDir>
#include <QFile>

#include <QColor>
#include <cmath>
#include "font_catalog_test.h"
#include "term/colour_scheme.h"
#include "term/font_catalog.h"
#include "ui/terminal_view.h"

void font_catalog_test::ids_are_stable_and_unique() {
	const bssh_font_catalog catalog;

	QSet<QString> seen;
	for (const bssh_font_t &font : catalog.all()) {
		QVERIFY2(!font.id.isEmpty(), "every font needs an id to be referenced by");
		QVERIFY2(!seen.contains(font.id), qPrintable("duplicate id: " + font.id));
		seen.insert(font.id);

		/*
		 * The id goes into stored profiles and JSON exports, so it is
		 * held to a shape that survives being written down: lowercase,
		 * hyphenated, no spaces. A display name that drifted into the id
		 * field would work right up until someone changed the display
		 * name.
		 */
		QCOMPARE(font.id, font.id.toLower());
		QVERIFY2(!font.id.contains(QLatin1Char(' ')),
		         qPrintable("id must not contain spaces: " + font.id));

		QVERIFY(!font.display_name.isEmpty());
		QVERIFY(!font.family.isEmpty());

		/* sec 9.4: the licence travels with the font or it cannot be shipped. */
		QVERIFY2(!font.licence.isEmpty(),
		         qPrintable("no licence recorded for " + font.id));
	}

	QVERIFY(catalog.by_id(QStringLiteral("jetbrains-mono")) != nullptr);
	QVERIFY(catalog.by_id(QStringLiteral("no-such-font")) == nullptr);
}

void font_catalog_test::the_picker_never_offers_fallback_only_families() {
	const bssh_font_catalog catalog;
	const QList<bssh_font_t> selectable = catalog.selectable();

	QVERIFY2(selectable.size() >= 6, "sec 9.2 asks for six to eight selectable families");
	QVERIFY(selectable.size() < catalog.all().size());

	for (const bssh_font_t &font : selectable) {
		QVERIFY(font.selectable);

		/*
		 * Unifont and Noto are in the set to stop tofu, not to be read
		 * in. Offering them invites a user to pick a bitmap face for a
		 * day's work and conclude the terminal looks terrible.
		 */
		QVERIFY2(font.id != QStringLiteral("unifont"), "Unifont must stay out of the picker");
		QVERIFY2(font.id != QStringLiteral("noto-sans-mono"), "Noto is fallback-only");
	}

	/* The condensed option sec 9.2 wants for phones exists in the set. */
	bool has_condensed = false;
	for (const bssh_font_t &font : selectable) {
		if (font.condensed) has_condensed = true;
	}
	QVERIFY(has_condensed);
}

void font_catalog_test::defaults_differ_by_form_factor_but_come_from_one_set() {
	const bssh_font_catalog catalog;

	const QString compact = catalog.default_id(true);
	const QString roomy = catalog.default_id(false);

	/*
	 * The two defaults differ only when both are actually installed. A
	 * default that names an absent font is a bug, not a preference: on
	 * Android the condensed face arrives in an asset pack, and until it
	 * does the compact default has to be something the device can draw.
	 */
	if (catalog.is_available(QStringLiteral("iosevka-term")) &&
	    catalog.is_available(QStringLiteral("jetbrains-mono"))) {
		QVERIFY(compact != roomy);
		QVERIFY(catalog.by_id(compact)->condensed);
	}

	/*
	 * The point of sec 9.2's rule: the DEFAULT may differ by form factor,
	 * the SET may not. Both defaults have to be members a profile can name
	 * and have resolve identically on the other platform.
	 */
	const bssh_font_t *compact_font = catalog.by_id(compact);
	const bssh_font_t *roomy_font = catalog.by_id(roomy);

	QVERIFY(compact_font != nullptr);
	QVERIFY(roomy_font != nullptr);
	QVERIFY(compact_font->selectable);
	QVERIFY(roomy_font->selectable);
}

void font_catalog_test::names_resolve_to_ids_and_unknown_names_survive() {
	const bssh_font_catalog catalog;

	QCOMPARE(catalog.canonical_id(QStringLiteral("jetbrains-mono")),
	         QStringLiteral("jetbrains-mono"));

	/* A display name, as an older profile or a hand-written import has it. */
	QCOMPARE(catalog.canonical_id(QStringLiteral("JetBrains Mono")),
	         QStringLiteral("jetbrains-mono"));
	QCOMPARE(catalog.canonical_id(QStringLiteral("dejavu sans mono")),
	         QStringLiteral("dejavu-sans-mono"));

	/*
	 * Passed through, not guessed at. The caller can then say "this
	 * profile asks for a font that is not in the set", which is true and
	 * useful; silently rewriting it to the default would lose the fact
	 * that the user ever asked for something else.
	 */
	QCOMPARE(catalog.canonical_id(QStringLiteral("Comic Sans MS")),
	         QStringLiteral("Comic Sans MS"));
	QCOMPARE(catalog.canonical_id(QString()), QString());
}

void font_catalog_test::the_chain_always_ends_somewhere_drawable() {
	const bssh_font_catalog catalog;

	const QStringList chain = catalog.fallback_chain(QStringLiteral("jetbrains-mono"));

	/*
	 * Never empty, even with none of the set installed: sec 9.3's chain
	 * bottoms out at the platform's own fixed-pitch font rather than at
	 * nothing, because "no family" hands QFont an empty string and lets it
	 * pick whatever it likes, including a proportional face.
	 */
	QVERIFY(!chain.isEmpty());
	QVERIFY(!chain.first().isEmpty());

	/* No family appears twice -- a repeat is a wasted coverage probe on
	 * every cache miss. */
	QCOMPARE(chain.size(), QSet<QString>(chain.begin(), chain.end()).size());

	QCOMPARE(catalog.family_for(QStringLiteral("jetbrains-mono")), chain.first());

	/* An unknown id still gets a chain: it just starts at the fallbacks. */
	QVERIFY(!catalog.fallback_chain(QStringLiteral("no-such-font")).isEmpty());
}

void font_catalog_test::an_unavailable_font_is_reported_rather_than_swapped_silently() {
	const bssh_font_catalog catalog;

	/*
	 * sec 9.4: an unavailable font "falls back down the chain with a
	 * visible note rather than silently substituting". Both halves are
	 * asserted here -- something is still drawn, AND the user can be told
	 * why it is not what they picked.
	 */
	const QString note = catalog.substitution_note(QStringLiteral("no-such-font"));
	QVERIFY(!note.isEmpty());
	QVERIFY(!catalog.family_for(QStringLiteral("no-such-font")).isEmpty());

	/*
	 * A font that IS available must produce no note at all. A note shown
	 * when nothing was substituted is worse than none: it trains the user
	 * to ignore the one that matters.
	 */
	for (const bssh_font_t &font : catalog.all()) {
		if (!catalog.is_available(font.id)) continue;
		QCOMPARE(catalog.substitution_note(font.id), QString());
	}
}

void font_catalog_test::glyph_lookup_answers_from_the_chain_and_repeats_itself() {
	const bssh_font_catalog catalog;

	const QString id = QStringLiteral("jetbrains-mono");
	const QStringList chain = catalog.fallback_chain(id);

	/*
	 * A plain letter, a box-drawing glyph (what DejaVu is in the chain
	 * for) and a CJK ideograph (what Noto is in the chain for). Which
	 * family serves each depends on what is installed, so the assertion is
	 * the invariant rather than the outcome: the answer is always a member
	 * of the chain, never some family Qt found on its own.
	 */
	const char32_t samples[] = { U'A', U'─', U'中' };

	for (char32_t sample : samples) {
		const QString family = catalog.family_for_glyph(id, sample);
		QVERIFY(!family.isEmpty());
		QVERIFY2(chain.contains(family),
		         qPrintable("glyph served by a family outside the chain: " + family));

		/* Cached, and the cache must not change the answer. */
		QCOMPARE(catalog.family_for_glyph(id, sample), family);
	}
}

void font_catalog_test::fonts_are_built_with_merging_off() {
	const QFont font = bssh_font_catalog::make_font(QStringLiteral("DejaVu Sans Mono"), 11);

	/*
	 * The single most important property of every font this catalog hands
	 * out. With merging on, Qt silently fills a missing glyph from any
	 * family it can find -- including proportional ones -- and one such
	 * substitution knocks every following cell in the row out of the grid
	 * the remote program laid out (sec 9.3).
	 */
	QCOMPARE(font.styleStrategy() & QFont::NoFontMerging, QFont::NoFontMerging);
	QVERIFY(font.fixedPitch());
	QCOMPARE(font.pointSize(), 11);
}

void font_catalog_test::the_bundled_set_loads_and_covers_what_it_promises() {
	/*
	 * The only test here that touches the real font files, and therefore
	 * the only one that can prove sec 9.3's chain does what it is for.
	 * Everything above works on a machine with no fonts at all; this one
	 * skips there, because a red suite on a fresh checkout teaches people
	 * to ignore red suites.
	 */
	const QString directory = QStringLiteral(BSSH_TEST_FONT_DIR);
	if (!QDir(directory).exists()) {
		QSKIP("no bundled fonts -- run tools/fetch-fonts.sh");
	}

	bssh_font_catalog catalog;
	const int loaded = catalog.load_bundled(directory);

	/* Eight families, and more files than that: sec 9.1 wants real bold
	 * and italic faces, not synthesised ones. */
	QVERIFY2(loaded >= 8, qPrintable(QStringLiteral("only %1 faces loaded").arg(loaded)));

	for (const bssh_font_t &font : catalog.all()) {
		QVERIFY2(catalog.is_available(font.id),
		         qPrintable("fetched but not available: " + font.id));
		QCOMPARE(catalog.substitution_note(font.id), QString());
	}

	const QString id = QStringLiteral("jetbrains-mono");

	/* Its own glyphs, served by itself. */
	QCOMPARE(catalog.family_for_glyph(id, U'A'), QStringLiteral("JetBrains Mono"));

	/*
	 * Box drawing: JetBrains Mono has it, which is why it passed sec 9.1
	 * and is in the picker at all. If this ever falls through to DejaVu
	 * the font in the picker is not the font sec 9.6 validated.
	 */
	QCOMPARE(catalog.family_for_glyph(id, U'─'), QStringLiteral("JetBrains Mono"));

	/*
	 * CJK: it does NOT have it, and this is the case the whole chain
	 * exists for. The answer must be one of the fallbacks -- never
	 * JetBrains Mono (which would mean tofu) and never some proportional
	 * family Qt found by itself (which would mean a broken grid).
	 */
	const QString cjk = catalog.family_for_glyph(id, U'中');
	QVERIFY2(cjk != QStringLiteral("JetBrains Mono"),
	         "CJK must fall through to the broad-coverage fallbacks");
	QVERIFY(cjk == QStringLiteral("Noto Sans Mono") || cjk == QStringLiteral("Unifont"));
}

void font_catalog_test::a_colour_scheme_repaints_only_the_sixteen_it_owns() {
	const bssh_colour_scheme_catalog catalog;

	QVERIFY(catalog.all().size() >= 4);
	QVERIFY(catalog.by_id(QStringLiteral("solarized-dark")) != nullptr);

	for (const bssh_colour_scheme_t &scheme : catalog.all()) {
		QVERIFY(!scheme.id.isEmpty());
		QVERIFY(!scheme.display_name.isEmpty());

		/* Somebody else's design: it has to say whose (sec 9.4's rule,
		 * applied to colours). */
		QVERIFY2(!scheme.licence.isEmpty(), qPrintable("no licence: " + scheme.id));

		/* Opaque, or every cell is drawn transparent onto whatever was
		 * behind it -- a table written as 0xRRGGBB is easy to forget the
		 * alpha byte on. */
		for (int i = 0; i < 16; ++i) {
			QCOMPARE(qAlpha(scheme.ansi[i]), 255);
		}
		QCOMPARE(qAlpha(scheme.background), 255);
		QCOMPARE(qAlpha(scheme.foreground), 255);

		/* Foreground and background must actually differ, which is the
		 * one way a colour scheme can be catastrophically wrong. */
		QVERIFY2(scheme.background != scheme.foreground,
		         qPrintable("invisible text in " + scheme.id));
	}

	/*
	 * The cube and the grey ramp are NOT the scheme's to touch: a program
	 * asking for colour 137 asked for a specific RGB value, and a
	 * terminal that recoloured it would disagree with every other one
	 * about what 256-colour output looks like.
	 */
	bssh_terminal_view view;
	bssh_color_t cube;
	cube.kind = bssh_color_t::INDEXED;
	cube.index = 137;

	QJsonObject settings;
	settings.insert(QStringLiteral("colour_scheme"), QStringLiteral("default"));
	bssh_apply_appearance(&view, settings);
	const QColor before = view.colour_for_test(cube);

	settings.insert(QStringLiteral("colour_scheme"), QStringLiteral("gruvbox-dark"));
	bssh_apply_appearance(&view, settings);

	QCOMPARE(view.colour_for_test(cube), before);

	/* While colour 1 -- which the scheme DOES own -- changes with it. */
	bssh_color_t red;
	red.kind = bssh_color_t::INDEXED;
	red.index = 1;
	QVERIFY(view.colour_for_test(red) != QColor(catalog.by_id(QStringLiteral("default"))->ansi[1]));
}

void font_catalog_test::an_unknown_scheme_is_kept_but_drawn_in_the_default() {
	bssh_terminal_view view;

	QJsonObject settings;
	settings.insert(QStringLiteral("colour_scheme"), QStringLiteral("some-future-scheme"));
	bssh_apply_appearance(&view, settings);

	/*
	 * Kept, so the picker can show what the profile asked for, and drawn
	 * in the default so there is something on screen. Rewriting the value
	 * to "default" would lose the fact that the user ever chose otherwise
	 * (sec 10.1).
	 */
	QCOMPARE(view.colour_scheme(), QStringLiteral("some-future-scheme"));

	const bssh_colour_scheme_catalog catalog;
	bssh_color_t red;
	red.kind = bssh_color_t::INDEXED;
	red.index = 1;
	QCOMPARE(view.colour_for_test(red),
	         QColor(catalog.by_id(catalog.default_id())->ansi[1]));
}

void font_catalog_test::every_bundled_font_names_a_licence_file_that_exists() {
	const bssh_font_catalog catalog;

	/*
	 * sec 9.4 requires the licence to travel with the font and be shown.
	 * The screen that shows it originally FOUND the file by transforming
	 * the display name, which worked for five of the eight: "Iosevka
	 * Term" ships under OFL-Iosevka.md, "Cascadia Code PL" under
	 * OFL-CascadiaCode.txt, "DejaVu Sans Mono" under LICENSE-DejaVu.txt.
	 * Three fonts were credited with a line saying their licence was not
	 * installed, on the one screen whose whole purpose is crediting
	 * people. The names are now data rather than a guess, and this checks
	 * they point at something.
	 */
	for (const bssh_font_t &font : catalog.all()) {
		QVERIFY2(!font.licence_file.isEmpty(),
		         qPrintable("no licence file named for " + font.id));
	}

	const QString directory = QStringLiteral(BSSH_TEST_FONT_DIR);
	if (!QDir(directory).exists()) {
		QSKIP("no bundled fonts -- run tools/fetch-fonts.sh");
	}

	for (const bssh_font_t &font : catalog.all()) {
		const QString path = directory + QStringLiteral("/licences/") + font.licence_file;

		QVERIFY2(QFile::exists(path),
		         qPrintable(QStringLiteral("%1 names %2, which is not there")
		                            .arg(font.id, font.licence_file)));

		QFile file(path);
		QVERIFY(file.open(QIODevice::ReadOnly));

		/* A truncated licence is worse than none: it looks satisfied. */
		QVERIFY2(file.size() > 500,
		         qPrintable(QStringLiteral("%1 is only %2 bytes")
		                            .arg(font.licence_file).arg(file.size())));
	}
}

namespace {

double srgb_channel(int value) {
	const double c = value / 255.0;
	return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double relative_luminance(QRgb rgb) {
	const QColor c(rgb);
	return 0.2126 * srgb_channel(c.red()) + 0.7152 * srgb_channel(c.green()) +
		      0.0722 * srgb_channel(c.blue());
}

double contrast_ratio(QRgb a, QRgb b) {
	const double la = relative_luminance(a);
	const double lb = relative_luminance(b);
	return (qMax(la, lb) + 0.05) / (qMin(la, lb) + 0.05);
}

} /* namespace */

void font_catalog_test::our_own_scheme_is_legible() {
	/*
	 * A contrast floor for the schemes this project AUTHORS, and only
	 * those. The third-party sets are reproduced exactly on purpose --
	 * a "Solarized Dark" whose values are not Solarized's is a
	 * mislabelled scheme, and several of them do sit below this floor by
	 * their designers' choice (Solarized's bright black IS its
	 * background). Holding someone else's palette to our bar would mean
	 * either failing the build over their decision or silently editing
	 * their work; the licences recorded beside each set are why neither
	 * is acceptable.
	 *
	 * 3:1 rather than 4.5:1 for the ANSI entries, and the reason is
	 * arithmetic rather than laziness: blue contributes 0.0722 of
	 * luminance, so on a near-black background no recognisably saturated
	 * red or blue can reach 4.5 -- pure #ff0000 tops out at 4.26. The
	 * default foreground, which is what most text actually is, is held to
	 * the full 4.5.
	 */
	const bssh_colour_scheme_catalog &catalog = bssh_colour_scheme_catalog::instance();

	int checked = 0;
	for (const bssh_colour_scheme_t &scheme : catalog.all()) {
		if (scheme.licence != QStringLiteral("BeerSSH")) continue;
		++checked;

		const double fg = contrast_ratio(scheme.foreground, scheme.background);
		QVERIFY2(fg >= 4.5,
		          qPrintable(QStringLiteral("%1: default text is only %2:1")
		                             .arg(scheme.id).arg(fg, 0, 'f', 2)));

		for (int i = 0; i < 16; ++i) {
			/*
			 * The colour at the background's own end is meant to
			 * disappear into it: ANSI black on a dark scheme, white on a
			 * light one. A program printing colour 0 as text on a dark
			 * terminal is the broken one, and every terminal in
			 * existence renders it this way.
			 */
			if (scheme.dark && (i == 0 || i == 8)) continue;
			if (!scheme.dark && (i == 7 || i == 15)) continue;

			const double ratio = contrast_ratio(scheme.ansi[i], scheme.background);
			QVERIFY2(ratio >= 3.0,
			          qPrintable(QStringLiteral("%1: ANSI %2 is only %3:1 on its background")
			                             .arg(scheme.id).arg(i).arg(ratio, 0, 'f', 2)));
		}
	}

	QVERIFY2(checked > 0, "no scheme was checked -- the licence marker must have changed");
}
