#ifndef BSSH_PICTURE_SPILL_H
#define BSSH_PICTURE_SPILL_H

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QString>

#include <functional>
#include <memory>

class QLockFile;
class QTemporaryDir;

/*
 * bssh_picture_spill -- where a terminal's pictures go once they have
 * scrolled back past what it keeps in memory (project.md sec 7.1).
 *
 * The holder's direction of 2026-09-26: bound what is RESIDENT, not what
 * is remembered, loading a scrolled-off picture back when it is scrolled
 * to. So a picture over the budget is written out here instead of being
 * dropped, and read back when the view reaches it.
 *
 * One file per picture, compressed and then sealed with AES-256-GCM
 * under a key that exists only in this object's memory. That is what
 * lets it be on by default where kept scrollback had to be opt-in (sec
 * 10.5): a picture is screen content like any other, and a file of it
 * left behind by a crash is noise without the key, which died with the
 * process. The picture's serial is the GCM nonce and its additional
 * data, so a file renamed onto another serial does not decrypt either.
 *
 * The directory is created on the first spill -- most sessions never
 * draw a picture at all -- and removed with this object. It holds a lock
 * file for its whole life, which is how sweep_abandoned() tells a
 * directory a crashed process left from one a running process owns.
 */
class bssh_picture_spill {
public:
	/* `base` is where the per-terminal directories go. */
	explicit bssh_picture_spill(const QString &base);
	~bssh_picture_spill();

	bool write(quint64 serial, const QImage &image);

	/*
	 * write() in two halves, so the slow one can leave the thread that is
	 * scrolling: prepare() gives back a job that compresses and seals the
	 * picture -- a pure function of copies, safe to run on any thread --
	 * and store() writes what it produced, on this object's thread. Null
	 * when nothing could be spilled at all.
	 */
	std::function<QByteArray()> prepare(quint64 serial, const QImage &image);
	bool store(quint64 serial, const QByteArray &sealed);

	/* A null image when the file is gone or does not verify. */
	QImage read(quint64 serial) const;

	void remove(quint64 serial);

	/* Bytes on disk, summed over what this holds. */
	qint64 bytes() const { return m_bytes; }
	bool contains(quint64 serial) const { return m_sizes.contains(serial); }

	/* Empty until the first write has made it. */
	QString directory() const;

	/*
	 * Removes the spill directories under `base` whose owner is gone --
	 * a lock file present and provably stale. One with no lock file, or
	 * a live owner, is left alone: it may be another instance's, made a
	 * moment ago. Returns how many it removed.
	 */
	static int sweep_abandoned(const QString &base);

	/* AppCacheLocation's "pictures", where the app's spills go. */
	static QString default_base();

private:
	bool ensure_key();
	bool ensure_directory();
	QString path_of(quint64 serial) const;

	QString m_base;
	QByteArray m_key;
	std::unique_ptr<QTemporaryDir> m_dir;
	std::unique_ptr<QLockFile> m_lock;
	QHash<quint64, qint64> m_sizes;
	qint64 m_bytes = 0;
};

#endif /* BSSH_PICTURE_SPILL_H */
