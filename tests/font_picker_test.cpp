#include <QtTest>
#include <QComboBox>
#include <QSignalSpy>
#include <QTemporaryDir>

#include "font_picker_test.h"
#include "model/profile_store.h"
#include "model/settings_resolver.h"
#include "term/font_catalog.h"
#include "ui/font_picker.h"
#include "ui/session_dialog.h"

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
