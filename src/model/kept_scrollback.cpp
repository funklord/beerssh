#include "model/kept_scrollback.h"

#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStandardPaths>

#include "term/scrollback.h"

namespace {

/* "BSSH scrollback", and the format it was written in. */
const quint32 BSSH_KEPT_MAGIC = 0x42534842;
const quint16 BSSH_KEPT_VERSION = 1;

QString kept_directory(const QString &directory) {
	const QString base = directory.isEmpty()
	        ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
	        : directory;
	return base + QStringLiteral("/scrollback");
}

void write_color(QDataStream &out, const bssh_color_t &color) {
	out << quint8(color.kind) << color.index << quint32(color.rgb);
}

bool read_color(QDataStream &in, bssh_color_t &color) {
	quint8 kind = 0;
	quint32 rgb = 0;
	in >> kind >> color.index >> rgb;
	if (kind > bssh_color_t::RGB) return false;
	color.kind = bssh_color_t::kind_t(kind);
	color.rgb = QRgb(rgb);
	return true;
}

} /* namespace */

QByteArray bssh_scrollback_encode(const bssh_scrollback &history) {
	QByteArray raw;
	QDataStream out(&raw, QIODevice::WriteOnly);
	out.setVersion(QDataStream::Qt_6_0);

	out << quint32(history.count());
	for (int i = 0; i < history.count(); ++i) {
		const QList<bssh_cell_t> &line = history.line(i);
		out << history.is_continuation(i) << quint32(line.size());
		for (const bssh_cell_t &cell : line) {
			out << quint32(cell.ch);
			write_color(out, cell.fg);
			write_color(out, cell.bg);
			out << cell.attrs << cell.width;
		}
	}

	QByteArray file;
	QDataStream head(&file, QIODevice::WriteOnly);
	head.setVersion(QDataStream::Qt_6_0);
	head << BSSH_KEPT_MAGIC << BSSH_KEPT_VERSION << qCompress(raw);
	return file;
}

bool bssh_scrollback_decode(const QByteArray &bytes, bssh_scrollback *into) {
	if (!into) return false;

	QDataStream head(bytes);
	head.setVersion(QDataStream::Qt_6_0);
	quint32 magic = 0;
	quint16 version = 0;
	QByteArray packed;
	head >> magic >> version >> packed;
	if (head.status() != QDataStream::Ok || magic != BSSH_KEPT_MAGIC
	    || version != BSSH_KEPT_VERSION)
		return false;

	const QByteArray raw = qUncompress(packed);
	if (raw.isEmpty() && !packed.isEmpty()) return false;

	QDataStream in(raw);
	in.setVersion(QDataStream::Qt_6_0);

	quint32 lines = 0;
	in >> lines;

	/*
	 * Read whole before anything is kept, so a file cut short or damaged
	 * partway restores nothing rather than half a history.
	 */
	QList<QList<bssh_cell_t>> read;
	QList<bool> continued;
	for (quint32 i = 0; i < lines; ++i) {
		bool continuation = false;
		quint32 cells = 0;
		in >> continuation >> cells;
		if (in.status() != QDataStream::Ok || cells > 65536) return false;

		QList<bssh_cell_t> line;
		line.reserve(int(cells));
		for (quint32 c = 0; c < cells; ++c) {
			bssh_cell_t cell;
			quint32 ch = 0;
			in >> ch;
			if (!read_color(in, cell.fg) || !read_color(in, cell.bg)) return false;
			in >> cell.attrs >> cell.width;
			cell.ch = char32_t(ch);
			line.append(cell);
		}
		if (in.status() != QDataStream::Ok) return false;

		read.append(line);
		continued.append(continuation);
	}

	for (int i = 0; i < read.size(); ++i) into->push(read.at(i), continued.at(i));
	return true;
}

QString bssh_kept_scrollback_path(int profile_id, const QString &directory) {
	return kept_directory(directory) + QStringLiteral("/%1.bin").arg(profile_id);
}

bool bssh_save_kept_scrollback(int profile_id, const bssh_scrollback &history,
                               const QString &directory) {
	if (profile_id == 0) return false;

	const QString dir = kept_directory(directory);
	if (!QDir().mkpath(dir)) return false;
	QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner
	                            | QFileDevice::ExeOwner);

	QSaveFile file(bssh_kept_scrollback_path(profile_id, directory));
	if (!file.open(QIODevice::WriteOnly)) return false;

	/*
	 * Owner-only before a byte is written: this is whatever was on the
	 * screen, and a moment of a world-readable file is a moment too many.
	 */
	file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);

	file.write(bssh_scrollback_encode(history));

	/*
	 * No second setPermissions on the renamed file. QSaveFile renames its
	 * temporary into place and the mode goes with it -- measured: with
	 * the call above removed the file came out group-writable, and with
	 * it in the file is its owner's alone. A second call after the commit
	 * made each of the two untestable, since either alone passed.
	 */
	return file.commit();
}

bool bssh_load_kept_scrollback(int profile_id, bssh_scrollback *into,
                               const QString &directory) {
	if (profile_id == 0) return false;

	QFile file(bssh_kept_scrollback_path(profile_id, directory));
	if (!file.open(QIODevice::ReadOnly)) return false;
	return bssh_scrollback_decode(file.readAll(), into);
}

void bssh_erase_kept_scrollback(int profile_id, const QString &directory) {
	if (profile_id == 0) return;
	QFile::remove(bssh_kept_scrollback_path(profile_id, directory));
}
