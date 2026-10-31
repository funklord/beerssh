#include "ssh/pty_session.h"

#include "platform/platform.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QDataStream>
#include <QPointer>
#include <QThread>
#include <QIODevice>
#include <QList>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace {

/*
 * How much is taken from the pty per readable notification.
 *
 * The same size the SSH transport reads in, and for the same reason: the
 * emulator holds back an incomplete UTF-8 tail between calls (sec 7), so a
 * boundary landing mid-character is already handled and the number only
 * decides how often the loop runs.
 */
const int BSSH_PTY_READ_SIZE = 8192;

/*
 * How long a shell is given to act on SIGHUP before SIGKILL, and how often
 * it is checked meanwhile. Short: nothing useful happens in the window,
 * and it is paid twice on every session that closes.
 */
const int BSSH_PTY_HANGUP_MS = 200;
const int BSSH_PTY_POLL_MS = 10;

/*
 * Waits briefly for a child, then insists.
 *
 * Never an unbounded waitpid. This runs on the UI thread, and a child that
 * ignores SIGHUP -- which any process may do, and nohup exists to arrange
 * -- would otherwise hang the window, or the entire test suite, on one
 * process declining to exit.
 *
 * So: poll for the length of BSSH_PTY_HANGUP_MS, then SIGKILL, which
 * cannot be caught or ignored, then poll again for the same short while.
 * If even that does not collect it the child is left to init rather than
 * waited on -- an orphan is reaped by pid 1, while a blocked caller is
 * not recovered by anything.
 */
int collect(pid_t child) {
	int status = 0;

	for (int stage = 0; stage < 2; ++stage) {
		for (int waited = 0; waited < BSSH_PTY_HANGUP_MS; waited += BSSH_PTY_POLL_MS) {
			const pid_t done = ::waitpid(child, &status, WNOHANG);
			if (done == child) return WIFEXITED(status) ? WEXITSTATUS(status) : 0;
			if (done < 0) return 0; /* already reaped, or never ours */

			::usleep(BSSH_PTY_POLL_MS * 1000);
		}

		if (stage == 0) ::kill(child, SIGKILL);
	}

	return 0;
}

/*
 * Complains from the child, between fork and exec, and does not return.
 *
 * write(2) and _exit(2) only, because this process has more than one
 * thread -- the SSH transport runs its own (sec 4) -- and after a fork the
 * child holds whatever locks the other threads happened to be inside.
 * dprintf and strerror can both take the allocator's, so a message about
 * a command that would not run could hang the child forever instead of
 * printing anything. strerror_r writes into the caller's buffer.
 *
 * The pty is this child's stderr, so what it writes lands in the terminal
 * that asked for it -- with a CR, since nothing on the far side of a pty
 * translates newlines for us.
 */
void child_fail(const char *what, const char *name, int code) {
	char buffer[128];
	const char *why = strerror_r(errno, buffer, sizeof(buffer));

	const struct iovec parts[] = {
		{ const_cast<char *>("beerssh: "), 9 },
		{ const_cast<char *>(what), std::strlen(what) },
		{ const_cast<char *>(name), std::strlen(name) },
		{ const_cast<char *>(": "), 2 },
		{ const_cast<char *>(why), std::strlen(why) },
		{ const_cast<char *>("\r\n"), 2 },
	};

	/* Nothing to be done if it fails; the exit status still carries it. */
	ssize_t ignored = ::writev(STDERR_FILENO, parts, 6);
	(void)ignored;

	_exit(code);
}

} /* namespace */

bssh_pty_session::bssh_pty_session(QObject *parent)
    : bssh_ssh_session(parent),
      m_master(-1),
      m_child(-1),
      m_notifier(nullptr),
      m_write_notifier(nullptr) {
}

bssh_pty_session::~bssh_pty_session() {
	disconnect_from_host();
}

void bssh_pty_session::connect_to_host(const QString &host, quint16 port, const QString &user) {
	Q_UNUSED(host);
	Q_UNUSED(port);
	Q_UNUSED(user);

	/*
	 * There is nothing to resolve, nothing to authenticate and nobody to
	 * ask. The state still moves through CONNECTED because everything
	 * downstream -- the status line, the tab dot, the reconnection
	 * machine -- reads these states and would otherwise show a local
	 * shell as permanently disconnected.
	 */
	set_state(CONNECTED);
}

