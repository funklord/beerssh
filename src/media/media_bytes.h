#ifndef BSSH_MEDIA_BYTES_H
#define BSSH_MEDIA_BYTES_H

#include <QMutex>
#include <QString>
#include <QWaitCondition>

#include <cstdint>
#include <memory>

class bssh_piece_map;

/*
 * A file a copy back is still writing, read at an offset without waiting
 * (project.md sec 6.4, *Playing a file*): what a player reads while a
 * remote file arrives. Its four calls are snurra_bytes' (snurra's
 * include/snurra/source.h) in BeerSSH's own type, so the default build
 * needs nothing of snurra's and the media build's adapter is a forward.
 *
 * What has arrived is what the staging file holds on disk, as the FUSE
 * shelf reads it: a copy back writes a piece at a time and never goes
 * back, so every byte below the file's size is the host's. The copy says
 * when it has ended, whole or not; past that, a read short of the size
 * is the end rather than a stall.
 *
 * Read from several threads at once -- snurra reads a file's sound,
 * waveform, picture and beats each on its own -- while the window's
 * thread reports the copy's progress and end: each read opens the file
 * for itself, and the rest is under a lock.
 */
class bssh_media_bytes {
public:
	/* `staging` is the file the copy writes; `size` what it will reach. */
	bssh_media_bytes(const QString &staging, qint64 size);
	/*
	 * A file filled out of order: what has arrived is what `pieces` says,
	 * and want() is passed on, so the copy fetches where this reader goes
	 * -- a seek ahead gets its bytes without waiting for those before.
	 */
	explicit bssh_media_bytes(std::shared_ptr<bssh_piece_map> pieces);

	qint64 size() const { return m_size; }

	/*
	 * Up to `n` bytes at `offset` into `out`: how many are here, 0 when
	 * none are yet (a stall), -1 at or past the end, or once the copy has
	 * ended short of `offset`.
	 */
	qint64 read(qint64 offset, uint8_t *out, qint64 n);

	/* Whether all of [offset, offset + n) is here. */
	bool has(qint64 offset, qint64 n) const;

	/*
	 * The reader goes on from `offset`. The copy is in order, so this
	 * only records it -- a range fetch that follows the playhead is the
	 * later step (sec 6.4) and would read it here.
	 */
	void want(qint64 offset);
	qint64 wanted() const;

	/* Waits up to `ms` for more to arrive, or for the copy to end. */
	void wait(int ms);

	/* The window's side: more has been written, or the copy has ended. */
	void arrived();
	void finished(bool whole);
	bool is_finished() const;

private:
	qint64 on_disk() const;

	const QString m_staging;
	const qint64 m_size;
	const std::shared_ptr<bssh_piece_map> m_pieces;
	mutable QMutex m_lock;
	QWaitCondition m_more;
	bool m_finished = false;
	bool m_whole = false;
	qint64 m_wanted = 0;
};

#endif
