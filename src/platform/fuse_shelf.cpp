#include "platform/fuse_shelf.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QMutex>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
#include <linux/fuse.h>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace {

/*
 * What the mount shows. The root, one directory per file added -- so two
 * drags of files of one name never collide -- and the file inside it.
 */
struct node {
	quint64 parent = 0;
	QString name;
	bool directory = false;
	QString staging;        /* a file: what the copy is writing */
	qint64 size = 0;        /* what it will reach */
	bool finished = false;
	bool whole = false;
	QList<quint64> children;
};

struct waiting_read {
	quint64 unique = 0;
	quint64 node = 0;
	quint64 offset = 0;
	quint32 size = 0;
};

QMutex lock;                       /* guards everything below */
QHash<quint64, node> nodes;
quint64 next_node = FUSE_ROOT_ID + 1;
QString mounted_at;
int fuse_fd = -1;
int commfd = -1;                   /* kept open: fusermount3 unmounts when it closes */
pid_t watcher = 0;                 /* that fusermount3 */
QThread *serving = nullptr;
bool tried = false;

/* ---- the mount ---------------------------------------------------------- */

/*
 * Mounts as libfuse does, without it: fusermount3 is setuid, mounts
 * /dev/fuse on `where` and hands the descriptor back over a socket named
 * in _FUSE_COMMFD. auto_unmount keeps a fusermount3 watching that socket,
 * so a process that dies -- crash or kill -- leaves no dead mount behind.
 */
int mount_with_fusermount(const QString &where) {
	int pair[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) return -1;
	fcntl(pair[0], F_SETFD, FD_CLOEXEC);

	const QByteArray options = "ro,nosuid,nodev,noexec,fsname=beerssh,subtype=beerssh,auto_unmount";
	const QByteArray target = QFile::encodeName(where);
	const QByteArray comm = "_FUSE_COMMFD=" + QByteArray::number(pair[1]);
	char *argv[] = { const_cast<char *>("fusermount3"), const_cast<char *>("-o"), const_cast<char *>(options.constData()),
	                 const_cast<char *>("--"), const_cast<char *>(target.constData()), nullptr };
	QList<QByteArray> env_store;
	QList<char *> env;
	for (char **e = environ; e && *e; ++e)
		if (qstrncmp(*e, "_FUSE_COMMFD=", 13) != 0) env << *e;
	env_store << comm;
	env << env_store.last().data() << nullptr;

	pid_t pid = 0;
	const int spawned = posix_spawnp(&pid, "fusermount3", nullptr, nullptr, argv, env.data());
	close(pair[1]);
	if (spawned != 0) {
		close(pair[0]);
		return -1;
	}

	/* The descriptor, passed as a control message. */
	char data = 0;
	iovec io{ &data, 1 };
	char control[CMSG_SPACE(sizeof(int))] = {};
	msghdr message{};
	message.msg_iov = &io;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	ssize_t got;
	do {
		got = recvmsg(pair[0], &message, 0);
	} while (got < 0 && errno == EINTR);
	int fd = -1;
	if (got > 0) {
		for (cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
			if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) std::memcpy(&fd, CMSG_DATA(c), sizeof(int));
		}
	}
	/*
	 * Not waited for: with auto_unmount fusermount3 stays, watching the
	 * socket to unmount when this process goes -- libfuse waits only
	 * without it, and waiting here hung the first mount for ever. Reaped
	 * at shut_down(), once closing the socket has let it finish.
	 */
	watcher = pid;
	if (fd < 0) {
		close(pair[0]);
		int status = 0;
		waitpid(pid, &status, 0);
		watcher = 0;
		return -1;
	}
	commfd = pair[0];
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}

/* ---- answering ------------------------------------------------------------ */

void reply(quint64 unique, int error, const void *payload = nullptr, size_t size = 0) {
	fuse_out_header header{};
	header.len = uint32_t(sizeof(header) + (error ? 0 : size));
	header.error = -error;
	header.unique = unique;
	iovec parts[2] = { { &header, sizeof(header) }, { const_cast<void *>(payload), error ? 0 : size } };
	const ssize_t written = writev(fuse_fd, parts, payload && !error ? 2 : 1);
	Q_UNUSED(written);   /* ENOENT: the request was interrupted; nothing to do */
}

