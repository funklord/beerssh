#include "model/font_download.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>

#include "term/font_catalog.h"

bssh_network_range_source::bssh_network_range_source(QObject *parent)
    : QObject(parent), m_network(new QNetworkAccessManager(this)) {
}

void bssh_network_range_source::size(const QUrl &url, std::function<void(qint64, QString)> done) {
	QNetworkReply *reply = m_network->head(QNetworkRequest(url));
	connect(reply, &QNetworkReply::finished, this, [reply, done] {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			done(-1, reply->errorString());
			return;
		}
		const qint64 length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong();
		done(length > 0 ? length : -1,
		     length > 0 ? QString() : QStringLiteral("the server did not say how large it is"));
	});
}

void bssh_network_range_source::fetch(const QUrl &url, qint64 from, qint64 length,
                                      std::function<void(QByteArray, QString)> done) {
	QNetworkRequest request(url);
	if (length >= 0) {
		request.setRawHeader("Range", QStringLiteral("bytes=%1-%2")
		                                      .arg(from).arg(from + length - 1).toLatin1());
	}
	QNetworkReply *reply = m_network->get(request);
	connect(reply, &QNetworkReply::finished, this, [reply, length, done] {
		reply->deleteLater();
		if (reply->error() != QNetworkReply::NoError) {
			done(QByteArray(), reply->errorString());
			return;
		}
		const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
		if (length >= 0 && status != 206) {
			done(QByteArray(), QStringLiteral("the server will not send part of the file"));
			return;
		}
		done(reply->readAll(), QString());
	});
}

bssh_font_download::bssh_font_download(bssh_range_source *source, QObject *parent)
    : QObject(parent), m_source(source) {
}

void bssh_font_download::fail(const QString &error) {
	if (m_failed) return;
	m_failed = true;

	/* Nothing half-kept: every file written so far is still only a .part. */
	for (const QString &output : m_written) QFile::remove(m_directory + QLatin1Char('/') + output + QStringLiteral(".part"));
	emit finished(false, error);
}

bool bssh_font_download::keep(const QString &output, const QByteArray &contents) {
	const QString pin = m_pins.pin(output);
	if (pin.isEmpty()) {
		fail(QStringLiteral("%1 has no pin to check it against").arg(output));
		return false;
	}
	const QString got = QString::fromLatin1(
	        QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
	if (got != pin) {
		fail(QStringLiteral("%1 is not the file that was pinned").arg(output));
		return false;
	}

	const QString path = m_directory + QLatin1Char('/') + output + QStringLiteral(".part");
	QDir().mkpath(QFileInfo(path).path());
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit()) {
		fail(QStringLiteral("could not write %1").arg(output));
		return false;
	}
	m_written.append(output);
	emit progress(m_written.size(), m_archive.members.size() + 1);
	return true;
}

void bssh_font_download::commit() {
	for (const QString &output : m_written) {
		const QString path = m_directory + QLatin1Char('/') + output;
		QFile::remove(path);
		if (!QFile::rename(path + QStringLiteral(".part"), path)) {
			fail(QStringLiteral("could not put %1 in place").arg(output));
			return;
		}
	}
	emit finished(true, QString());
}

void bssh_font_download::start(const bssh_font_archive_t &archive, const bssh_font_sources &pins,
                               const QString &directory) {
	m_archive = archive;
	m_pins = pins;
	m_directory = directory;
	m_entries.clear();
	m_written.clear();
	m_next = 0;
	m_failed = false;
	emit progress(0, archive.members.size() + 1);

	const QUrl url(archive.url);

	if (archive.is_bare()) {
		m_source->fetch(url, 0, -1, [this](QByteArray bytes, QString error) {
			if (!error.isEmpty()) { fail(error); return; }
			if (keep(m_archive.members.first().second, bytes)) fetch_licence();
		});
		return;
	}

	/* The index first: the archive's length, its last bytes, then its directory. */
	m_source->size(url, [this, url](qint64 length, QString error) {
		if (length < 0) { fail(error); return; }
		m_archive_size = length;
		const qint64 tail = qMin(length, BSSH_ZIP_TAIL_BYTES);
		m_source->fetch(url, length - tail, tail, [this, url](QByteArray bytes, QString error) {
			if (!error.isEmpty()) { fail(error); return; }
			qint64 offset = 0, span = 0;
			QString why;
			if (!bssh_zip_find_directory(bytes, m_archive_size, &offset, &span, &why)) { fail(why); return; }
			m_source->fetch(url, offset, span, [this](QByteArray directory, QString error) {
				if (!error.isEmpty()) { fail(error); return; }
				QString why;
				if (!bssh_zip_read_directory(directory, &m_entries, &why)) { fail(why); return; }
				next_member();
			});
		});
	});
}

