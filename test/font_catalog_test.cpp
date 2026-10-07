#include <QFontMetricsF>
#include <QtTest>
#include <QFontDatabase>
#include <QDir>
#include <QFile>

#include <QColor>
#include <cmath>
#include "font_catalog_test.h"
#include "term/colour_scheme.h"
#include "term/font_catalog.h"
#include "model/custom_font.h"
#include <QTemporaryDir>
#include "ui/terminal_view.h"

void font_catalog_test::an_installed_font_resolves_like_a_set_font() {
	const bssh_font_catalog &catalog = bssh_font_catalog::instance();
	const QStringList installed = catalog.installed_monospace();

	/* Whatever this machine has: fixed-pitch, not a set family, in order. */
	for (const QString &family : installed) {
		QVERIFY2(QFontDatabase::isFixedPitch(family), qPrintable(family));
		for (const bssh_font_t &font : catalog.all())
			QVERIFY2(font.family.compare(family, Qt::CaseInsensitive) != 0, qPrintable(family));
	}
	for (int i = 1; i < installed.size(); ++i)
		QVERIFY(installed.at(i - 1).compare(installed.at(i), Qt::CaseInsensitive) <= 0);

	if (installed.isEmpty())
		QSKIP("no monospace font is installed here beyond the set; the half that "
		      "resolves one cannot be asked on this machine");

	const QString id = bssh_font_catalog::system_id(installed.first());
	QVERIFY(bssh_font_catalog::is_system_id(id));
	QVERIFY(catalog.is_available(id));
	QCOMPARE(catalog.family_for(id), installed.first());
	QCOMPARE(catalog.display_name(id), installed.first());
	QVERIFY(catalog.substitution_note(id).isEmpty());

	/* And the set behind it, for the glyphs an unvetted family lacks. */
	QVERIFY(catalog.fallback_chain(id).size() > 1);
}

void font_catalog_test::an_installed_font_that_is_gone_falls_back_and_says_so() {
	const bssh_font_catalog &catalog = bssh_font_catalog::instance();
	const QString id = bssh_font_catalog::system_id(QStringLiteral("No Such Family Anywhere"));

	QVERIFY(bssh_font_catalog::is_system_id(id));
	QVERIFY(!bssh_font_catalog::is_system_id(QStringLiteral("system:")));
	QVERIFY(!catalog.is_available(id));

	/* Drawn in something real, and the note says what and why. */
	QVERIFY(!catalog.family_for(id).isEmpty());
	QVERIFY(catalog.family_for(id) != QStringLiteral("No Such Family Anywhere"));
	const QString note = catalog.substitution_note(id);
	QVERIFY2(note.contains(QStringLiteral("No Such Family Anywhere"))
	          && note.contains(QStringLiteral("not installed")), qPrintable(note));
	QCOMPARE(catalog.display_name(id), QStringLiteral("No Such Family Anywhere"));
}

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

