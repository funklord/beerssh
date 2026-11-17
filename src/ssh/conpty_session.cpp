#include "ssh/ssh_session.h"

#include <QByteArray>
#include <QDir>
#include <QProcessEnvironment>
#include <QStringList>
#include <QThread>
#include <QWinEventNotifier>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

#include <windows.h>

/*
 * The local shell on Windows (project.md sec 1, best-effort): ConPTY,
 * which is to Windows what forkpty() is to ssh/pty_session.cpp -- a
 * pseudo-console whose two pipes carry VT sequences both ways, so the
 * emulator sees what it sees from any other transport.
 *
 * What runs is the profile's shell as a command line, or %COMSPEC%
 * (cmd.exe), the one shell every Windows has. A -e command runs instead
 * of either, its words quoted as CommandLineToArgvW reads them back.
 *
 * Two threads, and each is there for a hang it prevents. ReadFile on the
 * output pipe blocks, so a reader thread drains it and posts what it got
 * to this object. WriteFile on the input pipe blocks when the child is
 * not reading, so a paste bigger than the pipe would stop the window on
 * the UI thread -- the hazard pty_session.cpp queues its writes for -- so
 * writes go to a queue a writer thread empties.
 *
 * The exit comes from the PROCESS handle, not from the output pipe: the
 * pseudo-console keeps that pipe open after its client has gone, until
 * ClosePseudoConsole. So the process ending closes the console, the
 * reader drains the rest and stops, and the exit status is posted behind
 * the last bytes so nothing arrives after it.
 *
 * ConPTY is Windows 10 1809 and later. Its three calls are looked up at
 * run time rather than linked, so an older Windows fails to open a local
 * shell with a reason instead of failing to start the program at all.
 */
namespace {

typedef void *pseudo_console_t;
typedef HRESULT(WINAPI *create_console_t)(COORD, HANDLE, HANDLE, DWORD, pseudo_console_t *);
typedef HRESULT(WINAPI *resize_console_t)(pseudo_console_t, COORD);
typedef void(WINAPI *close_console_t)(pseudo_console_t);

/* PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, which older headers lack. */
const DWORD_PTR attribute_pseudoconsole = 0x00020016;

struct conpty_api_t {
	create_console_t create = nullptr;
	resize_console_t resize = nullptr;
	close_console_t close = nullptr;
	bool usable() const { return create && resize && close; }
};

const conpty_api_t &conpty_api() {
	static const conpty_api_t api = [] {
		conpty_api_t found;
		HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
		if (kernel) {
			found.create = reinterpret_cast<create_console_t>(
			        reinterpret_cast<void *>(GetProcAddress(kernel, "CreatePseudoConsole")));
			found.resize = reinterpret_cast<resize_console_t>(
			        reinterpret_cast<void *>(GetProcAddress(kernel, "ResizePseudoConsole")));
			found.close = reinterpret_cast<close_console_t>(
			        reinterpret_cast<void *>(GetProcAddress(kernel, "ClosePseudoConsole")));
		}
		return found;
	}();
	return api;
}

QString windows_error(DWORD code) {
	wchar_t *text = nullptr;
	const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
	                                    | FORMAT_MESSAGE_IGNORE_INSERTS,
	                                    nullptr, code, 0, reinterpret_cast<wchar_t *>(&text), 0, nullptr);
	QString message = length ? QString::fromWCharArray(text, int(length)).trimmed()
	                         : QStringLiteral("error %1").arg(code);
	if (text) LocalFree(text);
	return message;
}

/* One argument as CommandLineToArgvW will read it back. */
QString quoted(const QString &argument) {
	if (!argument.isEmpty() && !argument.contains(QLatin1Char(' '))
	    && !argument.contains(QLatin1Char('\t')) && !argument.contains(QLatin1Char('"'))) {
		return argument;
	}
	QString out = QStringLiteral("\"");
	int backslashes = 0;
	for (const QChar c : argument) {
		if (c == QLatin1Char('\\')) {
			++backslashes;
			continue;
		}
		if (c == QLatin1Char('"')) {
			out += QString(backslashes * 2 + 1, QLatin1Char('\\'));
		} else {
			out += QString(backslashes, QLatin1Char('\\'));
		}
		out += c;
		backslashes = 0;
	}
	out += QString(backslashes * 2, QLatin1Char('\\'));
	out += QLatin1Char('"');
	return out;
}

COORD console_size(int cols, int rows) {
	COORD size;
	size.X = SHORT(cols > 0 ? cols : 80);
	size.Y = SHORT(rows > 0 ? rows : 24);
	return size;
}

class bssh_conpty_session : public bssh_ssh_session {
public:
	explicit bssh_conpty_session(QObject *parent) : bssh_ssh_session(parent) {}
	~bssh_conpty_session() override { shut_down(); }

	void set_shell(const QString &shell) { m_shell = shell; }
	void set_working_directory(const QString &directory) { m_directory = directory; }

	void connect_to_host(const QString &host, quint16 port, const QString &user) override {
		Q_UNUSED(host);
		Q_UNUSED(port);
		Q_UNUSED(user);
		/* As pty_session.cpp: nothing to reach, but everything downstream reads the state. */
		set_state(CONNECTED);
	}