void bssh_pty_session::open_shell(int cols, int rows) {
	if (m_master >= 0) return;

	struct winsize size;
	std::memset(&size, 0, sizeof(size));
	size.ws_col = ushort(cols > 0 ? cols : 80);
	size.ws_row = ushort(rows > 0 ? rows : 24);

	/*
	 * Everything the child needs, converted BEFORE the fork.
	 *
	 * Between fork and exec only async-signal-safe calls are sound, and
	 * QString::toLocal8Bit allocates. The child below therefore does
	 * nothing but arrange descriptors, chdir and exec -- the strings it
	 * hands to them were built here, in the parent, while there was still
	 * one thread and one heap.
	 */
	QList<QByteArray> words;
	if (m_command.isEmpty()) {
		QByteArray shell = m_shell.toLocal8Bit();
		if (shell.isEmpty()) shell = qgetenv("SHELL");
		if (shell.isEmpty()) shell = QByteArrayLiteral("/bin/sh");
		words.append(shell);
	} else {
		for (const QString &word : m_command) words.append(word.toLocal8Bit());
	}

	/*
	 * A LOGIN shell -- argv[0] with a leading '-', which is what getty
	 * and ssh both do and what makes a shell read its profile. A terminal
	 * that opens without the user's own environment is one they have to
	 * fix by hand every time.
	 *
	 * A COMMAND is not given the treatment: `-e ls` runs ls with argv[0]
	 * of "ls", because a program reads its own argv[0] and "-ls" is a
	 * different thing to several of them.
	 */
	const QByteArray program = words.first();
	QByteArray arg0 = program;
	if (m_command.isEmpty()) {
		arg0 = QByteArrayLiteral("-") + program.mid(program.lastIndexOf('/') + 1);
	}

	QList<char *> argv;
	argv.append(const_cast<char *>(arg0.constData()));
	for (int i = 1; i < words.size(); ++i) {
		argv.append(const_cast<char *>(words.at(i).constData()));
	}
	argv.append(nullptr);

	const QByteArray directory = m_working_directory.toLocal8Bit();
	const QByteArray term = m_term.toLocal8Bit();
	/*
	 * Converted here for the reason the comment above gives: the child
	 * may not allocate, and toLocal8Bit does.
	 */
	const QByteArray terminfo = m_terminfo_directory.toLocal8Bit();

	int master = -1;
	const pid_t child = forkpty(&master, nullptr, nullptr, &size);

	if (child < 0) {
		set_state(FAILED);
		emit failed(QStringLiteral("could not open a pseudo-terminal: %1")
		                    .arg(QString::fromLocal8Bit(std::strerror(errno))));
		return;
	}

	if (child == 0) {
		/*
		 * The child. Nothing here may return: on any failure it must
		 * _exit rather than unwind back into Qt, which would leave two
		 * processes running the same event loop.
		 */
		if (!term.isEmpty()) setenv("TERM", term.constData(), 1);

		/*
		 * And WHERE to look that TERM up, when the platform ships a
		 * database because the system has none (sec 12.3p). Empty on a
		 * desktop, where /usr/share/terminfo is already there and
		 * pointing TERMINFO at one entry of ours would shadow it.
		 *
		 * setenv rather than putenv: the buffer below is a local, and
		 * putenv would keep a pointer into it.
		 */
		if (!terminfo.isEmpty()) setenv("TERMINFO", terminfo.constData(), 1);

		/*
		 * chdir before exec, and refuse loudly (sec 8.6's --workdir).
		 * A directory that cannot be entered would otherwise leave the
		 * shell wherever beerssh was started, which looks like the
		 * option having been ignored -- and the message reaches the
		 * pty, so it is on screen in the terminal that asked for it.
		 */
		if (!directory.isEmpty() && chdir(directory.constData()) != 0) {
			child_fail("cannot enter ", directory.constData(), 126);
		}

		execvp(program.constData(), argv.data());

		/*
		 * 127 is the shell's code for "not found" and 126 for "found
		 * and not runnable", which is what anything reading our exit
		 * status already understands. Said out loud as well: a mistyped
		 * -e is the ordinary way to get here, and a pane that opens and
		 * closes with no message is indistinguishable from a crash.
		 */
		child_fail("cannot run ", program.constData(), errno == ENOENT ? 127 : 126);
	}

	m_master = master;
	m_child = child;

	/*
	 * Non-blocking, because a read from a pty with no data blocks and
	 * this runs on the UI thread. The notifier says when there is
	 * something; O_NONBLOCK is what stops a spurious wakeup freezing the
	 * window.
	 *
	 * THE FAILURE OF THIS GUARD IS THE CALLER'S FAILURE, which it was
	 * not: `if (flags >= 0) fcntl(...)` proceeded to install the notifier
	 * on a blocking descriptor whenever F_GETFL failed, and said nothing.
	 * The whole point of the flag is that a read on this thread must not
	 * block, so carrying on without it keeps exactly the hazard the line
	 * exists to remove -- and the symptom would be the window freezing,
	 * a long way from here.
	 *
	 * evidence.md states the rule ("a guard's failure must be the
	 * caller's failure", from a cleanup whose helper had gone missing and
	 * whose suite reported all cases passed). A shell that refuses to
	 * open, with a reason, is plainly better than an app that stops
	 * repainting.
	 *
	 * Found by auditing this file against three ways a sibling project's
	 * pty harness had lied to it. Its three were a descriptor pointing at
	 * the wrong terminal, stdout buffered into the pty, and a blocking
	 * read on a master -- none of which applies here, since forkpty()
	 * owns the child's descriptors and nothing writes through stdio. The
	 * third is guarded, and the guard was the thing worth reading.
	 *
	 * There is no sabotage entry: F_GETFL does not fail on a descriptor
	 * forkpty has just returned, so there is no way to reach this branch
	 * from a test and an entry would report MISSED for ever.
	 *
	 * THE CHILD IS TAKEN DOWN HERE, and that half was missing from the
	 * first version of this guard -- written an hour earlier, in the
	 * course of fixing a guard that failed silently.
	 *
	 * The fork has already happened by this line, so an early return is
	 * downstream of a side effect that has nothing to do with a file
	 * descriptor flag. It closed the master and left a shell running
	 * with m_child set, relying on disconnect_from_host() being called
	 * on a session that had just announced it never opened, and on the
	 * slave's hangup persuading the shell to leave.
	 *
	 * The trap is the sibling qtty's, named after it nearly did the same
	 * thing: fixing an inert failure path by writing another, because
	 * the instinct the lesson creates is "return on failure" and
	 * returning is how you acquire the next one. Its tell is the sharp
	 * part -- the danger is highest where the function has LATER side
	 * effects that are not obviously part of the same job, which is
	 * exactly a forked child sitting above a line about O_NONBLOCK.
	 *
	 * collect() is bounded: it polls, escalates to SIGKILL, and returns.
	 */
	const int flags = fcntl(m_master, F_GETFL, 0);
	if (flags < 0 || fcntl(m_master, F_SETFL, flags | O_NONBLOCK) < 0) {
		const QString reason = QString::fromLocal8Bit(std::strerror(errno));

		if (m_child > 0) {
			::kill(pid_t(m_child), SIGHUP);
			collect(pid_t(m_child));
			m_child = -1;
		}

		::close(m_master);
		m_master = -1;

		set_state(FAILED);
		emit failed(QStringLiteral("could not make the pseudo-terminal non-blocking: %1")
		                    .arg(reason));
		return;
	}

	install_notifiers();
}

