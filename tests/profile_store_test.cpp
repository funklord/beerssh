#include <QTemporaryDir>
#include <QtTest>
#include <QFile>

#include "model/profile_store.h"
#include "profile_store_test.h"
#include "ui/host_editor.h"
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
	editor.set_profile(imported);
	QCOMPARE(editor.profile().identity_file, QStringLiteral("~/.ssh/id_work"));

	/* Cleared means "the default identities", not an empty path. */
	bssh_profile_t cleared = imported;
	cleared.identity_file.clear();
	editor.set_profile(cleared);
	QVERIFY(editor.profile().identity_file.isEmpty());

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
