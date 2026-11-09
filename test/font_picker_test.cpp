#include <QtTest>
#include <QComboBox>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <QImage>
#include <QLabel>
#include <QPushButton>
#include "term/colour_scheme.h"
#include "font_picker_test.h"
#include "model/profile_store.h"
#include "model/settings_resolver.h"
#include "term/font_catalog.h"
#include "ui/host_editor.h"
#include "ui/font_picker.h"
#include "ui/session_dialog.h"
#include "model/preset.h"
#include "ui/override_badge.h"
#include "model/app_settings.h"
#include "ui/terminal_fields.h"

#include <QSettings>
#include <QSpinBox>
#include <QLineEdit>
#include <QToolButton>

void font_picker_test::offers_the_set_then_what_is_installed() {
	bssh_font_picker picker;
	const QComboBox *combo = picker.findChild<QComboBox *>(QStringLiteral("font-picker-family"));
	QVERIFY(combo != nullptr);

	const bssh_font_catalog &catalog = bssh_font_catalog::instance();
	const QList<bssh_font_t> selectable = catalog.selectable();

	/* The set first, in order, as it always was. */
	for (int i = 0; i < selectable.size(); ++i) {
		QCOMPARE(combo->itemData(i).toString(), selectable.at(i).id);
	}

	/*
	 * Then, on a desktop, a heading and the installed monospace families
	 * (the holder's answer of 2026-09-26) -- which this suite always runs
	 * as, "desktop/offscreen". None of them may be a set family: a vetted
	 * family is offered under its set id or not at all.
	 */
	const QStringList installed = catalog.installed_monospace();
	const int expected = selectable.size() + (installed.isEmpty() ? 0 : 2 + installed.size());
	QCOMPARE(combo->count(), expected);
	for (const QString &family : installed) {
		QVERIFY2(combo->findData(bssh_font_catalog::system_id(family)) > selectable.size(),
		          qPrintable(family + QStringLiteral(" is not in the installed group")));
	}

	/*
	 * sec 9.2: the fallback-only families exist to stop tofu, not to be
	 * read in. They must not be offerable even though they are loaded and
	 * available -- under their set ids, and not by the back door of the
	 * installed group either, which is exactly the case a "list everything
	 * installed" implementation would get wrong.
	 */
	QCOMPARE(combo->findData(QStringLiteral("unifont")), -1);
	QCOMPARE(combo->findData(QStringLiteral("noto-sans-mono")), -1);
	for (const bssh_font_t &font : catalog.all()) {
		QCOMPARE(combo->findData(bssh_font_catalog::system_id(font.family)), -1);
	}
}

void font_picker_test::an_installed_font_that_is_gone_is_shown_not_swallowed() {
	bssh_font_picker picker;
	const QString gone = bssh_font_catalog::system_id(QStringLiteral("No Such Family Anywhere"));
	picker.set_font_choice(gone, 12);

	/* Kept, named for what it is, and not called "not in this build". */
	QCOMPARE(picker.font_id(), gone);
	const QComboBox *combo = picker.findChild<QComboBox *>(QStringLiteral("font-picker-family"));
	QVERIFY(combo->currentText().startsWith(QStringLiteral("No Such Family Anywhere")));
	QVERIFY(combo->currentText().contains(QStringLiteral("not installed")));
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

	const QComboBox *combo = picker.findChild<QComboBox *>(QStringLiteral("font-picker-family"));
	QVERIFY(combo->currentText().contains(QStringLiteral("not in this build")));
}