void bssh_pty_session::on_readable() {
	if (m_master < 0) return;

	QByteArray buffer(BSSH_PTY_READ_SIZE, Qt::Uninitialized);
	const ssize_t got = ::read(m_master, buffer.data(), BSSH_PTY_READ_SIZE);

	if (got > 0) {
		buffer.resize(int(got));
		emit bytes_received(buffer);
		return;
	}

	if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;

	/*
	 * Zero, or EIO -- which is what Linux gives when the last process on
	 * the far side of the pty has gone. That is the shell exiting, NOT a
	 * connection dropping, and the difference matters: sec 5.3's
	 * reconnection machine would otherwise reopen a shell somebody
	 * deliberately typed `exit` into, forever.
	 */
	reap();
}

void bssh_pty_session::reap() {
	const int code = release_child();
	emit shell_exited(code);
	set_state(DISCONNECTED);
}

void bssh_pty_session::restart_shell(const QStringList &command) {
	if (m_master < 0) return;

	/* The size it has now, asked of the pty rather than remembered. */
	struct winsize size;
	std::memset(&size, 0, sizeof(size));
	::ioctl(m_master, TIOCGWINSZ, &size);

	/*
	 * SIGHUP, as closing the terminal would send: a tmux or screen client
	 * detaches and leaves its session running. Released without the
	 * exit reap() announces, because the session has not ended -- the
	 * next command starts in its place at once.
	 */
	if (m_child > 0) ::kill(pid_t(m_child), SIGHUP);
	release_child();

	set_command(command);
	open_shell(size.ws_col, size.ws_row);
}

