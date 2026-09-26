#include <QtTest>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QRandomGenerator>

#include <zlib.h>

#include "font_download_test.h"
#include "model/font_download.h"
#include "model/font_sources.h"
#include "term/zip_index.h"

namespace {

void put16(QByteArray &out, quint16 value) {
	out.append(char(value & 0xff));
	out.append(char(value >> 8));
}

void put32(QByteArray &out, quint32 value) {
	put16(out, quint16(value & 0xffff));
	put16(out, quint16(value >> 16));
}

QByteArray raw_deflate(const QByteArray &data) {
	QByteArray out(int(compressBound(uLong(data.size()))) + 64, '\0');
	z_stream stream = {};
	deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
	stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(data.constData()));
	stream.avail_in = uInt(data.size());
	stream.next_out = reinterpret_cast<Bytef *>(out.data());
	stream.avail_out = uInt(out.size());
	deflate(&stream, Z_FINISH);
	out.resize(int(stream.total_out));
	deflateEnd(&stream);
	return out;
}

/*
 * A zip, written by hand, so the reader is tested against the format and
 * not against itself. Each file is stored or deflated as asked; the local
 * headers carry an extra field the central directory does not, which real
 * archives do and which a reader trusting the directory's lengths misses.
 */
QByteArray make_zip(const QList<QPair<QString, QByteArray>> &files, bool deflated,
                    const QByteArray &comment = QByteArray()) {
	QByteArray zip, directory;
	for (const QPair<QString, QByteArray> &file : files) {
		const QByteArray name = file.first.toUtf8();
		const QByteArray packed = deflated ? raw_deflate(file.second) : file.second;
		const quint32 crc = quint32(crc32(0L, reinterpret_cast<const Bytef *>(file.second.constData()),
		                                  uInt(file.second.size())));
		const quint32 offset = quint32(zip.size());
		const QByteArray extra("\x55\x54\x05\x00\x01\x00\x00\x00\x00", 9);

		put32(zip, 0x04034b50); put16(zip, 20); put16(zip, 0); put16(zip, deflated ? 8 : 0);
		put16(zip, 0); put16(zip, 0); put32(zip, crc);
		put32(zip, quint32(packed.size())); put32(zip, quint32(file.second.size()));
		put16(zip, quint16(name.size())); put16(zip, quint16(extra.size()));
		zip += name; zip += extra; zip += packed;

		put32(directory, 0x02014b50); put16(directory, 20); put16(directory, 20);
		put16(directory, 0); put16(directory, deflated ? 8 : 0); put16(directory, 0); put16(directory, 0);
		put32(directory, crc); put32(directory, quint32(packed.size()));
		put32(directory, quint32(file.second.size()));
		put16(directory, quint16(name.size())); put16(directory, 0); put16(directory, 0);
		put16(directory, 0); put16(directory, 0); put32(directory, 0); put32(directory, offset);
		directory += name;
	}
	const quint32 start = quint32(zip.size());
	zip += directory;
	put32(zip, 0x06054b50); put16(zip, 0); put16(zip, 0);
	put16(zip, quint16(files.size())); put16(zip, quint16(files.size()));
	put32(zip, quint32(directory.size())); put32(zip, start);
	put16(zip, quint16(comment.size()));
	zip += comment;
	return zip;
}

QString sha(const QByteArray &bytes) {
	return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

/* Bytes by range from buffers, counting what was asked for. */
class buffer_source : public bssh_range_source {
public:
	QHash<QString, QByteArray> files;
	qint64 fetched = 0;

	void size(const QUrl &url, std::function<void(qint64, QString)> done) override {
		if (!files.contains(url.toString())) { done(-1, QStringLiteral("404")); return; }
		done(files.value(url.toString()).size(), QString());
	}
	void fetch(const QUrl &url, qint64 from, qint64 length,
	           std::function<void(QByteArray, QString)> done) override {
		if (!files.contains(url.toString())) { done(QByteArray(), QStringLiteral("404")); return; }
		const QByteArray whole = files.value(url.toString());
		const QByteArray part = length < 0 ? whole : whole.mid(from, length);
		fetched += part.size();
		done(part, QString());
	}
};

} /* namespace */

