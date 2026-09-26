#include "term/font_catalog.h"

#include <algorithm>

#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontMetricsF>

/*
 * The chain of last resort (sec 9.3), by id. Order matters and is not
 * arbitrary: DejaVu first because the gap it fills is box drawing, which
 * is what TUIs are built out of; Noto next for the broad-Unicode and CJK
 * range; Unifont last because it is complete and ugly, in that order of
 * priority. Nothing proportional is on this list and nothing proportional
 * may be added to it.
 */
static const char *const BSSH_FALLBACK_CHAIN[] = {
	"dejavu-sans-mono",
	"noto-sans-mono",
	"unifont"
};

/* Nominal size for coverage probes. Which glyphs a font has does not
 * depend on the size it is rendered at, so this never needs to match the
 * size anything is actually drawn in. */
static const int BSSH_PROBE_POINT_SIZE = 12;

bssh_font_catalog &bssh_font_catalog::instance() {
	static bssh_font_catalog catalog;
	return catalog;
}

bssh_font_catalog::bssh_font_catalog() {
	/*
	 * sec 9.2's table, trimmed to the set its own [OPEN] note asks for:
	 * six selectable families plus the two fallback-only ones. The full
	 * table lists more, and shipping all of them would be tens of
	 * megabytes of near-duplicates for a picker nobody scrolls to the
	 * bottom of.
	 *
	 * Each entry is a licence obligation as much as a font, which is why
	 * the licence travels in the row rather than in a README somebody has
	 * to remember to update.
	 */
	struct entry_t {
		const char *id;
		const char *display_name;
		const char *family;
		const char *file;
		const char *licence;
		const char *licence_file;
		bool selectable;
		bool condensed;
	};

	static const entry_t entries[] = {
		/*
		 * The default on anything that is not a phone: tall x-height and
		 * strong 0/O, 1/l/I disambiguation (sec 9.1). Ligatures are
		 * present in the file and switched off in make_font.
		 */
		{ "jetbrains-mono", "JetBrains Mono", "JetBrains Mono",
			"JetBrainsMono-Regular.ttf", "OFL-1.1", "OFL-JetBrainsMono.txt", true, false },

		/*
		 * The Term variant, not plain Iosevka: it is the one whose
		 * metrics are meant for a character grid. The family name
		 * differs from the project name, and asking for "Iosevka" on a
		 * machine that has "Iosevka Term" gets a substitution instead
		 * of a match.
		 */
		{ "iosevka-term", "Iosevka Term", "Iosevka Term",
			"IosevkaTerm-Regular.ttf", "OFL-1.1", "OFL-Iosevka.md", true, true },

		/* Selectable in its own right and the first fallback (sec 9.3). */
		{ "dejavu-sans-mono", "DejaVu Sans Mono", "DejaVu Sans Mono",
			"DejaVuSansMono.ttf", "Bitstream Vera / DejaVu", "LICENSE-DejaVu.txt", true, false },

		{ "hack", "Hack", "Hack",
			"Hack-Regular.ttf", "Hack Open Font License", "LICENSE-Hack.md", true, false },

		{ "fira-code", "Fira Code", "Fira Code",
			"FiraCode-Regular.ttf", "OFL-1.1", "OFL-FiraCode.txt", true, false },

		/*
		 * The PL variant carries the powerline glyphs that sec 9.2 says
		 * to cover this way rather than by bundling Nerd Font patched
		 * derivatives, whose licensing needs reviewing case by case.
		 */
		{ "cascadia-code-pl", "Cascadia Code PL", "Cascadia Code PL",
			"CascadiaCodePL.ttf", "OFL-1.1", "OFL-CascadiaCode.txt", true, false },

		/*
		 * Fallback-only from here down: never in the picker (sec 9.2).
		 * Noto is here for the CJK and broad-Unicode coverage no
		 * terminal-first font has.
		 */
		{ "noto-sans-mono", "Noto Sans Mono", "Noto Sans Mono",
			"NotoSansMono-Regular.ttf", "OFL-1.1", "OFL-NotoSansMono.txt", false, false },

		/*
		 * Last resort, so that nothing renders as tofu. Unifont is a
		 * bitmap face and looks it; that is the trade sec 9.3 makes
		 * knowingly, because an unreadable glyph still beats a box.
		 */
		{ "unifont", "Unifont", "Unifont",
			"unifont.otf", "OFL-1.1", "LICENSE-Unifont.txt", false, false }
	};

	for (const entry_t &entry : entries) {
		bssh_font_t font;
		font.id = QString::fromLatin1(entry.id);
		font.display_name = QString::fromLatin1(entry.display_name);
		font.family = QString::fromLatin1(entry.family);
		font.file = QString::fromLatin1(entry.file);
		font.licence = QString::fromLatin1(entry.licence);
		font.licence_file = QString::fromLatin1(entry.licence_file);
		font.selectable = entry.selectable;
		font.condensed = entry.condensed;
		m_fonts.append(font);
	}
}

