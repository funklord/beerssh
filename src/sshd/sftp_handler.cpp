#include "sshd/sftp_handler.h"

#include <QByteArray>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

struct bssh_sftp_handler::open_t {
	int fd = -1;
	DIR *dir = nullptr;
	QByteArray real;   /* what was opened, for a setstat on a directory handle */
};

namespace {

/* How many names one READDIR reply carries; the client asks again for more. */
const int READDIR_BATCH = 64;

int status_for(int error) {
	switch (error) {
	case ENOENT:
	case ENOTDIR:
		return SSH_FX_NO_SUCH_FILE;
	case EACCES:
	case EPERM:
	case EROFS:
		return SSH_FX_PERMISSION_DENIED;
	default:
		return SSH_FX_FAILURE;
	}
}

void fill_attributes(struct sftp_attributes_struct &attr, const struct stat &st) {
	std::memset(&attr, 0, sizeof attr);
	attr.flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_UIDGID
	           | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;
	attr.size = quint64(st.st_size);
	attr.uid = st.st_uid;
	attr.gid = st.st_gid;
	attr.permissions = st.st_mode;
	attr.atime = quint32(st.st_atime);
	attr.mtime = quint32(st.st_mtime);
}

/*
 * The "ls -l" line SFTP version 3 sends beside each name. Clients print it
 * verbatim for a long listing and parse nothing out of it, so it only has
 * to read like ls.
 */
QByteArray long_name(const QByteArray &name, const struct stat &st) {
	char mode[11] = "----------";
	if (S_ISDIR(st.st_mode)) mode[0] = 'd';
	else if (S_ISLNK(st.st_mode)) mode[0] = 'l';
	const char *const bits = "rwxrwxrwx";
	for (int i = 0; i < 9; ++i) {
		if (st.st_mode & (0400 >> i)) mode[i + 1] = bits[i];
	}

	char when[32] = "";
	struct tm local;
	const time_t mtime = st.st_mtime;
	if (localtime_r(&mtime, &local)) std::strftime(when, sizeof when, "%b %e %Y", &local);

	char line[128];
	std::snprintf(line, sizeof line, "%s %3lu %-8u %-8u %10lld %s ", mode,
	              static_cast<unsigned long>(st.st_nlink), unsigned(st.st_uid),
	              unsigned(st.st_gid), static_cast<long long>(st.st_size), when);
	return QByteArray(line) + name;
}

QByteArray path_of(sftp_client_message msg) {
	const char *name = sftp_client_message_get_filename(msg);
	return name ? QByteArray(name) : QByteArray();
}

} /* namespace */

bssh_sftp_handler::bssh_sftp_handler(sftp_session sftp, const bssh_root_path &root)
    : m_sftp(sftp), m_root(root) {
}

bssh_sftp_handler::~bssh_sftp_handler() {
	for (open_t *open : std::as_const(m_open)) {
		if (open->fd >= 0) ::close(open->fd);
		if (open->dir) ::closedir(open->dir);
		sftp_handle_remove(m_sftp, open);
		delete open;
	}
}

bool bssh_sftp_handler::reply_errno(sftp_client_message msg, int error) {
	return sftp_reply_status(msg, status_for(error), std::strerror(error)) == 0;
}

bool bssh_sftp_handler::reply_ok(sftp_client_message msg) {
	return sftp_reply_status(msg, SSH_FX_OK, nullptr) == 0;
}

bssh_sftp_handler::open_t *bssh_sftp_handler::lookup(sftp_client_message msg) {
	if (!msg->handle) return nullptr;
	open_t *open = static_cast<open_t *>(sftp_handle(m_sftp, msg->handle));

	/*
	 * libssh hands back whatever pointer sits in that slot. Only ones this
	 * handler made are trusted: a handle is the client's to send, and its
	 * bytes are an index the client chose.
	 */
	return m_open.contains(open) ? open : nullptr;
}

