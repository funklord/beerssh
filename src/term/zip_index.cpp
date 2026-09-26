#include "term/zip_index.h"

#include <zlib.h>

namespace {

quint16 u16(const QByteArray &bytes, qint64 at) {
	return quint16(quint8(bytes.at(at))) | quint16(quint8(bytes.at(at + 1))) << 8;
}

quint32 u32(const QByteArray &bytes, qint64 at) {
	return quint32(u16(bytes, at)) | quint32(u16(bytes, at + 2)) << 16;
}

const quint32 END_SIGNATURE = 0x06054b50;
const quint32 ENTRY_SIGNATURE = 0x02014b50;
const quint32 LOCAL_SIGNATURE = 0x04034b50;

} /* namespace */

bool bssh_zip_find_directory(const QByteArray &tail, qint64 archive_size,
                             qint64 *offset, qint64 *length, QString *error) {
	/*
	 * Backwards from the end, because the record is followed only by its
	 * comment -- and the comment is checked against the space left, so a
	 * signature that happens to appear inside a comment is not believed.
	 */
	for (qint64 at = tail.size() - 22; at >= 0; --at) {
		if (u32(tail, at) != END_SIGNATURE) continue;
		const quint16 comment = u16(tail, at + 20);
		if (at + 22 + comment != tail.size()) continue;

		const quint16 entries = u16(tail, at + 10);
		const quint32 size = u32(tail, at + 12);
		const quint32 start = u32(tail, at + 16);
		if (entries == 0xffff || size == 0xffffffff || start == 0xffffffff) {
			if (error) *error = QStringLiteral("the archive is zip64, which is not read here");
			return false;
		}
		if (qint64(start) + size > archive_size) {
			if (error) *error = QStringLiteral("the archive's directory lies past its end");
			return false;
		}
		*offset = start;
		*length = size;
		return true;
	}

	if (error) *error = QStringLiteral("no end of the zip directory in the archive's last bytes");
	return false;
}

bool bssh_zip_read_directory(const QByteArray &directory, QList<bssh_zip_entry_t> *entries,
                             QString *error) {
	qint64 at = 0;
	while (at + 46 <= directory.size()) {
		if (u32(directory, at) != ENTRY_SIGNATURE) {
			if (error) *error = QStringLiteral("a damaged entry in the zip directory");
			return false;
		}

		bssh_zip_entry_t entry;
		entry.method = u16(directory, at + 10);
		entry.crc = u32(directory, at + 16);
		entry.compressed_size = u32(directory, at + 20);
		entry.size = u32(directory, at + 24);
		const quint16 name = u16(directory, at + 28);
		const quint16 extra = u16(directory, at + 30);
		const quint16 comment = u16(directory, at + 32);
		entry.local_header_offset = u32(directory, at + 42);

		if (at + 46 + name > directory.size()) {
			if (error) *error = QStringLiteral("an entry name runs past the zip directory");
			return false;
		}
		entry.name = QString::fromUtf8(directory.mid(at + 46, name));
		entries->append(entry);

		at += 46 + name + extra + comment;
	}
	return true;
}

qint64 bssh_zip_member_span(const bssh_zip_entry_t &entry) {
	/* Local header, the longest name and extra field it could carry, the data. */
	return 30 + 65535 + 65535 + qint64(entry.compressed_size);
}

bool bssh_zip_extract(const bssh_zip_entry_t &entry, const QByteArray &from_local_header,
                      QByteArray *contents, QString *error) {
	const QByteArray &bytes = from_local_header;
	if (bytes.size() < 30 || u32(bytes, 0) != LOCAL_SIGNATURE) {
		if (error) *error = QStringLiteral("%1 is not where the directory says").arg(entry.name);
		return false;
	}

	const qint64 data = 30 + u16(bytes, 26) + u16(bytes, 28);
	if (data + entry.compressed_size > bytes.size()) {
		if (error) *error = QStringLiteral("%1 came back short").arg(entry.name);
		return false;
	}
	const QByteArray packed = bytes.mid(data, entry.compressed_size);

	QByteArray out;
	if (entry.method == 0) {
		out = packed;
	} else if (entry.method == 8) {
		out.resize(entry.size);

		z_stream stream = {};
		stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(packed.constData()));
		stream.avail_in = uInt(packed.size());
		stream.next_out = reinterpret_cast<Bytef *>(out.data());
		stream.avail_out = uInt(out.size());

		/* -15: raw deflate, which is what a zip member holds -- no zlib header. */
		if (inflateInit2(&stream, -15) != Z_OK) {
			if (error) *error = QStringLiteral("could not start inflating %1").arg(entry.name);
			return false;
		}
		const int result = inflate(&stream, Z_FINISH);
		const uLong produced = stream.total_out;
		inflateEnd(&stream);
		if (result != Z_STREAM_END || produced != entry.size) {
			if (error) *error = QStringLiteral("%1 did not inflate").arg(entry.name);
			return false;
		}
	} else {
		if (error) *error = QStringLiteral("%1 is compressed in a way not read here").arg(entry.name);
		return false;
	}

	if (quint32(out.size()) != entry.size
	    || crc32(0L, reinterpret_cast<const Bytef *>(out.constData()), uInt(out.size())) != entry.crc) {
		if (error) *error = QStringLiteral("%1 does not match its checksum in the archive").arg(entry.name);
		return false;
	}

	*contents = out;
	return true;
}
