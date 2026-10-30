#include "model/handoff_point.h"

#include "model/handoff_wire.h"
#include "model/session.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QSocketNotifier>
#include <QStandardPaths>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

const char BSSH_HANDOFF_TAKE[] = "take";
const char BSSH_HANDOFF_STATE[] = "state";
const char BSSH_HANDOFF_REFUSED[] = "refused";
const char BSSH_HANDOFF_TAKEN[] = "taken";
const char BSSH_HANDOFF_RELEASED[] = "released";
const char BSSH_HANDOFF_OFFER[] = "offer";
const char BSSH_HANDOFF_LIST[] = "list";
const char BSSH_HANDOFF_IDS[] = "ids";
const char BSSH_HANDOFF_QUIT[] = "quit";
const char BSSH_HANDOFF_HOVER[] = "hover";
const char BSSH_HANDOFF_OK[] = "ok";

bssh_handoff_point *g_instance = nullptr;

/*
 * Drops a session this process imported but may not keep: its transport
 * lets go without a word, so deleting it sends nothing down a connection
 * the giver still owns.
 */
void drop_import(bssh_session *session) {
	if (session->transport()) session->transport()->release_transport();
	delete session;
}

/* A small message: a word and a text. */
QByteArray word(const char *verb, const QString &text = QString()) {
	QByteArray bytes;
	QDataStream out(&bytes, QIODevice::WriteOnly);
	out.setVersion(QDataStream::Qt_6_0);
	out << QByteArray(verb) << text;
	return bytes;
}

bool read_word(const QByteArray &bytes, QByteArray &verb, QString &text) {
	QDataStream in(bytes);
	in.setVersion(QDataStream::Qt_6_0);
	in >> verb >> text;
	return in.status() == QDataStream::Ok;
}

void close_all(const QList<int> &fds) {
	for (int fd : fds) ::close(fd);
}

bool fill_address(sockaddr_un &address, const QString &path, QString *why) {
	const QByteArray encoded = QFile::encodeName(path);
	if (encoded.size() >= int(sizeof(address.sun_path))) {
		if (why) *why = QStringLiteral("the handoff socket's path is too long: %1").arg(path);
		return false;
	}
	std::memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	std::memcpy(address.sun_path, encoded.constData(), size_t(encoded.size()));
	return true;
}

}  // namespace

bssh_handoff_point::bssh_handoff_point(finder_t finder, QObject *parent)
    : QObject(parent), m_finder(std::move(finder)) {
}

bssh_handoff_point *bssh_handoff_point::instance() {
	return g_instance;
}

QString bssh_handoff_point::path_for(qint64 pid) {
	return directory() + QStringLiteral("/handoff-%1.sock").arg(pid);
}

bssh_handoff_point::~bssh_handoff_point() {
	if (g_instance == this) g_instance = nullptr;
	delete m_notifier;
	if (m_listener >= 0) ::close(m_listener);
	/* This process's own socket, named for its pid: nobody else's. */
	if (!m_path.isEmpty()) ::unlink(QFile::encodeName(m_path).constData());
}

QString bssh_handoff_point::directory() {
	QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
	if (base.isEmpty()) base = QDir::tempPath();
	return base + QStringLiteral("/beerssh");
}