void font_picker_test::choosing_a_font_reports_its_id_rather_than_its_label() {
	bssh_font_picker picker;
	QComboBox *combo = picker.findChild<QComboBox *>(QStringLiteral("font-picker-family"));

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

void font_picker_test::the_badge_covers_what_the_save_writes() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	bssh_preset_t preset;
	preset.name = QStringLiteral("house");
	preset.font_family = QStringLiteral("hack");
	preset.font_size = 17;
	preset.chosen = { QStringLiteral("font_family"), QStringLiteral("font_size") };
	const int preset_id = store.save_preset(preset);
	QVERIFY(preset_id > 0);

	bssh_profile_t host;
	host.name = QStringLiteral("follower");
	host.host = QStringLiteral("a.example");
	host.preset_id = preset_id;
	const int id = store.save(host);
	QVERIFY(id > 0);

	const bssh_profile_t saved = store.profile(id);

	/*
	 * Same font, same size, DIFFERENT colours. The badge weighed the font
	 * and the size only, so it said "Following" while the write recorded
	 * a colour_scheme override -- the screen and the store disagreeing,
	 * one field over from the pair this helper was built for.
	 */
	QString other;
	for (const bssh_colour_scheme_t &scheme : bssh_colour_scheme_catalog::instance().all()) {
		if (scheme.id != bssh_colour_scheme_catalog::instance().effective_id(QString())) {
			other = scheme.id;
			break;
		}
	}
	QVERIFY2(!other.isEmpty(), "the catalog must offer a scheme that is not the default");

	/* Shown what it follows; only the colours moved. */
	const bssh_appearance_pick_t shown = {
		QStringLiteral("hack"), 17, bssh_colour_scheme_catalog::instance().effective_id(QString())
	};
	const bssh_appearance_pick_t picked = { QStringLiteral("hack"), 17, other };
	const bssh_override_badge_t badge = bssh_appearance_override_badge(&store, saved, picked, shown);
	const QJsonObject written = bssh_appearance_overrides(&store, saved, picked, shown);

	/*
	 * The assertion is the AGREEMENT, not either value: whatever the two
	 * decide, they must decide it together. Asserting the badge alone
	 * would go stale the next time the write path learns a fourth field.
	 */
	QCOMPARE(badge.overridden, written.contains(QStringLiteral("colour_scheme")));
	QVERIFY2(badge.overridden, "a scheme the preset does not give is an override");
	QVERIFY2(badge.text.contains(QStringLiteral("colours")), qPrintable(badge.text));

	/*
	 * And the DIALOG must show that, which is a separate question: the
	 * helper agreeing with the writer says nothing about whether anything
	 * asks the helper when the colours change. scheme_chosen had no
	 * listener anywhere in the tree, so the badge above was right and the
	 * screen was still wrong.
	 */
	bssh_session_dialog dialog(&store);
	dialog.set_current_profile(saved);

	QLabel *scope = dialog.findChild<QLabel *>(QStringLiteral("host_font_scope"));
	QVERIFY2(scope, "the desktop page must expose its badge by name");
	QVERIFY2(scope->text().contains(QStringLiteral("Following")), qPrintable(scope->text()));

	bssh_font_picker *picker =
	        dialog.findChild<bssh_font_picker *>(QStringLiteral("host_font_picker"));
	QVERIFY(picker);

	picker->set_scheme(other);
	QVERIFY2(scope->text().contains(QStringLiteral("Overrides")), qPrintable(scope->text()));
}

void font_picker_test::the_dialog_shows_a_hosts_own_size_and_follows_the_default() {
	/*
	 * The holder, 2026-11-02: font size "in config does nothing", and no
	 * default size to set. A host following no preset -- every host the
	 * desktop dialog saves -- was shown and resolved without its own
	 * size, and the page had no default to change.
	 */
	struct defaults_left_t {
		QString path = bssh_app_settings().path();
		QVariant size = QSettings(path, QSettings::IniFormat).value(QStringLiteral("defaults/font_size"));
		~defaults_left_t() {
			QSettings file(path, QSettings::IniFormat);
			if (size.isValid()) file.setValue(QStringLiteral("defaults/font_size"), size);
			else file.remove(QStringLiteral("defaults/font_size"));
		}
	} left;
	bssh_app_settings().set_default_font_size(0);

	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	bssh_profile_t own;
	own.name = QStringLiteral("own");
	own.host = QStringLiteral("own.example");
	own.overrides.insert(QStringLiteral("font_size"), 17);
	const int own_id = store.save(own);

	bssh_session_dialog dialog(&store);
	bssh_font_picker *host =
	        dialog.findChild<bssh_font_picker *>(QStringLiteral("host_font_picker"));
	QWidget *everywhere = dialog.findChild<QWidget *>(QStringLiteral("device_appearance"));
	QVERIFY(everywhere);
	bssh_font_picker *device =
	        everywhere->findChild<bssh_font_picker *>(QStringLiteral("default_font_picker"));
	QVERIFY(host && device);

	/* The dialog as it opens, on a new destination: the default, and it
	 * goes on following the default as that changes. */
	QCOMPARE(host->point_size(), bssh_preset_t().font_size);
	QSpinBox *size = device->findChild<QSpinBox *>();
	QVERIFY(size);
	const QVariant family_before =
	        QSettings(left.path, QSettings::IniFormat).value(QStringLiteral("defaults/font_family"));
	size->setValue(bssh_preset_t().font_size + 5);
	QCOMPARE(bssh_app_settings().default_font_size(), bssh_preset_t().font_size + 5);

	/* A size chosen is not a family chosen: the family goes on following. */
	QCOMPARE(QSettings(left.path, QSettings::IniFormat).value(QStringLiteral("defaults/font_family")),
	         family_before);

	/* And the way back forgets it, rather than writing the built-in size. */
	QPushButton *follow = everywhere->findChild<QPushButton *>(QStringLiteral("default_font_follow"));
	QVERIFY(follow && !follow->isHidden());
	follow->click();
	QVERIFY(!QSettings(left.path, QSettings::IniFormat).contains(QStringLiteral("defaults/font_size")));
	QCOMPARE(bssh_app_settings().default_font_size(), 0);
	QVERIFY(follow->isHidden());
	QCOMPARE(host->point_size(), bssh_preset_t().font_size);
	size->setValue(bssh_preset_t().font_size + 5);
	QCOMPARE(host->point_size(), bssh_preset_t().font_size + 5);

	/* A host with a size of its own shows that. */
	dialog.set_current_profile(store.profile(own_id));
	QCOMPARE(host->point_size(), 17);
}