bool bssh_sftp_handler::handle(sftp_client_message msg) {
	switch (sftp_client_message_get_type(msg)) {
	case SSH_FXP_OPEN:     return do_open(msg);
	case SSH_FXP_CLOSE:    return do_close(msg);
	case SSH_FXP_READ:     return do_read(msg);
	case SSH_FXP_WRITE:    return do_write(msg);
	case SSH_FXP_OPENDIR:  return do_opendir(msg);
	case SSH_FXP_READDIR:  return do_readdir(msg);
	case SSH_FXP_STAT:
	case SSH_FXP_LSTAT:    return do_stat(msg, false);
	case SSH_FXP_FSTAT:    return do_stat(msg, true);
	case SSH_FXP_SETSTAT:  return do_setstat(msg, false);
	case SSH_FXP_FSETSTAT: return do_setstat(msg, true);
	case SSH_FXP_MKDIR:    return do_mkdir(msg);
	case SSH_FXP_REMOVE:   return do_remove(msg, false);
	case SSH_FXP_RMDIR:    return do_remove(msg, true);
	case SSH_FXP_RENAME:   return do_rename(msg, false);
	case SSH_FXP_REALPATH: return do_realpath(msg);
	case SSH_FXP_EXTENDED: return do_extended(msg);

	/*
	 * Links are neither made nor read. A link made here could point out
	 * of the root for the next request to follow, and reading one would
	 * tell the client about a path outside it.
	 */
	case SSH_FXP_READLINK:
	case SSH_FXP_SYMLINK:
	default:
		return sftp_reply_status(msg, SSH_FX_OP_UNSUPPORTED, "Not supported") == 0;
	}
}

bool bssh_sftp_handler::do_open(sftp_client_message msg) {
	const quint32 flags = sftp_client_message_get_flags(msg);

	int mode = 0;
	if ((flags & SSH_FXF_READ) && (flags & SSH_FXF_WRITE)) mode = O_RDWR;
	else if (flags & SSH_FXF_WRITE) mode = O_WRONLY;
	else mode = O_RDONLY;

	if (flags & SSH_FXF_APPEND) mode |= O_APPEND;
	if (flags & SSH_FXF_CREAT) mode |= O_CREAT;
	if (flags & SSH_FXF_TRUNC) mode |= O_TRUNC;
	if (flags & SSH_FXF_EXCL) mode |= O_EXCL;

	/* Creating names a leaf; anything else must already be there. */
	const bssh_root_path::resolved_t path = (flags & SSH_FXF_CREAT) ? m_root.leaf(path_of(msg))
	                                                                : m_root.existing(path_of(msg));
	if (path.real.isEmpty()) return reply_errno(msg, path.error);

	mode_t permissions = 0644;
	if (msg->attr && (msg->attr->flags & SSH_FILEXFER_ATTR_PERMISSIONS))
		permissions = msg->attr->permissions & 07777;

	const int fd = ::open(path.real.constData(), mode | O_CLOEXEC, permissions);
	if (fd < 0) return reply_errno(msg, errno);

	struct stat st;
	if (::fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
		::close(fd);
		return reply_errno(msg, EISDIR);
	}

	open_t *open = new open_t;
	open->fd = fd;
	open->real = path.real;

	ssh_string handle = sftp_handle_alloc(m_sftp, open);
	if (!handle) {
		::close(fd);
		delete open;
		return sftp_reply_status(msg, SSH_FX_FAILURE, "Too many open files") == 0;
	}
	m_open.insert(open);

	const bool sent = sftp_reply_handle(msg, handle) == 0;
	ssh_string_free(handle);
	return sent;
}

bool bssh_sftp_handler::do_close(sftp_client_message msg) {
	open_t *open = lookup(msg);
	if (!open) return sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle") == 0;

	int error = 0;
	if (open->fd >= 0 && ::close(open->fd) != 0) error = errno;
	if (open->dir) ::closedir(open->dir);

	sftp_handle_remove(m_sftp, open);
	m_open.remove(open);
	delete open;

	/* A failed close is a failed write that the client is owed news of. */
	return error ? reply_errno(msg, error) : reply_ok(msg);
}

bool bssh_sftp_handler::do_read(sftp_client_message msg) {
	open_t *open = lookup(msg);
	if (!open || open->fd < 0) return sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle") == 0;

	QByteArray buffer(int(qMin<quint32>(msg->len, READ_LIMIT)), Qt::Uninitialized);
	const ssize_t got = ::pread(open->fd, buffer.data(), size_t(buffer.size()), off_t(msg->offset));
	if (got < 0) return reply_errno(msg, errno);
	if (got == 0) return sftp_reply_status(msg, SSH_FX_EOF, nullptr) == 0;

	return sftp_reply_data(msg, buffer.constData(), int(got)) == 0;
}