	void open_shell(int cols, int rows) override;

	void resize(int cols, int rows) override {
		if (!m_console || cols <= 0 || rows <= 0) return;
		conpty_api().resize(m_console, console_size(cols, rows));
	}

	void send(const QByteArray &bytes) override {
		if (!m_console || bytes.isEmpty()) return;
		{
			std::lock_guard<std::mutex> lock(m_queue_lock);
			m_queue += bytes;
		}
		m_queue_wake.notify_one();
	}

	void disconnect_from_host() override {
		if (!m_process && !m_console) return;
		const bool was_running = m_process != nullptr;
		shut_down();
		if (was_running) set_state(DISCONNECTED);
	}

	void set_command(const QStringList &command) override { m_command = command; }
	void set_term(const QString &term) override { m_term = term; }
	void authenticate_password(const QString &password) override { Q_UNUSED(password); }
	void set_keepalive(int seconds) override { Q_UNUSED(seconds); }
	void set_compression(bool compression) override { Q_UNUSED(compression); }
	void set_identity_file(const QString &path) override { Q_UNUSED(path); }
	void set_proxy_jump(const QString &spec) override { Q_UNUSED(spec); }
	void accept_host_key() override {}
	void supply_key_passphrase(const QString &passphrase) override { Q_UNUSED(passphrase); }
	QString kind() const override { return QStringLiteral("local"); }

private:
	void fail(const QString &why) {
		shut_down();
		set_state(FAILED);
		emit failed(why);
	}
	void on_process_exit();
	void close_console();
	void shut_down();
	void write_loop();

	QString m_shell;
	QString m_directory;
	QString m_term;
	QStringList m_command;

	pseudo_console_t m_console = nullptr;
	HANDLE m_input = nullptr;   /* we write, the console reads */
	HANDLE m_output = nullptr;  /* the console writes, the reader reads */
	HANDLE m_process = nullptr;
	QWinEventNotifier *m_exit = nullptr;
	QThread *m_reader = nullptr;
	QThread *m_writer = nullptr;

	std::mutex m_queue_lock;
	std::condition_variable m_queue_wake;
	QByteArray m_queue;
	bool m_stopping = false;
};

void bssh_conpty_session::open_shell(int cols, int rows) {
	if (m_console || m_process) return;

	const conpty_api_t &api = conpty_api();
	if (!api.usable()) {
		fail(QStringLiteral("A local shell needs ConPTY, which is Windows 10 version 1809 or later"));
		return;
	}

	HANDLE console_reads = nullptr;
	HANDLE console_writes = nullptr;
	if (!CreatePipe(&console_reads, &m_input, nullptr, 65536)) {
		fail(QStringLiteral("could not make the console's input pipe: %1").arg(windows_error(GetLastError())));
		return;
	}
	if (!CreatePipe(&m_output, &console_writes, nullptr, 65536)) {
		const DWORD code = GetLastError();
		CloseHandle(console_reads);
		fail(QStringLiteral("could not make the console's output pipe: %1").arg(windows_error(code)));
		return;
	}

	const HRESULT made = api.create(console_size(cols, rows), console_reads, console_writes, 0, &m_console);
	/* The console holds its own copies of these two now. */
	CloseHandle(console_reads);
	CloseHandle(console_writes);
	if (FAILED(made)) {
		m_console = nullptr;
		fail(QStringLiteral("could not open a pseudo-console: %1").arg(windows_error(DWORD(made))));
		return;
	}

	QString command_line;
	if (!m_command.isEmpty()) {
		QStringList words;
		for (const QString &word : m_command) words << quoted(word);
		command_line = words.join(QLatin1Char(' '));
	} else if (!m_shell.trimmed().isEmpty()) {
		command_line = m_shell.trimmed();
	} else {
		command_line = qEnvironmentVariable("COMSPEC", QStringLiteral("cmd.exe"));
	}

	/*
	 * TERM for the programs that read it -- Git's bash, anything from
	 * MSYS2 -- in the child's own environment block rather than ours, so
	 * nothing else in this process sees it change.
	 */
	QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
	if (!m_term.isEmpty()) environment.insert(QStringLiteral("TERM"), m_term);
	QStringList pairs = environment.toStringList();
	pairs.sort(Qt::CaseInsensitive);
	QString block;
	for (const QString &pair : pairs) {
		block += pair;
		block += QChar(0);
	}
	block += QChar(0);

	SIZE_T attributes_size = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attributes_size);
	/* new[] for its alignment, which the list wants and a QByteArray's data need not have. */
	std::unique_ptr<char[]> attributes(new char[attributes_size]);
	STARTUPINFOEXW startup;
	ZeroMemory(&startup, sizeof(startup));
	startup.StartupInfo.cb = sizeof(startup);
	/*
	 * Null standard handles, said explicitly. Left unset, a child whose
	 * parent has its own stdout redirected takes THOSE rather than the
	 * pseudo-console's: the first CI run's `echo` landed in the job log
	 * and the terminal saw nothing but the console's setup sequences.
	 */
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = nullptr;
	startup.StartupInfo.hStdOutput = nullptr;
	startup.StartupInfo.hStdError = nullptr;
	startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.get());
	if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributes_size)
	    || !UpdateProcThreadAttribute(startup.lpAttributeList, 0, attribute_pseudoconsole,
	                                  m_console, sizeof(m_console), nullptr, nullptr)) {
		fail(QStringLiteral("could not attach the pseudo-console: %1").arg(windows_error(GetLastError())));
		return;
	}

	std::wstring command = command_line.toStdWString();
	const std::wstring directory = QDir::toNativeSeparators(m_directory).toStdWString();
	const std::wstring environment_block = block.toStdWString();
	PROCESS_INFORMATION started;
	ZeroMemory(&started, sizeof(started));
	const BOOL ran = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
	                                EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
	                                const_cast<wchar_t *>(environment_block.c_str()),
	                                m_directory.isEmpty() ? nullptr : directory.c_str(),
	                                &startup.StartupInfo, &started);
	const DWORD run_error = GetLastError();
	DeleteProcThreadAttributeList(startup.lpAttributeList);
	if (!ran) {
		/*
		 * Said in the pane as well as by the signal, as pty_session.cpp
		 * does for a mistyped -e: a pane that opens and closes with no
		 * message is indistinguishable from a crash.
		 */
		fail(QStringLiteral("cannot run %1: %2").arg(command_line, windows_error(run_error)));
		return;
	}
	CloseHandle(started.hThread);
	m_process = started.hProcess;

	HANDLE output = m_output;
	m_reader = QThread::create([this, output] {
		char buffer[16384];
		for (;;) {
			DWORD got = 0;
			if (!ReadFile(output, buffer, sizeof(buffer), &got, nullptr) || got == 0) break;
			const QByteArray bytes(buffer, int(got));
			QMetaObject::invokeMethod(this, [this, bytes] { emit bytes_received(bytes); },
			                          Qt::QueuedConnection);
		}
	});
	m_reader->start();

	m_stopping = false;
	m_writer = QThread::create([this] { write_loop(); });
	m_writer->start();

	m_exit = new QWinEventNotifier(m_process, this);
	connect(m_exit, &QWinEventNotifier::activated, this, [this] { on_process_exit(); });
}

