#ifndef BSSH_FONT_SOURCES_H
#define BSSH_FONT_SOURCES_H

#include <QHash>
#include <QList>
#include <QPair>
#include <QString>

/*
 * The curated set's pins, as tool/font-sources.txt states them (project.md
 * sec 9.4) -- the file tool/fetch-fonts.sh reads for a build, embedded in
 * the app so a phone fetching one family and a build fetching all of them
 * cannot disagree about what the set is. Its header gives the format.
 */
struct bssh_font_archive_t {
	QString id;
	QString url;
	QString sha256;
	QString licence_url;      /* "archive:<path>" for a member of the archive */
	QString licence_file;
	QList<QPair<QString, QString>> members;   /* path in the archive -> output name */

	/* A bare font file rather than an archive: the one member is ".". */
	bool is_bare() const { return members.size() == 1 && members.first().first == QStringLiteral("."); }
};

class bssh_font_sources {
public:
	/* Reads the format; lines it does not recognise are refused, not skipped. */
	bool parse(const QString &text, QString *error = nullptr);

	/* The copy built into the app. */
	static const bssh_font_sources &embedded();

	const bssh_font_archive_t *archive(const QString &id) const;
	const QList<bssh_font_archive_t> &archives() const { return m_archives; }

	/* The SHA-256 an output must have on its own; empty when it has no pin. */
	QString pin(const QString &output) const { return m_pins.value(output); }

private:
	QList<bssh_font_archive_t> m_archives;
	QHash<QString, QString> m_pins;
};

#endif /* BSSH_FONT_SOURCES_H */