void fill_attr(quint64 id, const node &n, fuse_attr &attr) {
	std::memset(&attr, 0, sizeof(attr));
	attr.ino = id;
	/*
	 * The modes a downloaded file gets, not read-only ones: the mount is
	 * ro at the kernel, which refuses every write whatever these say, and
	 * a file manager copies the mode -- pcmanfm-qt's copies of 0444 files
	 * arrived read-only in the user's folder (2026-11-12).
	 */
	attr.mode = n.directory ? (S_IFDIR | 0755) : (S_IFREG | 0644);
	attr.nlink = n.directory ? 2 : 1;
	attr.size = n.directory ? 0 : uint64_t(n.size);
	attr.blocks = (attr.size + 511) / 512;
	attr.blksize = 4096;
	attr.uid = getuid();
	attr.gid = getgid();
	const uint64_t now = uint64_t(time(nullptr));
	attr.atime = attr.mtime = attr.ctime = now;
}

/*
 * A read answered now if what it asks for has arrived -- or the copy is
 * over -- and false if it must wait. Called with the lock held.
 */
bool try_read(const waiting_read &read) {
	const auto it = nodes.constFind(read.node);
	if (it == nodes.constEnd() || it->directory) {
		reply(read.unique, EIO);
		return true;
	}
	const node &n = *it;
	if (read.offset >= quint64(n.size)) {
		reply(read.unique, 0, "", 0);
		return true;
	}
	struct stat st{};
	const qint64 have = ::stat(QFile::encodeName(n.staging).constData(), &st) == 0 ? qint64(st.st_size) : 0;
	const qint64 want = qMin<qint64>(qint64(read.offset) + read.size, n.size);
	if (have < want && !(n.finished && n.whole)) {
		if (n.finished) {
			reply(read.unique, EIO);   /* stopped or failed: never to arrive */
			return true;
		}
		return false;
	}
	const int file = ::open(QFile::encodeName(n.staging).constData(), O_RDONLY | O_CLOEXEC);
	if (file < 0) {
		reply(read.unique, EIO);
		return true;
	}
	QByteArray buffer(int(want - qint64(read.offset)), Qt::Uninitialized);
	const ssize_t got = pread(file, buffer.data(), size_t(buffer.size()), off_t(read.offset));
	::close(file);
	if (got < 0) reply(read.unique, EIO);
	else reply(read.unique, 0, buffer.constData(), size_t(got));
	return true;
}

/* The thread that serves the mount. */
class server : public QObject {
public:
	void start() {
		m_notifier = new QSocketNotifier(fuse_fd, QSocketNotifier::Read, this);
		connect(m_notifier, &QSocketNotifier::activated, this, [this] { serve_one(); });
		/* Waiting reads looked at again as the copies grow. */
		m_retry = new QTimer(this);
		m_retry->setInterval(40);
		connect(m_retry, &QTimer::timeout, this, [this] { retry(); });
		m_retry->start();
	}