int bssh_pty_session::release_child() {
	if (m_notifier) {
		m_notifier->setEnabled(false);
		m_notifier->deleteLater();
		m_notifier = nullptr;
	}

	/* Goes with the descriptor it watches, and so does what it was for. */
	if (m_write_notifier) {
		m_write_notifier->setEnabled(false);
		m_write_notifier->deleteLater();
		m_write_notifier = nullptr;
	}
	m_pending.clear();

	if (m_master >= 0) {
		::close(m_master);
		m_master = -1;
	}

	int code = 0;
	if (m_child > 0) {
		code = collect(pid_t(m_child));
		m_child = -1;
	}
	return code;
}

void bssh_pty_session::resize(int cols, int rows) {
	if (m_master < 0 || cols <= 0 || rows <= 0) return;

	struct winsize size;
	std::memset(&size, 0, sizeof(size));
	size.ws_col = ushort(cols);
	size.ws_row = ushort(rows);

	/*
	 * The local equivalent of a window-change message (sec 7): the kernel
	 * sends SIGWINCH to the foreground process group, which is how the
	 * shell and whatever it is running learn the new geometry.
	 */
	::ioctl(m_master, TIOCSWINSZ, &size);
}

/*
 * Hands the kernel as much as it will take right now, and says whether
 * anything is left over.
 *
 * Never waits. A full pty answers EAGAIN on this non-blocking master, and
 * what cannot go now goes when the notifier says there is room.
 */
