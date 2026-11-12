#include "ssh/ssh_session.h"

#include "model/piece_map.h"

bssh_ssh_session::bssh_ssh_session(QObject *parent)
    : QObject(parent),
      m_state(DISCONNECTED) {
}

bssh_ssh_session::~bssh_ssh_session() {
}

void bssh_ssh_session::set_state(state_t state) {
	/*
	 * Silent on a no-op transition. Every implementation calls this from
	 * its own event handling, where the same state is easily reached
	 * twice, and a status UI that flickers "reconnecting" at a session
	 * that never left CONNECTED is worse than no status at all.
	 */
	if (m_state == state) return;

	m_state = state;
	emit state_changed(m_state);
}

/*
 * Empty except in tests. A function pointer rather than a compile-time
 * switch so the production build still links exactly one backend.
 */
static std::function<bssh_ssh_session *(QObject *)> g_factory;

void bssh_ssh_session::set_factory(std::function<bssh_ssh_session *(QObject *)> factory) {
	g_factory = std::move(factory);
}

bssh_ssh_session *bssh_ssh_session::create_from_factory(QObject *parent) {
	return g_factory ? g_factory(parent) : nullptr;
}

bool bssh_ssh_session::export_transport(QByteArray &state, QList<int> &fds, QString *why) {
	state.clear();
	fds.clear();
	if (why) *why = QStringLiteral("this connection cannot move to another process");
	return false;
}

bool bssh_ssh_session::import_transport(const QByteArray &state, const QList<int> &fds,
                                        QString *why) {
	Q_UNUSED(state);
	Q_UNUSED(fds);
	if (why) *why = QStringLiteral("this connection cannot move to another process");
	return false;
}

void bssh_ssh_session::find_working_directory() {
	QMetaObject::invokeMethod(this, [this] { emit working_directory_found(bssh_working_directory_t()); },
	                          Qt::QueuedConnection);
}

void bssh_ssh_session::upload(const QStringList &paths, const QString &directory) {
	Q_UNUSED(paths);
	QMetaObject::invokeMethod(this, [this, directory] {
		emit upload_finished(false, directory, QStringList(),
		                     QStringLiteral("this connection cannot copy files"));
	}, Qt::QueuedConnection);
}

void bssh_ssh_session::list_directory(const QString &path) {
	QMetaObject::invokeMethod(this, [this, path] {
		emit directory_listed(path, QList<bssh_remote_entry_t>(),
		                      QStringLiteral("this connection has no files to list"));
	}, Qt::QueuedConnection);
}

void bssh_ssh_session::download_pieces(const QString &path, const QString &directory,
                                       std::shared_ptr<bssh_piece_map> pieces) {
	if (pieces) pieces->set_in_order();
	download({ path }, directory);
}

void bssh_ssh_session::download(const QStringList &paths, const QString &directory) {
	Q_UNUSED(paths);
	QMetaObject::invokeMethod(this, [this, directory] {
		emit download_finished(false, directory, QStringList(),
		                       QStringLiteral("this connection cannot copy files"));
	}, Qt::QueuedConnection);
}

void bssh_ssh_session::change_remote(change_t change, const QString &path, const QString &to) {
	Q_UNUSED(change);
	Q_UNUSED(to);
	QMetaObject::invokeMethod(this, [this, path] {
		emit remote_changed(false, path, QStringLiteral("this connection has no files to change"));
	}, Qt::QueuedConnection);
}

void bssh_ssh_session::run_on_the_side(const QString &command) {
	Q_UNUSED(command);
	QMetaObject::invokeMethod(this, [this] { emit side_output_ready(QByteArray()); },
	                          Qt::QueuedConnection);
}