bool bssh_handoff_point::listen(QString *why) {
	const QString dir = directory();
	const QByteArray encoded_dir = QFile::encodeName(dir);

	/*
	 * Only this user may enter: 0700, and checked rather than assumed --
	 * a directory somebody else made first, or made wider, is refused.
	 */
	if (::mkdir(encoded_dir.constData(), 0700) != 0 && errno != EEXIST) {
		if (why) *why = QStringLiteral("cannot make %1: %2").arg(dir, QString::fromLocal8Bit(std::strerror(errno)));
		return false;
	}
	struct stat info = {};
	if (::lstat(encoded_dir.constData(), &info) != 0 || !S_ISDIR(info.st_mode) ||
	    info.st_uid != ::getuid() || (info.st_mode & 0077) != 0) {
		if (why) *why = QStringLiteral("%1 is not a directory only this user can enter").arg(dir);
		return false;
	}

	sweep_stale();

	m_path = path_for(QCoreApplication::applicationPid());
	sockaddr_un address;
	if (!fill_address(address, m_path, why)) {
		m_path.clear();
		return false;
	}
	/* A socket left by a process that had this pid before and died. */
	::unlink(address.sun_path);

	m_listener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (m_listener < 0 ||
	    ::bind(m_listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0 ||
	    ::listen(m_listener, 4) != 0) {
		if (why) *why = QStringLiteral("cannot listen at %1: %2")
		                        .arg(m_path, QString::fromLocal8Bit(std::strerror(errno)));
		if (m_listener >= 0) ::close(m_listener);
		m_listener = -1;
		m_path.clear();
		return false;
	}
	m_notifier = new QSocketNotifier(m_listener, QSocketNotifier::Read, this);
	connect(m_notifier, &QSocketNotifier::activated, this, [this] { on_connection(); });
	if (!g_instance) g_instance = this;
	return true;
}

void bssh_handoff_point::on_connection() {
	const int socket = ::accept4(m_listener, nullptr, nullptr, SOCK_CLOEXEC);
	if (socket < 0) return;
	if (bssh_handoff_wire::peer_is_same_user(socket)) serve(socket);
	::close(socket);
}

/*
 * One exchange, on this thread, bounded: the session is stopped for its
 * length, and the taker is another process of the same user that answers
 * within a step or is given up on.
 */
void bssh_handoff_point::serve(int socket) {
	bssh_handoff_message_t request;
	if (!bssh_handoff_wire::receive(socket, request, BSSH_HANDOFF_STEP_MS)) return;
	close_all(request.fds);
	QByteArray verb;
	QString id;
	if (!read_word(request.bytes, verb, id)) return;

	/* The short exchanges first: each is answered and done. */
	if (verb == BSSH_HANDOFF_OFFER) {
		/* "id\nsource\nx,y": taken after this returns, from the event
		 * loop, so the source is not kept waiting on its own answer. */
		const QStringList parts = id.split(QLatin1Char('\n'));
		const QStringList at = parts.value(2).split(QLatin1Char(','));
		bssh_handoff_wire::send(socket, word(BSSH_HANDOFF_OK), {}, BSSH_HANDOFF_STEP_MS);
		if (parts.size() == 3 && at.size() == 2) {
			const QString offered_id = parts.at(0), source = parts.at(1);
			const QPoint global(at.at(0).toInt(), at.at(1).toInt());
			QMetaObject::invokeMethod(this, [this, source, offered_id, global] {
				emit offered(source, offered_id, global);
			}, Qt::QueuedConnection);
		}
		return;
	}
	if (verb == BSSH_HANDOFF_HOVER) {
		/* "x,y" while over this process; empty when the drag left it. */
		bssh_handoff_wire::send(socket, word(BSSH_HANDOFF_OK), {}, BSSH_HANDOFF_STEP_MS);
		const QStringList at = id.split(QLatin1Char(','));
		const bool active = at.size() == 2;
		const QPoint global = active ? QPoint(at.at(0).toInt(), at.at(1).toInt()) : QPoint();
		QMetaObject::invokeMethod(this, [this, global, active] {
			emit hovered(global, active);
		}, Qt::QueuedConnection);
		return;
	}
	if (verb == BSSH_HANDOFF_LIST) {
		const QStringList ids = m_lister ? m_lister() : QStringList();
		bssh_handoff_wire::send(socket, word(BSSH_HANDOFF_IDS, ids.join(QLatin1Char('\n'))), {},
		                        BSSH_HANDOFF_STEP_MS);
		return;
	}
	if (verb == BSSH_HANDOFF_QUIT) {
		bssh_handoff_wire::send(socket, word(BSSH_HANDOFF_OK), {}, BSSH_HANDOFF_STEP_MS);
		QMetaObject::invokeMethod(this, [this] { emit quit_requested(); }, Qt::QueuedConnection);
		return;
	}
	if (verb != BSSH_HANDOFF_TAKE) return;

	bssh_session *session = m_finder ? m_finder(id) : nullptr;
	if (!session) {
		bssh_handoff_wire::send(socket, word(BSSH_HANDOFF_REFUSED, QStringLiteral("no such session here")),
		                        {}, BSSH_HANDOFF_STEP_MS);
		return;
	}

	QByteArray state;
	QList<int> fds;
	QString why;
	if (!session->export_handoff(state, fds, &why)) {
		bssh_handoff_wire::send(socket, word(BSSH_HANDOFF_REFUSED, why), {}, BSSH_HANDOFF_STEP_MS);
		return;
	}

	QByteArray message = word(BSSH_HANDOFF_STATE);
	message.append(state);
	const bool sent = bssh_handoff_wire::send(socket, message, fds, BSSH_HANDOFF_STEP_MS);

	/* The state carried the keys: not left lying in this heap. */
	message.fill('\0');
	state.fill('\0');

	bssh_handoff_message_t answer;
	QByteArray reply;
	QString reason;
	const bool taken = sent &&
	        bssh_handoff_wire::receive(socket, answer, BSSH_HANDOFF_STEP_MS) &&
	        read_word(answer.bytes, reply, reason) && reply == BSSH_HANDOFF_TAKEN;
	close_all(answer.fds);

	if (!taken) {
		/*
		 * Shut first, resume second: a "taken" the taker sends after this
		 * fails on its side rather than landing unread, so the taker
		 * cannot go on to use a connection this side is resuming.
		 */
		::shutdown(socket, SHUT_RDWR);
		session->resume_handoff();
		return;
	}
	session->release_handoff();
	/*
	 * And say so: the taker uses the session only on hearing this, which
	 * closes the window where a "taken" sent at this side's deadline was
	 * already in flight. If it does not get through, the session is lost
	 * to both rather than written by both -- the safe side of that edge.
	 */
	bssh_handoff_wire::send(socket, word(BSSH_HANDOFF_RELEASED), {}, BSSH_HANDOFF_STEP_MS);
	emit session_given(session);
}

bssh_session *bssh_handoff_point::take(const QString &path, const QString &handoff_id,
                                       QObject *parent, QString *why, int timeout_ms) {
	sockaddr_un address;
	if (!fill_address(address, path, why)) return nullptr;

	const int socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (socket < 0 || ::connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
		if (why) *why = QStringLiteral("the other BeerSSH is not listening");
		if (socket >= 0) ::close(socket);
		return nullptr;
	}
	if (!bssh_handoff_wire::peer_is_same_user(socket)) {
		if (why) *why = QStringLiteral("the process listening there is not this user's");
		::close(socket);
		return nullptr;
	}

	bssh_session *session = nullptr;
	bssh_handoff_message_t answer;
	if (!bssh_handoff_wire::send(socket, word(BSSH_HANDOFF_TAKE, handoff_id), {}, timeout_ms, why) ||
	    !bssh_handoff_wire::receive(socket, answer, timeout_ms, why)) {
		::close(socket);
		return nullptr;
	}

	/* "state" and the session after it, or a refusal and its reason. */
	QDataStream in(answer.bytes);
	in.setVersion(QDataStream::Qt_6_0);
	QByteArray verb;
	QString text;
	in >> verb >> text;
	if (in.status() != QDataStream::Ok || verb != BSSH_HANDOFF_STATE) {
		if (why) *why = verb == BSSH_HANDOFF_REFUSED ? text : QStringLiteral("the other BeerSSH answered nonsense");
		close_all(answer.fds);
		::close(socket);
		return nullptr;
	}
	const QByteArray state = answer.bytes.mid(int(in.device()->pos()));
	answer.bytes.fill('\0');

	QString reason;
	session = bssh_session::import_handoff(state, answer.fds, parent, &reason);
	if (!session) {
		close_all(answer.fds);
		bssh_handoff_wire::send(socket, word(BSSH_HANDOFF_REFUSED, reason), {}, timeout_ms);
		if (why) *why = reason;
		::close(socket);
		return nullptr;
	}

	/*
	 * The commit: "taken", then wait for "released" before using the
	 * session. Without the second, a "taken" landing at the giver's
	 * deadline would leave both sides holding the connection; without it
	 * arriving, this side drops its import, and the giver either resumed
	 * or -- at that one edge -- lost the session. Never two writers.
	 */
	bssh_handoff_message_t released;
	QByteArray last;
	QString ignored;
	const bool ours =
	        bssh_handoff_wire::send(socket, word(BSSH_HANDOFF_TAKEN), {}, timeout_ms, why) &&
	        bssh_handoff_wire::receive(socket, released, timeout_ms, why) &&
	        read_word(released.bytes, last, ignored) && last == BSSH_HANDOFF_RELEASED;
	close_all(released.fds);
	::close(socket);
	if (!ours) {
		drop_import(session);
		if (why && why->isEmpty()) *why = QStringLiteral("the other BeerSSH did not let the session go");
		return nullptr;
	}
	return session;
}

namespace {

/* One short request and its answer, for offer(), list() and request_quit(). */
bool ask(const QString &path, const QByteArray &request, QByteArray &verb, QString &text,
         QString *why) {
	sockaddr_un address;
	if (!fill_address(address, path, why)) return false;
	const int socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (socket < 0 || ::connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
		if (why) *why = QStringLiteral("the other BeerSSH is not listening");
		if (socket >= 0) ::close(socket);
		return false;
	}
	bssh_handoff_message_t answer;
	const bool ok = bssh_handoff_wire::peer_is_same_user(socket) &&
	        bssh_handoff_wire::send(socket, request, {}, BSSH_HANDOFF_STEP_MS, why) &&
	        bssh_handoff_wire::receive(socket, answer, BSSH_HANDOFF_STEP_MS, why) &&
	        read_word(answer.bytes, verb, text);
	close_all(answer.fds);
	::close(socket);
	return ok;
}

}  // namespace

bool bssh_handoff_point::offer(const QString &target, const QString &source,
                               const QString &handoff_id, const QPoint &global, QString *why) {
	QByteArray verb;
	QString text;
	const QString what = handoff_id + QLatin1Char('\n') + source + QLatin1Char('\n') +
	                     QString::number(global.x()) + QLatin1Char(',') + QString::number(global.y());
	return ask(target, word(BSSH_HANDOFF_OFFER, what), verb, text, why) && verb == BSSH_HANDOFF_OK;
}

QStringList bssh_handoff_point::list(const QString &path, QString *why) {
	QByteArray verb;
	QString text;
	if (!ask(path, word(BSSH_HANDOFF_LIST), verb, text, why) || verb != BSSH_HANDOFF_IDS)
		return QStringList();
	return text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

int bssh_handoff_point::sweep_stale() {
	int removed = 0;
	const QDir dir(directory());
	const QStringList names = dir.entryList({ QStringLiteral("handoff-*.sock") }, QDir::System);
	for (const QString &name : names) {
		const QString path = dir.filePath(name);
		sockaddr_un address;
		if (!fill_address(address, path, nullptr)) continue;
		const int probe = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (probe < 0) continue;
		const bool refused = ::connect(probe, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0
		                     && errno == ECONNREFUSED;
		::close(probe);
		if (refused && ::unlink(address.sun_path) == 0) ++removed;
	}
	return removed;
}

bool bssh_handoff_point::hover(const QString &path, const QPoint &global, bool active,
                               QString *why) {
	QByteArray verb;
	QString text;
	const QString what = active ? QString::number(global.x()) + QLatin1Char(',') + QString::number(global.y())
	                            : QString();
	return ask(path, word(BSSH_HANDOFF_HOVER, what), verb, text, why) && verb == BSSH_HANDOFF_OK;
}

bool bssh_handoff_point::request_quit(const QString &path, QString *why) {
	QByteArray verb;
	QString text;
	return ask(path, word(BSSH_HANDOFF_QUIT), verb, text, why) && verb == BSSH_HANDOFF_OK;
}
