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
const int BSSH_SCHEMA_VERSION = 3;

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

	if (version < 2) {
		/*
		 * One row per remembered tab. A table rather than a blob so it
		 * stays readable in sqlite3 -- sec 10.6 assumes this file will
		 * be inspected by hand.
		 */
		const bool created = query.exec(QStringLiteral(
		        "CREATE TABLE IF NOT EXISTS layout ("
		        "  position INTEGER PRIMARY KEY,"
		        "  profile_id INTEGER NOT NULL,"
		        "  is_pane INTEGER NOT NULL DEFAULT 0,"
		        "  is_active INTEGER NOT NULL DEFAULT 0"
		        ")"));
		if (!created) {
			m_last_error = query.lastError().text();
			return false;
		}
	}

	if (version < 3) {
		/*
		 * A flag, not a credential. ALTER TABLE rather than a rebuild so
		 * an existing user's rows survive -- which is the case the
		 * migration test builds a real v1 file to cover.
		 */
		query.exec(QStringLiteral(
		        "ALTER TABLE profiles ADD COLUMN has_password INTEGER NOT NULL DEFAULT 0"));
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
	        "SELECT id, name, host, user, port, has_password FROM profiles"
	        " ORDER BY name COLLATE NOCASE"))) {
		return result;
	}

	while (query.next()) {
		bssh_profile_t profile;
		profile.id = query.value(0).toInt();
		profile.name = query.value(1).toString();
		profile.host = query.value(2).toString();
		profile.user = query.value(3).toString();
		profile.port = static_cast<quint16>(query.value(4).toUInt());
		profile.has_stored_password = query.value(5).toInt() != 0;
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
		        "INSERT INTO profiles (name, host, user, port, has_password)"
		        " VALUES (?, ?, ?, ?, ?)"));
	} else {
		query.prepare(QStringLiteral(
		        "UPDATE profiles SET name = ?, host = ?, user = ?, port = ?,"
		        " has_password = ? WHERE id = ?"));
	}

	query.addBindValue(profile.name.isEmpty() ? profile.host : profile.name);
	query.addBindValue(profile.host);
	query.addBindValue(profile.user);
	query.addBindValue(profile.port);
	query.addBindValue(profile.has_stored_password ? 1 : 0);
	if (profile.id != 0) query.addBindValue(profile.id);

	if (!query.exec()) {
		m_last_error = query.lastError().text();
		return 0;
	}

	const int id = profile.id == 0 ? query.lastInsertId().toInt() : profile.id;
	emit changed();
	return id;
}

bool bssh_profile_store::save_layout(const layout_state_t &layout) {
	if (!m_open) return false;

	QSqlDatabase database = QSqlDatabase::database(m_connection_name);
	QSqlQuery query(database);

	/* Replaced wholesale: a workspace is one value, not a set of rows to
	 * reconcile. */
	if (!query.exec(QStringLiteral("DELETE FROM layout"))) {
		m_last_error = query.lastError().text();
		return false;
	}

	for (int i = 0; i < layout.profile_ids.size(); ++i) {
		query.prepare(QStringLiteral(
		        "INSERT INTO layout (position, profile_id, is_pane, is_active)"
		        " VALUES (?, ?, ?, ?)"));
		query.addBindValue(i);
		query.addBindValue(layout.profile_ids.at(i));
		query.addBindValue(i < layout.pane_count ? 1 : 0);
		query.addBindValue(i == layout.active_index ? 1 : 0);

		if (!query.exec()) {
			m_last_error = query.lastError().text();
			return false;
		}
	}

	emit changed();
	return true;
}

bssh_profile_store::layout_state_t bssh_profile_store::layout() const {
	layout_state_t state;
	if (!m_open) return state;

	/*
	 * Joined against profiles so an id deleted between runs simply does
	 * not come back -- restoring a tab for a host the user removed would
	 * be the store second-guessing them.
	 */
	QSqlQuery query(QSqlDatabase::database(m_connection_name));
	if (!query.exec(QStringLiteral(
	        "SELECT l.profile_id, l.is_pane, l.is_active FROM layout l"
	        " JOIN profiles p ON p.id = l.profile_id"
	        " ORDER BY l.position"))) {
		return state;
	}

	state.pane_count = 0;
	while (query.next()) {
		state.profile_ids.append(query.value(0).toInt());
		if (query.value(1).toInt() != 0) ++state.pane_count;
		if (query.value(2).toInt() != 0) state.active_index = state.profile_ids.size() - 1;
	}

	if (state.pane_count < 1) state.pane_count = 1;
	return state;
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
