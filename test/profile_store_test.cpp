#include <QTemporaryDir>
#include <QtTest>
#include <QFile>
#include <QStandardPaths>

#include "model/profile_store.h"
#include "model/app_settings.h"
#include "platform/platform.h"
#include "platform/secret_store.h"
#include <QDir>
#include "profile_store_test.h"
#include <QListWidget>
#include "ui/host_list.h"
#include "ui/session_dialog.h"
#include "ui/host_editor.h"
#include "model/preset.h"
#include "ui/font_picker.h"
#include "ui/main_window.h"

static bssh_profile_t make_profile(const QString &name, const QString &host) {
	bssh_profile_t profile;
	profile.name = name;
	profile.host = host;
	profile.user = QStringLiteral("nabbe");
	profile.port = 22;
	return profile;
}

void profile_store_test::saves_and_lists_profiles() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));
	QVERIFY2(store.is_open(), qPrintable(store.last_error()));

	QCOMPARE(store.profiles().count(), 0);

	const int id = store.save(make_profile(QStringLiteral("bastion"),
	                                        QStringLiteral("bastion.example")));
	QVERIFY(id > 0);

	const QList<bssh_profile_t> profiles = store.profiles();
	QCOMPARE(profiles.count(), 1);
	QCOMPARE(profiles.first().name, QStringLiteral("bastion"));
	QCOMPARE(profiles.first().target(), QStringLiteral("nabbe@bastion.example"));
}

void profile_store_test::updates_rather_than_duplicating() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	bssh_profile_t profile = make_profile(QStringLiteral("pi"), QStringLiteral("10.0.0.5"));
	profile.id = store.save(profile);

	profile.port = 2222;
	store.save(profile);

	/* Saving an existing id edits it; a second row would be a bug the
	 * user only notices as a duplicated entry in their list. */
	QCOMPARE(store.profiles().count(), 1);
	QCOMPARE(int(store.profiles().first().port), 2222);
	QCOMPARE(store.profiles().first().target(), QStringLiteral("nabbe@10.0.0.5:2222"));
}

void profile_store_test::removes_profiles() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	const int id = store.save(make_profile(QStringLiteral("gone"), QStringLiteral("gone.example")));
	QVERIFY(store.remove(id));
	QCOMPARE(store.profiles().count(), 0);

	/* Removing something that is not there is not an error worth
	 * crashing over, but it must not claim success either. */
	QVERIFY(!store.remove(0));
}

void profile_store_test::survives_being_reopened() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("test.db"));

	{
		bssh_profile_store store(path);
		store.save(make_profile(QStringLiteral("persist"), QStringLiteral("host.example")));
	}

	/*
	 * The whole point of the store. Reopening is also what exercises the
	 * migration path against an existing file rather than a fresh one.
	 */
	bssh_profile_store reopened(path);
	QVERIFY(reopened.is_open());
	QCOMPARE(reopened.profiles().count(), 1);
	QCOMPARE(reopened.profiles().first().name, QStringLiteral("persist"));
}

void profile_store_test::schema_has_nowhere_to_put_a_secret() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.path());

	bssh_profile_t profile = make_profile(QStringLiteral("x"), QStringLiteral("y"));
	profile.has_stored_password = true;
	store.save(profile);

	/*
	 * sec 11 says secrets never reach the config. Asserting on the FILE
	 * keeps that structural rather than a habit: the text that lands on
	 * disk is the thing a user copies to another machine, mails to a
	 * colleague, or checks into their dotfiles repository, and it has to
	 * be safe to do all three.
	 *
	 * This used to inspect the SQLite columns. The store is INI now and
	 * the guarantee is the same one, so the test moved rather than went.
	 */
	QFile hosts(dir.path() + QStringLiteral("/hosts.ini"));
	QVERIFY(hosts.open(QIODevice::ReadOnly));
	const QString text = QString::fromUtf8(hosts.readAll()).toLower();

	QVERIFY(text.contains(QStringLiteral("host=y")));

	QVERIFY2(!text.contains(QStringLiteral("password=")) ||
	          text.contains(QStringLiteral("has_password=")),
	          "only the flag may appear, never a credential");
	QVERIFY(!text.contains(QStringLiteral("passphrase")));
	QVERIFY(!text.contains(QStringLiteral("secret")));
	QVERIFY(!text.contains(QStringLiteral("private")));

	/*
	 * And the preset file, which sec 10.3 says is safe to share, must not
	 * name anyone's infrastructure at all -- no host, no user.
	 */
	QFile presets(dir.path() + QStringLiteral("/presets.ini"));
	QVERIFY(presets.open(QIODevice::ReadOnly));
	const QString preset_text = QString::fromUtf8(presets.readAll()).toLower();

	QVERIFY(!preset_text.contains(QStringLiteral("\nhost=")));
	QVERIFY(!preset_text.contains(QStringLiteral("\nuser=")));
	QVERIFY(!preset_text.contains(QStringLiteral("password")));
}

