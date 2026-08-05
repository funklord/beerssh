#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
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

void preset_test::the_exported_file_contains_nothing_private() {
	/*
	 * sec 10.3's guarantee, asserted on the artifact that actually
	 * travels. Every other test here reads to_json(), which is an
	 * intermediate nobody sends; what lands in ~/Downloads and gets
	 * attached to a chat window is the INI, and nothing had ever looked
	 * inside one.
	 *
	 * write_ini() is built on to_json(), so the property transfers -- but
	 * transitively, and a field added straight to the writer would not be
	 * caught by a test that reads the other end.
	 */
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = dir.filePath(QStringLiteral("shared.ini"));

	bssh_preset_t preset;
	preset.name = QStringLiteral("work-bastion");
	preset.on_connect_command = QStringLiteral("tmux attach || tmux new");
	QVERIFY(preset.write_ini(path));

	QFile file(path);
	QVERIFY(file.open(QIODevice::ReadOnly));
	const QByteArray text = file.readAll();
	QVERIFY(!text.isEmpty());

	const QStringList forbidden = {
		QStringLiteral("host"), QStringLiteral("hostname"), QStringLiteral("port"),
		QStringLiteral("user"), QStringLiteral("username"), QStringLiteral("password"),
		QStringLiteral("passphrase"), QStringLiteral("identity"),
		QStringLiteral("secret"), QStringLiteral("known_hosts")
	};

	for (const QString &key : forbidden) {
		QVERIFY2(!text.contains((key + QStringLiteral("=")).toUtf8()),
		          qPrintable(QStringLiteral("the exported file carries %1=").arg(key)));
	}

	/* And the installation-local bookkeeping does not travel either. */
	QVERIFY2(!text.contains("id="), "an exported preset must not carry an id");
	QVERIFY2(!text.contains("builtin"), "an exported preset must not claim to be shipped");

	/* What it does carry is behaviour. */
	QVERIFY(text.contains("tmux attach"));
}

void preset_test::an_ini_round_trip_keeps_every_field() {
	/*
	 * The INI is a SECOND reader -- QSettings and a hand-written type
	 * conversion, not from_json's -- and it was untested end to end while
	 * the JSON round trip next door was covered. A field the writer emits
	 * and the reader drops is a preset that imports subtly wrong, which
	 * is the failure the one-format decision was meant to rule out.
	 */
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = dir.filePath(QStringLiteral("preset.ini"));

	bssh_preset_t preset;
	preset.name = QStringLiteral("slow link");
	preset.term = QStringLiteral("vt100");
	preset.scrollback_lines = 1234;
	preset.bell_enabled = false;
	preset.cursor_style = QStringLiteral("bar");
	preset.cursor_blink = false;
	preset.font_family = QStringLiteral("Iosevka");
	preset.font_size = 17;
	preset.colour_scheme = QStringLiteral("solarized-dark");
	preset.allow_clipboard_write = true;
	preset.keepalive_seconds = 15;
	preset.compression = true;
	preset.on_connect_command = QStringLiteral("tmux attach");
	preset.key_row_layout = QStringLiteral("compact");

	QVERIFY(preset.write_ini(path));

	bool ok = false;
	const bssh_preset_t restored = bssh_preset_t::read_ini(path, &ok);
	QVERIFY2(ok, "a file this app just wrote must read back");

	QCOMPARE(restored.name, preset.name);
	QCOMPARE(restored.term, preset.term);
	QCOMPARE(restored.scrollback_lines, preset.scrollback_lines);
	QCOMPARE(restored.bell_enabled, preset.bell_enabled);
	QCOMPARE(restored.cursor_style, preset.cursor_style);
	QCOMPARE(restored.cursor_blink, preset.cursor_blink);
	QCOMPARE(restored.font_family, preset.font_family);
	QCOMPARE(restored.font_size, preset.font_size);
	QCOMPARE(restored.colour_scheme, preset.colour_scheme);
	QCOMPARE(restored.keepalive_seconds, preset.keepalive_seconds);
	QCOMPARE(restored.compression, preset.compression);
	QCOMPARE(restored.on_connect_command, preset.on_connect_command);
	QCOMPARE(restored.key_row_layout, preset.key_row_layout);

	/*
	 * allow_clipboard_write is the one worth naming: it is a trust
	 * decision (sec 7.1), and a round trip that quietly dropped it would
	 * turn "yes, this host may write my clipboard" back into the default
	 * without anybody being told.
	 */
	QCOMPARE(restored.allow_clipboard_write, preset.allow_clipboard_write);
}

void preset_test::a_hostile_ini_cannot_claim_to_be_builtin() {
	/*
	 * The same attack an_imported_preset_cannot_claim_to_be_builtin()
	 * covers for JSON, aimed at the reader that actually opens files
	 * somebody sent. A shared preset importing as one of the shipped ones
	 * would take a read-only status and an id it was not given, and the
	 * shipped set is what every other preset is cloned from.
	 */
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	const QString path = dir.filePath(QStringLiteral("hostile.ini"));

	{
		QFile file(path);
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write("[preset]\n"
		            "name=Default Linux\n"
		            "is_builtin=true\n"
		            "builtin=true\n"
		            "id=1\n"
		            "term=vt100\n");
	}

	bool ok = false;
	const bssh_preset_t imported = bssh_preset_t::read_ini(path, &ok);
	QVERIFY(ok);

	QVERIFY2(!imported.is_builtin, "an imported preset must never be shipped");
	QCOMPARE(imported.id, 0);

	/* The rest of it still imports -- it is a preset, just not a shipped one. */
	QCOMPARE(imported.term, QStringLiteral("vt100"));
}
