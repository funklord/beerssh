#include <QFontMetricsF>
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

void font_catalog_test::our_own_scheme_is_legible() {
	/*
	 * The schemes are FAITHFUL reproductions and are deliberately not
	 * held to a contrast floor -- several of them fail one as published,
	 * xterm's blue at 1.81:1 loudest, and that is their authors' record
	 * to keep rather than ours to edit. What is asserted instead is the
	 * promise the app actually makes: whatever a scheme says, what
	 * reaches the screen clears sec 9.3's floor once the lift is on.
	 */
	const bssh_colour_scheme_catalog &catalog = bssh_colour_scheme_catalog::instance();

	int lifted = 0;
	for (const bssh_colour_scheme_t &scheme : catalog.all()) {
		for (int i = 0; i < 16; ++i) {
			const QRgb published = scheme.ansi[i];
			const QRgb drawn = bssh_raise_contrast(published, scheme.background);

			QVERIFY2(bssh_contrast_ratio(drawn, scheme.background) >= BSSH_CONTRAST_FLOOR,
			          qPrintable(QStringLiteral("%1: ANSI %2 still under the floor after lifting")
			                             .arg(scheme.id).arg(i)));

			if (drawn != published) ++lifted;

			/* A colour that already passes is returned untouched -- the
			 * lift is a floor, not a filter over everything. */
			if (bssh_contrast_ratio(published, scheme.background) >= BSSH_CONTRAST_FLOOR) {
				QCOMPARE(drawn, published);
			}
		}
	}

	QVERIFY2(lifted > 0, "nothing was lifted -- the faithful palettes must still need it");

	/*
	 * The published values are untouched, which is the other half of the
	 * bargain: a user can check any scheme against its source. xterm's
	 * blue is the one this whole design argued about, so it is named.
	 */
	const bssh_colour_scheme_t *fallback = catalog.by_id(catalog.default_id());
	QVERIFY(fallback);
	/* qRgb, not the bare literal: the catalog carries a full alpha byte. */
	QCOMPARE(fallback->ansi[4], qRgb(0x00, 0x00, 0xee));
	QCOMPARE(fallback->ansi[1], qRgb(0xcd, 0x00, 0x00));
	QVERIFY(bssh_contrast_ratio(fallback->ansi[4], fallback->background) < 2.0);

	/* And drawn, it is readable. */
	QVERIFY(bssh_contrast_ratio(bssh_raise_contrast(fallback->ansi[4], fallback->background),
	                             fallback->background) >= BSSH_CONTRAST_FLOOR);
}

void font_catalog_test::the_lift_keeps_a_colour_recognisable() {
	const QRgb background = 0x1c1c1c;

	/* Hue survives: a lifted red is still red, not a pale wash. */
	const QColor lifted(bssh_raise_contrast(0xcd0000, background));
	QVERIFY(lifted.hslHueF() < 0.05 || lifted.hslHueF() > 0.95);
	QVERIFY(lifted.red() > lifted.green() && lifted.red() > lifted.blue());

	/* On a LIGHT background the move is the other way. */
	const QRgb pale = 0xf0f0f0;
	const QColor darkened(bssh_raise_contrast(0xd0d0d0, pale));
	QVERIFY(bssh_relative_luminance(darkened.rgb()) < bssh_relative_luminance(0xd0d0d0));
	QVERIFY(bssh_contrast_ratio(darkened.rgb(), pale) >= BSSH_CONTRAST_FLOOR);

	/*
	 * Black on black is the case the floor exists for and the one a
	 * scheme cannot help with: it comes back as something readable
	 * rather than as black.
	 */
	QVERIFY(bssh_contrast_ratio(bssh_raise_contrast(0x000000, background), background)
	         >= BSSH_CONTRAST_FLOOR);
}

void font_catalog_test::every_bundled_face_draws_a_box_the_width_of_its_cell() {
	bssh_font_catalog catalog;
	if (catalog.load_bundled(QString::fromLatin1(BSSH_TEST_FONT_DIR)) < 1) {
		QSKIP("no bundled fonts fetched; tools/fetch-fonts.sh has not run");
	}

	/*
	 * The property a box-drawing character actually needs, and the one
	 * nobody had written down: its advance must equal the cell.
	 *
	 * The cell is measured from 'W' (see bssh_terminal_view), so a face
	 * whose U+2500 advanced by even a fraction less would leave a hairline
	 * at every cell boundary and a TUI's borders would come apart. It
	 * matters most for the FALLBACK: sec 9.3 sends a missing box glyph to
	 * DejaVu, and a DejaVu whose advance disagreed with the selected face
	 * would break lines only for the faces that lack the glyph -- which is
	 * the hardest kind of fault to see and the easiest to blame on the
	 * wrong thing.
	 *
	 * This test exists because that blame was misplaced once already. A
	 * note in this project claimed box drawing "does not meet in JetBrains
	 * Mono", from looking at a screenshot scaled up with smooth
	 * interpolation -- which turns the antialiased edge above a one-pixel
	 * stroke into what reads as a gap. Measuring the pixels of every
	 * terminal screenshot found the stroke continuous in all of them, and
	 * measuring the advances found every face exact. An impression is not
	 * evidence; this is what evidence looks like for this question.
	 */
	const char32_t box[] = {
		U'\u2500', U'\u2502', U'\u250c', U'\u253c',
		U'\u2534', U'\u2588', U'\u2592',
	};

	for (const bssh_font_t &face : catalog.all()) {
		for (int size = 8; size <= 20; ++size) {
			if (!catalog.is_available(face.id)) continue;

			QFont font(catalog.family_for(face.id));
			font.setPointSize(size);

			const QFontMetricsF metrics(font);
			const qreal cell = metrics.horizontalAdvance(QLatin1Char('W'));
			QVERIFY(cell > 0);

			for (char32_t glyph : box) {
				const QString text = QString::fromUcs4(&glyph, 1);
				const qreal advance = metrics.horizontalAdvance(text);

				QString complaint = QStringLiteral(
				        "%1 at %2pt: U+%3 advances %4 where the cell "
				        "is %5 -- its lines would not meet");
				complaint = complaint.arg(face.display_name).arg(size);
				complaint = complaint.arg(uint(glyph), 4, 16, QLatin1Char('0'));
				complaint = complaint.arg(advance).arg(cell);

				QVERIFY2(qAbs(advance - cell) < 0.01, qPrintable(complaint));
			}
		}
	}
}