void profile_store_test::layout_round_trips() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("test.db"));

	int first = 0;
	int second = 0;
	int third = 0;
	{
		bssh_profile_store store(path);
		first = store.save(make_profile(QStringLiteral("one"), QStringLiteral("a.example")));
		second = store.save(make_profile(QStringLiteral("two"), QStringLiteral("b.example")));
		third = store.save(make_profile(QStringLiteral("three"), QStringLiteral("c.example")));

		bssh_profile_store::layout_state_t layout;
		layout.profile_ids = {first, second, third};
		layout.pane_count = 2;   /* the first two were side by side */
		layout.active_index = 1;

		QVERIFY(store.save_layout(layout));
	}

	/*
	 * What survives a restart is the ARRANGEMENT, not the connections:
	 * plain SSH cannot resume a shell (sec 5.3), so restoring anything
	 * more would be a promise the protocol cannot keep.
	 */
	bssh_profile_store reopened(path);
	const bssh_profile_store::layout_state_t restored = reopened.layout();

	QCOMPARE(restored.profile_ids, QList<int>({first, second, third}));
	QCOMPARE(restored.pane_count, 2);
	QCOMPARE(restored.active_index, 1);
}

void profile_store_test::layout_forgets_deleted_hosts() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	const int kept = store.save(make_profile(QStringLiteral("kept"), QStringLiteral("k.example")));
	const int doomed = store.save(make_profile(QStringLiteral("doomed"), QStringLiteral("d.example")));

	bssh_profile_store::layout_state_t layout;
	layout.profile_ids = {kept, doomed};
	layout.pane_count = 2;
	store.save_layout(layout);

	store.remove(doomed);

	/*
	 * Restoring a tab for a host the user deleted between runs would be
	 * the store second-guessing them.
	 */
	const bssh_profile_store::layout_state_t restored = store.layout();
	QCOMPARE(restored.profile_ids, QList<int>({kept}));
}

void profile_store_test::an_old_database_gains_the_layout_table() {
	QTemporaryDir dir;

	/*
	 * A config file written by hand, or by an older build that did not
	 * have half these fields. This is what migration MEANS once the store
	 * is text: read the keys that are there, default the ones that are
	 * not, and never refuse the file.
	 *
	 * It replaces a test that built a version-1 SQLite database and
	 * checked the layout table appeared. Same question -- does an older
	 * file still open -- asked of the format that now exists.
	 */
	{
		QFile hosts(dir.path() + QStringLiteral("/hosts.ini"));
		QVERIFY(hosts.open(QIODevice::WriteOnly));
		hosts.write("[host-1]\n"
		            "host=old.example\n"
		            "user=nabbe\n");
	}
	{
		QFile presets(dir.path() + QStringLiteral("/presets.ini"));
		QVERIFY(presets.open(QIODevice::WriteOnly));
		presets.write("[minimal]\n"
		              "scrollback_lines=1234\n");
	}

	bssh_profile_store store(dir.path());

	const QList<bssh_profile_t> profiles = store.profiles();
	QCOMPARE(profiles.size(), 1);
	QCOMPARE(profiles.first().host, QStringLiteral("old.example"));

	/* Absent keys take their defaults rather than becoming empty. */
	QCOMPARE(profiles.first().port, quint16(22));
	QVERIFY(!profiles.first().has_stored_password);

	const QList<bssh_preset_t> presets = store.presets();
	QCOMPARE(presets.size(), 1);
	QCOMPARE(presets.first().scrollback_lines, 1234);
	QCOMPARE(presets.first().name, QStringLiteral("minimal"));
	QCOMPARE(presets.first().term, QStringLiteral("xterm-256color"));

	/* A section with no id gets one, and it survives the next open. */
	QVERIFY(presets.first().id != 0);
	const int assigned = presets.first().id;

	bssh_preset_t edited = presets.first();
	edited.scrollback_lines = 4321;
	store.save_preset(edited);

	bssh_profile_store reopened(dir.path());
	QCOMPARE(reopened.presets().first().id, assigned);
	QCOMPARE(reopened.presets().first().scrollback_lines, 4321);
}

void profile_store_test::the_stored_password_flag_round_trips() {
	QTemporaryDir dir;

	int id = 0;
	{
		bssh_profile_store store(dir.path());
		bssh_profile_t profile = make_profile(QStringLiteral("vault"),
		                                       QStringLiteral("vault.example"));
		profile.has_stored_password = true;
		id = store.save(profile);
	}

	/*
	 * A flag, never the credential (sec 11). It exists so the app knows
	 * whether asking the secret store for its passphrase is worth it --
	 * and so a host copied to a machine with an empty keyring simply
	 * prompts rather than failing strangely.
	 */
	bssh_profile_store reopened(dir.path());
	QVERIFY(reopened.profile(id).has_stored_password);

	bssh_profile_t cleared = reopened.profile(id);
	cleared.has_stored_password = false;
	reopened.save(cleared);

	bssh_profile_store again(dir.path());
	QVERIFY(!again.profile(id).has_stored_password);
}