void font_download_test::a_zip_member_comes_out_whole_stored_or_deflated() {
	const QByteArray font = QByteArray("a font's bytes, ") .repeated(4000);
	const QByteArray licence("SIL Open Font License");

	QList<QPair<QString, QByteArray>> files;
	files.append({QStringLiteral("ttf/Some-Regular.ttf"), font});
	files.append({QStringLiteral("OFL.txt"), licence});

	for (const bool deflated : {false, true}) {
		const QByteArray zip = make_zip(files, deflated, QByteArray("a comment at the end"));

		qint64 offset = 0, length = 0;
		QString error;
		const QByteArray tail = zip.right(int(qMin<qint64>(zip.size(), BSSH_ZIP_TAIL_BYTES)));
		QVERIFY2(bssh_zip_find_directory(tail, zip.size(), &offset, &length, &error), qPrintable(error));

		QList<bssh_zip_entry_t> entries;
		QVERIFY2(bssh_zip_read_directory(zip.mid(offset, length), &entries, &error), qPrintable(error));
		QCOMPARE(entries.size(), 2);
		QCOMPARE(entries.at(0).name, QStringLiteral("ttf/Some-Regular.ttf"));
		QCOMPARE(int(entries.at(0).method), deflated ? 8 : 0);

		/* From the local header on, as a range request would return it. */
		QByteArray out;
		const bssh_zip_entry_t &entry = entries.at(0);
		QVERIFY2(bssh_zip_extract(entry, zip.mid(entry.local_header_offset,
		                                         int(bssh_zip_member_span(entry))), &out, &error),
		          qPrintable(error));
		QCOMPARE(out, font);
	}
}

void font_download_test::a_damaged_or_foreign_zip_is_refused() {
	/*
	 * Stored, not deflated: a byte flipped inside a deflate stream usually
	 * breaks the inflate before the checksum is reached, and it is the
	 * checksum this is about.
	 */
	const QByteArray font = QByteArray("glyphs ").repeated(500);
	QByteArray zip = make_zip({{QStringLiteral("A.ttf"), font}}, false);

	qint64 offset = 0, length = 0;
	QString error;
	QVERIFY(bssh_zip_find_directory(zip, zip.size(), &offset, &length, &error));
	QList<bssh_zip_entry_t> entries;
	QVERIFY(bssh_zip_read_directory(zip.mid(offset, length), &entries, &error));
	const bssh_zip_entry_t entry = entries.first();

	QByteArray out;

	/* A byte flipped in the data: refused by the checksum, not written as a font. */
	QByteArray flipped = zip;
	flipped[int(entry.local_header_offset) + 30 + 5 + 9 + 3] ^= 0x01;
	QVERIFY(!bssh_zip_extract(entry, flipped.mid(entry.local_header_offset), &out, &error));

	/* A range that came back short. */
	QVERIFY(!bssh_zip_extract(entry, zip.mid(entry.local_header_offset, 40), &out, &error));

	/* Not a zip at all, and one that says it is zip64. */
	QVERIFY(!bssh_zip_find_directory(QByteArray(200, 'x'), 200, &offset, &length, &error));
	QByteArray zip64 = zip;
	zip64[zip64.size() - 12] = char(0xff); zip64[zip64.size() - 11] = char(0xff);
	zip64[zip64.size() - 10] = char(0xff); zip64[zip64.size() - 9] = char(0xff);
	QVERIFY(!bssh_zip_find_directory(zip64, zip64.size(), &offset, &length, &error));
	QVERIFY2(error.contains(QStringLiteral("zip64")), qPrintable(error));
}

