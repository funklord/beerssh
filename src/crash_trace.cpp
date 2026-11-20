#include "crash_trace.h"

#include <QtGlobal>

#if defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)

#include <QDir>
#include <QString>

#include <execinfo.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

namespace {

/*
 * Everything the handler needs, made before any crash: inside one nothing
 * may allocate or lock, since the heap or a lock may be what broke.
 */
char directory_path[1024];
char version_text[64];

/* Sixty-four kilobytes, so the handler runs after the stack has overflowed. */
char alternate_stack[64 * 1024];

volatile sig_atomic_t tracing = 0;

const int SIGNALS[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT };

void put(int fd, const char *text, size_t length) {
	while (length > 0) {
		const ssize_t written = ::write(fd, text, length);
		if (written <= 0) return;
		text += written;
		length -= size_t(written);
	}
}

void put(int fd, const char *text) { put(fd, text, strlen(text)); }

/* A number in `base`, without printf, which is not safe here. */
void put_number(int fd, unsigned long value, unsigned base = 10) {
	char digits[32];
	int at = int(sizeof digits);
	do {
		digits[--at] = "0123456789abcdef"[value % base];
		value /= base;
	} while (value > 0 && at > 0);
	put(fd, digits + at, sizeof digits - size_t(at));
}

const char *signal_name(int number) {
	switch (number) {
	case SIGSEGV: return "SIGSEGV";
	case SIGBUS:  return "SIGBUS";
	case SIGILL:  return "SIGILL";
	case SIGFPE:  return "SIGFPE";
	case SIGABRT: return "SIGABRT";
	default:      return "signal";
	}
}

void trace(int number, siginfo_t *info, void *) {
	/* A crash inside the trace does not trace again: it dies as it would. */
	if (!tracing) {
		tracing = 1;

		char path[sizeof directory_path + 32];
		size_t used = strlen(directory_path);
		memcpy(path, directory_path, used);
		const char prefix[] = "/crash-";
		memcpy(path + used, prefix, sizeof prefix - 1);
		used += sizeof prefix - 1;
		char digits[16];
		int at = int(sizeof digits);
		unsigned long pid = static_cast<unsigned long>(::getpid());
		do {
			digits[--at] = char('0' + pid % 10);
			pid /= 10;
		} while (pid > 0 && at > 0);
		memcpy(path + used, digits + at, sizeof digits - size_t(at));
		used += sizeof digits - size_t(at);
		memcpy(path + used, ".txt", 5);

		const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
		if (fd >= 0) {
			put(fd, "BeerSSH ");
			put(fd, version_text);
			put(fd, " crashed: ");
			put(fd, signal_name(number));
			put(fd, " (signal ");
			put_number(fd, unsigned(number));
			put(fd, ")");
			/* A fault the kernel found has an address; a raised signal has none. */
			if (info && info->si_code > 0 && (number == SIGSEGV || number == SIGBUS)) {
				put(fd, " at 0x");
				put_number(fd, reinterpret_cast<unsigned long>(info->si_addr), 16);
			}
			put(fd, "\npid ");
			put_number(fd, static_cast<unsigned long>(::getpid()));
			put(fd, "\n\nbacktrace:\n");
			void *frames[64];
			const int count = backtrace(frames, 64);
			backtrace_symbols_fd(frames, count, fd);

			/* Where each library was loaded, to turn the addresses into offsets. */
			put(fd, "\nmaps:\n");
			const int maps = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
			if (maps >= 0) {
				char chunk[4096];
				ssize_t got;
				while ((got = ::read(maps, chunk, sizeof chunk)) > 0) put(fd, chunk, size_t(got));
				::close(maps);
			}
			::close(fd);

			put(STDERR_FILENO, "beerssh: crashed with ");
			put(STDERR_FILENO, signal_name(number));
			put(STDERR_FILENO, "; what it was doing is in ");
			put(STDERR_FILENO, path);
			put(STDERR_FILENO, "\n");
		}
	}

	/* SA_RESETHAND has put the default back: die of the same signal. */
	::raise(number);
}

}  // namespace

void bssh_install_crash_trace(const QByteArray &directory) {
	if (directory.isEmpty() || size_t(directory.size()) >= sizeof directory_path) return;
	QDir().mkpath(QString::fromLocal8Bit(directory));
	memcpy(directory_path, directory.constData(), size_t(directory.size()));
	directory_path[directory.size()] = '\0';
	strncpy(version_text, BSSH_VERSION_STRING, sizeof version_text - 1);

	/* The first backtrace() loads libgcc, which allocates: do it now. */
	void *warm[1];
	backtrace(warm, 1);

	stack_t stack;
	memset(&stack, 0, sizeof stack);
	stack.ss_sp = alternate_stack;
	stack.ss_size = sizeof alternate_stack;
	::sigaltstack(&stack, nullptr);

	struct sigaction action;
	memset(&action, 0, sizeof action);
	action.sa_sigaction = trace;
	action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
	sigemptyset(&action.sa_mask);
	for (int number : SIGNALS) ::sigaction(number, &action, nullptr);
}

#else

void bssh_install_crash_trace(const QByteArray &) {}

#endif
