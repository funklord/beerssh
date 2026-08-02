#include <QJsonDocument>
#include <QtTest>

#include "model/preset.h"
#include "model/settings_resolver.h"
#include "preset_test.h"

void preset_test::an_exported_preset_contains_nothing_private() {
	bssh_preset_t preset;
	preset.name = QStringLiteral("work-bastion");
	preset.on_connect_command = QStringLiteral("tmux attach || tmux new");

	const QJsonObject exported = preset.to_json();
	const QByteArray text = QJsonDocument(exported).toJson();

	/*
	 * sec 10.3, asserted rather than trusted. A preset is meant to be
	 * shareable, and the failure mode is not a crash -- it is a file
	 * someone sends to a colleague that names their bastion host or
	 * carries a key reference.
	 */
	const QStringList forbidden = {
		QStringLiteral("host"), QStringLiteral("hostname"), QStringLiteral("port"),
		QStringLiteral("user"), QStringLiteral("username"), QStringLiteral("password"),
		QStringLiteral("passphrase"), QStringLiteral("key"), QStringLiteral("identity"),
		QStringLiteral("secret"), QStringLiteral("known_hosts")
	};

	for (const QString &key : forbidden) {
		QVERIFY2(!exported.contains(key), qPrintable(QStringLiteral("exported key: %1").arg(key)));
	}

	/* Nor the installation-local bookkeeping. */
	QVERIFY(!exported.contains(QStringLiteral("id")));
	QVERIFY(!exported.contains(QStringLiteral("is_builtin")));

	/* What it DOES carry is terminal behaviour. */
	QVERIFY(exported.contains(QStringLiteral("term")));
	QVERIFY(exported.contains(QStringLiteral("scrollback_lines")));
	QVERIFY(text.contains("tmux attach"));
}

void preset_test::export_and_import_round_trip() {
	bssh_preset_t preset;
	preset.name = QStringLiteral("slow link");
	preset.term = QStringLiteral("vt100");
	preset.scrollback_lines = 1234;
	preset.bell_enabled = false;
	preset.compression = true;
	preset.keepalive_seconds = 15;
	preset.font_family = QStringLiteral("Iosevka");
	preset.on_connect_command = QStringLiteral("tmux attach");

	const bssh_preset_t restored = bssh_preset_t::from_json(preset.to_json());

	QCOMPARE(restored.name, preset.name);
	QCOMPARE(restored.term, preset.term);
	QCOMPARE(restored.scrollback_lines, preset.scrollback_lines);
	QCOMPARE(restored.bell_enabled, preset.bell_enabled);
	QCOMPARE(restored.compression, preset.compression);
	QCOMPARE(restored.keepalive_seconds, preset.keepalive_seconds);
	QCOMPARE(restored.font_family, preset.font_family);
	QCOMPARE(restored.on_connect_command, preset.on_connect_command);
}

void preset_test::an_imported_preset_cannot_claim_to_be_builtin() {
	QJsonObject hostile;
	hostile.insert(QStringLiteral("name"), QStringLiteral("Default Linux"));
	hostile.insert(QStringLiteral("is_builtin"), true);
	hostile.insert(QStringLiteral("id"), 1);

	const bssh_preset_t imported = bssh_preset_t::from_json(hostile);

	/*
	 * The shipped presets are what users clone from (sec 10.4). A shared
	 * file that could import as one of them -- taking its id, or its
	 * read-only status -- would let a preset from anywhere masquerade as
	 * a starting point the app vouches for.
	 */
	QVERIFY(!imported.is_builtin);
	QCOMPARE(imported.id, 0);
}

void preset_test::the_preset_beats_the_defaults() {
	bssh_preset_t preset;
	preset.term = QStringLiteral("vt100");
	preset.scrollback_lines = 999;

	const QJsonObject resolved = bssh_settings_resolver::resolve(preset, QJsonObject());

	QCOMPARE(resolved.value(QStringLiteral("term")).toString(), QStringLiteral("vt100"));
	QCOMPARE(resolved.value(QStringLiteral("scrollback_lines")).toInt(), 999);

	/* Fields the preset left alone still come from the defaults. */
	QCOMPARE(resolved.value(QStringLiteral("cursor_style")).toString(), QStringLiteral("block"));
}

