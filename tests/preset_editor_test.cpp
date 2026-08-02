#include <QtTest>
#include <QFile>
#include <QUrl>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>

#include "preset_editor_test.h"
#include "model/profile_store.h"
#include "model/settings_resolver.h"
#include "ui/preset_editor.h"

/* The editable preset the fixtures below work on: a clone, because every
 * shipped one is read-only by design (sec 10.4). */
static int make_editable_preset(bssh_profile_store &store, const QString &name) {
	bssh_preset_t preset;
	preset.name = name;
	preset.scrollback_lines = 5000;
	return store.save_preset(preset);
}

void preset_editor_test::lists_every_preset_including_the_shipped_ones() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));
	bssh_preset_editor editor(&store);

	const QListWidget *list = editor.findChild<QListWidget *>();
	QVERIFY(list != nullptr);

	/* The six starter presets are seeded on first open (sec 10.4). */
	QCOMPARE(list->count(), store.presets().size());
	QVERIFY(list->count() > 0);

	/*
	 * Shipped ones are marked in the list itself, so that the reason Save
	 * is unavailable is visible before the button is reached.
	 */
	bool marked = false;
	for (int i = 0; i < list->count(); ++i) {
		if (list->item(i)->text().contains(QStringLiteral("(shipped)"))) marked = true;
	}
	QVERIFY(marked);
}

void preset_editor_test::a_shipped_preset_cannot_be_saved_over() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));
	bssh_preset_editor editor(&store);

	/* The list is sorted by name and the starter set is all built-in, so
	 * row 0 is a shipped preset. */
	QListWidget *list = editor.findChild<QListWidget *>();
	list->setCurrentRow(0);

	const bssh_preset_t shown = store.preset(editor.current_preset_id());
	QVERIFY2(shown.is_builtin, "expected the starter set to be built-in");

	QPushButton *save = editor.findChild<QPushButton *>();
	Q_UNUSED(save);

	/*
	 * The store refuses regardless -- this asserts the editor does not
	 * offer the action in the first place. A Save button that silently
	 * does nothing is worse than one that is visibly unavailable, because
	 * the user cannot tell a refusal from a bug.
	 */
	for (QPushButton *button : editor.findChildren<QPushButton *>()) {
		if (button->text() == QStringLiteral("Save")) {
			QVERIFY2(!button->isEnabled(), "Save must be disabled for a shipped preset");
		}
		if (button->text() == QStringLiteral("Delete")) {
			QVERIFY2(!button->isEnabled(), "a shipped preset cannot be deleted either");
		}
	}
}

void preset_editor_test::cloning_a_shipped_preset_yields_an_editable_copy() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));
	bssh_preset_editor editor(&store);

	QListWidget *list = editor.findChild<QListWidget *>();
	list->setCurrentRow(0);

	const int before = store.presets().size();
	const bssh_preset_t original = store.preset(editor.current_preset_id());

	for (QPushButton *button : editor.findChildren<QPushButton *>()) {
		if (button->text() == QStringLiteral("Save as new")) button->click();
	}

	QCOMPARE(store.presets().size(), before + 1);

	const bssh_preset_t clone = store.preset(editor.current_preset_id());
	QVERIFY2(!clone.is_builtin, "a clone must be editable");
	QVERIFY2(clone.name != original.name,
	         "two presets with the same name make an unusable list");
	QCOMPARE(clone.scrollback_lines, original.scrollback_lines);
}

void preset_editor_test::the_host_count_is_shown_before_the_edit() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));

	const int preset_id = make_editable_preset(store, QStringLiteral("Servers"));

	for (int i = 0; i < 3; ++i) {
		bssh_profile_t host;
		host.host = QStringLiteral("host%1.example.com").arg(i);
		host.preset_id = preset_id;
		store.save(host);
	}

	bssh_preset_editor editor(&store);
	QListWidget *list = editor.findChild<QListWidget *>();
	for (int i = 0; i < list->count(); ++i) {
		if (list->item(i)->data(Qt::UserRole).toInt() == preset_id) list->setCurrentRow(i);
	}

	/*
	 * sec 10.1's "used by 7 hosts", and it is on screen while the fields
	 * are being typed into rather than only in the confirmation: knowing
	 * beforehand is what stops the edit being a surprise at all.
	 */
	bool found = false;
	for (const QLabel *label : editor.findChildren<QLabel *>()) {
		if (label->text().contains(QStringLiteral("3 hosts"))) found = true;
	}
	QVERIFY2(found, "the number of hosts following the preset must be visible");
}

