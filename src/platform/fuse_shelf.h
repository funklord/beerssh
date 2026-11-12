#ifndef BSSH_FUSE_SHELF_H
#define BSSH_FUSE_SHELF_H

#include <QString>

#include <memory>

class bssh_piece_map;

/*
 * The drag-out's fallback (project.md sec 6.4, the holder's decision of
 * 2026-11-12): files being copied here from a host, shown as ordinary
 * read-only files in a FUSE mount of this process's own, so a file
 * manager that speaks neither XDS nor Ark's protocol can be dropped a
 * file:// path and copy from it while the bytes are still arriving. Its
 * reads of what has not arrived wait -- the file manager's, never ours --
 * and are answered as the copy grows; the copy's progress is the
 * panel's copy-back row.
 *
 * It speaks the kernel's FUSE protocol itself (linux/fuse.h) and mounts
 * through fusermount3 as libfuse does, with no library to link: read-only,
 * INIT, LOOKUP, GETATTR, OPEN, READ, OPENDIR, READDIR, RELEASE(DIR),
 * FORGET, INTERRUPT, STATFS, DESTROY; anything else is ENOSYS. It serves
 * on a thread of its own, so nothing in this process waits on it and
 * nothing it serves can wait on this process's window.
 *
 * Linux only, and only where /dev/fuse and fusermount3 are there; where
 * they are not, available() says so and the drag offers what else it has.
 */
namespace bssh_fuse_shelf {

/* Mounted, or mountable now; false where FUSE cannot be had. */
bool available();

/*
 * Shows `staging` -- a file being written by a copy that will reach
 * `size` bytes -- as `name` in a directory of its own on the mount, and
 * returns that path; empty where it cannot. Reads past what has been
 * written wait for it. With `pieces`, a copy filled out of order: a read
 * waits for its own pieces only, and asks the copy for them first.
 */
QString add(const QString &name, const QString &staging, qint64 size,
            std::shared_ptr<bssh_piece_map> pieces = nullptr);

/* The copy behind `path` ended: whole, or not (its waiting reads fail). */
void finish(const QString &path, bool whole);

/* Taken off the mount; a read still waiting fails. */
void remove(const QString &path);

/* Where the mount is, for a test or a cleanup; empty until mounted. */
QString mount_point();

/* Unmounts, as the process ends. */
void shut_down();

}

#endif