bool bssh_sftp_handler::do_write(sftp_client_message msg) {
	open_t *open = lookup(msg);
	if (!open || open->fd < 0) return sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle") == 0;
	if (!msg->data) return sftp_reply_status(msg, SSH_FX_BAD_MESSAGE, "No data") == 0;

	const char *data = static_cast<const char *>(ssh_string_data(msg->data));
	size_t left = ssh_string_len(msg->data);
	off_t offset = off_t(msg->offset);

	/* pwrite may write less than asked; the client is told only once all of it is down. */
	while (left > 0) {
		const ssize_t wrote = ::pwrite(open->fd, data, left, offset);
		if (wrote < 0) {
			if (errno == EINTR) continue;
			return reply_errno(msg, errno);
		}
		data += wrote;
		left -= size_t(wrote);
		offset += wrote;
	}
	return reply_ok(msg);
}

bool bssh_sftp_handler::do_opendir(sftp_client_message msg) {
	const bssh_root_path::resolved_t path = m_root.existing(path_of(msg));
	if (path.real.isEmpty()) return reply_errno(msg, path.error);

	DIR *dir = ::opendir(path.real.constData());
	if (!dir) return reply_errno(msg, errno);

	open_t *open = new open_t;
	open->dir = dir;
	open->real = path.real;

	ssh_string handle = sftp_handle_alloc(m_sftp, open);
	if (!handle) {
		::closedir(dir);
		delete open;
		return sftp_reply_status(msg, SSH_FX_FAILURE, "Too many open files") == 0;
	}
	m_open.insert(open);

	const bool sent = sftp_reply_handle(msg, handle) == 0;
	ssh_string_free(handle);
	return sent;
}

