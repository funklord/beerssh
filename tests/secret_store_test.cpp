#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "platform/secret_store.h"
#include "secret_store_test.h"

/*
 * The store writes under AppDataLocation, so the tests redirect that to a
 * temporary directory rather than touching the developer's real
 * credentials -- a test that can destroy the thing it is testing is a test
 * nobody dares run twice.
 */
static QString redirect_app_data(QTemporaryDir &dir) {
	QStandardPaths::setTestModeEnabled(false);
	qputenv("XDG_DATA_HOME", dir.path().toUtf8());
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
	        + QStringLiteral("/secrets.enc");
}

void secret_store_test::stores_and_loads_a_secret() {
	QTemporaryDir dir;
	redirect_app_data(dir);

	bssh_secret_store *store = bssh_secret_store::instance();
	store->lock();

	QVERIFY(!store->is_unlocked());
	QVERIFY(store->unlock(QStringLiteral("correct horse")));
	QVERIFY(store->is_unlocked());

	QVERIFY(store->store(QStringLiteral("profile:1"), QStringLiteral("hunter2")));
	QCOMPARE(store->load(QStringLiteral("profile:1")), QStringLiteral("hunter2"));

	/* Reopening with the same passphrase reads it back. */
	store->lock();
	QVERIFY(store->unlock(QStringLiteral("correct horse")));
	QCOMPARE(store->load(QStringLiteral("profile:1")), QStringLiteral("hunter2"));
}

void secret_store_test::the_wrong_passphrase_is_rejected_not_guessed_at() {
	QTemporaryDir dir;
	redirect_app_data(dir);

	bssh_secret_store *store = bssh_secret_store::instance();
	store->lock();
	QVERIFY(store->unlock(QStringLiteral("right")));
	store->store(QStringLiteral("id"), QStringLiteral("secret"));
	store->lock();

	/*
	 * The AEAD tag is what makes this a detectable failure rather than a
	 * silent one: without it a wrong passphrase decrypts to garbage that
	 * a caller would go on to use as a password.
	 */
	QVERIFY(!store->unlock(QStringLiteral("wrong")));
	QVERIFY(!store->is_unlocked());
	QCOMPARE(store->load(QStringLiteral("id")), QString());

	/* The right one still works afterwards -- a failed attempt must not
	 * damage the store. */
	QVERIFY(store->unlock(QStringLiteral("right")));
	QCOMPARE(store->load(QStringLiteral("id")), QStringLiteral("secret"));
}

void secret_store_test::the_file_does_not_contain_the_secret() {
	QTemporaryDir dir;
	const QString path = redirect_app_data(dir);

	bssh_secret_store *store = bssh_secret_store::instance();
	store->lock();
	QVERIFY(store->unlock(QStringLiteral("passphrase")));
	QVERIFY(store->store(QStringLiteral("profile:7"), QStringLiteral("s3kr3t-value")));

	QFile file(path);
	QVERIFY(file.open(QIODevice::ReadOnly));
	const QByteArray blob = file.readAll();
	file.close();

	/*
	 * The whole point, asserted directly. It is embarrassingly easy to
	 * ship a "secret store" that writes JSON with an encryption step that
	 * silently did nothing.
	 */
	QVERIFY(!blob.contains("s3kr3t-value"));
	QVERIFY(!blob.contains("profile:7"));
	QVERIFY(blob.startsWith("BSSH"));
}

void secret_store_test::tampering_is_detected() {
	QTemporaryDir dir;
	const QString path = redirect_app_data(dir);

	bssh_secret_store *store = bssh_secret_store::instance();
	store->lock();
	QVERIFY(store->unlock(QStringLiteral("passphrase")));
	store->store(QStringLiteral("id"), QStringLiteral("value"));
	store->lock();

	/* Flip a byte in the ciphertext. */
	QFile file(path);
	QVERIFY(file.open(QIODevice::ReadWrite));
	QByteArray blob = file.readAll();
	blob[blob.size() - 20] = char(blob.at(blob.size() - 20) ^ 0x40);
	file.seek(0);
	file.write(blob);
	file.close();

	/* A modified store must fail to open rather than yield something. */
	QVERIFY(!store->unlock(QStringLiteral("passphrase")));
}

void secret_store_test::a_locked_store_yields_nothing() {
	QTemporaryDir dir;
	redirect_app_data(dir);

	bssh_secret_store *store = bssh_secret_store::instance();
	store->lock();
	QVERIFY(store->unlock(QStringLiteral("p")));
	store->store(QStringLiteral("id"), QStringLiteral("value"));

	store->lock();

	/* Locking is not cosmetic: nothing readable, nothing writable. */
	QCOMPARE(store->load(QStringLiteral("id")), QString());
	QVERIFY(!store->store(QStringLiteral("id2"), QStringLiteral("v")));
	QVERIFY(!store->erase(QStringLiteral("id")));
}

void secret_store_test::erasing_removes_it_from_the_file() {
	QTemporaryDir dir;
	redirect_app_data(dir);

	bssh_secret_store *store = bssh_secret_store::instance();
	store->lock();
	QVERIFY(store->unlock(QStringLiteral("p")));
	store->store(QStringLiteral("gone"), QStringLiteral("value"));
	QVERIFY(store->erase(QStringLiteral("gone")));

	QCOMPARE(store->load(QStringLiteral("gone")), QString());

	/* And it is gone from the file, not just from memory. */
	store->lock();
	QVERIFY(store->unlock(QStringLiteral("p")));
	QCOMPARE(store->load(QStringLiteral("gone")), QString());
}

void secret_store_test::the_backend_describes_itself_honestly() {
	bssh_secret_store *store = bssh_secret_store::instance();

	/*
	 * sec 11 asks the UI to say which backend answered. A passphrase file
	 * and a hardware keystore are not equivalent security, and a name
	 * that hides the difference invites trusting the weaker one.
	 */
	QVERIFY(!store->is_hardware_backed());
	QVERIFY(store->needs_passphrase());
	QVERIFY(store->backend_name().contains(QStringLiteral("passphrase")));
}
