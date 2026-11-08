/*
 * What Linux spells one way and macOS another (project.md sec 1: macOS is
 * best-effort, and breakage there is a bug). Plain C, so the session helper
 * includes it as readily as the C++ does.
 *
 * Linux keeps its atomic forms. macOS has no SOCK_CLOEXEC, accept4, pipe2,
 * SO_PEERCRED or MSG_NOSIGNAL, so there the flags are set after the call,
 * and a fork in another thread between the two could inherit the descriptor
 * -- a narrow window, and one the alternative of not building does not have.
 */
#ifndef BSSH_POSIX_COMPAT_H
#define BSSH_POSIX_COMPAT_H

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

#define BSSH_CLOEXEC  1
#define BSSH_NONBLOCK 2

#ifdef MSG_CMSG_CLOEXEC
#define BSSH_MSG_CMSG_CLOEXEC MSG_CMSG_CLOEXEC
#else
/* Received descriptors are marked by bssh_set_fd_flags after the call. */
#define BSSH_MSG_CMSG_CLOEXEC 0
#endif

#ifdef MSG_NOSIGNAL
#define BSSH_MSG_NOSIGNAL MSG_NOSIGNAL
#else
/* SO_NOSIGPIPE, set where the descriptor is made, does the same there. */
#define BSSH_MSG_NOSIGNAL 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

static inline int bssh_set_fd_flags(int fd, int flags) {
	if ((flags & BSSH_CLOEXEC) && fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC) != 0)
		return -1;
	if ((flags & BSSH_NONBLOCK) && fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != 0)
		return -1;
	return 0;
}

static inline int bssh_no_sigpipe(int fd) {
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
	const int on = 1;
	return setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
	(void)fd;
	return 0;
#endif
}

static inline int bssh_socket(int domain, int type, int flags) {
#ifdef SOCK_CLOEXEC
	return socket(domain, type | ((flags & BSSH_CLOEXEC) ? SOCK_CLOEXEC : 0)
	                           | ((flags & BSSH_NONBLOCK) ? SOCK_NONBLOCK : 0), 0);
#else
	const int fd = socket(domain, type, 0);
	if (fd < 0) return -1;
	if (bssh_set_fd_flags(fd, flags) != 0 || bssh_no_sigpipe(fd) != 0) {
		close(fd);
		return -1;
	}
	return fd;
#endif
}

static inline int bssh_accept(int listener, int flags) {
#ifdef SOCK_CLOEXEC
	return accept4(listener, NULL, NULL, ((flags & BSSH_CLOEXEC) ? SOCK_CLOEXEC : 0)
	                                     | ((flags & BSSH_NONBLOCK) ? SOCK_NONBLOCK : 0));
#else
	const int fd = accept(listener, NULL, NULL);
	if (fd < 0) return -1;
	if (bssh_set_fd_flags(fd, flags) != 0 || bssh_no_sigpipe(fd) != 0) {
		close(fd);
		return -1;
	}
	return fd;
#endif
}

static inline int bssh_pipe_cloexec(int fds[2]) {
#ifdef __APPLE__
	if (pipe(fds) != 0) return -1;
	if (bssh_set_fd_flags(fds[0], BSSH_CLOEXEC) != 0 || bssh_set_fd_flags(fds[1], BSSH_CLOEXEC) != 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	return 0;
#else
	return pipe2(fds, O_CLOEXEC);
#endif
}

/* The uid at the other end of a unix socket; 0 on success. */
static inline int bssh_peer_uid(int fd, uid_t *uid) {
#ifdef SO_PEERCRED
	struct ucred peer;
	socklen_t size = sizeof(peer);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0) return -1;
	*uid = peer.uid;
	return 0;
#else
	gid_t gid;
	return getpeereid(fd, uid, &gid);
#endif
}

#ifdef __cplusplus
}
#endif

#endif
