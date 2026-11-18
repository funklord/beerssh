#include "sshd/helper_channel.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include "posix_compat.h"
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vector>

namespace {

const char PROGRAM[] = "beerssh-session";

/* The helper's own rule for a name (src/session/session_main.c), checked here too. */
bool name_is_safe(const QByteArray &name) {
	if (name.isEmpty() || name.size() > 64) return false;
	for (const char c : name) {
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'
		      || c == '_'))
			return false;
	}
	return true;
}

} /* namespace */

QList<QByteArray> bssh_helper_arguments(const QByteArray &command) {
	const QList<QByteArray> words = command.split(' ');
	if (words.value(0) != PROGRAM) return {};
	if (words.size() == 2 && words.at(1) == "version") return words;
	if (words.size() == 3 && words.at(1) == "attach" && name_is_safe(words.at(2))) return words;
	return {};
}

bool bssh_helper_start(const QByteArray &program, const QList<QByteArray> &arguments,
                       bssh_helper_process_t *out) {
	int io[2];
	int errors[2];
	if (::socketpair(AF_UNIX, SOCK_STREAM, 0, io) != 0) return false;
	if (bssh_set_fd_flags(io[0], BSSH_CLOEXEC | BSSH_NONBLOCK) != 0
	    || bssh_set_fd_flags(io[1], BSSH_CLOEXEC) != 0 || bssh_no_sigpipe(io[0]) != 0
	    || bssh_pipe_cloexec(errors) != 0) {
		::close(io[0]);
		::close(io[1]);
		return false;
	}

	/* Everything the child needs, made before fork: after it only async-signal-safe calls. */
	std::vector<char *> argv;
	for (const QByteArray &argument : arguments) argv.push_back(const_cast<char *>(argument.constData()));
	argv.push_back(nullptr);
	struct rlimit limit;
	const int highest = ::getrlimit(RLIMIT_NOFILE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY
	                            && limit.rlim_cur < 65536
	                    ? int(limit.rlim_cur)
	                    : 65536;

	const pid_t pid = ::fork();
	if (pid < 0) {
		::close(io[0]);
		::close(io[1]);
		::close(errors[0]);
		::close(errors[1]);
		return false;
	}
	if (pid == 0) {
		if (::dup2(io[1], STDIN_FILENO) < 0 || ::dup2(io[1], STDOUT_FILENO) < 0
		    || ::dup2(errors[1], STDERR_FILENO) < 0)
			::_exit(127);
		for (int fd = STDERR_FILENO + 1; fd < highest; ++fd) ::close(fd);
		/* A server may ignore SIGPIPE; a program it starts should not inherit that. */
		::signal(SIGPIPE, SIG_DFL);
		::execv(program.constData(), argv.data());
		::_exit(127);
	}

	::close(io[1]);
	::close(errors[1]);
	bssh_set_fd_flags(errors[0], BSSH_NONBLOCK);
	out->pid = pid;
	out->io = io[0];
	out->errors = errors[0];
	return true;
}

int bssh_helper_reap(bssh_helper_process_t *process, bool wait) {
	if (process->pid < 0) return -1;
	int status = 0;
	pid_t got;
	do {
		got = ::waitpid(process->pid, &status, wait ? 0 : WNOHANG);
	} while (got < 0 && errno == EINTR);
	if (got == 0) return -1;
	process->pid = -1;
	if (got < 0) return 255;
	if (WIFEXITED(status)) return WEXITSTATUS(status);
	if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
	return 255;
}

void bssh_helper_close(bssh_helper_process_t *process) {
	if (process->io >= 0) ::close(process->io);
	if (process->errors >= 0) ::close(process->errors);
	process->io = process->errors = -1;
}
