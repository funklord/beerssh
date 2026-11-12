#ifndef BSSH_PIECE_MAP_H
#define BSSH_PIECE_MAP_H

#include <QMutex>
#include <QString>
#include <QWaitCondition>

#include <vector>

/*
 * A file a copy is filling out of order (project.md sec 6.4, *Playing a
 * file*): which fixed-size pieces of it are on disk, and where its reader
 * wants to read next. The copy fetches the piece the reader wants first,
 * then on from there, then goes back for what it skipped -- so a seek in a
 * player, or a read of the FUSE mount, gets its bytes without waiting for
 * everything before them.
 *
 * Shared between the SSH worker, which marks pieces as it writes them,
 * and readers on other threads: a player's (bssh_media_bytes) and the
 * mount's. A transport that can only copy in order marks the map so, and
 * then what has arrived is the staging file's size, as before.
 */
class bssh_piece_map {
public:
	static constexpr qint64 PIECE = 64 * 1024;

	/* `staging` is the file being filled; `size` what it will reach. */
	bssh_piece_map(const QString &staging, qint64 size, qint64 piece = PIECE);

	QString staging() const { return m_staging; }
	qint64 size() const { return m_size; }
	qint64 piece() const { return m_piece; }

	/* Whether all of [offset, offset + n) is on disk. */
	bool has(qint64 offset, qint64 n) const;
	/* How many bytes from `offset` on are on disk without a gap. */
	qint64 run_from(qint64 offset) const;

	/* The reader goes on from `offset`: the copy fetches there next. */
	void want(qint64 offset);
	qint64 wanted() const;

	/* Waits up to `ms` for a piece to arrive or the copy to end. */
	void wait(int ms);

	/* ---- the copy's side ---- */

	/* The piece to fetch next: the first missing at or after the one
	 * wanted, else the first missing at all; -1 once none is. */
	qint64 next_missing() const;
	/* Piece `index` is on disk. */
	void mark(qint64 index);
	/* This copy goes in order: what has arrived is the file's size. */
	void set_in_order();
	bool in_order() const;
	/* The copy has ended, whole or not: waits return. */
	void finish();
	bool complete() const;

private:
	qint64 on_disk() const;
	qint64 pieces() const { return (m_size + m_piece - 1) / m_piece; }

	const QString m_staging;
	const qint64 m_size;
	const qint64 m_piece;
	mutable QMutex m_lock;
	QWaitCondition m_more;
	std::vector<bool> m_have;
	qint64 m_count = 0;
	qint64 m_wanted = 0;
	bool m_in_order = false;
	bool m_finished = false;
};

#endif
