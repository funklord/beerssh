#include "model/profile_store.h"

#include <QDir>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QVariant>

namespace {

/*
 * Bumped whenever the schema changes; migrate() walks from whatever the
 * file says to this. Present from the first version on purpose -- a store
 * that gains migrations only once it needs them has already shipped a
 * database it cannot upgrade (project.md sec 10.6).
 */
const int BSSH_SCHEMA_VERSION = 1;

int next_connection_id() {
	static int counter = 0;
	return ++counter;
}

} /* namespace */

bssh_profile_store::bssh_profile_store(const QString &path, QObject *parent)
    : QObject(parent),
      m_open(false) {
	QString target = path;
	if (target.isEmpty()) {
		const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
		QDir().mkpath(dir);
		target = dir + QStringLiteral("/beerssh.db");
	}

	m_open = open(target);
}

bssh_profile_store::~bssh_profile_store() {
	/*
	 * The connection has to be closed and then removed by name, and the
	 * QSqlDatabase handle must be out of scope before removeDatabase or
	 * Qt warns that the connection is still in use.
	 */
	{
		QSqlDatabase database = QSqlDatabase::database(m_connection_name, false);
		if (database.isOpen()) database.close();
	}
	QSqlDatabase::removeDatabase(m_connection_name);
}

bool bssh_profile_store::open(const QString &path) {
	/*
	 * A named connection per store, so two stores -- the app's and a
	 * test's -- never collide on Qt's default connection.
	 */
	m_connection_name = QStringLiteral("bssh_profiles_%1").arg(next_connection_id());

	QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection_name);
	database.setDatabaseName(path);

	if (!database.open()) {
		m_last_error = database.lastError().text();
		return false;
	}

	return migrate();
}

bool bssh_profile_store::migrate() {
	QSqlDatabase database = QSqlDatabase::database(m_connection_name);
	QSqlQuery query(database);

	int version = 0;
	if (query.exec(QStringLiteral("PRAGMA user_version")) && query.next()) {
		version = query.value(0).toInt();
	}

	if (version < 1) {
		/*
		 * No password or key column, by design: secrets live in the
		 * platform secret store behind an id (sec 11). A schema that has
		 * nowhere to put a credential cannot accidentally grow one.
		 */
		const bool created = query.exec(QStringLiteral(
		        "CREATE TABLE IF NOT EXISTS profiles ("
		        "  id INTEGER PRIMARY KEY AUTOINCREMENT,"
		        "  name TEXT NOT NULL,"
		        "  host TEXT NOT NULL,"
		        "  user TEXT,"
		        "  port INTEGER NOT NULL DEFAULT 22"
		        ")"));
		if (!created) {
			m_last_error = query.lastError().text();
			return false;
		}
	}

	if (version != BSSH_SCHEMA_VERSION) {
		query.exec(QStringLiteral("PRAGMA user_version = %1").arg(BSSH_SCHEMA_VERSION));
	}

	return true;
}

QList<bssh_profile_t> bssh_profile_store::profiles() const {
	QList<bssh_profile_t> result;
	if (!m_open) return result;

	QSqlQuery query(QSqlDatabase::database(m_connection_name));
	if (!query.exec(QStringLiteral(
	        "SELECT id, name, host, user, port FROM profiles ORDER BY name COLLATE NOCASE"))) {
		return result;
	}

	while (query.next()) {
		bssh_profile_t profile;
		profile.id = query.value(0).toInt();
		profile.name = query.value(1).toString();
		profile.host = query.value(2).toString();
		profile.user = query.value(3).toString();
		profile.port = static_cast<quint16>(query.value(4).toUInt());
		result.append(profile);
	}

	return result;
}

bssh_profile_t bssh_profile_store::profile(int id) const {
	const QList<bssh_profile_t> all = profiles();
	for (const bssh_profile_t &profile : all) {
		if (profile.id == id) return profile;
	}
	return bssh_profile_t();
}

int bssh_profile_store::save(const bssh_profile_t &profile) {
	if (!m_open || !profile.is_valid()) return 0;

	QSqlQuery query(QSqlDatabase::database(m_connection_name));

	if (profile.id == 0) {
		query.prepare(QStringLiteral(
		        "INSERT INTO profiles (name, host, user, port) VALUES (?, ?, ?, ?)"));
	} else {
		query.prepare(QStringLiteral(
		        "UPDATE profiles SET name = ?, host = ?, user = ?, port = ? WHERE id = ?"));
	}

	query.addBindValue(profile.name.isEmpty() ? profile.host : profile.name);
	query.addBindValue(profile.host);
	query.addBindValue(profile.user);
	query.addBindValue(profile.port);
	if (profile.id != 0) query.addBindValue(profile.id);

	if (!query.exec()) {
		m_last_error = query.lastError().text();
		return 0;
	}

	const int id = profile.id == 0 ? query.lastInsertId().toInt() : profile.id;
	emit changed();
	return id;
}

bool bssh_profile_store::remove(int id) {
	if (!m_open || id == 0) return false;

	QSqlQuery query(QSqlDatabase::database(m_connection_name));
	query.prepare(QStringLiteral("DELETE FROM profiles WHERE id = ?"));
	query.addBindValue(id);

	if (!query.exec()) {
		m_last_error = query.lastError().text();
		return false;
	}

	emit changed();
	return true;
}