void bssh_pty_session::flush_pending() {
	while (!m_pending.isEmpty() && m_master >= 0) {
		const ssize_t put = ::write(m_master, m_pending.constData(),
		                             size_t(m_pending.size()));
		if (put > 0) {
			m_pending.remove(0, int(put));
			continue;
		}
		if (put < 0 && errno == EINTR) continue;

		/*
		 * Full. Not an error and not a reason to drop anything: leave
		 * the remainder queued and let on_writable() carry on when the
		 * child reads.
		 */
		if (put < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;

		/* A real write error: the pty is going or gone. */
		m_pending.clear();
		break;
	}

	if (m_write_notifier) m_write_notifier->setEnabled(!m_pending.isEmpty());
}

void bssh_pty_session::on_writable() {
	flush_pending();
}

void bssh_pty_session::send(const QByteArray &bytes) {
	if (m_master < 0 || bytes.isEmpty()) return;

	/*
	 * QUEUED, then flushed as far as it goes. This used to loop on
	 * EAGAIN with a usleep, which is an unbounded wait on the UI thread
	 * -- the thing collect() is carefully written not to do, for the
	 * reason stated there: it hangs the window.
	 *
	 * A paste bigger than the pty buffer is enough to reach it, and only
	 * needs the child not to be reading: measured, 15,360 bytes of a 1 MB
	 * paste land and the loop is still going 3,500ms later with the slave
	 * in raw mode. Canonical mode hides it, because the line discipline
	 * discards instead of filling -- so the configuration that hangs is
	 * the one every full-screen program sets.
	 *
	 * Nothing is dropped, which was the old comment's concern and is
	 * still right: the remainder waits for room rather than being
	 * truncated.
	 */
	m_pending.append(bytes);
	flush_pending();
}

void bssh_pty_session::disconnect_from_host() {
	if (m_master < 0 && m_child < 0) return;

	/*
	 * SIGHUP, which is what closing a terminal window means to the
	 * process group on the other side of it -- a shell treats it as the
	 * line dropping and cleans up after itself.
	 *
	 * It is a REQUEST, and collect() below stops waiting for an answer.
	 * A process is free to ignore SIGHUP -- that is what nohup exists to
	 * arrange -- and the first version of this then blocked in waitpid
	 * forever, on the UI thread. In a test run that is the whole suite
	 * hung on one child that declined to die.
	 */
	if (m_child > 0) ::kill(pid_t(m_child), SIGHUP);

	reap();
}

bssh_ssh_session *bssh_ssh_session::create_local(const QString &shell,
                                                  const QString &working_directory,
                                                  QObject *parent) {
	if (bssh_ssh_session *injected = create_from_factory(parent)) return injected;

	bssh_pty_session *session = new bssh_pty_session(parent);
	session->set_shell(shell);
	session->set_working_directory(working_directory);

	/*
	 * The platform's terminfo database, asked for HERE rather than
	 * inside open_shell(), so the one place that knows it is building a
	 * local shell is the one place that consults the platform -- and so
	 * a test can set it to a directory it made itself.
	 */
	if (const bssh_platform *platform = bssh_platform::instance())
		session->set_terminfo_directory(platform->terminfo_directory());

	return session;
}

/* The master's read notifier, and the write one that waits for room. */
void bssh_pty_session::install_notifiers() {
	m_notifier = new QSocketNotifier(m_master, QSocketNotifier::Read, this);
	connect(m_notifier, &QSocketNotifier::activated, this, [this] { on_readable(); });

	/*
	 * Disabled until there is something waiting: an always-on write
	 * notifier fires continuously while the pty has room, which is
	 * almost always.
	 */
	m_write_notifier = new QSocketNotifier(m_master, QSocketNotifier::Write, this);
	m_write_notifier->setEnabled(false);
	connect(m_write_notifier, &QSocketNotifier::activated, this, [this] { on_writable(); });
}

/* ---- moving the shell to another process (sec 8.4) ---------------- */

namespace {

const char BSSH_PTY_HANDOFF_MAGIC[] = "bssh-pty-transport";
const quint32 BSSH_PTY_HANDOFF_FORMAT = 1;

}  // namespace

void bssh_pty_session::find_working_directory() {
	const bssh_working_directory_t found = bssh_local_working_directory(m_child);
	QMetaObject::invokeMethod(this, [this, found] { emit working_directory_found(found); },
	                          Qt::QueuedConnection);
}

void bssh_pty_session::upload(const QStringList &paths, const QString &directory) {
	/*
	 * On a thread of its own: a large file copied here would hold the
	 * window. Reports go through the application object with a guarded
	 * pointer, so a tab closed during the copy is simply not told.
	 */
	const QPointer<bssh_pty_session> guard(this);
	QThread *copier = QThread::create([guard, paths, directory] {
		QStringList written;
		QString why;
		QElapsedTimer since;
		since.start();
		const bool ok = bssh_copy_into(paths, directory, written, why,
		                               [guard, &since](qint64 done, qint64 total) {
			if (since.elapsed() < 100 && done < total) return;
			since.restart();
			QMetaObject::invokeMethod(qApp, [guard, done, total] {
				if (guard) emit guard->upload_progress(done, total);
			}, Qt::QueuedConnection);
		});
		const QString into = directory.isEmpty() ? QDir::homePath() : directory;
		QMetaObject::invokeMethod(qApp, [guard, ok, into, written, why] {
			if (guard) emit guard->upload_finished(ok, into, written, why);
		}, Qt::QueuedConnection);
	});
	QObject::connect(copier, &QThread::finished, copier, &QObject::deleteLater);
	copier->start();
}

void bssh_pty_session::run_on_the_side(const QString &command) {
	/*
	 * A process beside the shell, through /bin/sh as an SSH exec request
	 * goes through the account's shell, so one command line serves both.
	 * Bounded like the SSH side: five seconds, then it is killed and what
	 * it printed so far is the answer, and at most 64 KiB of that.
	 */
	QProcess *process = new QProcess(this);
	process->setProcessChannelMode(QProcess::SeparateChannels);
	process->setStandardInputFile(QProcess::nullDevice());
	process->setStandardErrorFile(QProcess::nullDevice());

	QTimer *deadline = new QTimer(process);
	deadline->setSingleShot(true);
	connect(deadline, &QTimer::timeout, process, [process] { process->kill(); });

	connect(process, &QProcess::finished, this, [this, process] {
		emit side_output_ready(process->readAllStandardOutput().left(64 * 1024));
		process->deleteLater();
	});
	connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
		if (error != QProcess::FailedToStart) return;
		emit side_output_ready(QByteArray());
		process->deleteLater();
	});

	deadline->start(5000);
	process->start(QStringLiteral("/bin/sh"), { QStringLiteral("-c"), command });
}