void preset_test::the_host_beats_the_preset() {
	bssh_preset_t preset;
	preset.term = QStringLiteral("vt100");
	preset.font_family = QStringLiteral("Iosevka");

	QJsonObject overrides;
	overrides.insert(QStringLiteral("term"), QStringLiteral("xterm-256color"));

	const QJsonObject resolved = bssh_settings_resolver::resolve(preset, overrides);

	QCOMPARE(resolved.value(QStringLiteral("term")).toString(), QStringLiteral("xterm-256color"));

	/* And the fields it did not override still follow the preset. */
	QCOMPARE(resolved.value(QStringLiteral("font_family")).toString(), QStringLiteral("Iosevka"));

	/* A stale override for a field that no longer exists is ignored
	 * rather than added to the resolved settings. */
	QJsonObject stale;
	stale.insert(QStringLiteral("removed_setting"), 7);
	QVERIFY(!bssh_settings_resolver::resolve(preset, stale)
	                 .contains(QStringLiteral("removed_setting")));
}

void preset_test::an_override_equal_to_the_preset_is_not_an_override() {
	bssh_preset_t preset;
	preset.term = QStringLiteral("vt100");

	QJsonObject overrides;
	overrides.insert(QStringLiteral("term"), QStringLiteral("vt100"));      /* same */
	overrides.insert(QStringLiteral("font_size"), 14);                       /* different */

	const QSet<QString> fields = bssh_settings_resolver::overridden_fields(preset, overrides);

	/*
	 * A badge saying "this host differs" on a value that is identical
	 * teaches people to ignore badges, and its revert control would do
	 * nothing visible (sec 10.1).
	 */
	QVERIFY(!fields.contains(QStringLiteral("term")));
	QVERIFY(fields.contains(QStringLiteral("font_size")));
	QCOMPARE(fields.size(), 1);
}

void preset_test::reverting_a_field_follows_the_preset_again() {
	bssh_preset_t preset;
	preset.font_size = 11;

	QJsonObject overrides;
	overrides.insert(QStringLiteral("font_size"), 20);

	QCOMPARE(bssh_settings_resolver::resolve(preset, overrides)
	                 .value(QStringLiteral("font_size")).toInt(), 20);

	const QJsonObject reverted =
	        bssh_settings_resolver::revert_field(overrides, QStringLiteral("font_size"));

	QCOMPARE(bssh_settings_resolver::resolve(preset, reverted)
	                 .value(QStringLiteral("font_size")).toInt(), 11);
	QVERIFY(bssh_settings_resolver::overridden_fields(preset, reverted).isEmpty());
}

void preset_test::editing_a_preset_reaches_every_host_that_did_not_override() {
	bssh_preset_t preset;
	preset.name = QStringLiteral("work-bastion");
	preset.scrollback_lines = 5000;

	/* Three hosts on one preset; the middle one has its own opinion. */
	const QJsonObject plain_a;
	QJsonObject opinionated;
	opinionated.insert(QStringLiteral("scrollback_lines"), 100);
	const QJsonObject plain_b;

	preset.scrollback_lines = 20000;   /* the user edits the preset */

	/*
	 * "Editing a preset propagates live to every host using it -- that's
	 * the whole point" (sec 10.1). Propagation is not an event to
	 * broadcast: it falls out of resolving against the preset each time,
	 * which is why nothing here has to be notified.
	 */
	QCOMPARE(bssh_settings_resolver::resolve(preset, plain_a)
	                 .value(QStringLiteral("scrollback_lines")).toInt(), 20000);
	QCOMPARE(bssh_settings_resolver::resolve(preset, plain_b)
	                 .value(QStringLiteral("scrollback_lines")).toInt(), 20000);

	/* Except the host that overrode it, which is the other half. */
	QCOMPARE(bssh_settings_resolver::resolve(preset, opinionated)
	                 .value(QStringLiteral("scrollback_lines")).toInt(), 100);
}

void preset_test::the_starter_presets_differ_where_it_matters() {
	const QList<bssh_preset_t> builtins = bssh_preset_t::builtins();
	QVERIFY(builtins.size() >= 5);

	QMap<QString, bssh_preset_t> by_name;
	for (const bssh_preset_t &preset : builtins) {
		QVERIFY2(preset.is_builtin, "a shipped preset must be read-only (sec 10.4)");
		by_name.insert(preset.name, preset);
	}

	/*
	 * Each starter preset earns its place by differing in a way that
	 * matters, not cosmetically -- they double as documentation of what a
	 * preset can express.
	 */
	QCOMPARE(by_name.value(QStringLiteral("OpenWrt / BusyBox router")).term,
	          QStringLiteral("vt100"));
	QVERIFY(!by_name.value(QStringLiteral("Network switch / serial-ish")).bell_enabled);
	QVERIFY(by_name.value(QStringLiteral("tmux-first")).on_connect_command.contains(
	                QStringLiteral("tmux")));
	QVERIFY(by_name.value(QStringLiteral("Raspberry Pi / SBC")).compression);
}