void font_catalog_test::a_downloaded_font_does_not_become_the_default() {
	const bssh_font_catalog catalog;

	/*
	 * The phone's default was condensed Iosevka whenever it was installed.
	 * With the download built, fetching it for one host then changed the
	 * font of every host that never chose one -- so the default is
	 * JetBrains Mono on every form factor (the holder, 2026-09-29), and
	 * Iosevka being present must not move it. Asserted where Iosevka is
	 * installed, which is the case the old rule changed.
	 */
	if (!catalog.is_available(QStringLiteral("iosevka-term")))
		QSKIP("Iosevka is not installed here, so there is nothing to move the default");

	QVERIFY(catalog.is_available(QStringLiteral("jetbrains-mono")));
	QCOMPARE(catalog.default_id(), QStringLiteral("jetbrains-mono"));
	QVERIFY(!catalog.by_id(catalog.default_id())->condensed);
	QVERIFY(catalog.by_id(catalog.default_id())->selectable);
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

void font_catalog_test::ligatures_are_off_by_default_and_optional() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	/*
	 * Off by default (sec 9.2): a ligature draws two source characters as
	 * one shape while the emulator counts two columns, so the liga/clig/
	 * calt features are pinned to 0. The escape for the user who wants
	 * them anyway is to build the font with ligatures on, which leaves the
	 * features unset so the font's own defaults render.
	 */
	const QFont off = bssh_font_catalog::make_font(QStringLiteral("Fira Code"), 12, false);
	QVERIFY(off.isFeatureSet(QFont::Tag("liga")));
	QCOMPARE(off.featureValue(QFont::Tag("liga")), quint32(0));
	QCOMPARE(off.featureValue(QFont::Tag("calt")), quint32(0));

	const QFont on = bssh_font_catalog::make_font(QStringLiteral("Fira Code"), 12, true);
	QVERIFY(!on.isFeatureSet(QFont::Tag("liga")));
	QVERIFY(!on.isFeatureSet(QFont::Tag("calt")));
#else
	QSKIP("per-font OpenType feature control needs Qt 6.7");
#endif
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
		QSKIP("no bundled fonts -- run tool/fetch-fonts.sh");
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

void font_catalog_test::a_scheme_owns_the_background_and_the_text_not_the_palette() {
	const bssh_colour_scheme_catalog catalog;

	/*
	 * The set is named rather than counted, so removing one is a decision
	 * somebody has to make here as well as there: three phosphors on
	 * black, one inverse for e-paper, and the custom escape hatch.
	 */
	QVERIFY(catalog.by_id(QStringLiteral("default")) != nullptr);
	QVERIFY(catalog.by_id(QStringLiteral("green-on-black")) != nullptr);
	QVERIFY(catalog.by_id(QStringLiteral("yellow-on-black")) != nullptr);
	QVERIFY(catalog.by_id(QStringLiteral("black-on-white")) != nullptr);
	QVERIFY(catalog.by_id(QStringLiteral("custom")) != nullptr);

	/*
	 * ONE palette across all of them, which is the point of the set. A
	 * scheme chooses the background and the colour of ordinary text; what
	 * a program meant by red is not its to redefine, and a scheme that
	 * repainted the sixteen would make every other program's colour
	 * choices mean something else.
	 */
	const bssh_colour_scheme_t *shared = catalog.by_id(catalog.default_id());
	QVERIFY(shared);
	for (const bssh_colour_scheme_t &scheme : catalog.all()) {
		for (int i = 0; i < 16; ++i) {
			QVERIFY2(scheme.ansi[i] == shared->ansi[i],
			          qPrintable(QStringLiteral("%1 repaints ANSI %2")
			                             .arg(scheme.id).arg(i)));
		}
	}

	for (const bssh_colour_scheme_t &scheme : catalog.all()) {
		QVERIFY(!scheme.id.isEmpty());
		QVERIFY(!scheme.display_name.isEmpty());

		/* Where the numbers came from, still recorded (sec 9.4's rule,
		 * applied to colours) -- now "Classic ANSI" rather than a list
		 * of other people's designs. */
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

	settings.insert(QStringLiteral("colour_scheme"), QStringLiteral("yellow-on-black"));
	bssh_apply_appearance(&view, settings);

	QCOMPARE(view.colour_for_test(cube), before);

	/*
	 * And colour 1 does not change either, which is the rule that
	 * replaced the old one. A scheme used to own the sixteen; now it owns
	 * the background and the colour of ordinary text, and what a program
	 * meant by red survives switching between them.
	 *
	 * Compared between two schemes rather than against the raw palette
	 * value, because what reaches the screen has been through sec 9.3's
	 * contrast lift -- and both of these sit on pure black, so the lift
	 * produces the same answer for both. That is the invariant worth
	 * asserting: the drawn red is a property of the palette and the
	 * background, not of which scheme was picked.
	 */
	bssh_color_t red;
	red.kind = bssh_color_t::INDEXED;
	red.index = 1;

	settings.insert(QStringLiteral("colour_scheme"), QStringLiteral("default"));
	bssh_apply_appearance(&view, settings);
	const QColor red_on_default = view.colour_for_test(red);

	settings.insert(QStringLiteral("colour_scheme"), QStringLiteral("yellow-on-black"));
	bssh_apply_appearance(&view, settings);
	QCOMPARE(view.colour_for_test(red), red_on_default);
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
		QSKIP("no bundled fonts -- run tool/fetch-fonts.sh");
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

	QVERIFY2(lifted > 0, "nothing was lifted -- classic ANSI blue must still need it");

	/*
	 * The palette is untouched, which is the other half of the bargain: a
	 * program asking for blue gets the blue the standard defines, and the
	 * fix for its legibility happens at draw time.
	 *
	 * Classic ANSI blue is the one this whole design argued about, so it
	 * is named. On pure black it is WORSE than xterm's was -- 0x0000aa
	 * against 0x000000 rather than 0x0000ee against 0x1c1c1c -- which is
	 * the argument for the contrast floor rather than against the
	 * palette: the standard is not going to change, so the renderer is
	 * where the problem gets solved.
	 */
	const bssh_colour_scheme_t *fallback = catalog.by_id(catalog.default_id());
	QVERIFY(fallback);
	/* qRgb, not the bare literal: the catalog carries a full alpha byte. */
	QCOMPARE(fallback->ansi[4], qRgb(0x00, 0x00, 0xaa));
	QCOMPARE(fallback->ansi[1], qRgb(0xaa, 0x00, 0x00));

	/* Pure black, not a near-black: an OLED's switched-off pixel is free. */
	QCOMPARE(fallback->background, qRgb(0x00, 0x00, 0x00));
	QCOMPARE(fallback->foreground, qRgb(0xff, 0xff, 0xff));
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
		QSKIP("no bundled fonts fetched; tool/fetch-fonts.sh has not run");
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

void font_catalog_test::a_monospace_font_file_becomes_the_custom_entry() {
	/*
	 * sec 9.4's reserved entry: a font file of the user's own, copied into
	 * app storage, registered, and named by the "custom" id -- available,
	 * named for its family, first in its own fallback chain, and back
	 * after a restart from the copy rather than the original.
	 */
	const QString source = QStringLiteral(BSSH_TEST_FONT_DIR) + QStringLiteral("/Hack-Regular.ttf");
	if (!QFile::exists(source)) QSKIP("the fonts have not been fetched here (tool/fetch-fonts.sh)");

	QTemporaryDir storage;
	QVERIFY(storage.isValid());
	bssh_set_custom_font_directory(storage.path());

	bssh_font_catalog &catalog = bssh_font_catalog::instance();
	const QString before = catalog.custom_family();

	QString family;
	QString why;
	QVERIFY2(bssh_import_custom_font(source, &family, &why), qPrintable(why));
	QCOMPARE(family, QStringLiteral("Hack"));

	const QString id = bssh_font_catalog::custom_id();
	QVERIFY(catalog.is_available(id));
	QCOMPARE(catalog.display_name(id), QStringLiteral("Your font: Hack"));
	QCOMPARE(catalog.fallback_chain(id).first(), QStringLiteral("Hack"));
	QVERIFY(QFile::exists(storage.path() + QStringLiteral("/custom-font")));

	/*
	 * Shown, so nothing to explain. The phone's picker said '"custom" is
	 * not one of the bundled fonts' here, about the imported font it was
	 * drawing in.
	 */
	QCOMPARE(catalog.substitution_note(id), QString());

	/* A restart: nothing in the catalog, then the copy loaded again. */
	catalog.set_custom_family(QString());
	QVERIFY(!catalog.is_available(id));
	QVERIFY(catalog.substitution_note(id).startsWith(QStringLiteral("No font of your own")));
	QCOMPARE(bssh_load_custom_font(), QStringLiteral("Hack"));
	QVERIFY(catalog.is_available(id));

	catalog.set_custom_family(before);
	bssh_set_custom_font_directory(QString());
}

void font_catalog_test::a_font_that_is_not_monospace_is_refused_and_changes_nothing() {
	/*
	 * sec 9.1 excludes proportional fonts because a grid cannot hold one,
	 * and the check is a measurement, not the font's own flag. A refusal
	 * leaves the font that was there before in place -- in the catalog
	 * and on disk.
	 */
	const QString mono = QStringLiteral(BSSH_TEST_FONT_DIR) + QStringLiteral("/Hack-Regular.ttf");
	const QString proportional = QStringLiteral("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
	if (!QFile::exists(mono)) QSKIP("the fonts have not been fetched here (tool/fetch-fonts.sh)");
	if (!QFile::exists(proportional)) QSKIP("no proportional DejaVu Sans on this machine to refuse");

	QTemporaryDir storage;
	QVERIFY(storage.isValid());
	bssh_set_custom_font_directory(storage.path());

	bssh_font_catalog &catalog = bssh_font_catalog::instance();
	const QString before = catalog.custom_family();

	QString family;
	QString why;
	QVERIFY2(bssh_import_custom_font(mono, &family, &why), qPrintable(why));
	QFile kept(storage.path() + QStringLiteral("/custom-font"));
	QVERIFY(kept.open(QIODevice::ReadOnly));
	const QByteArray bytes = kept.readAll();
	kept.close();

	QVERIFY(!bssh_import_custom_font(proportional, &family, &why));
	QVERIFY2(why.contains(QStringLiteral("not monospace")), qPrintable(why));
	QCOMPARE(catalog.custom_family(), QStringLiteral("Hack"));
	QVERIFY(kept.open(QIODevice::ReadOnly));
	QCOMPARE(kept.readAll(), bytes);
	kept.close();

	/* And something that is not a font at all. */
	QFile text(storage.path() + QStringLiteral("/not-a-font.txt"));
	QVERIFY(text.open(QIODevice::WriteOnly));
	text.write("hello, I am not a font\n");
	text.close();
	QVERIFY(!bssh_import_custom_font(text.fileName(), &family, &why));
	QCOMPARE(catalog.custom_family(), QStringLiteral("Hack"));

	catalog.set_custom_family(before);
	bssh_set_custom_font_directory(QString());
}