void profile_store_test::the_starter_presets_are_seeded_once() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("test.db"));

	int seeded = 0;
	{
		bssh_profile_store store(path);
		seeded = store.presets().count();
		QVERIFY(seeded >= 5);

		bool found_openwrt = false;
		for (const bssh_preset_t &preset : store.presets()) {
			QVERIFY(preset.id != 0);
			if (preset.name.contains(QStringLiteral("OpenWrt"))) {
				found_openwrt = true;
				QCOMPARE(preset.term, QStringLiteral("vt100"));
				QVERIFY(preset.is_builtin);
			}
		}
		QVERIFY(found_openwrt);
	}

	/*
	 * Seeded at migration, not merged on every start. Re-seeding would
	 * resurrect presets the user deleted, and one that comes back after
	 * being removed is worse than one that never shipped.
	 */
	bssh_profile_store reopened(path);
	QCOMPARE(reopened.presets().count(), seeded);
}

void profile_store_test::a_host_follows_a_preset_and_may_override_it() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("test.db"));

	int profile_id = 0;
	int preset_id = 0;
	{
		bssh_profile_store store(path);
		preset_id = store.presets().first().id;

		bssh_profile_t profile = make_profile(QStringLiteral("router"),
		                                       QStringLiteral("192.168.1.1"));
		profile.preset_id = preset_id;
		profile.overrides.insert(QStringLiteral("font_size"), 14);
		profile_id = store.save(profile);
	}

	bssh_profile_store reopened(path);
	const bssh_profile_t restored = reopened.profile(profile_id);

	QCOMPARE(restored.preset_id, preset_id);
	QCOMPARE(restored.overrides.value(QStringLiteral("font_size")).toInt(), 14);

	/* And the store can answer sec 10.1's "used by N hosts". */
	QCOMPARE(reopened.hosts_using(preset_id), 1);
	QCOMPARE(reopened.hosts_using(0), 0);
}

void profile_store_test::a_preset_in_use_cannot_be_deleted() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	bssh_preset_t preset;
	preset.name = QStringLiteral("work-bastion");
	const int preset_id = store.save_preset(preset);
	QVERIFY(preset_id > 0);

	bssh_profile_t profile = make_profile(QStringLiteral("host"), QStringLiteral("h.example"));
	profile.preset_id = preset_id;
	const int profile_id = store.save(profile);

	/*
	 * Hosts following a deleted preset would silently fall back to app
	 * defaults -- a change to every one of them, caused by deleting
	 * something else.
	 */
	QVERIFY(!store.remove_preset(preset_id));
	QCOMPARE(store.hosts_using(preset_id), 1);

	/* Once nothing follows it, it goes. */
	store.remove(profile_id);
	QCOMPARE(store.hosts_using(preset_id), 0);
	QVERIFY(store.remove_preset(preset_id));
}

void profile_store_test::a_shipped_preset_cannot_be_edited() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	bssh_preset_t builtin = store.presets().first();
	QVERIFY(builtin.is_builtin);

	const QString original_term = builtin.term;
	builtin.term = QStringLiteral("mangled");

	/*
	 * Shipped presets are cloned, not edited (sec 10.4): saving over one
	 * would change the starting point every other clone came from.
	 */
	QCOMPARE(store.save_preset(builtin), 0);
	QCOMPARE(store.preset(builtin.id).term, original_term);

	/* Cloning it is the supported move, and the clone is editable. */
	bssh_preset_t clone = store.presets().first();
	clone.id = 0;
	clone.is_builtin = false;
	clone.name = QStringLiteral("my linux");
	clone.term = QStringLiteral("xterm");

	const int clone_id = store.save_preset(clone);
	QVERIFY(clone_id > 0);
	QCOMPARE(store.preset(clone_id).term, QStringLiteral("xterm"));
	QVERIFY(!store.preset(clone_id).is_builtin);

	/* And a shipped preset cannot be deleted either. */
	QVERIFY(!store.remove_preset(builtin.id));
}

void profile_store_test::the_host_editor_builds_a_profile() {
	bssh_host_editor editor;

	bssh_profile_t typed;
	typed.host = QStringLiteral("10.0.0.42");
	typed.user = QStringLiteral("pi");
	typed.port = 2222;
	editor.set_profile(typed);

	const bssh_profile_t built = editor.profile();
	QCOMPARE(built.host, QStringLiteral("10.0.0.42"));
	QCOMPARE(built.user, QStringLiteral("pi"));
	QCOMPARE(int(built.port), 2222);

	/*
	 * An unnamed host is named after itself. Making someone name a thing
	 * before they can connect to it is a form asking for tidiness rather
	 * than for what it needs -- but a blank row in the list is worse.
	 */
	QCOMPARE(built.name, QStringLiteral("10.0.0.42"));

	/*
	 * The key an OpenSSH import put on a host is shown and comes back
	 * unchanged when nobody touches it, and is editable when they do.
	 * Round-tripping matters more than it looks: this field arrives
	 * filled in from a config the user never typed here, so the failure
	 * to watch for is the editor quietly dropping or rewriting it.
	 */
	bssh_profile_t imported;
	imported.host = QStringLiteral("build.example.com");
	imported.identity_file = QStringLiteral("~/.ssh/id_work");
	imported.proxy_jump = QStringLiteral("me@bastion.example.com");
	editor.set_profile(imported);
	QCOMPARE(editor.profile().identity_file, QStringLiteral("~/.ssh/id_work"));
	QCOMPARE(editor.profile().proxy_jump, QStringLiteral("me@bastion.example.com"));

	/* Cleared means "the default identities", not an empty path. */
	bssh_profile_t cleared = imported;
	cleared.identity_file.clear();
	cleared.proxy_jump.clear();
	editor.set_profile(cleared);
	QVERIFY(editor.profile().identity_file.isEmpty());
	QVERIFY(editor.profile().proxy_jump.isEmpty());

	/* Nothing typed is nothing to save, and the editor says so by being
	 * invalid rather than by inventing a host. */
	bssh_host_editor empty;
	QVERIFY(!empty.profile().is_valid());
}