bool bssh_sftp_handler::do_readdir(sftp_client_message msg) {
	open_t *open = lookup(msg);
	if (!open || !open->dir) return sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle") == 0;

	int count = 0;
	while (count < READDIR_BATCH) {
		errno = 0;
		const struct dirent *entry = ::readdir(open->dir);
		if (!entry) {
			if (errno != 0) return reply_errno(msg, errno);
			break;
		}

		/*
		 * lstat, so a link is listed as a link and not as what it points
		 * at -- which may be outside the root, and is not the client's to
		 * see. Following it is a separate request, and that one is checked.
		 */
		struct stat st;
		if (::fstatat(dirfd(open->dir), entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;

		struct sftp_attributes_struct attr;
		fill_attributes(attr, st);
		const QByteArray name(entry->d_name);
		if (sftp_reply_names_add(msg, name.constData(), long_name(name, st).constData(), &attr) != 0)
			return false;
		++count;
	}

	if (count == 0) return sftp_reply_status(msg, SSH_FX_EOF, nullptr) == 0;
	return sftp_reply_names(msg) == 0;
}

bool bssh_sftp_handler::do_stat(sftp_client_message msg, bool fstat) {
	struct stat st;

	if (fstat) {
		open_t *open = lookup(msg);
		if (!open) return sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle") == 0;
		const int rc = open->fd >= 0 ? ::fstat(open->fd, &st) : ::stat(open->real.constData(), &st);
		if (rc != 0) return reply_errno(msg, errno);
	} else {
		/*
		 * STAT and LSTAT both answer for the resolved path. Answering LSTAT
		 * truthfully about a link would mean resolving only the parent, and
		 * the one thing a client does with "it is a link" is ask to read
		 * it, which is refused anyway.
		 */
		const bssh_root_path::resolved_t path = m_root.existing(path_of(msg));
		if (path.real.isEmpty()) return reply_errno(msg, path.error);
		if (::stat(path.real.constData(), &st) != 0) return reply_errno(msg, errno);
	}

	struct sftp_attributes_struct attr;
	fill_attributes(attr, st);
	return sftp_reply_attr(msg, &attr) == 0;
}

bool bssh_sftp_handler::do_setstat(sftp_client_message msg, bool fsetstat) {
	int fd = -1;
	QByteArray real;

	if (fsetstat) {
		open_t *open = lookup(msg);
		if (!open) return sftp_reply_status(msg, SSH_FX_INVALID_HANDLE, "Invalid handle") == 0;
		fd = open->fd;
		real = open->real;
	} else {
		const bssh_root_path::resolved_t path = m_root.existing(path_of(msg));
		if (path.real.isEmpty()) return reply_errno(msg, path.error);
		real = path.real;
	}

	const sftp_attributes attr = msg->attr;
	if (!attr) return reply_ok(msg);

	if (attr->flags & SSH_FILEXFER_ATTR_SIZE) {
		const int rc = fd >= 0 ? ::ftruncate(fd, off_t(attr->size))
		                       : ::truncate(real.constData(), off_t(attr->size));
		if (rc != 0) return reply_errno(msg, errno);
	}

	if (attr->flags & SSH_FILEXFER_ATTR_ACMODTIME) {
		struct timespec times[2];
		times[0].tv_sec = time_t(attr->atime);
		times[0].tv_nsec = 0;
		times[1].tv_sec = time_t(attr->mtime);
		times[1].tv_nsec = 0;
		const int rc = fd >= 0 ? ::futimens(fd, times) : ::utimensat(AT_FDCWD, real.constData(), times, 0);
		if (rc != 0) return reply_errno(msg, errno);
	}

	/*
	 * Permissions last, and a refusal ignored. Shared storage on Android
	 * is a FUSE filesystem with no modes to set, and clients that preserve
	 * them (`put -p`, `scp -p`) would otherwise see every upload fail at
	 * the very end, after the data -- which they would then report as a
	 * failed transfer of a file that arrived intact.
	 */
	if (attr->flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
		const mode_t permissions = attr->permissions & 07777;
		if (fd >= 0) (void)::fchmod(fd, permissions);
		else (void)::chmod(real.constData(), permissions);
	}

	return reply_ok(msg);
}

bool bssh_sftp_handler::do_mkdir(sftp_client_message msg) {
	const bssh_root_path::resolved_t path = m_root.leaf(path_of(msg));
	if (path.real.isEmpty()) return reply_errno(msg, path.error);

	mode_t permissions = 0755;
	if (msg->attr && (msg->attr->flags & SSH_FILEXFER_ATTR_PERMISSIONS))
		permissions = msg->attr->permissions & 07777;

	if (::mkdir(path.real.constData(), permissions) != 0) return reply_errno(msg, errno);
	return reply_ok(msg);
}

bool bssh_sftp_handler::do_remove(sftp_client_message msg, bool directory) {
	const bssh_root_path::resolved_t path = m_root.leaf(path_of(msg));
	if (path.real.isEmpty()) return reply_errno(msg, path.error);

	const int rc = directory ? ::rmdir(path.real.constData()) : ::unlink(path.real.constData());
	if (rc != 0) return reply_errno(msg, errno);
	return reply_ok(msg);
}

bool bssh_sftp_handler::do_rename(sftp_client_message msg, bool overwrite) {
	const char *target_name = sftp_client_message_get_data(msg);
	if (!target_name) return sftp_reply_status(msg, SSH_FX_BAD_MESSAGE, "No target") == 0;

	const bssh_root_path::resolved_t from = m_root.leaf(path_of(msg));
	if (from.real.isEmpty()) return reply_errno(msg, from.error);
	const bssh_root_path::resolved_t to = m_root.leaf(QByteArray(target_name));
	if (to.real.isEmpty()) return reply_errno(msg, to.error);

	/*
	 * Plain SFTP rename refuses an existing target; posix-rename@openssh.com
	 * replaces it, which is what OpenSSH's client asks for when it can. The
	 * check and the rename are two steps, so a target appearing between
	 * them is replaced -- the same gap OpenSSH's own server leaves where
	 * link(2) is unavailable, and a race only against the client itself.
	 */
	struct stat st;
	if (!overwrite && ::lstat(to.real.constData(), &st) == 0)
		return sftp_reply_status(msg, SSH_FX_FAILURE, "Target exists") == 0;

	if (::rename(from.real.constData(), to.real.constData()) != 0) return reply_errno(msg, errno);
	return reply_ok(msg);
}

bool bssh_sftp_handler::do_realpath(sftp_client_message msg) {
	const bssh_root_path::resolved_t path = m_root.existing(path_of(msg));
	if (path.real.isEmpty()) return reply_errno(msg, path.error);
	return sftp_reply_name(msg, m_root.client_view(path.real).constData(), nullptr) == 0;
}

bool bssh_sftp_handler::do_extended(sftp_client_message msg) {
	const char *request = sftp_client_message_get_submessage(msg);

	if (request && std::strcmp(request, "posix-rename@openssh.com") == 0)
		return do_rename(msg, true);

	/*
	 * libssh's version reply advertises hardlink@openssh.com and
	 * statvfs@openssh.com as well, and there is no public way to send a
	 * different one. Both are refused here: a hard link is the link this
	 * server does not make, and statvfs only feeds `df`.
	 */
	return sftp_reply_status(msg, SSH_FX_OP_UNSUPPORTED, "Not supported") == 0;
}