QList<bssh_font_t> bssh_font_catalog::selectable() const {
	QList<bssh_font_t> result;
	for (const bssh_font_t &font : m_fonts) {
		if (font.selectable) result.append(font);
	}
	return result;
}

const bssh_font_t *bssh_font_catalog::by_id(const QString &id) const {
	for (const bssh_font_t &font : m_fonts) {
		if (font.id == id) return &font;
	}
	return nullptr;
}

QString bssh_font_catalog::canonical_id(const QString &name) const {
	if (name.isEmpty()) return QString();

	if (by_id(name)) return name;

	for (const bssh_font_t &font : m_fonts) {
		if (font.display_name.compare(name, Qt::CaseInsensitive) == 0 ||
		    font.family.compare(name, Qt::CaseInsensitive) == 0) {
			return font.id;
		}
	}

	/*
	 * Deliberately not a guess. A name that matches nothing is returned
	 * unchanged so the caller can say "this profile asks for a font that
	 * is not in the set" rather than quietly serving it something else
	 * under the same name.
	 */
	return name;
}

QString bssh_font_catalog::effective_id(const QString &stored, bool compact) const {
	if (stored.isEmpty()) return default_id(compact);
	return canonical_id(stored);
}

QString bssh_font_catalog::default_id(bool compact) const {
	const QString preferred = compact ? QStringLiteral("iosevka-term")
	                                  : QStringLiteral("jetbrains-mono");
	if (is_available(preferred)) return preferred;

	/*
	 * A default is a choice made on the user's behalf, so it must not be a
	 * font that is not there. The substitution note (sec 9.4) exists for a
	 * font the USER picked -- telling someone their font is unavailable
	 * when they never picked one is reporting our own packaging decision
	 * as though it were their problem.
	 *
	 * This is not hypothetical: Iosevka is 42 MB of the set on its own,
	 * so on Android it arrives in an asset pack rather than the base APK
	 * (sec 9.4) and is genuinely absent until the user fetches it, even
	 * though it is the form factor's preferred default.
	 */
	const QString standard = QStringLiteral("jetbrains-mono");
	if (is_available(standard)) return standard;

	for (const bssh_font_t &font : m_fonts) {
		if (font.selectable && family_installed(font.family)) return font.id;
	}

	/*
	 * Nothing in the set is installed. Returning the preferred id anyway
	 * is right: fallback_chain still produces something drawable, and the
	 * profile then records what this build meant to use rather than
	 * whatever happened to be on one machine at first run.
	 */
	return preferred;
}

int bssh_font_catalog::load_bundled(const QString &directory) {
	int loaded = 0;
	const QDir dir(directory);

	/*
	 * Every face in the directory, not just the regular one named in the
	 * table. sec 9.1 requires a real bold, and a bold face only gets used
	 * if it was registered: with regular alone, QFont::setBold makes Qt
	 * synthesise one by smearing the outline, which at terminal sizes is
	 * the difference between bold text and blurry text.
	 *
	 * Loading by extension rather than by name also means the italics and
	 * the obliques come along without this table having to enumerate every
	 * file upstream happens to ship.
	 */
	const QStringList faces =
	        dir.entryList({ QStringLiteral("*.ttf"), QStringLiteral("*.otf") },
	                      QDir::Files, QDir::Name);

	for (const QString &face : faces) {
		const int id = QFontDatabase::addApplicationFont(dir.filePath(face));
		if (id < 0) continue;

		/*
		 * Register what the FILE says its family is, not what this table
		 * says. When those disagree the table is wrong, and recording
		 * the file's answer means the font still resolves while the
		 * mismatch shows up as a substitution note rather than as text
		 * silently rendered in something else.
		 */
		const QStringList families = QFontDatabase::applicationFontFamilies(id);
		for (const QString &family : families) {
			if (!m_loaded.contains(family)) m_loaded.append(family);
		}

		++loaded;
	}

	/* Coverage answers can change when new families arrive. */
	m_glyph_cache.clear();

	return loaded;
}