void bssh_conpty_session::write_loop() {
	for (;;) {
		QByteArray chunk;
		{
			std::unique_lock<std::mutex> lock(m_queue_lock);
			m_queue_wake.wait(lock, [this] { return m_stopping || !m_queue.isEmpty(); });
			if (m_stopping) return;
			chunk.swap(m_queue);
		}
		const char *at = chunk.constData();
		qsizetype left = chunk.size();
		while (left > 0) {
			DWORD wrote = 0;
			if (!WriteFile(m_input, at, DWORD(left), &wrote, nullptr)) return;
			at += wrote;
			left -= qsizetype(wrote);
		}
	}
}

void bssh_conpty_session::on_process_exit() {
	DWORD code = 0;
	if (!m_process || !GetExitCodeProcess(m_process, &code)) code = DWORD(-1);
	shut_down();
	/* Queued, so it lands behind the last bytes the reader posted. */
	QMetaObject::invokeMethod(this, [this, code] {
		emit shell_exited(int(code));
		set_state(DISCONNECTED);
	}, Qt::QueuedConnection);
}

/*
 * The console first: closing it is what ends both threads -- the reader
 * reads end-of-file once the console's end of its pipe is gone, and a
 * writer blocked on a full pipe gets an error -- and it ends whatever is
 * still attached, as closing a terminal window does.
 */
void bssh_conpty_session::close_console() {
	if (m_console) {
		conpty_api().close(m_console);
		m_console = nullptr;
	}
}

void bssh_conpty_session::shut_down() {
	delete m_exit;
	m_exit = nullptr;

	close_console();

	{
		std::lock_guard<std::mutex> lock(m_queue_lock);
		m_stopping = true;
		m_queue.clear();
	}
	m_queue_wake.notify_one();

	for (QThread **thread : { &m_reader, &m_writer }) {
		if (!*thread) continue;
		(*thread)->wait();
		delete *thread;
		*thread = nullptr;
	}

	if (m_process) {
		/*
		 * Bounded, as pty_session.cpp's collect() is: a client that
		 * outlives its console by two seconds is ended rather than
		 * waited on, on the UI thread.
		 */
		if (WaitForSingleObject(m_process, 2000) == WAIT_TIMEOUT) TerminateProcess(m_process, 1);
		CloseHandle(m_process);
		m_process = nullptr;
	}
	for (HANDLE *handle : { &m_input, &m_output }) {
		if (*handle) {
			CloseHandle(*handle);
			*handle = nullptr;
		}
	}
}

} /* namespace */

bssh_ssh_session *bssh_ssh_session::create_local(const QString &shell,
                                                  const QString &working_directory,
                                                  QObject *parent) {
	if (bssh_ssh_session *injected = create_from_factory(parent)) return injected;
	bssh_conpty_session *session = new bssh_conpty_session(parent);
	session->set_shell(shell);
	session->set_working_directory(working_directory);
	return session;
}
