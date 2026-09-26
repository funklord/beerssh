#ifndef BSSH_FONT_DOWNLOAD_H
#define BSSH_FONT_DOWNLOAD_H

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QUrl>

#include <functional>

#include "model/font_sources.h"
#include "term/zip_index.h"

class QNetworkAccessManager;

/*
 * Where the bytes come from: a length, and a range of it. The network in
 * the app, a buffer in the tests -- which is what lets the whole of a
 * family's download be driven without one.
 */
class bssh_range_source {
public:
	virtual ~bssh_range_source() = default;

	/* The resource's length, or an error. */
	virtual void size(const QUrl &url, std::function<void(qint64, QString)> done) = 0;

	/* `length` bytes from `from`, or all of it when `length` is negative. */
	virtual void fetch(const QUrl &url, qint64 from, qint64 length,
	                   std::function<void(QByteArray, QString)> done) = 0;
};

/*
 * Over HTTP(S): HEAD for the length, and GET with a Range header for the
 * rest, requiring 206 -- a server that ignores the range and sends the
 * whole 165 MB is refused, not waited for.
 */
class bssh_network_range_source : public QObject, public bssh_range_source {
	Q_OBJECT

public:
	explicit bssh_network_range_source(QObject *parent = nullptr);

	void size(const QUrl &url, std::function<void(qint64, QString)> done) override;
	void fetch(const QUrl &url, qint64 from, qint64 length,
	           std::function<void(QByteArray, QString)> done) override;

private:
	QNetworkAccessManager *m_network;
};

/*
 * One family of the curated set, fetched onto the device (project.md sec
 * 9.4): from its pinned upstream release, one file at a time by byte range
 * where it is an archive, each file checked against its own pin before it
 * is kept, into `directory` as the catalog expects to find fonts -- and
 * its licence into `directory`/licences, since the font may not travel
 * without it.
 *
 * All or nothing: files are written beside their names and renamed only
 * once every one has verified, so a failure part way leaves no half a
 * family for the catalog to find.
 */
class bssh_font_download : public QObject {
	Q_OBJECT

public:
	bssh_font_download(bssh_range_source *source, QObject *parent = nullptr);

	void start(const bssh_font_archive_t &archive, const bssh_font_sources &pins,
	           const QString &directory);

signals:
	void progress(int done, int total);
	void finished(bool ok, const QString &error);

private:
	void fail(const QString &error);
	void next_member();
	void fetch_licence();
	bool keep(const QString &output, const QByteArray &contents);
	void commit();

	bssh_range_source *m_source;
	bssh_font_archive_t m_archive;
	bssh_font_sources m_pins;
	QString m_directory;
	qint64 m_archive_size = 0;
	QList<bssh_zip_entry_t> m_entries;
	int m_next = 0;
	QStringList m_written;
	bool m_failed = false;
};

/*
 * Where fonts fetched on the device are kept: app storage, beside nothing
 * the user sees, and loaded after the bundled set at every start.
 */
QString bssh_downloaded_font_directory();

/*
 * The source a picker's download uses: the network, unless a test has set
 * something else -- the seam bssh_ssh_session::set_factory is. The caller
 * owns what it is given.
 */
bssh_range_source *bssh_make_range_source();
void bssh_set_range_source_factory(std::function<bssh_range_source *()> factory);

/*
 * Whether a font should be offered for download: one of the set, which a
 * user may pick, that has pins to fetch it by and is not already here.
 * Asked with `available` given rather than looked up, so the rule can be
 * tested on a machine where every font is installed.
 */
bool bssh_font_offers_download(const QString &id, bool available);

#endif /* BSSH_FONT_DOWNLOAD_H */