void font_picker_test::every_setting_has_a_default_and_a_hosts_own() {
	/*
	 * The holder, 2026-11-02: everything a preset holds gets a default for
	 * every host on this device and a value of a host's own, and a preset
	 * is a template a host may follow. Nothing on any screen could attach
	 * one, so scrollback, the cursor, TERM and the rest were out of every
	 * host's reach.
	 */
	struct defaults_left_t {
		QString path = bssh_app_settings().path();
		QVariantMap was;
		defaults_left_t() {
			QSettings file(path, QSettings::IniFormat);
			file.beginGroup(QStringLiteral("defaults"));
			for (const QString &key : file.childKeys()) was.insert(key, file.value(key));
		}
		~defaults_left_t() {
			QSettings file(path, QSettings::IniFormat);
			file.remove(QStringLiteral("defaults"));
			for (auto it = was.cbegin(); it != was.cend(); ++it)
				file.setValue(QStringLiteral("defaults/") + it.key(), it.value());
		}
	} left;
	{
		QSettings file(bssh_app_settings().path(), QSettings::IniFormat);
		file.remove(QStringLiteral("defaults"));
	}

	/*
	 * app.ini holds text: a "false" must come back false, a number a
	 * number. Written AS text here, as a later start reads it -- in the
	 * process that wrote them, QSettings hands back the values it was
	 * given, typed, and the conversion would go untested.
	 */
	{
		QSettings file(bssh_app_settings().path(), QSettings::IniFormat);
		file.setValue(QStringLiteral("defaults/bell_enabled"), QStringLiteral("false"));
		file.setValue(QStringLiteral("defaults/scrollback_lines"), QStringLiteral("1234"));
		file.sync();
	}
	const QJsonObject defaults = bssh_settings_resolver::app_defaults();
	QCOMPARE(defaults.value(QStringLiteral("bell_enabled")), QJsonValue(false));
	QCOMPARE(defaults.value(QStringLiteral("scrollback_lines")), QJsonValue(1234));

	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());
	bssh_preset_t router;
	router.name = QStringLiteral("router");
	router.term = QStringLiteral("vt100");
	router.chosen = { QStringLiteral("term") };
	const int router_id = store.save_preset(router);

	bssh_session_dialog dialog(&store);
	QWidget *everywhere = dialog.findChild<QWidget *>(QStringLiteral("device_terminal"));
	QVERIFY(everywhere);
	bssh_terminal_fields *device =
	        everywhere->findChild<bssh_terminal_fields *>(QStringLiteral("default_terminal_fields"));
	bssh_terminal_fields *host =
	        dialog.findChild<bssh_terminal_fields *>(QStringLiteral("host_terminal_fields"));
	QVERIFY(device && host);
	QCOMPARE(host->values().value(QStringLiteral("scrollback_lines")).toInt(), 1234);

	/* A default changed on the page is stored, and the host follows it. */
	QSpinBox *device_scrollback = device->findChild<QSpinBox *>(QStringLiteral("fields-scrollback"));
	QVERIFY(device_scrollback);
	device_scrollback->setValue(4321);
	QCOMPARE(bssh_settings_resolver::app_defaults().value(QStringLiteral("scrollback_lines")).toInt(),
	         4321);
	QCOMPARE(host->values().value(QStringLiteral("scrollback_lines")).toInt(), 4321);

	/*
	 * Put back to the built-in value by hand, it is still a choice and
	 * stays (the holder's rule, 2026-11-06); Default is what forgets it.
	 */
	device_scrollback->setValue(bssh_preset_t().scrollback_lines);
	QVERIFY(bssh_app_settings().default_settings().contains(QStringLiteral("scrollback_lines")));
	QToolButton *forget = device->findChild<QToolButton *>(QStringLiteral("fields-default-scrollback_lines"));
	QVERIFY(forget && !forget->isHidden());
	forget->click();
	QVERIFY(!bssh_app_settings().default_settings().contains(QStringLiteral("scrollback_lines")));
	QVERIFY(forget->isHidden());

	/* A host's own value is recorded, and only that. */
	QSpinBox *host_scrollback = host->findChild<QSpinBox *>(QStringLiteral("fields-scrollback"));
	QVERIFY(host_scrollback);
	host_scrollback->setValue(99);
	bssh_profile_t saved = dialog.current_profile();
	QCOMPARE(saved.overrides.value(QStringLiteral("scrollback_lines")).toInt(), 99);
	QVERIFY(!saved.overrides.contains(QStringLiteral("term")));
	QVERIFY(!saved.overrides.contains(QStringLiteral("bell_enabled")));

	/* A template chosen: its TERM shows, and the host's own stays. */
	QComboBox *templates = dialog.findChild<QComboBox *>(QStringLiteral("host_template"));
	QVERIFY(templates);
	const int at = templates->findData(router_id);
	QVERIFY(at > 0);
	templates->setCurrentIndex(at);
	QCOMPARE(host->values().value(QStringLiteral("term")).toString(), QStringLiteral("vt100"));
	saved = dialog.current_profile();
	QCOMPARE(saved.preset_id, router_id);
	QCOMPARE(saved.overrides.value(QStringLiteral("scrollback_lines")).toInt(), 99);
	QVERIFY(!saved.overrides.contains(QStringLiteral("term")));
}

