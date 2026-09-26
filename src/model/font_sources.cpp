#include "model/font_sources.h"

#include <QFile>
#include <QStringList>

bool bssh_font_sources::parse(const QString &text, QString *error) {
	m_archives.clear();
	m_pins.clear();

	const QStringList lines = text.split(QLatin1Char('\n'));
	for (int number = 0; number < lines.size(); ++number) {
		const QString line = lines.at(number).trimmed();
		if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;

		const QStringList fields = line.split(QLatin1Char('|'));
		if (fields.first() == QStringLiteral("archive") && fields.size() == 7) {
			bssh_font_archive_t archive;
			archive.id = fields.at(1);
			archive.url = fields.at(2);
			archive.sha256 = fields.at(3);
			archive.licence_url = fields.at(4);
			archive.licence_file = fields.at(5);
			for (const QString &member : fields.at(6).split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
				const int arrow = member.indexOf(QLatin1Char('>'));
				if (arrow <= 0) {
					if (error) *error = QStringLiteral("line %1: a member without '>'").arg(number + 1);
					return false;
				}
				archive.members.append({member.left(arrow), member.mid(arrow + 1)});
			}
			m_archives.append(archive);
			continue;
		}
		if (fields.first() == QStringLiteral("file") && fields.size() == 3) {
			m_pins.insert(fields.at(1), fields.at(2));
			continue;
		}

		/*
		 * Refused rather than skipped: a line this does not read is a line
		 * whose pin would silently not be checked.
		 */
		if (error) *error = QStringLiteral("line %1 is not an archive| or file| record").arg(number + 1);
		return false;
	}
	return true;
}

const bssh_font_sources &bssh_font_sources::embedded() {
	static const bssh_font_sources sources = [] {
		bssh_font_sources parsed;
		QFile file(QStringLiteral(":/font-sources.txt"));
		if (file.open(QIODevice::ReadOnly)) parsed.parse(QString::fromUtf8(file.readAll()));
		return parsed;
	}();
	return sources;
}

const bssh_font_archive_t *bssh_font_sources::archive(const QString &id) const {
	for (const bssh_font_archive_t &archive : m_archives) {
		if (archive.id == id) return &archive;
	}
	return nullptr;
}