void font_download_test::the_pins_built_in_are_the_whole_set() {
	const bssh_font_sources &sources = bssh_font_sources::embedded();
	QVERIFY2(sources.archives().size() >= 8, "the pins file did not reach the app");

	/*
	 * Every output and every licence has its own pin -- a file the phone
	 * cannot check is a file it will refuse, which reads to a user as a
	 * download that is broken.
	 */
	for (const bssh_font_archive_t &archive : sources.archives()) {
		for (const auto &member : archive.members) {
			QVERIFY2(sources.pin(member.second).size() == 64,
			          qPrintable(archive.id + QStringLiteral(": no pin for ") + member.second));
		}
		QVERIFY2(sources.pin(QStringLiteral("licences/") + archive.licence_file).size() == 64,
		          qPrintable(archive.id + QStringLiteral(": no pin for its licence")));
	}

	bssh_font_sources junk;
	QVERIFY(!junk.parse(QStringLiteral("archive|too|few\n")));
	QVERIFY(!junk.parse(QStringLiteral("something|else|entirely\n")));
}

void font_download_test::a_family_downloads_file_by_file_and_is_checked() {
	const QByteArray regular = QByteArray("regular ").repeated(20000);
	const QByteArray bold = QByteArray("bold ").repeated(30000);
	const QByteArray licence("OFL 1.1");
	/*
	 * Incompressible, and large beside the tail and header allowances the
	 * download fetches around each file -- so "fetched part of it" means
	 * something. A padding of repeated text deflated to nothing, and the
	 * whole archive then fitted inside those allowances.
	 */
	QByteArray padding(2 * 1024 * 1024, Qt::Uninitialized);
	QRandomGenerator(7).fillRange(reinterpret_cast<quint32 *>(padding.data()),
	                              padding.size() / 4);

	buffer_source source;
	const QString url = QStringLiteral("https://example.invalid/Family-1.0.zip");
	source.files.insert(url, make_zip({{QStringLiteral("ttf/Family-Regular.ttf"), regular},
	                                   {QStringLiteral("ttf/Family-Light.ttf"), padding},
	                                   {QStringLiteral("ttf/Family-Bold.ttf"), bold},
	                                   {QStringLiteral("OFL.txt"), licence}}, true));

	bssh_font_sources pins;
	QVERIFY(pins.parse(QStringLiteral(
	        "archive|family|%1|unused|archive:OFL.txt|OFL-Family.txt|"
	        "ttf/Family-Regular.ttf>Family-Regular.ttf ttf/Family-Bold.ttf>Family-Bold.ttf\n"
	        "file|Family-Regular.ttf|%2\nfile|Family-Bold.ttf|%3\nfile|licences/OFL-Family.txt|%4\n")
	        .arg(url, sha(regular), sha(bold), sha(licence))));

	QTemporaryDir dir;
	bssh_font_download download(&source);
	QSignalSpy finished(&download, &bssh_font_download::finished);
	download.start(*pins.archive(QStringLiteral("family")), pins, dir.path());

	QCOMPARE(finished.count(), 1);
	QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));

	QFile got(dir.filePath(QStringLiteral("Family-Bold.ttf")));
	QVERIFY(got.open(QIODevice::ReadOnly));
	QCOMPARE(got.readAll(), bold);
	QVERIFY(QFile::exists(dir.filePath(QStringLiteral("Family-Regular.ttf"))));
	QVERIFY(QFile::exists(dir.filePath(QStringLiteral("licences/OFL-Family.txt"))));
	QVERIFY(QDir(dir.path()).entryList({QStringLiteral("*.part")}, QDir::Files).isEmpty());

	/* By range: the weight nobody asked for was not fetched. */
	QVERIFY2(source.fetched < source.files.value(url).size() / 2,
	          qPrintable(QStringLiteral("fetched %1 of %2 bytes for part of it")
	                     .arg(source.fetched).arg(source.files.value(url).size())));
}

