#include "ssh/pty_session.h"

#include <QProcessEnvironment>
#include <QSocketNotifier>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <pty.h>
#include <sys/ioctl.h>
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

} /* namespace */

bssh_pty_session::bssh_pty_session(QObject *parent)
    : bssh_ssh_session(parent),
      m_master(-1),
      m_child(-1),
      m_notifier(nullptr) {
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
		QByteArray shell = m_shell.toLocal8Bit();
		if (shell.isEmpty()) shell = qgetenv("SHELL");
		if (shell.isEmpty()) shell = QByteArrayLiteral("/bin/sh");

		if (!m_term.isEmpty()) setenv("TERM", m_term.toLocal8Bit().constData(), 1);

		/*
		 * A LOGIN shell: argv[0] with a leading '-', which is what
		 * getty and ssh both do and what makes a shell read its profile.
		 * A terminal that opens without the user's own environment is
		 * one they have to fix by hand every time.
		 */
		const QByteArray base = shell.mid(shell.lastIndexOf('/') + 1);
		const QByteArray arg0 = QByteArrayLiteral("-") + base;

		execl(shell.constData(), arg0.constData(), nullptr);
		_exit(127);
	}

	m_master = master;
	m_child = child;

	/*
	 * Non-blocking, because a read from a pty with no data blocks and
	 * this runs on the UI thread. The notifier says when there is
	 * something; O_NONBLOCK is what stops a spurious wakeup freezing the
	 * window.
	 */
	const int flags = fcntl(m_master, F_GETFL, 0);
	if (flags >= 0) fcntl(m_master, F_SETFL, flags | O_NONBLOCK);

	m_notifier = new QSocketNotifier(m_master, QSocketNotifier::Read, this);
	connect(m_notifier, &QSocketNotifier::activated, this, [this] { on_readable(); });
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
	if (m_notifier) {
		m_notifier->setEnabled(false);
		m_notifier->deleteLater();
		m_notifier = nullptr;
	}

	if (m_master >= 0) {
		::close(m_master);
		m_master = -1;
	}

	int code = 0;
	if (m_child > 0) {
		code = collect(pid_t(m_child));
		m_child = -1;
	}

	emit shell_exited(code);
	set_state(DISCONNECTED);
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

void bssh_pty_session::send(const QByteArray &bytes) {
	if (m_master < 0 || bytes.isEmpty()) return;

	qint64 written = 0;
	while (written < bytes.size()) {
		const ssize_t put = ::write(m_master, bytes.constData() + written,
		                             size_t(bytes.size() - written));
		if (put > 0) {
			written += put;
			continue;
		}
		if (put < 0 && errno == EINTR) continue;

		/*
		 * A full pty buffer is the one case worth not dropping on the
		 * floor: a paste larger than the kernel's buffer would
		 * otherwise arrive truncated, which is silent and looks like
		 * the remote losing input.
		 */
		if (put < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			::usleep(1000);
			continue;
		}
		break;
	}
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

bssh_ssh_session *bssh_ssh_session::create_local(const QString &shell, QObject *parent) {
	if (bssh_ssh_session *injected = create_from_factory(parent)) return injected;

	bssh_pty_session *session = new bssh_pty_session(parent);
	session->set_shell(shell);
	return session;
}