bool bssh_font_catalog::family_installed(const QString &family) const {
	if (family.isEmpty()) return false;
	if (m_loaded.contains(family, Qt::CaseInsensitive)) return true;

	/*
	 * A system-installed copy counts. Most desktop distributions already
	 * ship DejaVu and often more of the set, and refusing to use one
	 * because it arrived from the distribution rather than from our
	 * bundle would mean downloading a second copy of a font already on
	 * disk. sec 9.4's exclusion of *unvetted* system fonts is about
	 * offering arbitrary families in the picker, which this is not: the
	 * name asked for here is one this table already vetted.
	 */
	return QFontDatabase::families().contains(family, Qt::CaseInsensitive);
}

bool bssh_font_catalog::is_available(const QString &id) const {
	if (is_system_id(id)) return family_installed(id.mid(7));

	const bssh_font_t *font = by_id(id);
	if (!font) return false;
	return family_installed(font->family);
}

QString bssh_font_catalog::system_id(const QString &family) {
	return QStringLiteral("system:") + family;
}

bool bssh_font_catalog::is_system_id(const QString &id) {
	return id.startsWith(QStringLiteral("system:")) && id.size() > 7;
}

QStringList bssh_font_catalog::installed_monospace() const {
	QStringList offered;
	for (const QString &family : QFontDatabase::families()) {
		if (family.startsWith(QLatin1Char('.'))) continue;
		if (!QFontDatabase::isFixedPitch(family)) continue;
		if (m_loaded.contains(family, Qt::CaseInsensitive)) continue;

		bool in_set = false;
		for (const bssh_font_t &font : m_fonts) {
			if (font.family.compare(family, Qt::CaseInsensitive) == 0) in_set = true;
		}
		if (!in_set) offered.append(family);
	}

	std::sort(offered.begin(), offered.end(), [](const QString &a, const QString &b) {
		return a.compare(b, Qt::CaseInsensitive) < 0;
	});
	return offered;
}

QString bssh_font_catalog::display_name(const QString &id) const {
	if (is_system_id(id)) return id.mid(7);
	const bssh_font_t *font = by_id(id);
	return font ? font->display_name : id;
}

QStringList bssh_font_catalog::fallback_chain(const QString &id) const {
	QStringList chain;

	const bssh_font_t *selected = by_id(id);
	if (selected && family_installed(selected->family)) {
		chain.append(selected->family);
	}

	/*
	 * An installed font first when it is chosen and still there, and then
	 * the set's own chain behind it -- which is what covers the glyphs an
	 * unvetted family does not have.
	 */
	if (is_system_id(id) && family_installed(id.mid(7))) chain.append(id.mid(7));

	for (const char *fallback_id : BSSH_FALLBACK_CHAIN) {
		const bssh_font_t *font = by_id(QString::fromLatin1(fallback_id));
		if (!font || !family_installed(font->family)) continue;
		if (chain.contains(font->family)) continue;
		chain.append(font->family);
	}

	/*
	 * Nothing in the set is present -- a broken bundle, or a bare CI
	 * container. Falling back to the platform's own fixed-pitch font is
	 * the one substitution that keeps a grid: it is whatever the system
	 * considers monospace, which is a far weaker guarantee than sec 9.1
	 * but still not a proportional face.
	 */
	if (chain.isEmpty()) {
		chain.append(QFontDatabase::systemFont(QFontDatabase::FixedFont).family());
	}

	return chain;
}

