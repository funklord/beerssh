#ifndef BSSH_ZIP_INDEX_H
#define BSSH_ZIP_INDEX_H

#include <QByteArray>
#include <QList>
#include <QString>

/*
 * Just enough of the zip format to take single files out of an archive
 * without having all of it (project.md sec 9.4): a font family's release
 * archive can be 165 MB for the four files a phone wants, and a zip keeps
 * its index at the END, so the index can be read by itself and then each
 * wanted member fetched by its own byte range.
 *
 * Stored and deflated members only -- the two every font release uses --
 * and no zip64: an archive past 4 GB or 65535 entries is refused with a
 * reason rather than misread. Nothing here does any I/O; it is handed
 * bytes and says what they mean, which is what makes it testable without
 * a network.
 */

struct bssh_zip_entry_t {
	QString name;
	quint16 method = 0;             /* 0 stored, 8 deflated */
	quint32 crc = 0;
	quint32 compressed_size = 0;
	quint32 size = 0;
	quint32 local_header_offset = 0;
};

/*
 * How many bytes from the end of an archive to fetch to be sure of holding
 * its end-of-central-directory record: the record is 22 bytes plus a
 * comment of at most 65535.
 */
const qint64 BSSH_ZIP_TAIL_BYTES = 22 + 65535;

/*
 * Where the central directory is, read from the archive's last bytes.
 * `tail` is the final bytes of an archive `archive_size` long. False, with
 * `error` set, when there is no end record in them or it describes zip64.
 */
bool bssh_zip_find_directory(const QByteArray &tail, qint64 archive_size,
                             qint64 *offset, qint64 *length, QString *error);

/* The entries of a central directory, as fetched by the range above. */
bool bssh_zip_read_directory(const QByteArray &directory, QList<bssh_zip_entry_t> *entries,
                             QString *error);

/*
 * How many bytes to fetch from `entry.local_header_offset` to be sure of
 * the whole member: its local header (whose name and extra field need not
 * match the central directory's) and then its data.
 */
qint64 bssh_zip_member_span(const bssh_zip_entry_t &entry);

/*
 * The member's contents from bytes that begin at its local header:
 * inflated if it was deflated, and checked against the directory's CRC and
 * size, so a range that came back short or wrong is refused rather than
 * written out as a font.
 */
bool bssh_zip_extract(const bssh_zip_entry_t &entry, const QByteArray &from_local_header,
                      QByteArray *contents, QString *error);

#endif /* BSSH_ZIP_INDEX_H */
