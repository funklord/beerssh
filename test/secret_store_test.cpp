#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QAbstractButton>
#include <QLabel>
#include <QtTest>

#include "platform/secret_store.h"
#include <QCheckBox>
#include <QComboBox>
#include "ui/app_settings_view.h"
#include "ui/password_prompt.h"
#include "secret_store_test.h"

/*
 * These exercise the FILE backend by name rather than through
 * instance(), which picks whatever this machine happens to be running --
 * a test that goes through the selector asserts the tester's session, not
 * the code.
 *
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

	bssh_secret_store *store = bssh_make_file_secret_store();
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

	bssh_secret_store *store = bssh_make_file_secret_store();
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

	bssh_secret_store *store = bssh_make_file_secret_store();
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

	bssh_secret_store *store = bssh_make_file_secret_store();
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

	bssh_secret_store *store = bssh_make_file_secret_store();
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

	bssh_secret_store *store = bssh_make_file_secret_store();
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
	bssh_secret_store *store = bssh_make_file_secret_store();

	/*
	 * sec 11 asks the UI to say which backend answered. A passphrase file
	 * and a hardware keystore are not equivalent security, and a name
	 * that hides the difference invites trusting the weaker one.
	 */
	QVERIFY(!store->is_hardware_backed());
	QVERIFY(store->needs_passphrase());
	QVERIFY(store->backend_name().contains(QStringLiteral("passphrase")));
}

void secret_store_test::the_selector_always_returns_a_working_store() {
	bssh_secret_store *selected = bssh_secret_store::instance();

	/*
	 * Whichever backend answered, there must BE one: a machine with no
	 * keyring falls back to the file rather than to nothing, because the
	 * alternative is an app that silently cannot save a credential.
	 */
	QVERIFY(selected != nullptr);
	QVERIFY(!selected->backend_name().isEmpty());

	/* And the choice is stable -- re-probing could answer from the
	 * keyring before the session bus appeared and from the file after,
	 * leaving secrets written to one invisible in the other. */
	QCOMPARE(bssh_secret_store::instance(), selected);

	/* The keyring needs no passphrase; the file does. Whichever this is,
	 * the two answers must agree with each other. */
	if (selected->needs_passphrase()) {
		QVERIFY(selected->backend_name().contains(QStringLiteral("passphrase")));
	} else {
		QVERIFY(selected->is_unlocked());
	}
}

void secret_store_test::a_second_settings_page_does_not_write_back_a_stale_snapshot()
{
	/*
	 * There is one of these pages per WINDOW, and each snapshots app.ini
	 * into its widgets when it is built. commit() then wrote EVERY widget
	 * back, so the older snapshot won: set the theme in one window, touch
	 * any single control in the other, and that other window's idea of
	 * the theme -- as it stood when it opened -- was written over the
	 * choice just made. Nothing subscribes to app.ini, so neither page
	 * ever learns.
	 *
	 * Two views here stand for two windows. The second is built FIRST so
	 * its snapshot is the stale one, which is the case that bites.
	 */
	bssh_app_settings().set_theme(QStringLiteral("light"));

	bssh_app_settings_view stale;      /* opened while the theme was light */
	bssh_app_settings_view current;

	QComboBox *stale_theme = stale.findChild<QComboBox *>(QStringLiteral("theme"));
	QCheckBox *stale_caps = stale.findChild<QCheckBox *>(QStringLiteral("caps_as_ctrl"));
	QComboBox *live_theme = current.findChild<QComboBox *>(QStringLiteral("theme"));

	if (!stale_theme || !stale_caps || !live_theme) {
		QSKIP("this build's settings page does not name its controls");
	}

	/* The other window chooses a theme. */
	const int dark = live_theme->findData(QStringLiteral("dark"));
	QVERIFY(dark >= 0);
	live_theme->setCurrentIndex(dark);
	QCOMPARE(bssh_app_settings().theme(), QStringLiteral("dark"));

	/*
	 * And now the stale page touches something else entirely. Its theme
	 * combo still says "light", because nothing told it otherwise.
	 */
	const bool before = stale_caps->isChecked();
	stale_caps->setChecked(!before);

	/* The choice the user actually made has to survive... */
	QCOMPARE(bssh_app_settings().theme(), QStringLiteral("dark"));

	/*
	 * ...and the control they actually touched has to have been written,
	 * or a commit() that wrote nothing at all would satisfy the line
	 * above.
	 */
	QCOMPARE(bssh_app_settings().caps_as_ctrl(), !before);
}

void secret_store_test::the_settings_page_names_the_secret_backend()
{
	const bssh_secret_store *store = bssh_secret_store::instance();
	QVERIFY(store);

	bssh_app_settings_view view;

	/*
	 * The name VERBATIM. Asserting on a substring, or on "some label
	 * mentions a keyring", would pass a page that softened the wording --
	 * which is the failure sec 11 actually warns about, not a missing
	 * label.
	 */
	bool named = false;
	bool explained = false;
	for (const QLabel *label : view.findChildren<QLabel *>()) {
		if (label->text() == store->backend_name()) named = true;

		/*
		 * And what it MEANS. A passphrase-derived store that says only
		 * its own name reads like reassurance to anyone who does not
		 * already know what it implies.
		 */
		if (store->needs_passphrase() && label->text().contains(QStringLiteral("passphrase"))
		     && label->text().contains(QStringLiteral("machine"))) {
			explained = true;
		}
		if (!store->needs_passphrase() && label->text().contains(QStringLiteral("hardware"))) {
			explained = true;
		}
	}

	QVERIFY2(named, "the settings page must show the backend's own name, unsoftened");
	QVERIFY2(explained, "and say what that means for the passwords in it");
}