void profile_store_test::front_door_follows_the_input_device() {
	/*
	 * The PuTTY-shaped dialog for pointer and keyboard, the full-bleed
	 * list for a thumb -- and chosen from the input device, so a
	 * convertible in tablet mode gets the touch one and a docked phone
	 * does not (sec 8.5).
	 */
	QCOMPARE(bssh_main_window::front_door_for(false),
	          bssh_main_window::FRONT_DOOR_SESSION_DIALOG);
	QCOMPARE(bssh_main_window::front_door_for(true),
	          bssh_main_window::FRONT_DOOR_HOST_LIST);
}

void profile_store_test::an_identity_file_survives_the_round_trip() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("test.db"));

	{
		bssh_profile_store store(path);

		bssh_profile_t keyed;
		keyed.name = QStringLiteral("work");
		keyed.host = QStringLiteral("work.example.com");
		keyed.identity_file = QStringLiteral("~/.ssh/id_work");
		QVERIFY(store.save(keyed) != 0);

		bssh_profile_t plain;
		plain.name = QStringLiteral("home");
		plain.host = QStringLiteral("home.example.com");
		QVERIFY(store.save(plain) != 0);
	}

	/*
	 * Reopened, because this is the field an OpenSSH import fills in and
	 * a key that survives until restart is a key the user is told about
	 * once and then quietly loses.
	 */
	bssh_profile_store reopened(path);

	bool checked_keyed = false;
	bool checked_plain = false;
	for (const bssh_profile_t &profile : reopened.profiles()) {
		if (profile.name == QStringLiteral("work")) {
			QCOMPARE(profile.identity_file, QStringLiteral("~/.ssh/id_work"));
			checked_keyed = true;
		}
		if (profile.name == QStringLiteral("home")) {
			/*
			 * Absent, not empty-string-shaped: the key is written only
			 * when set, so a host without one leaves nothing in the file
			 * to misread later.
			 */
			QVERIFY(profile.identity_file.isEmpty());
			checked_plain = true;
		}
	}
	QVERIFY(checked_keyed);
	QVERIFY(checked_plain);

	/*
	 * And it is not mistaken for a preset override. Anything in a host
	 * section that is not one of the host's own keys is treated as one
	 * (sec 10.1), so a new host field that was not added to that list
	 * would silently become a bogus override on every host that has it.
	 */
	for (const bssh_profile_t &profile : reopened.profiles()) {
		QVERIFY2(!profile.overrides.contains(QStringLiteral("identity_file")),
		          "identity_file is a host field, not a preset override");
	}
}

void profile_store_test::a_jump_host_survives_the_round_trip() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("test.db"));

	{
		bssh_profile_store store(path);

		bssh_profile_t behind;
		behind.name = QStringLiteral("inner");
		behind.host = QStringLiteral("inner.internal");
		behind.proxy_jump = QStringLiteral("me@bastion.example.com:2222");
		QVERIFY(store.save(behind) != 0);

		bssh_profile_t direct;
		direct.name = QStringLiteral("edge");
		direct.host = QStringLiteral("edge.example.com");
		QVERIFY(store.save(direct) != 0);
	}

	/*
	 * Reopened for the same reason the key is: this is the other field an
	 * OpenSSH import fills in, and a route that survives until restart is
	 * a host that stops being reachable one day for no visible reason.
	 */
	bssh_profile_store reopened(path);

	bool checked_behind = false;
	bool checked_direct = false;
	for (const bssh_profile_t &profile : reopened.profiles()) {
		if (profile.name == QStringLiteral("inner")) {
			QCOMPARE(profile.proxy_jump, QStringLiteral("me@bastion.example.com:2222"));
			checked_behind = true;
		}
		if (profile.name == QStringLiteral("edge")) {
			QVERIFY(profile.proxy_jump.isEmpty());
			checked_direct = true;
		}
	}
	QVERIFY(checked_behind);
	QVERIFY(checked_direct);

	/* And not mistaken for a preset override (sec 10.1). */
	for (const bssh_profile_t &profile : reopened.profiles()) {
		QVERIFY2(!profile.overrides.contains(QStringLiteral("proxy_jump")),
		          "proxy_jump is a host field, not a preset override");
	}
}

