#include "model/custom_font.h"

#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QSaveFile>
#include <QStandardPaths>

#include <cmath>

#include "term/font_catalog.h"

namespace {

/*
 * One name, whatever the source was called: Qt reads the data, not the
 * extension, and a fixed name is what makes replacing it deleting a named
 * file rather than a pattern.
 */
const QString BSSH_CUSTOM_FONT_FILE = QStringLiteral("custom-font");

/*
 * Larger than any single face anybody means -- Iosevka's biggest is 11 MB
 * -- and small enough that a wrong file is refused before it is held.
 */
const qint64 BSSH_CUSTOM_FONT_LIMIT = 64 * 1024 * 1024;

/* What is registered now, so a replacement can let it go. */
int g_registered = -1;

QString custom_path() {
	return bssh_custom_font_directory() + QLatin1Char('/') + BSSH_CUSTOM_FONT_FILE;
}

void adopt(int id, const QString &family) {
	if (g_registered >= 0 && g_registered != id) QFontDatabase::removeApplicationFont(g_registered);
	g_registered = id;
	bssh_font_catalog::instance().set_custom_family(family);
}

} /* namespace */

static QString g_forced_directory;

QString bssh_custom_font_directory() {
	if (!g_forced_directory.isEmpty()) return g_forced_directory;
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
	       + QStringLiteral("/fonts/custom");
}

void bssh_set_custom_font_directory(const QString &directory) {
	g_forced_directory = directory;
}

bool bssh_family_is_monospace(const QString &family) {
	QFont font(family);
	font.setPointSizeF(24);

	/* A family Qt does not have comes back as a substitute, which proves nothing. */
	if (QFontInfo(font).family().compare(family, Qt::CaseInsensitive) != 0) return false;

	/*
	 * Measured, not asked. The narrowest and widest letters of a
	 * proportional face -- i and W, l and m -- differ by half or more; a
	 * monospace face has them equal to the rounding of its hinting.
	 */
	const QFontMetricsF metrics(font);
	const QString probe = QStringLiteral("iWlm.0_M");
	const qreal width = metrics.horizontalAdvance(probe.at(0));
	if (width <= 0) return false;

	for (const QChar c : probe) {
		if (std::fabs(metrics.horizontalAdvance(c) - width) > width * 0.02) return false;
	}
	return true;
}

bool bssh_import_custom_font(const QString &source, QString *family, QString *error) {
	auto refuse = [error](const QString &why) {
		if (error) *error = why;
		return false;
	};

	QFile in(source);
	if (!in.open(QIODevice::ReadOnly)) return refuse(QStringLiteral("The file could not be opened."));

	const QByteArray bytes = in.read(BSSH_CUSTOM_FONT_LIMIT + 1);
	if (bytes.isEmpty()) return refuse(QStringLiteral("The file is empty."));
	if (bytes.size() > BSSH_CUSTOM_FONT_LIMIT)
		return refuse(QStringLiteral("The file is larger than any font this takes."));

	const int id = QFontDatabase::addApplicationFontFromData(bytes);
	if (id < 0) return refuse(QStringLiteral("That is not a font file this can read."));

	const QStringList families = QFontDatabase::applicationFontFamilies(id);
	if (families.isEmpty()) {
		QFontDatabase::removeApplicationFont(id);
		return refuse(QStringLiteral("That font names no family."));
	}

	const QString registered = families.first();
	if (!bssh_family_is_monospace(registered)) {
		QFontDatabase::removeApplicationFont(id);
		return refuse(registered + QStringLiteral(" is not monospace: its letters are different "
		                                           "widths, and a terminal's grid needs one width."));
	}

	/*
	 * Written whole or not at all: a half-written file would be a font
	 * that fails to load at the next start, having replaced one that did.
	 */
	QDir().mkpath(bssh_custom_font_directory());
	QSaveFile out(custom_path());
	if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() || !out.commit()) {
		QFontDatabase::removeApplicationFont(id);
		return refuse(QStringLiteral("The font could not be saved."));
	}

	adopt(id, registered);
	if (family) *family = registered;
	return true;
}

QString bssh_load_custom_font() {
	const QString path = custom_path();
	if (!QFile::exists(path)) return QString();

	const int id = QFontDatabase::addApplicationFont(path);
	if (id < 0) return QString();

	const QStringList families = QFontDatabase::applicationFontFamilies(id);
	if (families.isEmpty() || !bssh_family_is_monospace(families.first())) {
		QFontDatabase::removeApplicationFont(id);
		return QString();
	}

	adopt(id, families.first());
	return families.first();
}