void secret_store_test::no_setting_is_cut_off_at_a_phone_width()
{
	bssh_app_settings_view view;
	view.resize(360, 640);
	view.show();
	QVERIFY(QTest::qWaitForWindowExposed(&view));

	/*
	 * Asked of every wrapped label rather than of the one that was
	 * wrong: they are all explanatory text on a narrow screen, and the
	 * next one added would fail the same way for the same reason.
	 */
	for (QLabel *label : view.findChildren<QLabel *>()) {
		if (!label->isVisible() || !label->wordWrap() || label->text().isEmpty()) continue;

		const int needed = label->heightForWidth(label->width());
		QVERIFY2(label->height() >= needed,
		          qPrintable(QStringLiteral("cut off: \"%1\" has %2px for %3px of text")
		                             .arg(label->text().left(40))
		                             .arg(label->height()).arg(needed)));
	}
}

void secret_store_test::the_password_prompt_fits_a_phone()
{
	/*
	 * A destination of the length people really have: a user, a host and
	 * a non-default port. The old layout was fine for "box" and broke for
	 * this, which is why a short one would prove nothing.
	 */
	bssh_password_prompt prompt(QStringLiteral("nabbe@build.internal.example.com:2299"),
	                             true);
	prompt.show();
	QVERIFY(QTest::qWaitForWindowExposed(&prompt));

	/*
	 * 360dp is the narrow phone width sec 8.2 designs to. Asked of the
	 * MINIMUM size hint: that is what a window manager cannot go below,
	 * so anything wider than the screen is content the user cannot reach.
	 */
	const int phone = 360;
	QVERIFY2(prompt.minimumSizeHint().width() <= phone,
	          qPrintable(QStringLiteral("the prompt demands %1px on a %2px screen")
	                             .arg(prompt.minimumSizeHint().width()).arg(phone)));

	/* And the buttons are inside it, not off the edge. */
	prompt.resize(phone, prompt.sizeHint().height());
	for (const QAbstractButton *button : prompt.findChildren<QAbstractButton *>()) {
		if (!button->isVisible()) continue;
		QVERIFY2(button->geometry().right() <= phone,
		          qPrintable(QStringLiteral("\"%1\" ends at %2px on a %3px screen")
		                             .arg(button->text()).arg(button->geometry().right())
		                             .arg(phone)));
	}
}

void secret_store_test::the_store_is_replaced_atomically_and_stays_owner_only() {
	/*
	 * flush() used to open the real file with Truncate, so the existing
	 * store was destroyed BEFORE the replacement was written. An
	 * interrupted or failed write then left a truncated blob -- and
	 * unlock() rejects a short blob, so what is lost is not the secret
	 * being saved but EVERY credential in the file. The UI reports that as
	 * "either it is wrong, or the store has been modified", which sends
	 * somebody looking for an attacker.
	 *
	 * It is a QSaveFile now: a temporary beside it, renamed over the
	 * target on commit.
	 *
	 * THE MODE IS THE HALF A TEMP-AND-RENAME BREAKS. A temporary created
	 * with the default mode and renamed into place would leave the
	 * credential file world-readable, and nothing else in the tree would
	 * notice -- the secrets stay encrypted, so no test of their content
	 * would fail. This asserts the permissions rather than trusting that
	 * setPermissions survives the rename.
	 */
	QTemporaryDir dir;
	redirect_app_data(dir);

	bssh_secret_store *store = bssh_make_file_secret_store();
	store->lock();
	QVERIFY(store->unlock(QStringLiteral("correct horse")));
	QVERIFY(store->store(QStringLiteral("profile:1:password"), QStringLiteral("hunter2")));

	const QString path = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
	                      + QStringLiteral("/secrets.enc");
	QVERIFY2(QFile::exists(path), qPrintable(QStringLiteral("no store at ") + path));

	const QFileDevice::Permissions mode = QFile::permissions(path);
	QVERIFY2(mode.testFlag(QFileDevice::ReadOwner), "the owner has to be able to read it");
	QVERIFY2(!mode.testFlag(QFileDevice::ReadGroup) && !mode.testFlag(QFileDevice::ReadOther),
	          qPrintable(QStringLiteral("a credential file must not be readable by anybody "
	                                     "else; ReadGroup=%1 ReadOther=%2")
	                      .arg(mode.testFlag(QFileDevice::ReadGroup))
	                      .arg(mode.testFlag(QFileDevice::ReadOther))));

	/*
	 * And a second write, which is the one that goes through the rename
	 * path with the target already present -- the case the old code
	 * truncated.
	 */
	QVERIFY(store->store(QStringLiteral("profile:2:password"), QStringLiteral("second")));

	const QFileDevice::Permissions after = QFile::permissions(path);
	QVERIFY2(!after.testFlag(QFileDevice::ReadGroup) && !after.testFlag(QFileDevice::ReadOther),
	          "replacing the store must not widen its permissions");

	/* And it is still the store it was: both secrets readable after a reopen. */
	store->lock();
	QVERIFY(store->unlock(QStringLiteral("correct horse")));
	QCOMPARE(store->load(QStringLiteral("profile:1:password")), QStringLiteral("hunter2"));
	QCOMPARE(store->load(QStringLiteral("profile:2:password")), QStringLiteral("second"));
}