void bssh_font_download::next_member() {
	if (m_failed) return;
	if (m_next >= m_archive.members.size()) {
		fetch_licence();
		return;
	}

	const QPair<QString, QString> member = m_archive.members.at(m_next++);
	const bssh_zip_entry_t *found = nullptr;
	for (const bssh_zip_entry_t &entry : m_entries) {
		if (entry.name == member.first) found = &entry;
	}
	if (!found) {
		fail(QStringLiteral("%1 is not in the archive").arg(member.first));
		return;
	}

	const bssh_zip_entry_t entry = *found;
	const qint64 span = qMin(bssh_zip_member_span(entry), m_archive_size - entry.local_header_offset);
	m_source->fetch(QUrl(m_archive.url), entry.local_header_offset, span,
	                [this, entry, output = member.second](QByteArray bytes, QString error) {
		if (!error.isEmpty()) { fail(error); return; }
		QByteArray contents;
		QString why;
		if (!bssh_zip_extract(entry, bytes, &contents, &why)) { fail(why); return; }
		if (keep(output, contents)) next_member();
	});
}

void bssh_font_download::fetch_licence() {
	if (m_failed) return;
	const QString output = QStringLiteral("licences/") + m_archive.licence_file;

	if (m_archive.licence_url.startsWith(QStringLiteral("archive:"))) {
		const QString name = m_archive.licence_url.mid(8);
		for (const bssh_zip_entry_t &entry : m_entries) {
			if (entry.name != name) continue;
			const qint64 span = qMin(bssh_zip_member_span(entry),
			                         m_archive_size - entry.local_header_offset);
			m_source->fetch(QUrl(m_archive.url), entry.local_header_offset, span,
			                [this, entry, output](QByteArray bytes, QString error) {
				if (!error.isEmpty()) { fail(error); return; }
				QByteArray contents;
				QString why;
				if (!bssh_zip_extract(entry, bytes, &contents, &why)) { fail(why); return; }
				if (keep(output, contents)) commit();
			});
			return;
		}
		fail(QStringLiteral("the licence %1 is not in the archive").arg(name));
		return;
	}

	m_source->fetch(QUrl(m_archive.licence_url), 0, -1, [this, output](QByteArray bytes, QString error) {
		if (!error.isEmpty()) { fail(error); return; }
		if (keep(output, bytes)) commit();
	});
}

QString bssh_downloaded_font_directory() {
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
	       + QStringLiteral("/fonts");
}

namespace {
std::function<bssh_range_source *()> &range_source_factory() {
	static std::function<bssh_range_source *()> factory;
	return factory;
}
} /* namespace */

bssh_range_source *bssh_make_range_source() {
	if (range_source_factory()) return range_source_factory()();
	return new bssh_network_range_source();
}

void bssh_set_range_source_factory(std::function<bssh_range_source *()> factory) {
	range_source_factory() = std::move(factory);
}

bool bssh_font_offers_download(const QString &id, bool available) {
	if (available || bssh_font_catalog::is_system_id(id)) return false;
	const bssh_font_t *font = bssh_font_catalog::instance().by_id(id);
	if (!font || !font->selectable) return false;
	return bssh_font_sources::embedded().archive(id) != nullptr;
}
