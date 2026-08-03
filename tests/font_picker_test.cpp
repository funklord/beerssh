#include <QtTest>
#include <QComboBox>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <QImage>
#include <QLabel>
#include "term/colour_scheme.h"
#include "font_picker_test.h"
#include "model/profile_store.h"
#include "model/settings_resolver.h"
#include "term/font_catalog.h"
#include "ui/font_picker.h"
#include "ui/session_dialog.h"
#include "model/preset.h"
#include "ui/override_badge.h"

void font_picker_test::offers_the_selectable_set_and_nothing_else() {
	bssh_font_picker picker;
	const QComboBox *combo = picker.findChild<QComboBox *>();
	QVERIFY(combo != nullptr);

	const bssh_font_catalog catalog;
	const QList<bssh_font_t> selectable = catalog.selectable();

	QCOMPARE(combo->count(), selectable.size());

	for (const bssh_font_t &font : selectable) {
		QVERIFY2(combo->findData(font.id) >= 0,
		         qPrintable("picker is missing " + font.id));
	}

	/*
	 * sec 9.2: the fallback-only families exist to stop tofu, not to be
	 * read in. They must not be offerable even though they are loaded and
	 * available, which is exactly the case a "list everything installed"
	 * implementation would get wrong.
	 */
	QCOMPARE(combo->findData(QStringLiteral("unifont")), -1);
	QCOMPARE(combo->findData(QStringLiteral("noto-sans-mono")), -1);
}

void font_picker_test::an_empty_choice_shows_the_platform_default() {
	bssh_font_picker picker;
	picker.set_font_choice(QString(), 0);

	const bssh_font_catalog catalog;

	/* Empty means the default, never "nothing selected" (sec 10.4's
	 * shipped presets all leave the font unset). */
	QVERIFY(!picker.font_id().isEmpty());
	QVERIFY(catalog.by_id(picker.font_id()) != nullptr);
	QVERIFY(picker.point_size() > 0);
}

void font_picker_test::a_font_this_build_does_not_have_is_shown_not_swallowed() {
	bssh_font_picker picker;
	picker.set_font_choice(QStringLiteral("some-future-font"), 13);

	/*
	 * Keeping the value visible is what lets a profile survive being
	 * opened by an older build. Resetting it to the default would look
	 * tidy and would silently rewrite the user's choice the moment they
	 * pressed Save.
	 */
	QCOMPARE(picker.font_id(), QStringLiteral("some-future-font"));
	QCOMPARE(picker.point_size(), 13);

	const QComboBox *combo = picker.findChild<QComboBox *>();
	QVERIFY(combo->currentText().contains(QStringLiteral("not in this build")));
}

void font_picker_test::choosing_a_font_reports_its_id_rather_than_its_label() {
	bssh_font_picker picker;
	QComboBox *combo = picker.findChild<QComboBox *>();

	QSignalSpy spy(&picker, &bssh_font_picker::font_chosen);

	const int index = combo->findData(QStringLiteral("hack"));
	QVERIFY(index >= 0);
	combo->setCurrentIndex(index);

	QCOMPARE(spy.count(), 1);

	/*
	 * The id, not "Hack". A display name in a stored profile is a profile
	 * that stops resolving the day the label is re-spelled (sec 9.4).
	 */
	QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("hack"));
	QCOMPARE(picker.font_id(), QStringLiteral("hack"));
}

void font_picker_test::the_session_dialog_keeps_what_its_form_does_not_show() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));
	bssh_session_dialog dialog(&store);

	/*
	 * A regression test with a real cause: current_profile() used to build
	 * a fresh bssh_profile_t out of the four text fields, so a host's
	 * preset link and every override it had were dropped the moment
	 * someone opened it and pressed Save. The form shows four fields; a
	 * profile has more than four fields.
	 */
	bssh_profile_t profile;
	profile.id = 7;
	profile.name = QStringLiteral("build box");
	profile.host = QStringLiteral("build.vibes.se");
	profile.user = QStringLiteral("nabbe");
	profile.port = 2222;
	profile.preset_id = 3;
	profile.has_stored_password = true;

	QJsonObject overrides;
	overrides.insert(QStringLiteral("scrollback_lines"), 99999);
	profile.overrides = overrides;

	dialog.set_current_profile(profile);
	const bssh_profile_t round_tripped = dialog.current_profile();

	QCOMPARE(round_tripped.id, profile.id);
	QCOMPARE(round_tripped.host, profile.host);
	QCOMPARE(round_tripped.port, profile.port);
	QCOMPARE(round_tripped.preset_id, profile.preset_id);
	QCOMPARE(round_tripped.has_stored_password, profile.has_stored_password);
	QCOMPARE(round_tripped.overrides.value(QStringLiteral("scrollback_lines")).toInt(),
	         99999);
}

