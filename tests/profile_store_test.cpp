#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

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
	const QString path = dir.filePath(QStringLiteral("test.db"));
	bssh_profile_store store(path);
	store.save(make_profile(QStringLiteral("x"), QStringLiteral("y")));

	/*
	 * sec 11 says secrets never touch this database. Asserting on the
	 * columns keeps that structural rather than a habit: a table with no
	 * password column cannot grow one by accident, and a future migration
	 * that adds one fails here first.
	 */
	QStringList columns;
	{
		/*
		 * Both the query AND the database handle have to be out of scope
		 * before removeDatabase, or Qt warns that the connection is
		 * still in use. A warning nobody cleans up is a warning nobody
		 * reads when it finally matters.
		 */
		QSqlDatabase check = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
		                                                QStringLiteral("schema_check"));
		check.setDatabaseName(path);
		QVERIFY(check.open());

		QSqlQuery query(check);
		QVERIFY(query.exec(QStringLiteral("PRAGMA table_info(profiles)")));
		while (query.next()) columns.append(query.value(1).toString().toLower());

		check.close();
	}
	QSqlDatabase::removeDatabase(QStringLiteral("schema_check"));

	QVERIFY(!columns.contains(QStringLiteral("password")));
	QVERIFY(!columns.contains(QStringLiteral("passphrase")));
	QVERIFY(!columns.contains(QStringLiteral("key")));
	QVERIFY(!columns.contains(QStringLiteral("secret")));
	QVERIFY(columns.contains(QStringLiteral("host")));
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
	const QString path = dir.filePath(QStringLiteral("v1.db"));

	/*
	 * A version-1 file: profiles, no layout table, user_version 1. This
	 * is what an existing user's database looks like, and migrating it
	 * has to work on the file rather than only on a fresh one -- which is
	 * the whole reason sec 10.6 wanted migrations from day one.
	 */
	{
		QSqlDatabase old = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
		                                              QStringLiteral("v1_setup"));
		old.setDatabaseName(path);
		QVERIFY(old.open());
		{
			QSqlQuery query(old);
			QVERIFY(query.exec(QStringLiteral(
			        "CREATE TABLE profiles (id INTEGER PRIMARY KEY AUTOINCREMENT,"
			        " name TEXT NOT NULL, host TEXT NOT NULL, user TEXT,"
			        " port INTEGER NOT NULL DEFAULT 22)")));
			QVERIFY(query.exec(QStringLiteral(
			        "INSERT INTO profiles (name, host, user, port)"
			        " VALUES ('legacy', 'old.example', 'nabbe', 22)")));
			QVERIFY(query.exec(QStringLiteral("PRAGMA user_version = 1")));
		}
		old.close();
	}
	QSqlDatabase::removeDatabase(QStringLiteral("v1_setup"));

	bssh_profile_store store(path);
	QVERIFY2(store.is_open(), qPrintable(store.last_error()));

	/* The existing profile survived the migration... */
	QCOMPARE(store.profiles().count(), 1);
	QCOMPARE(store.profiles().first().name, QStringLiteral("legacy"));

	/* ...and the new table works. */
	bssh_profile_store::layout_state_t layout;
	layout.profile_ids = {store.profiles().first().id};
	QVERIFY(store.save_layout(layout));
	QCOMPARE(store.layout().profile_ids.count(), 1);
}

void profile_store_test::the_stored_password_flag_round_trips() {
	QTemporaryDir dir;
	const QString path = dir.filePath(QStringLiteral("test.db"));

	int id = 0;
	{
		bssh_profile_store store(path);
		bssh_profile_t profile = make_profile(QStringLiteral("saved"), QStringLiteral("s.example"));

		/* Off unless someone says otherwise -- keeping a credential is a
		 * decision, not a default. */
		QVERIFY(!profile.has_stored_password);

		profile.has_stored_password = true;
		id = store.save(profile);
	}

	bssh_profile_store reopened(path);
	QCOMPARE(reopened.profile(id).has_stored_password, true);

	/*
	 * A flag, not a credential. It exists so the app knows whether asking
	 * for the store's passphrase is worth it -- without it, every
	 * connection would ask, including the ones with nothing saved.
	 */
	QSqlDatabase check = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
	                                                QStringLiteral("flag_check"));
	QStringList columns;
	{
		check.setDatabaseName(path);
		QVERIFY(check.open());
		QSqlQuery query(check);
		QVERIFY(query.exec(QStringLiteral("PRAGMA table_info(profiles)")));
		while (query.next()) columns.append(query.value(1).toString().toLower());
		check.close();
	}
	QSqlDatabase::removeDatabase(QStringLiteral("flag_check"));

	QVERIFY(columns.contains(QStringLiteral("has_password")));
	QVERIFY(!columns.contains(QStringLiteral("password")));
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