	void retry() {
		QMutexLocker held(&lock);
		for (int i = int(m_waiting.size()) - 1; i >= 0; --i)
			if (try_read(m_waiting.at(i))) m_waiting.removeAt(i);
	}

private:
	void serve_one() {
		QByteArray buffer(FUSE_MIN_READ_BUFFER + 64 * 1024, Qt::Uninitialized);
		const ssize_t got = ::read(fuse_fd, buffer.data(), size_t(buffer.size()));
		if (got < 0) {
			if (errno == ENODEV) m_notifier->setEnabled(false);   /* unmounted */
			return;
		}
		if (size_t(got) < sizeof(fuse_in_header)) return;
		const auto *in = reinterpret_cast<const fuse_in_header *>(buffer.constData());
		const char *body = buffer.constData() + sizeof(fuse_in_header);
		QMutexLocker held(&lock);
		switch (in->opcode) {
		case FUSE_INIT: {
			const auto *init = reinterpret_cast<const fuse_init_in *>(body);
			fuse_init_out out{};
			out.major = FUSE_KERNEL_VERSION;
			out.minor = qMin<uint32_t>(init->minor, FUSE_KERNEL_MINOR_VERSION);
			out.max_readahead = init->max_readahead;
			out.max_write = 4096;
			out.max_background = 16;
			out.congestion_threshold = 12;
			out.time_gran = 1;
			reply(in->unique, 0, &out, init->minor < 23 ? FUSE_COMPAT_22_INIT_OUT_SIZE : sizeof(out));
			break;
		}
		case FUSE_LOOKUP: {
			const QString name = QString::fromUtf8(body);
			const auto parent = nodes.constFind(in->nodeid);
			quint64 found = 0;
			if (parent != nodes.constEnd())
				for (quint64 child : parent->children)
					if (nodes.value(child).name == name) found = child;
			if (!found) {
				reply(in->unique, ENOENT);
				break;
			}
			fuse_entry_out out{};
			out.nodeid = found;
			out.entry_valid = 1;
			out.attr_valid = 1;
			fill_attr(found, nodes.value(found), out.attr);
			reply(in->unique, 0, &out, sizeof(out));
			break;
		}
		case FUSE_GETATTR: {
			const auto it = nodes.constFind(in->nodeid);
			if (it == nodes.constEnd()) {
				reply(in->unique, ENOENT);
				break;
			}
			fuse_attr_out out{};
			out.attr_valid = 1;
			fill_attr(in->nodeid, *it, out.attr);
			reply(in->unique, 0, &out, sizeof(out));
			break;
		}
		case FUSE_OPEN:
		case FUSE_OPENDIR: {
			const auto *open = reinterpret_cast<const fuse_open_in *>(body);
			if ((open->flags & O_ACCMODE) != O_RDONLY) {
				reply(in->unique, EROFS);
				break;
			}
			if (!nodes.contains(in->nodeid)) {
				reply(in->unique, ENOENT);
				break;
			}
			fuse_open_out out{};
			out.fh = in->nodeid;
			reply(in->unique, 0, &out, sizeof(out));
			break;
		}
		case FUSE_READ: {
			const auto *read = reinterpret_cast<const fuse_read_in *>(body);
			const waiting_read wanted{ in->unique, in->nodeid, read->offset, read->size };
			if (!try_read(wanted)) m_waiting << wanted;
			break;
		}
		case FUSE_READDIR: {
			const auto *read = reinterpret_cast<const fuse_read_in *>(body);
			const auto it = nodes.constFind(in->nodeid);
			if (it == nodes.constEnd() || !it->directory) {
				reply(in->unique, ENOTDIR);
				break;
			}
			QList<QPair<quint64, QString>> entries{ { in->nodeid, QStringLiteral(".") },
			                                        { it->parent ? it->parent : in->nodeid, QStringLiteral("..") } };
			for (quint64 child : it->children) entries << qMakePair(child, nodes.value(child).name);
			QByteArray out;
			for (quint64 i = read->offset; i < quint64(entries.size()); ++i) {
				const QByteArray name = entries.at(int(i)).second.toUtf8();
				const size_t record = FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + size_t(name.size()));
				if (size_t(out.size()) + record > read->size) break;
				QByteArray entry(int(record), '\0');
				auto *dirent = reinterpret_cast<fuse_dirent *>(entry.data());
				dirent->ino = entries.at(int(i)).first;
				dirent->off = i + 1;
				dirent->namelen = uint32_t(name.size());
				dirent->type = nodes.value(entries.at(int(i)).first).directory || i < 2 ? DT_DIR : DT_REG;
				std::memcpy(entry.data() + FUSE_NAME_OFFSET, name.constData(), size_t(name.size()));
				out += entry;
			}
			reply(in->unique, 0, out.constData(), size_t(out.size()));
			break;
		}
		case FUSE_STATFS: {
			fuse_statfs_out out{};
			out.st.bsize = 4096;
			out.st.frsize = 4096;
			out.st.namelen = 255;
			reply(in->unique, 0, &out, sizeof(out));
			break;
		}
		case FUSE_INTERRUPT: {
			const auto *interrupt = reinterpret_cast<const fuse_interrupt_in *>(body);
			for (int i = 0; i < m_waiting.size(); ++i) {
				if (m_waiting.at(i).unique != interrupt->unique) continue;
				reply(interrupt->unique, EINTR);
				m_waiting.removeAt(i);
				break;
			}
			break;   /* an interrupt is not answered itself */
		}
		case FUSE_RELEASE:
		case FUSE_RELEASEDIR:
		case FUSE_FLUSH:
			reply(in->unique, 0);
			break;
		case FUSE_FORGET:
		case FUSE_BATCH_FORGET:
			break;   /* answered by nothing, by the protocol */
		case FUSE_DESTROY:
			reply(in->unique, 0);
			m_notifier->setEnabled(false);
			break;
		default:
			reply(in->unique, ENOSYS);
			break;
		}
	}

	QSocketNotifier *m_notifier = nullptr;
	QTimer *m_retry = nullptr;
	QList<waiting_read> m_waiting;
};

server *the_server = nullptr;