void font_picker_test::a_font_matching_the_preset_is_not_recorded_as_an_override() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));
	bssh_session_dialog dialog(&store);

	bssh_profile_t profile;
	profile.host = QStringLiteral("plain.example.com");

	dialog.set_current_profile(profile);
	const bssh_profile_t saved = dialog.current_profile();

	/*
	 * Opening a host and saving it unchanged must not mark it as
	 * overriding its font. sec 10.1: an override that changes nothing is a
	 * badge that tells the user something untrue -- and worse, it detaches
	 * the host from its preset, so editing the preset later stops
	 * reaching it.
	 */
	QVERIFY2(!saved.overrides.contains(QStringLiteral("font_family")),
	         "an untouched dialog must not write a font override");
	QVERIFY2(!saved.overrides.contains(QStringLiteral("font_size")),
	         "an untouched dialog must not write a size override");
}

void font_picker_test::reverting_drops_the_override_rather_than_copying_the_value() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.path());

	bssh_preset_t preset;
	preset.name = QStringLiteral("Servers");
	preset.font_family = QStringLiteral("hack");
	preset.font_size = 12;
	const int preset_id = store.save_preset(preset);

	bssh_profile_t host;
	host.host = QStringLiteral("one.example.com");
	host.preset_id = preset_id;

	QJsonObject overrides;
	overrides.insert(QStringLiteral("font_family"), QStringLiteral("fira-code"));
	host.overrides = overrides;
	store.save(host);

	/*
	 * sec 10.1's revert. The distinction it turns on: dropping the
	 * override makes the host FOLLOW the preset again, while setting the
	 * field to the preset's current value merely happens to agree with it
	 * today and stops agreeing the next time the preset is edited --
	 * which is exactly the behaviour the user asked to undo.
	 */
	QJsonObject reverted =
	        bssh_settings_resolver::revert_field(host.overrides,
	                                              QStringLiteral("font_family"));
	QVERIFY(!reverted.contains(QStringLiteral("font_family")));

	QCOMPARE(bssh_settings_resolver::resolve(store.preset(preset_id), reverted)
	                 .value(QStringLiteral("font_family")).toString(),
	         QStringLiteral("hack"));

	/* And it still follows: edit the preset, and the host moves with it. */
	bssh_preset_t edited = store.preset(preset_id);
	edited.font_family = QStringLiteral("cascadia-code-pl");
	store.save_preset(edited);

	QCOMPARE(bssh_settings_resolver::resolve(store.preset(preset_id), reverted)
	                 .value(QStringLiteral("font_family")).toString(),
	         QStringLiteral("cascadia-code-pl"));

	/*
	 * The overridden field is the one sec 10.1 says gets a badge, and
	 * only that one -- a badge on a field that follows its preset is a
	 * badge that means nothing.
	 */
	const QSet<QString> badged =
	        bssh_settings_resolver::overridden_fields(store.preset(preset_id),
	                                                   host.overrides);
	QVERIFY(badged.contains(QStringLiteral("font_family")));
	QVERIFY(!badged.contains(QStringLiteral("font_size")));
}

void font_picker_test::the_session_dialog_round_trips_the_key_file() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));
	bssh_session_dialog dialog(&store);

	/*
	 * The desktop half of the same field the touch editor carries
	 * (sec 10.5). It is the one field on this form a user is likely to
	 * arrive at already filled in -- an OpenSSH import put it there --
	 * so what matters is that opening a host and pressing Save gives the
	 * key back rather than a subtly different string.
	 */
	bssh_profile_t profile;
	profile.host = QStringLiteral("build.example.com");
	profile.identity_file = QStringLiteral("~/.ssh/id_work");
	dialog.set_current_profile(profile);

	QCOMPARE(dialog.current_profile().identity_file, QStringLiteral("~/.ssh/id_work"));

	/* Blank means the defaults, and must not become a path of its own. */
	bssh_profile_t plain;
	plain.host = QStringLiteral("home.example.com");
	dialog.set_current_profile(plain);
	QVERIFY(dialog.current_profile().identity_file.isEmpty());
}