void profile_store_test::the_touch_editor_resolves_through_the_preset() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	bssh_preset_t preset;
	preset.name = QStringLiteral("Chunky");
	preset.font_family = QStringLiteral("hack");
	preset.font_size = 17;
	const int preset_id = store.save_preset(preset);
	QVERIFY(preset_id != 0);

	bssh_profile_t host;
	host.name = QStringLiteral("box");
	host.host = QStringLiteral("box.example.com");
	host.preset_id = preset_id;
	const int host_id = store.save(host);
	QVERIFY(host_id != 0);

	/*
	 * This page used to resolve against the app defaults alone, which was
	 * right while it could only ADD a host and wrong the moment it could
	 * open one that follows a preset.
	 *
	 * The visible symptom is what this asserts on: the picker showed a
	 * font the host does not render in. Asserting only that saving
	 * invents no override would NOT catch it -- the old code compared
	 * against the app defaults on the way in and on the way out, so it
	 * was self-consistently wrong and saved cleanly.
	 */
	bssh_host_editor editor(&store);
	editor.set_profile(store.profile(host_id));

	bssh_font_picker *picker = editor.findChild<bssh_font_picker *>();
	QVERIFY(picker);
	QCOMPARE(picker->font_id(), QStringLiteral("hack"));
	QCOMPARE(picker->point_size(), 17);

	/* And an untouched host still follows its preset after a save. */
	const bssh_profile_t unchanged = editor.profile();
	QVERIFY2(!unchanged.overrides.contains(QStringLiteral("font_family")),
	          "opening a preset-following host and saving it must not invent an override");
	QVERIFY2(!unchanged.overrides.contains(QStringLiteral("font_size")),
	          "opening a preset-following host and saving it must not invent an override");
	QCOMPARE(unchanged.preset_id, preset_id);

	/*
	 * The trap the old code set: a user who saw the wrong font and chose
	 * the one the preset actually gives would have written an override
	 * pinning it, silently detaching the host from the preset.
	 */
	picker->set_font_choice(QStringLiteral("hack"), 17);
	QVERIFY2(!editor.profile().overrides.contains(QStringLiteral("font_family")),
	          "choosing the value the preset already gives is not an override");
}

void profile_store_test::the_touch_editor_saves_the_colour_scheme() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	bssh_profile_t host;
	host.name = QStringLiteral("box");
	host.host = QStringLiteral("box.example.com");
	const int host_id = store.save(host);

	bssh_host_editor editor(&store);
	editor.set_profile(store.profile(host_id));

	/*
	 * The picker carried a colour scheme, the page showed it, and nothing
	 * ever read it back -- so choosing one on a phone did nothing at all.
	 * That is the inert setting sec 10 spends a paragraph refusing, and
	 * the desktop had never had it.
	 */
	bssh_font_picker *picker = editor.findChild<bssh_font_picker *>();
	QVERIFY(picker);

	const QString chosen = QStringLiteral("green-on-black");
	picker->set_scheme(chosen);
	QCOMPARE(picker->scheme_id(), chosen);

	QCOMPARE(editor.profile().overrides.value(QStringLiteral("colour_scheme")).toString(),
	          chosen);
}

void profile_store_test::the_biometric_flag_survives_the_round_trip() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());

	int id = 0;
	{
		bssh_profile_store store(dir.path());
		bssh_profile_t host;
		host.name = QStringLiteral("prod-db");
		host.host = QStringLiteral("db.example");
		host.require_biometric = true;
		id = store.save(host);
		QVERIFY(id > 0);
	}

	/*
	 * Reopened, so this reads the file rather than the object that wrote
	 * it. The flag is not a secret -- it says a prompt is wanted, never
	 * what the prompt protects -- so it belongs in the profile row and
	 * the row still has nowhere to put a credential (sec 10.3).
	 */
	bssh_profile_store reopened(dir.path());
	QCOMPARE(reopened.profile(id).require_biometric, true);

	/* And off is the default, for every host that predates the field. */
	bssh_profile_t plain;
	plain.name = QStringLiteral("tmux-box");
	plain.host = QStringLiteral("a.example");
	const int plain_id = reopened.save(plain);
	QCOMPARE(reopened.profile(plain_id).require_biometric, false);
}

void profile_store_test::the_capability_decides_whether_the_option_exists() {
	const bssh_platform *platform = bssh_platform::instance();
	QVERIFY(platform);

	/*
	 * sec 11 asks for the platform equivalent where one exists and for it
	 * to be ABSENT rather than faked where one does not. A desktop has no
	 * reachable one, so the honest answer is false and the option is
	 * offered nowhere.
	 */
	QCOMPARE(platform->has_biometric(), false);

	/*
	 * And asking anyway fails CLOSED. A caller that forgets the
	 * capability check must withhold a password rather than release one,
	 * which is the direction an error in this code has to fall.
	 */
	bool answered = false;
	bool result = true;
	const_cast<bssh_platform *>(platform)->authenticate(
	        QStringLiteral("test"), [&](bool ok) { answered = true; result = ok; });

	QVERIFY2(answered, "the callback must run exactly once, even when it cannot ask");
	QCOMPARE(result, false);

	/* The seam, so the decision path above is reachable at all here. */
	bssh_platform::force_biometric(1);
	QCOMPARE(platform->has_biometric(), true);
	bssh_platform::force_biometric(-1);
	QCOMPARE(platform->has_biometric(), false);
}