void preset_editor_test::saving_changes_what_following_hosts_resolve_to() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));

	const int preset_id = make_editable_preset(store, QStringLiteral("Servers"));

	bssh_profile_t host;
	host.host = QStringLiteral("one.example.com");
	host.preset_id = preset_id;
	const int host_id = store.save(host);

	/* Edit the preset directly -- what is under test is the resolution
	 * that follows, not the button that triggers it. */
	bssh_preset_t preset = store.preset(preset_id);
	preset.scrollback_lines = 12345;
	store.save_preset(preset);

	const bssh_profile_t stored = store.profile(host_id);
	const QJsonObject resolved =
	        bssh_settings_resolver::resolve(store.preset(preset_id), stored.overrides);

	/* The entire point of the middle layer: one edit, every host. */
	QCOMPARE(resolved.value(QStringLiteral("scrollback_lines")).toInt(), 12345);
}

void preset_editor_test::an_overridden_field_does_not_follow_the_preset() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.db")));

	const int preset_id = make_editable_preset(store, QStringLiteral("Servers"));

	bssh_profile_t host;
	host.host = QStringLiteral("special.example.com");
	host.preset_id = preset_id;

	QJsonObject overrides;
	overrides.insert(QStringLiteral("scrollback_lines"), 50);
	host.overrides = overrides;
	const int host_id = store.save(host);

	bssh_preset_t preset = store.preset(preset_id);
	preset.scrollback_lines = 12345;
	store.save_preset(preset);

	const bssh_profile_t stored = store.profile(host_id);
	const QJsonObject resolved =
	        bssh_settings_resolver::resolve(store.preset(preset_id), stored.overrides);

	/*
	 * The other half of the promise, and the half that makes the feature
	 * safe to use: a host that said something different keeps saying it.
	 * Propagation that trampled overrides would make every deliberate
	 * exception temporary.
	 */
	QCOMPARE(resolved.value(QStringLiteral("scrollback_lines")).toInt(), 50);
}

void preset_editor_test::a_preset_round_trips_through_an_exported_file() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("servers.ini"));

	bssh_preset_t preset;
	preset.name = QStringLiteral("Servers");
	preset.term = QStringLiteral("xterm-direct");
	preset.scrollback_lines = 20000;
	preset.bell_enabled = false;
	preset.cursor_style = QStringLiteral("bar");
	preset.font_family = QStringLiteral("hack");
	preset.font_size = 13;
	preset.keepalive_seconds = 30;
	preset.compression = true;
	preset.on_connect_command = QStringLiteral("tmux attach || tmux new");

	QVERIFY(preset.write_ini(path));

	bool ok = false;
	const bssh_preset_t restored = bssh_preset_t::read_ini(path, &ok);
	QVERIFY(ok);

	/*
	 * Every field, including the booleans and the numbers -- INI holds
	 * strings, so a type that is not restored comes back as "true" or
	 * "30" and quietly stops meaning anything.
	 */
	QCOMPARE(restored.name, preset.name);
	QCOMPARE(restored.term, preset.term);
	QCOMPARE(restored.scrollback_lines, preset.scrollback_lines);
	QCOMPARE(restored.bell_enabled, preset.bell_enabled);
	QCOMPARE(restored.cursor_style, preset.cursor_style);
	QCOMPARE(restored.font_family, preset.font_family);
	QCOMPARE(restored.font_size, preset.font_size);
	QCOMPARE(restored.keepalive_seconds, preset.keepalive_seconds);
	QCOMPARE(restored.compression, preset.compression);
	QCOMPARE(restored.on_connect_command, preset.on_connect_command);
}

void preset_editor_test::an_exported_preset_names_nobodys_infrastructure() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("share-me.ini"));

	bssh_preset_t preset;
	preset.name = QStringLiteral("Servers");
	QVERIFY(preset.write_ini(path));

	QFile file(path);
	QVERIFY(file.open(QIODevice::ReadOnly));
	const QString text = QString::fromUtf8(file.readAll()).toLower();

	/*
	 * sec 10.3's whole promise, checked on the artefact rather than on the
	 * struct: this is the file a user mails to a colleague or pastes into
	 * a forum, and it must not describe their network. bssh_preset_t has
	 * no field for any of these -- this is the test that notices if one
	 * ever gains one.
	 */
	QVERIFY(!text.contains(QStringLiteral("host")));
	QVERIFY(!text.contains(QStringLiteral("user")));
	QVERIFY(!text.contains(QStringLiteral("password")));
	QVERIFY(!text.contains(QStringLiteral("passphrase")));
	QVERIFY(!text.contains(QStringLiteral("identity")));
	QVERIFY(!text.contains(QStringLiteral("port")));

	/* A schema version, so a future build knows what it is reading. */
	QVERIFY(text.contains(QStringLiteral("schema=")));
}