void font_picker_test::agreeing_with_the_preset_clears_the_override() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));

	bssh_preset_t preset;
	preset.name = QStringLiteral("Chunky");
	preset.font_family = QStringLiteral("hack");
	preset.font_size = 17;
	const int preset_id = store.save_preset(preset);

	bssh_profile_t host;
	host.name = QStringLiteral("box");
	host.host = QStringLiteral("box.example.com");
	host.preset_id = preset_id;
	host.overrides.insert(QStringLiteral("font_family"), QStringLiteral("jetbrains-mono"));
	const int id = store.save(host);

	bssh_session_dialog dialog(&store);
	dialog.set_current_profile(store.profile(id));

	/*
	 * The dialog embeds two pickers -- its own and the preset editor's --
	 * so the one under test is identified by what it is showing rather
	 * than by findChild order, which is not a contract.
	 */
	bssh_font_picker *picker = nullptr;
	for (bssh_font_picker *candidate : dialog.findChildren<bssh_font_picker *>()) {
		if (candidate->font_id() == QStringLiteral("jetbrains-mono")) picker = candidate;
	}
	QVERIFY(picker);

	/* The user picks what the preset gives, meaning "go back to following it". */
	picker->set_font_choice(QStringLiteral("hack"), 17);

	const bssh_override_badge_t badge = bssh_font_override_badge(
	        &store, store.profile(id), picker->font_id(), picker->point_size());
	QVERIFY2(!badge.overridden, "the badge must see agreement with the preset");

	/*
	 * And the save must agree with the badge. It did not: the write path
	 * compared the picker against what the host resolves to WITH its
	 * override still applied -- itself -- so it could never see
	 * agreement, and recorded an override pinning the preset's own value
	 * while the badge said "Following". The screen and the store said
	 * different things about the same host.
	 */
	const bssh_profile_t saved = dialog.current_profile();
	QVERIFY2(!saved.overrides.contains(QStringLiteral("font_family")),
	          "agreeing with the preset must CLEAR the override, not re-pin it");
	QVERIFY2(!saved.overrides.contains(QStringLiteral("font_size")),
	          "agreeing with the preset must CLEAR the override, not re-pin it");
}

void font_picker_test::the_desktop_form_never_saves_a_nameless_host() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));
	bssh_session_dialog dialog(&store);

	/*
	 * The touch editor has always named an unnamed host after itself;
	 * only this form let a nameless one through, and it lands as a blank
	 * row in the saved-sessions list -- the one place a host has to be
	 * recognisable.
	 */
	bssh_profile_t nameless;
	nameless.host = QStringLiteral("10.0.0.42");
	dialog.set_current_profile(nameless);

	QCOMPARE(dialog.current_profile().name, QStringLiteral("10.0.0.42"));
}

void font_picker_test::the_swatches_show_what_the_terminal_will_draw() {
	/*
	 * The picker's whole claim is that what is on screen is what the
	 * terminal will look like -- its comment says contrast is the whole
	 * decision. The draw-time floor (sec 9.3) broke that quietly: the
	 * strip kept painting the published values while the terminal painted
	 * lifted ones, so the one control whose job is judging contrast was
	 * the one place reporting it wrongly.
	 *
	 * Asserted on the pixels, because the bug was in what got painted
	 * rather than in what was computed.
	 */
	bssh_font_picker picker;
	picker.set_scheme(QStringLiteral("default"));

	QLabel *swatches = nullptr;
	for (QLabel *candidate : picker.findChildren<QLabel *>()) {
		if (!candidate->pixmap().isNull()) swatches = candidate;
	}
	QVERIFY2(swatches, "the swatch strip must be on screen to be checked");

	const bssh_colour_scheme_t *scheme =
	        bssh_colour_scheme_catalog::instance().by_id(QStringLiteral("default"));
	QVERIFY(scheme);

	const QImage strip = swatches->pixmap().toImage();
	const int cell = strip.width() / 8;

	/*
	 * ANSI 4 is xterm's blue, 1.81:1 as published -- the colour this
	 * whole design argued about. Its swatch must be the lifted one.
	 */
	const QColor painted = strip.pixelColor(4 * cell + cell / 2, strip.height() / 4);
	const QRgb expected = bssh_raise_contrast(scheme->ansi[4], scheme->background);

	QCOMPARE(painted.rgb(), expected);
	QVERIFY2(painted.rgb() != scheme->ansi[4],
	          "this colour is below the floor, so the swatch must differ from the palette");
	QVERIFY(bssh_contrast_ratio(painted.rgb(), scheme->background) >= BSSH_CONTRAST_FLOOR);
}