void font_download_test::a_file_that_is_not_the_pinned_one_leaves_nothing() {
	const QByteArray regular = QByteArray("regular ").repeated(2000);
	const QByteArray bold = QByteArray("bold ").repeated(2000);

	buffer_source source;
	const QString url = QStringLiteral("https://example.invalid/Family-1.0.zip");
	source.files.insert(url, make_zip({{QStringLiteral("Family-Regular.ttf"), regular},
	                                   {QStringLiteral("Family-Bold.ttf"), bold},
	                                   {QStringLiteral("OFL.txt"), QByteArray("OFL")}}, false));

	/* The bold's pin is for something else: a release changed under its tag. */
	bssh_font_sources pins;
	QVERIFY(pins.parse(QStringLiteral(
	        "archive|family|%1|unused|archive:OFL.txt|OFL-Family.txt|"
	        "Family-Regular.ttf>Family-Regular.ttf Family-Bold.ttf>Family-Bold.ttf\n"
	        "file|Family-Regular.ttf|%2\nfile|Family-Bold.ttf|%3\nfile|licences/OFL-Family.txt|%4\n")
	        .arg(url, sha(regular), sha(QByteArray("not the bold")), sha(QByteArray("OFL")))));

	QTemporaryDir dir;
	bssh_font_download download(&source);
	QSignalSpy finished(&download, &bssh_font_download::finished);
	download.start(*pins.archive(QStringLiteral("family")), pins, dir.path());

	QCOMPARE(finished.count(), 1);
	QVERIFY(!finished.first().at(0).toBool());
	QVERIFY2(finished.first().at(1).toString().contains(QStringLiteral("Family-Bold.ttf")),
	          qPrintable(finished.first().at(1).toString()));

	/* All or nothing: not even the regular, which did verify, is left. */
	QVERIFY2(QDir(dir.path()).entryList(QDir::Files | QDir::NoDotAndDotDot).isEmpty(),
	          qPrintable(QDir(dir.path()).entryList(QDir::Files).join(QLatin1Char(' '))));
}

void font_download_test::only_a_set_font_that_is_missing_is_offered_for_download() {
	/* A font of the set, pinned and missing: offered. */
	QVERIFY(bssh_font_offers_download(QStringLiteral("iosevka-term"), false));

	/* Already here: nothing to fetch. */
	QVERIFY(!bssh_font_offers_download(QStringLiteral("iosevka-term"), true));

	/*
	 * Fallback-only families are pinned and fetchable, and still not
	 * offered: they are not for picking (sec 9.2), and a download button
	 * for one would be an invitation to pick it.
	 */
	QVERIFY(!bssh_font_offers_download(QStringLiteral("unifont"), false));

	/* Nothing to fetch it from: an installed family gone missing, an unknown id. */
	QVERIFY(!bssh_font_offers_download(QStringLiteral("system:Some Mono"), false));
	QVERIFY(!bssh_font_offers_download(QStringLiteral("no-such-font"), false));
}

void font_download_test::a_real_family_downloads_from_its_release() {
	if (qEnvironmentVariable("BSSH_TEST_LIVE_FONTS") != QStringLiteral("1"))
		QSKIP("set BSSH_TEST_LIVE_FONTS=1 to fetch a real family over the network");

	const QString id = qEnvironmentVariable("BSSH_TEST_LIVE_FONT_ID", QStringLiteral("hack"));
	const bssh_font_sources &sources = bssh_font_sources::embedded();
	const bssh_font_archive_t *archive = sources.archive(id);
	QVERIFY2(archive, qPrintable(id + QStringLiteral(" is not in the pins")));

	QTemporaryDir dir;
	bssh_network_range_source source;
	bssh_font_download download(&source);
	QSignalSpy finished(&download, &bssh_font_download::finished);
	download.start(*archive, sources, dir.path());

	QVERIFY(finished.wait(300000));
	QVERIFY2(finished.first().at(0).toBool(), qPrintable(finished.first().at(1).toString()));
	for (const auto &member : archive->members)
		QVERIFY2(QFile::exists(dir.filePath(member.second)), qPrintable(member.second));
	QVERIFY(QFile::exists(dir.filePath(QStringLiteral("licences/") + archive->licence_file)));
}