void preset_editor_test::an_import_never_claims_an_id_or_shipped_status() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("hostile.ini"));

	/*
	 * A file claiming to be preset number four and shipped with the app.
	 * Neither is the file's to decide: an id belongs to this
	 * installation's store, and "shipped" is what makes a preset
	 * read-only, so an import that could set it would be an import that
	 * could plant an uneditable preset.
	 */
	{
		QFile file(path);
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write("[preset]\n"
		           "name=Trojan\n"
		           "id=4\n"
		           "builtin=true\n"
		           "scrollback_lines=777\n");
	}

	bool ok = false;
	const bssh_preset_t imported = bssh_preset_t::read_ini(path, &ok);

	QVERIFY(ok);
	QCOMPARE(imported.scrollback_lines, 777);
	QCOMPARE(imported.id, 0);
	QVERIFY(!imported.is_builtin);

	/* And a file that is not a preset at all is refused rather than
	 * imported as an empty one. */
	const QString junk = dir.filePath(QStringLiteral("junk.ini"));
	{
		QFile file(junk);
		QVERIFY(file.open(QIODevice::WriteOnly));
		file.write("this is not an ini file at all\n");
	}

	bool junk_ok = true;
	bssh_preset_t::read_ini(junk, &junk_ok);
	QVERIFY(!junk_ok);
}

void preset_editor_test::an_untouched_font_stays_unset_rather_than_being_pinned() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.path());
	bssh_preset_editor editor(&store);

	/* A shipped preset: every one of them leaves the font unset (sec 10.4). */
	QListWidget *list = editor.findChild<QListWidget *>();
	list->setCurrentRow(0);
	QVERIFY(store.preset(editor.current_preset_id()).font_family.isEmpty());

	/*
	 * Cloning without touching the font must not pin one. The picker
	 * cannot display "unset" -- it shows the default that an empty value
	 * resolves to -- so writing back what it displays would turn "use
	 * this device's default" into "always use JetBrains Mono", and a
	 * preset exported from a desktop would override the condensed
	 * default a phone is supposed to get (sec 9.2).
	 */
	for (QPushButton *button : editor.findChildren<QPushButton *>()) {
		if (button->text() == QStringLiteral("Save as new")) button->click();
	}

	const bssh_preset_t clone = store.preset(editor.current_preset_id());
	QVERIFY2(clone.font_family.isEmpty(),
	         "an untouched font must not be recorded as an explicit choice");
}

void preset_editor_test::a_file_url_is_read_as_a_file_rather_than_as_a_name() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("handed-over.ini"));

	bssh_preset_t preset;
	preset.name = QStringLiteral("Handed Over");
	preset.scrollback_lines = 31337;
	QVERIFY(preset.write_ini(path));

	/*
	 * Being handed a document by URL is how the platform does it -- an
	 * Android intent carries one, and so does a desktop file manager.
	 *
	 * This is a regression test for a bug found by firing a real intent:
	 * a file:// URL was treated as "not a path" and pushed through the
	 * content:// bridge, where QFile opened it as a relative FILENAME
	 * that did not exist. The app then said "not a preset" about a file
	 * that was a perfectly good preset -- a failure that blames the user's
	 * file for the app's own mistake.
	 */
	const QString url = QUrl::fromLocalFile(path).toString();
	QVERIFY(url.startsWith(QStringLiteral("file://")));

	bool ok = false;
	const bssh_preset_t restored = bssh_preset_t::read_ini(url, &ok);

	QVERIFY2(ok, "a file:// URL must be readable");
	QCOMPARE(restored.name, QStringLiteral("Handed Over"));
	QCOMPARE(restored.scrollback_lines, 31337);

	/* And writing back through a URL lands in the same place. */
	const QString written = dir.filePath(QStringLiteral("written.ini"));
	QVERIFY(restored.write_ini(QUrl::fromLocalFile(written).toString()));
	QVERIFY(QFile::exists(written));
}
