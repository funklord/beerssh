#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include "model/profile_store.h"
#include "profile_store_test.h"
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
	QSqlDatabase check = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
	                                                QStringLiteral("schema_check"));
	check.setDatabaseName(path);
	QVERIFY(check.open());

	QStringList columns;
	QSqlQuery query(check);
	QVERIFY(query.exec(QStringLiteral("PRAGMA table_info(profiles)")));
	while (query.next()) columns.append(query.value(1).toString().toLower());

	check.close();
	QSqlDatabase::removeDatabase(QStringLiteral("schema_check"));

	QVERIFY(!columns.contains(QStringLiteral("password")));
	QVERIFY(!columns.contains(QStringLiteral("passphrase")));
	QVERIFY(!columns.contains(QStringLiteral("key")));
	QVERIFY(!columns.contains(QStringLiteral("secret")));
	QVERIFY(columns.contains(QStringLiteral("host")));
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