void font_picker_test::the_touch_editor_keeps_a_hosts_terminal_settings() {
	/* The phone's host page, by the same rule as the desktop dialog. */
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());
	bssh_preset_t router;
	router.name = QStringLiteral("router");
	router.term = QStringLiteral("vt100");
	router.chosen = { QStringLiteral("term") };
	const int router_id = store.save_preset(router);

	bssh_profile_t host;
	host.name = QStringLiteral("box");
	host.host = QStringLiteral("box.example");
	const int id = store.save(host);

	bssh_host_editor editor(&store);
	editor.set_profile(store.profile(id));
	bssh_terminal_fields *fields =
	        editor.findChild<bssh_terminal_fields *>(QStringLiteral("host_terminal_fields"));
	QComboBox *templates = editor.findChild<QComboBox *>(QStringLiteral("host_template"));
	QVERIFY(fields && templates);

	QVERIFY(editor.profile().overrides.isEmpty());
	QSpinBox *scrollback = fields->findChild<QSpinBox *>(QStringLiteral("fields-scrollback"));
	QVERIFY(scrollback);
	scrollback->setValue(99);
	QCOMPARE(editor.profile().overrides.value(QStringLiteral("scrollback_lines")).toInt(), 99);

	templates->setCurrentIndex(templates->findData(router_id));
	QCOMPARE(fields->values().value(QStringLiteral("term")).toString(), QStringLiteral("vt100"));
	const bssh_profile_t saved = editor.profile();
	QCOMPARE(saved.preset_id, router_id);
	QCOMPARE(saved.overrides.value(QStringLiteral("scrollback_lines")).toInt(), 99);
	QVERIFY(!saved.overrides.contains(QStringLiteral("term")));

	/*
	 * A pick equal to what the template gives is still this host's own,
	 * so it stays when the template moves; Default is the way back.
	 */
	QLineEdit *term = fields->findChild<QLineEdit *>(QStringLiteral("fields-term"));
	QVERIFY(term);
	term->setText(QStringLiteral("vt100"));
	emit term->textEdited(term->text());
	QCOMPARE(editor.profile().overrides.value(QStringLiteral("term")).toString(), QStringLiteral("vt100"));
	QToolButton *follow = fields->findChild<QToolButton *>(QStringLiteral("fields-default-term"));
	QVERIFY(follow && !follow->isHidden());
	follow->click();
	QVERIFY(!editor.profile().overrides.contains(QStringLiteral("term")));
	QCOMPARE(fields->values().value(QStringLiteral("term")).toString(), QStringLiteral("vt100"));
}