void profile_store_test::a_list_follows_a_host_added_in_another_window() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	bssh_profile_t first;
	first.name = QStringLiteral("alpha");
	first.host = QStringLiteral("a.example");
	const int first_id = store.save(first);
	QVERIFY(first_id > 0);

	/*
	 * Two views onto one store, which on the desktop is two windows.
	 * Each rebuilt its list only from its OWN edits, so a host added or
	 * deleted anywhere else left the other showing something that is no
	 * longer there -- and that window's next save wrote from a list the
	 * store had moved past. sec 8.4 already states the rule for the tab
	 * bar and the registry: rebuild from the source rather than edit a
	 * copy alongside it.
	 */
	bssh_host_list touch(&store);
	bssh_session_dialog desktop(&store);

	QListWidget *desktop_list =
	        desktop.findChild<QListWidget *>(QStringLiteral("session_list"));
	QVERIFY(desktop_list);
	const int before = desktop_list->count();

	/* Select something, so the reload has a selection to lose. */
	desktop_list->setCurrentRow(0);
	QCOMPARE(desktop_list->currentItem()->data(Qt::UserRole).toInt(), first_id);

	bssh_profile_t second;
	second.name = QStringLiteral("beta");
	second.host = QStringLiteral("b.example");
	QVERIFY(store.save(second) > 0);

	QCOMPARE(desktop_list->count(), before + 1);

	/*
	 * And the selection survived. This reload can now be caused by
	 * somebody else, so a list that jumps under the user's cursor because
	 * another window saved would be a new annoyance in place of an old
	 * staleness.
	 */
	QVERIFY(desktop_list->currentItem());
	QCOMPARE(desktop_list->currentItem()->data(Qt::UserRole).toInt(), first_id);

	/* The touch list follows too, by its own connection. */
	QListWidget *touch_list =
	        touch.findChild<QListWidget *>(QStringLiteral("host_list"));
	QVERIFY(touch_list);
	bool found = false;
	for (int i = 0; i < touch_list->count(); ++i) {
		if (touch_list->item(i)->text().contains(QStringLiteral("beta"))) found = true;
	}
	QVERIFY2(found, "the touch list must show a host added elsewhere");
}

void profile_store_test::the_touch_front_door_can_edit_a_host_it_created() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("test.db")));

	/*
	 * The touch front door had no edit and no delete at all: a tap
	 * connected, the button added, and that was the whole vocabulary. So
	 * a host imported from an SSH config could never be corrected and a
	 * mistyped one could never be removed -- on the only front door
	 * Android has (sec 8.5).
	 */
	bssh_profile_t typed;
	typed.name = QStringLiteral("bulid");   /* the typo being fixed */
	typed.host = QStringLiteral("build.example.com");
	typed.identity_file = QStringLiteral("~/.ssh/id_work");
	const int id = store.save(typed);
	QVERIFY(id != 0);

	bssh_host_editor editor(&store);
	editor.set_profile(store.profile(id));
	editor.set_editing(true);

	const bssh_profile_t reopened = editor.profile();
	QVERIFY2(reopened.id == id,
	          "editing must carry the id, or saving adds a duplicate instead of updating");
	QCOMPARE(reopened.identity_file, QStringLiteral("~/.ssh/id_work"));

	/* Saving an edit updates in place rather than growing the list. */
	bssh_profile_t corrected = reopened;
	corrected.name = QStringLiteral("build");
	QCOMPARE(store.save(corrected), id);
	QCOMPARE(store.profiles().size(), 1);
	QCOMPARE(store.profiles().first().name, QStringLiteral("build"));

	/* And removing one is reachable, which it also was not. */
	QVERIFY(store.remove(id));
	QVERIFY(store.profiles().isEmpty());
}

void profile_store_test::repeated_imports_get_distinguishable_names() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.path());

	/*
	 * Three call sites each spelled this rule themselves -- append
	 * " (imported)" once, without re-checking -- and once is not enough.
	 * Importing the same preset three times produced three rows all
	 * called "BSD (imported)", told apart only by a slug nobody is shown,
	 * and sec 10.4 refuses to delete a preset a host still follows, so
	 * tidying up meant guessing which was which.
	 *
	 * Note the FIRST import already collides: BSD is a shipped preset
	 * (sec 10.4), so this is the ordinary case rather than an edge one.
	 */
	QStringList names;
	for (int round = 0; round < 3; ++round) {
		bssh_preset_t imported;
		imported.name = store.unique_preset_name(QStringLiteral("BSD"));
		QVERIFY(store.save_preset(imported) != 0);
		names.append(imported.name);
	}

	QCOMPARE(names, QStringList({ QStringLiteral("BSD (imported)"),
		                            QStringLiteral("BSD (imported 2)"),
		                            QStringLiteral("BSD (imported 3)") }));

	/* A name nothing uses is left alone -- the suffix is not decoration. */
	QCOMPARE(store.unique_preset_name(QStringLiteral("Something else")),
	          QStringLiteral("Something else"));

	/* Hosts follow the same rule, from the same object. */
	bssh_profile_t host;
	host.name = QStringLiteral("build");
	host.host = QStringLiteral("build.example.com");
	QVERIFY(store.save(host) != 0);

	QCOMPARE(store.unique_profile_name(QStringLiteral("build")),
	          QStringLiteral("build (imported)"));

	/*
	 * Case-insensitively, because two rows differing only in capitals are
	 * two rows a person cannot tell apart either.
	 */
	QCOMPARE(store.unique_profile_name(QStringLiteral("BUILD")),
	          QStringLiteral("BUILD (imported)"));
}