QString bssh_pty_session::kind() const {
	return QStringLiteral("local");
}

/*
 * The pty master is a descriptor like a socket, so the shell keeps
 * running on the far side of the move and never notices. What the
 * receiver cannot have is the shell as its CHILD: it learns of the exit
 * by the master reading EOF, and collect() answers 0 at once for a
 * process that was never its own, so the exit code is lost. That is the
 * whole cost.
 */
bool bssh_pty_session::export_transport(QByteArray &state, QList<int> &fds, QString *why) {
	if (m_master < 0) {
		if (why) *why = QStringLiteral("the shell is not running");
		return false;
	}
	if (m_notifier) m_notifier->setEnabled(false);
	if (m_write_notifier) m_write_notifier->setEnabled(false);

	state.clear();
	QDataStream out(&state, QIODevice::WriteOnly);
	out.setVersion(QDataStream::Qt_6_0);
	out << QByteArray(BSSH_PTY_HANDOFF_MAGIC) << BSSH_PTY_HANDOFF_FORMAT << m_shell << m_command
	    << m_working_directory << m_terminfo_directory << m_term << m_child << m_pending;
	fds = { m_master };
	return true;
}

/* The receiver has it: close this copy, and no SIGHUP -- the shell is
 * still somebody's. */
void bssh_pty_session::release_transport() {
	delete m_notifier;
	m_notifier = nullptr;
	delete m_write_notifier;
	m_write_notifier = nullptr;
	m_pending.clear();
	if (m_master >= 0) ::close(m_master);
	m_master = -1;
	m_child = -1;
}

void bssh_pty_session::resume_transport() {
	if (m_master < 0) return;
	if (m_notifier) m_notifier->setEnabled(true);
	if (m_write_notifier && !m_pending.isEmpty()) m_write_notifier->setEnabled(true);
	on_readable();
}

bool bssh_pty_session::import_transport(const QByteArray &state, const QList<int> &fds,
                                        QString *why) {
	if (m_master >= 0 || fds.size() != 1) {
		if (why) *why = QStringLiteral("the shell arrived without its terminal");
		return false;
	}
	QDataStream in(state);
	in.setVersion(QDataStream::Qt_6_0);
	QByteArray magic, pending;
	quint32 format = 0;
	QString shell, working_directory, terminfo_directory, term;
	QStringList command;
	qint64 child = -1;
	in >> magic >> format >> shell >> command >> working_directory >> terminfo_directory >> term
	   >> child >> pending;
	if (in.status() != QDataStream::Ok || !in.atEnd() || magic != BSSH_PTY_HANDOFF_MAGIC ||
	    format != BSSH_PTY_HANDOFF_FORMAT) {
		if (why) *why = QStringLiteral("the shell's state is not one this BeerSSH reads");
		return false;
	}

	const int flags = fcntl(fds.first(), F_GETFL, 0);
	if (flags < 0 || fcntl(fds.first(), F_SETFL, flags | O_NONBLOCK) < 0) {
		if (why) *why = QStringLiteral("the shell's terminal could not be made non-blocking");
		return false;
	}

	m_shell = shell;
	m_command = command;
	m_working_directory = working_directory;
	m_terminfo_directory = terminfo_directory;
	m_term = term;
	m_child = child;
	m_pending = pending;
	m_master = fds.first();
	install_notifiers();
	if (!m_pending.isEmpty()) flush_pending();
	set_state(CONNECTED);
	/* Read once the session is listening, not now: see wire_transport. */
	QMetaObject::invokeMethod(this, [this] { on_readable(); }, Qt::QueuedConnection);
	return true;
}
