#include "sshd/root_path.h"

#include <QList>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <sys/stat.h>

namespace {

/* realpath(3), as bytes, or empty with errno set. */
QByteArray resolve(const QByteArray &path) {
	char *real = ::realpath(path.constData(), nullptr);
	if (!real) return QByteArray();
	QByteArray result(real);
	std::free(real);
	return result;
}

} /* namespace */

bssh_root_path::bssh_root_path(const QByteArray &root) {
	const QByteArray real = resolve(root);
	struct stat st;
	if (real.isEmpty() || ::stat(real.constData(), &st) != 0 || !S_ISDIR(st.st_mode)) return;
	m_root = real;
}

QByteArray bssh_root_path::normalise(const QByteArray &client_path) {
	QList<QByteArray> kept;
	for (const QByteArray &segment : client_path.split('/')) {
		if (segment.isEmpty() || segment == ".") continue;
		if (segment == "..") {
			if (!kept.isEmpty()) kept.removeLast();
			continue;
		}
		kept.append(segment);
	}

	QByteArray result;
	for (const QByteArray &segment : kept) result += '/' + segment;
	return result.isEmpty() ? QByteArray("/") : result;
}

bool bssh_root_path::contains(const QByteArray &real) const {
	if (m_root.isEmpty() || real.isEmpty()) return false;
	if (m_root == "/") return real.startsWith('/');
	return real == m_root || real.startsWith(m_root + '/');
}

QByteArray bssh_root_path::client_view(const QByteArray &real) const {
	if (!contains(real) || real == m_root) return QByteArray("/");
	if (m_root == "/") return real;
	return real.mid(m_root.size());
}

bssh_root_path::resolved_t bssh_root_path::existing(const QByteArray &client_path) const {
	resolved_t result;
	if (!valid()) {
		result.error = ENOENT;
		return result;
	}

	const QByteArray virtual_path = normalise(client_path);
	const QByteArray joined = virtual_path == "/" ? m_root
	                          : (m_root == "/" ? virtual_path : m_root + virtual_path);

	const QByteArray real = resolve(joined);
	if (real.isEmpty()) {
		result.error = errno ? errno : ENOENT;
		return result;
	}
	if (!contains(real)) {
		result.error = EACCES;
		return result;
	}

	result.real = real;
	return result;
}

bssh_root_path::resolved_t bssh_root_path::leaf(const QByteArray &client_path) const {
	resolved_t result;
	const QByteArray virtual_path = normalise(client_path);

	/* The root is served, not replaced: it cannot be removed or renamed. */
	if (!valid() || virtual_path == "/") {
		result.error = EACCES;
		return result;
	}

	const int slash = virtual_path.lastIndexOf('/');
	const QByteArray name = virtual_path.mid(slash + 1);
	const resolved_t parent = existing(virtual_path.left(slash));
	if (parent.real.isEmpty()) return parent;

	struct stat parent_st;
	if (::stat(parent.real.constData(), &parent_st) != 0 || !S_ISDIR(parent_st.st_mode)) {
		result.error = ENOTDIR;
		return result;
	}

	const QByteArray real = parent.real == "/" ? '/' + name : parent.real + '/' + name;

	/*
	 * A symlink as the leaf: creating or opening through it would land
	 * wherever it points, so where it points is checked too. A dangling
	 * one is refused outright -- O_CREAT through it would make a file at
	 * its target, and that target has nothing yet to check.
	 */
	struct stat st;
	if (::lstat(real.constData(), &st) == 0 && S_ISLNK(st.st_mode)) {
		const QByteArray target = resolve(real);
		if (target.isEmpty() || !contains(target)) {
			result.error = EACCES;
			return result;
		}
	}

	result.real = real;
	return result;
}