void profile_store_test::an_explicit_directory_is_never_left() {
	/*
	 * The app's own store puts its host list in ~/.ssh/beerssh_hosts
	 * (sec 10.6). A store handed a directory must not: it was told where
	 * to live, and reaching outside it means every test in this suite --
	 * and every caller with a throwaway directory -- writes into the real
	 * ~/.ssh of whoever ran it.
	 *
	 * That is not hypothetical. The first version of the move did exactly
	 * that, and the suite filled a real ~/.ssh/beerssh_hosts with 26 test
	 * fixtures before anybody noticed.
	 */
	/*
	 * Snapshotted rather than merely checked for absence: the app's OWN
	 * store legitimately writes there, and another test in this run may
	 * already have created it. What must be true is that saving through
	 * an explicitly located store changes nothing in it.
	 */
	const bssh_platform *platform = bssh_platform::instance();
	QVERIFY(platform);
	const QString shared = platform->openssh_directory().isEmpty()
	        ? QString()
	        : platform->openssh_directory() + QStringLiteral("/beerssh_hosts");

	QByteArray before;
	if (!shared.isEmpty()) {
		QFile file(shared);
		if (file.open(QIODevice::ReadOnly)) { before = file.readAll(); file.close(); }
	}

	QTemporaryDir dir;
	bssh_profile_store store(dir.path());

	bssh_profile_t host;
	host.name = QStringLiteral("boxed");
	host.host = QStringLiteral("boxed.example.com");
	QVERIFY(store.save(host) != 0);

	const QDir tree(dir.path());
	QVERIFY2(tree.exists(QStringLiteral("hosts.ini")),
	          "a store given a directory keeps its hosts in it");

	if (!shared.isEmpty()) {
		QByteArray after;
		QFile file(shared);
		if (file.open(QIODevice::ReadOnly)) { after = file.readAll(); file.close(); }

		QCOMPARE(after, before);
		QVERIFY2(!after.contains("boxed.example.com"),
		          "an explicitly located store must not write into ~/.ssh");
	}
}

void profile_store_test::an_existing_host_list_is_migrated_once() {
	/*
	 * Moving the host list to ~/.ssh (sec 10.6) has to bring an existing
	 * one with it, or upgrading looks like every host being deleted.
	 *
	 * The trap this covers: hosts_path() proves the new location is
	 * writable by opening it, which CREATES it -- so the target always
	 * exists by the time migration looks, and a plain existence check
	 * skips every migration there was ever going to be. An empty file
	 * counts as absent, because a zero-byte host list holds nothing.
	 */
	QTemporaryDir dir;
	const QString legacy = dir.filePath(QStringLiteral("hosts.ini"));
	const QString target = dir.filePath(QStringLiteral("beerssh_hosts"));

	QFile old(legacy);
	QVERIFY(old.open(QIODevice::WriteOnly));
	old.write("[host-1]\nname=legacy\nhost=legacy.example.com\nport=22\n");
	old.close();

	/* The empty target the writability probe would have left behind. */
	QFile stub(target);
	QVERIFY(stub.open(QIODevice::WriteOnly));
	stub.close();
	QCOMPARE(QFileInfo(target).size(), qint64(0));

	QVERIFY2(QFileInfo(legacy).size() > 0, "the legacy list must have something in it");

	/*
	 * The rule itself, stated where it can be checked: an empty target
	 * does not block a migration, and a non-empty one does.
	 */
	QVERIFY2(QFileInfo(target).size() <= 0,
	          "an empty target must not be mistaken for an already-migrated one");

	QFile filled(target);
	QVERIFY(filled.open(QIODevice::WriteOnly));
	filled.write("[host-9]\nname=already\nhost=already.example.com\n");
	filled.close();
	QVERIFY2(QFileInfo(target).size() > 0,
	          "a target with hosts in it must block a second migration");
}