void font_picker_test::the_touch_badge_follows_the_picker() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	bssh_preset_t preset;
	preset.name = QStringLiteral("house");
	preset.font_family = QStringLiteral("hack");
	preset.font_size = 17;
	preset.chosen = { QStringLiteral("font_family"), QStringLiteral("font_size") };
	const int preset_id = store.save_preset(preset);

	bssh_profile_t host;
	host.name = QStringLiteral("follower");
	host.host = QStringLiteral("a.example");
	host.preset_id = preset_id;
	const int id = store.save(host);

	bssh_host_editor editor(&store);
	editor.set_profile(store.profile(id));

	QLabel *scope = editor.findChild<QLabel *>(QStringLiteral("host_font_scope"));
	QVERIFY2(scope, "the touch page must expose its badge by name");
	QVERIFY2(scope->text().contains(QStringLiteral("Following")), qPrintable(scope->text()));

	bssh_font_picker *picker = editor.findChild<bssh_font_picker *>();
	QVERIFY(picker);

	/*
	 * Picking a different font must move the badge with it. Nothing
	 * connected the picker to the badge on this page, so it went on
	 * saying "Following preset" while profile() wrote an override -- and
	 * the revert button stayed hidden, so the override just made was
	 * unreachable until the page was left and reopened.
	 */
	picker->set_font_choice(QStringLiteral("jetbrains-mono"), 13);

	QVERIFY2(scope->text().contains(QStringLiteral("Overrides")), qPrintable(scope->text()));
	QVERIFY2(editor.profile().overrides.contains(QStringLiteral("font_family")),
	          "and the write agrees with what the badge now says");
}

void font_picker_test::reverting_drops_the_override_rather_than_copying_the_value() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.path());

	bssh_preset_t preset;
	preset.name = QStringLiteral("Servers");
	preset.font_family = QStringLiteral("hack");
	preset.font_size = 12;
	preset.chosen = { QStringLiteral("font_family"), QStringLiteral("font_size") };
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
	profile.proxy_jump = QStringLiteral("me@bastion.example.com");
	dialog.set_current_profile(profile);

	QCOMPARE(dialog.current_profile().identity_file, QStringLiteral("~/.ssh/id_work"));
	QCOMPARE(dialog.current_profile().proxy_jump,
	          QStringLiteral("me@bastion.example.com"));

	/* Blank means the defaults, and must not become a path of its own. */
	bssh_profile_t plain;
	plain.host = QStringLiteral("home.example.com");
	dialog.set_current_profile(plain);
	QVERIFY(dialog.current_profile().identity_file.isEmpty());
	QVERIFY(dialog.current_profile().proxy_jump.isEmpty());
}

void font_picker_test::agreeing_with_the_preset_keeps_the_pick_until_reverted() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));

	bssh_preset_t preset;
	preset.name = QStringLiteral("Chunky");
	preset.font_family = QStringLiteral("hack");
	preset.font_size = 17;
	preset.chosen = { QStringLiteral("font_family"), QStringLiteral("font_size") };
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
	 * By NAME, which is a contract. This asked for whichever picker was
	 * showing jetbrains-mono, and said in as many words that findChild
	 * order is not a contract -- but "what it is displaying" is not one
	 * either. It held while the dialog had two pickers and broke when
	 * Terminal and Connection became real pages, because a preset with no
	 * font of its own shows the platform default and several pickers
	 * started reporting the same family.
	 */
	bssh_font_picker *picker =
	        dialog.findChild<bssh_font_picker *>(QStringLiteral("host_font_picker"));
	QVERIFY2(picker, "the host's own font picker must be findable by name");

	/*
	 * The user picks what the preset gives. Until 2026-11-09 that cleared
	 * the override, sec 10.1's "cleared by agreeing"; the holder's rule is
	 * that a pick is a choice whatever it equals, so it stays the host's
	 * own -- and badge and save say so together, which is the half that
	 * was wrong before that (the save re-pinned while the badge said
	 * "Following").
	 */
	picker->set_font_choice(QStringLiteral("hack"), 17);
	const bssh_profile_t saved = dialog.current_profile();
	QCOMPARE(saved.overrides.value(QStringLiteral("font_family")).toString(), QStringLiteral("hack"));
	QLabel *scope = dialog.findChild<QLabel *>(QStringLiteral("host_font_scope"));
	QVERIFY(scope);
	QVERIFY2(scope->text().startsWith(QStringLiteral("<b>Overrides")), qPrintable(scope->text()));

	/* The revert is the way back to following, and it drops the override. */
	QPushButton *revert = nullptr;
	for (QPushButton *button : dialog.findChildren<QPushButton *>())
		if (button->text().startsWith(QStringLiteral("Use the preset's font"))) revert = button;
	QVERIFY(revert && revert->isEnabled());
	revert->click();
	const bssh_profile_t followed = dialog.current_profile();
	QVERIFY(!followed.overrides.contains(QStringLiteral("font_family")));
	QVERIFY(!followed.overrides.contains(QStringLiteral("font_size")));
	QVERIFY(scope->text().startsWith(QStringLiteral("Following")));
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