quint64 node_for(const QString &path) {
	if (mounted_at.isEmpty() || !path.startsWith(mounted_at + QLatin1Char('/'))) return 0;
	const QStringList parts = path.mid(mounted_at.size() + 1).split(QLatin1Char('/'));
	quint64 at = FUSE_ROOT_ID;
	for (const QString &part : parts) {
		quint64 found = 0;
		for (quint64 child : nodes.value(at).children)
			if (nodes.value(child).name == part) found = child;
		if (!found) return 0;
		at = found;
	}
	return at;
}

bool ensure_mounted() {
	if (fuse_fd >= 0) return true;
	if (tried) return false;
	tried = true;
	const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
	if (runtime.isEmpty()) return false;
	const QString where = runtime + QStringLiteral("/beerssh-drop-%1").arg(QCoreApplication::applicationPid());
	QDir().mkpath(where);
	const int fd = mount_with_fusermount(where);
	if (fd < 0) {
		QDir().rmdir(where);
		return false;
	}
	fuse_fd = fd;
	mounted_at = where;
	node root;
	root.directory = true;
	nodes.insert(FUSE_ROOT_ID, root);

	serving = new QThread;
	serving->setObjectName(QStringLiteral("bssh-fuse"));
	the_server = new server;
	the_server->moveToThread(serving);
	QObject::connect(serving, &QThread::started, the_server, [] { the_server->start(); });
	QObject::connect(serving, &QThread::finished, the_server, &QObject::deleteLater);
	serving->start();
	qAddPostRoutine(bssh_fuse_shelf::shut_down);
	return true;
}

}

namespace bssh_fuse_shelf {

bool available() {
	if (fuse_fd >= 0) return true;
	return ::access("/dev/fuse", R_OK | W_OK) == 0
	       && !QStandardPaths::findExecutable(QStringLiteral("fusermount3")).isEmpty()
	       && !QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation).isEmpty();
}

QString add(const QString &name, const QString &staging, qint64 size) {
	if (name.isEmpty() || name.contains(QLatin1Char('/'))) return QString();
	QMutexLocker held(&lock);
	if (!ensure_mounted()) return QString();
	const quint64 folder = next_node++;
	node dir;
	dir.parent = FUSE_ROOT_ID;
	dir.name = QString::number(folder);
	dir.directory = true;
	const quint64 file = next_node++;
	node entry;
	entry.parent = folder;
	entry.name = name;
	entry.staging = staging;
	entry.size = size;
	dir.children << file;
	nodes.insert(folder, dir);
	nodes.insert(file, entry);
	nodes[FUSE_ROOT_ID].children << folder;
	return mounted_at + QLatin1Char('/') + dir.name + QLatin1Char('/') + name;
}

void finish(const QString &path, bool whole) {
	QMutexLocker held(&lock);
	const quint64 id = node_for(path);
	if (!id) return;
	nodes[id].finished = true;
	nodes[id].whole = whole;
}

void remove(const QString &path) {
	QMutexLocker held(&lock);
	const quint64 id = node_for(path);
	if (!id) return;
	const quint64 folder = nodes.value(id).parent;
	/* A read still waiting for it fails on the next look, finding nothing. */
	nodes.remove(id);
	nodes.remove(folder);
	nodes[FUSE_ROOT_ID].children.removeAll(folder);
}

QString mount_point() {
	QMutexLocker held(&lock);
	return mounted_at;
}

void shut_down() {
	QString where;
	{
		QMutexLocker held(&lock);
		where = mounted_at;
		mounted_at.clear();
	}
	if (where.isEmpty()) return;
	/* Lazily, so a file manager still holding a file does not keep it. */
	const QByteArray target = QFile::encodeName(where);
	char *argv[] = { const_cast<char *>("fusermount3"), const_cast<char *>("-u"), const_cast<char *>("-z"),
	                 const_cast<char *>("--"), const_cast<char *>(target.constData()), nullptr };
	pid_t pid = 0;
	if (posix_spawnp(&pid, "fusermount3", nullptr, nullptr, argv, environ) == 0) {
		int status = 0;
		waitpid(pid, &status, 0);
	}
	if (serving) {
		serving->quit();
		serving->wait(2000);
	}
	if (fuse_fd >= 0) ::close(fuse_fd);
	if (commfd >= 0) ::close(commfd);
	fuse_fd = commfd = -1;
	if (watcher > 0) {
		int status = 0;
		waitpid(watcher, &status, 0);
		watcher = 0;
	}
	QDir().rmdir(where);
}

}

#else

namespace bssh_fuse_shelf {
bool available() { return false; }
QString add(const QString &, const QString &, qint64) { return QString(); }
void finish(const QString &, bool) {}
void remove(const QString &) {}
QString mount_point() { return QString(); }
void shut_down() {}
}

#endif