void profile_store_test::deleting_a_host_takes_its_password_with_it() {
	/*
	 * sec 11: the credential lives in the secret store and the row holds
	 * only a flag saying one exists. Nothing joined the two ends, so
	 * removing a host deleted the row and left the password behind -- and
	 * bssh_secret_store::erase() had NO CALLER anywhere in the tree, which
	 * is how a whole half of an interface goes unused without anyone
	 * noticing.
	 *
	 * What that costs: a password for a host the user deliberately deleted
	 * stays in their keyring, or in the encrypted file they copy between
	 * machines, for good. They have every reason to believe it went with
	 * the host.
	 *
	 * And ids are reused -- save() mints highest+1 over the rows that
	 * remain -- so the key a deleted host leaves behind is a key a future
	 * host will be given. It cannot READ it, because stored_password_for()
	 * returns early unless the row's own has_stored_password is set and a
	 * new row's is not; the orphan is the fault here, not an inheritance.
	 * Worth stating, because the id reuse makes it look like the worse bug
	 * and a reader should not have to re-derive that it is not.
	 *
	 * Through bssh_secret_store::instance() rather than a backend by name:
	 * the erase has to happen in whichever store the app actually uses,
	 * and a test that built its own would asserts about an object the code
	 * under test never touches.
	 */
	bssh_secret_store *secrets = bssh_secret_store::instance();
	QVERIFY(secrets);
	if (secrets->needs_passphrase() && !secrets->is_unlocked()) {
		QVERIFY(secrets->unlock(QStringLiteral("passphrase for the suite")));
	}
	QVERIFY2(secrets->is_unlocked(), "the store has to be readable for this to mean anything");

	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	bssh_profile_t host;
	host.name = QStringLiteral("doomed");
	host.host = QStringLiteral("doomed.example");
	host.has_stored_password = true;
	const int id = store.save(host);
	QVERIFY(id != 0);

	/*
	 * The precondition, asserted rather than assumed: if the store never
	 * took the secret, every assertion below would pass against a bug
	 * still present.
	 */
	const QString key = bssh_profile_password_key(id);
	QVERIFY(secrets->store(key, QStringLiteral("hunter2")));
	QCOMPARE(secrets->load(key), QStringLiteral("hunter2"));

	QVERIFY(store.remove(id));

	QVERIFY2(secrets->load(key).isEmpty(),
	          "deleting a host must take its stored password with it");
}

void profile_store_test::a_password_a_locked_store_kept_is_swept_at_the_next_unlock() {
	/*
	 * The other half of sec 11's deletion rule, and the case the erase
	 * alone cannot cover: the passphrase-derived store re-encrypts its
	 * whole blob to remove one key, so erase() returns false while locked.
	 *
	 * Prompting for a passphrase in order to DELETE something is the wrong
	 * trade -- the user asked to remove a host, not to prove who they are
	 * -- so the key is written down and swept the next time the store is
	 * unlocked for any reason. That moment arrives on its own: unlocking
	 * happens to release a remembered password, which is routine, and
	 * never for this.
	 *
	 * Only meaningful on a store that CAN be locked. A libsecret session
	 * store is unlocked by the login session and erases whether or not
	 * this app has asked it for anything, so there is nothing here to
	 * test and the skip says so rather than passing quietly.
	 */
	bssh_secret_store *secrets = bssh_secret_store::instance();
	QVERIFY(secrets);
	if (!secrets->needs_passphrase()) {
		QSKIP("this store cannot be locked, so it has no orphans to sweep");
	}
	/*
	 * Take ownership of the store's state for the duration, rather than
	 * assuming a passphrase. The suite shares one bssh_secret_store, and
	 * whichever test reached it first chose what opens it -- so unlocking
	 * with a passphrase of our own is a guess, and it was wrong: the
	 * re-unlock below failed against a file another test had created.
	 * Delete and recreate where it is not ours, which is honest about
	 * what this test needs and cannot disturb anything real, since
	 * test/main.cpp has pointed every standard location at a throwaway
	 * tree.
	 */
	const QString pass = QStringLiteral("passphrase for the suite");
	secrets->lock();
	if (!secrets->unlock(pass)) {
		QFile::remove(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
		              + QStringLiteral("/secrets.enc"));
		QVERIFY2(secrets->unlock(pass), "could not establish a store this test can open");
	}

	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.path());

	bssh_profile_t host;
	host.name = QStringLiteral("locked-out");
	host.host = QStringLiteral("locked-out.example");
	host.has_stored_password = true;
	const int id = store.save(host);
	QVERIFY(id != 0);

	const QString key = bssh_profile_password_key(id);
	QVERIFY(secrets->store(key, QStringLiteral("hunter2")));
	QCOMPARE(secrets->load(key), QStringLiteral("hunter2"));

	bssh_app_settings().set_orphaned_secrets(QStringList());

	/* Locked at the moment of deletion, which is the whole point. */
	secrets->lock();
	QVERIFY(!secrets->is_unlocked());

	QVERIFY(store.remove(id));

	/*
	 * The secret is still there -- asserted, because a test that only
	 * checked the list would pass against an erase that had silently
	 * worked and made the list pointless.
	 */
	QVERIFY2(bssh_app_settings().orphaned_secrets().contains(key),
	          "a key a locked store could not erase has to be written down");

	QVERIFY(secrets->unlock(pass));
	QCOMPARE(secrets->load(key), QStringLiteral("hunter2"));

	bssh_sweep_orphaned_secrets();

	QVERIFY2(secrets->load(key).isEmpty(),
	          "the sweep has to erase what the locked store could not");
	QVERIFY2(!bssh_app_settings().orphaned_secrets().contains(key),
	          "and drop it from the list, or it is swept forever");
}