QString bssh_font_catalog::family_for(const QString &id) const {
	const QStringList chain = fallback_chain(id);
	return chain.isEmpty() ? QString() : chain.first();
}

QString bssh_font_catalog::family_for_glyph(const QString &id, char32_t ch) const {
	/*
	 * Keyed by id and then by codepoint, in two levels. It used to build
	 * one key per call by concatenating the id with the number -- a
	 * QString allocation for every character on screen, every repaint,
	 * which is a hash lookup wearing the cost of a memory allocation. The
	 * renderer asks this once per cell.
	 */
	QHash<char32_t, QString> &glyphs = m_glyph_cache[id];

	const auto cached = glyphs.constFind(ch);
	if (cached != glyphs.constEnd()) return cached.value();

	const QStringList chain = fallback_chain(id);
	QString chosen = chain.isEmpty() ? QString() : chain.first();

	for (const QString &family : chain) {
		const QFontMetricsF metrics(make_font(family, BSSH_PROBE_POINT_SIZE));
		if (metrics.inFontUcs4(static_cast<uint>(ch))) {
			chosen = family;
			break;
		}
	}

	glyphs.insert(ch, chosen);
	return chosen;
}

QFont bssh_font_catalog::make_font(const QString &family, int point_size,
                                    bool ligatures) {
	QFont font(family);
	font.setPointSize(point_size);
	font.setFixedPitch(true);
	font.setStyleHint(QFont::Monospace);

	/*
	 * NoFontMerging is the whole reason the chain above exists. Left to
	 * itself Qt fills a missing glyph from any family it can find,
	 * including proportional ones, and sec 9.3 forbids exactly that: one
	 * CJK character served by a proportional face pushes every cell after
	 * it out of alignment, and the misalignment is the remote program's
	 * layout, not ours, so it looks like the shell is broken.
	 */
	font.setStyleStrategy(QFont::NoFontMerging);

#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
	/*
	 * Ligatures off (sec 9.2). JetBrains Mono, Fira Code and Cascadia
	 * Code all ship them, and in a character-cell grid a ligature draws
	 * two source characters as one shape while the emulator still counts
	 * two columns -- so a search that highlights "!=" highlights half of
	 * a glyph. Users who want them get a setting, not the default.
	 *
	 * Qt gained per-font OpenType feature control in 6.7; older Qt (which
	 * distribution CI images still carry) renders with the font's own
	 * defaults, which is a cosmetic difference rather than a broken grid.
	 */
	if (!ligatures) {
		font.setFeature(QFont::Tag("liga"), 0);
		font.setFeature(QFont::Tag("clig"), 0);
		font.setFeature(QFont::Tag("calt"), 0);
	}
#else
	(void) ligatures;
#endif

	return font;
}

QString bssh_font_catalog::substitution_note(const QString &id) const {
	const bssh_font_t *font = by_id(id);
	const QString served = family_for(id);

	if (is_system_id(id)) {
		if (is_available(id)) return QString();
		return QStringLiteral("%1 is not installed on this computer any more. "
		                      "Showing %2.").arg(id.mid(7), served);
	}

	if (!font) {
		return QStringLiteral("\"%1\" is not one of the bundled fonts. "
		                      "Showing %2.").arg(id, served);
	}

	if (is_available(id)) return QString();

	/*
	 * Phrased as "not installed yet" rather than "missing" because on
	 * Android that is literally true: the family is in the set and an
	 * asset pack away (sec 9.4). Saying "missing" would suggest the
	 * choice was invalid, which it was not.
	 */
	return QStringLiteral("%1 is not installed on this device yet. "
	                      "Showing %2 until it is.")
	        .arg(font->display_name, served);
}

QFont bssh_scaled_font(const QFont &font, qreal factor) {
	QFont scaled = font;

	if (font.pointSizeF() > 0) {
		scaled.setPointSizeF(font.pointSizeF() * factor);
	} else if (font.pixelSize() > 0) {
		scaled.setPixelSize(qMax(1, qRound(font.pixelSize() * factor)));
	}

	/*
	 * A font with neither is one Qt has not resolved yet; leaving it alone
	 * is better than guessing a size, because the caller's widget will
	 * inherit a sensible one from its parent either way.
	 */
	return scaled;
}
