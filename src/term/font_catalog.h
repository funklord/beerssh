#ifndef BSSH_FONT_CATALOG_H
#define BSSH_FONT_CATALOG_H

#include <QFont>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

/*
 * bssh_font_catalog -- the curated terminal font set (project.md sec 9).
 *
 * sec 9.2 decides WHICH fonts exist; this is that decision as data, in one
 * table, compiled into both platforms. Two rules from sec 9.4 shape the
 * whole type:
 *
 *   "The picker's contents must not depend on the platform." The list a
 *   user chooses from is the same on a phone and a workstation, even
 *   though Android ships two or three families in the base APK and
 *   downloads the rest. Availability is therefore a separate question
 *   from membership, and it is deliberately NOT allowed to filter the
 *   list -- a font that vanishes from settings when you switch devices is
 *   how a profile silently stops meaning what it said.
 *
 *   Families are identified by a stable internal id, not by their display
 *   name. A profile stores "jetbrains-mono", never "JetBrains Mono": the
 *   display name is a label that can be re-spelled, translated, or differ
 *   from the family name the font file actually registers, and a profile
 *   that carries one of those is a profile that resolves differently
 *   depending on where it is opened.
 *
 * What this type does NOT do is decide anything about size, weight or
 * layout. It answers "which font, and if not that one, then what" -- the
 * view owns metrics, because metrics depend on the widget's device pixel
 * ratio (sec 9.5) and the catalog is a process-wide singleton shared by
 * every pane.
 */

struct bssh_font_t {
	/*
	 * Stable, lowercase, hyphenated, and never changed once shipped: it
	 * is written into profiles and presets, so renaming one silently
	 * moves every host that referenced it down the fallback chain.
	 */
	QString id;

	QString display_name;

	/*
	 * The family name the font file registers, which is not always the
	 * display name -- "Iosevka Term" ships as a family distinct from
	 * "Iosevka", and asking for the wrong one gets a substitution rather
	 * than an error.
	 */
	QString family;

	/* File within the font directory, absent for fallback-by-system. */
	QString file;

	/*
	 * Kept because the OFL and Apache terms require the licence to travel
	 * with the font, and sec 9.4 wants an Open Source Licences screen
	 * built from something rather than maintained by hand.
	 */
	QString licence;

	/*
	 * False for the fallback-only families (sec 9.2): Noto Sans Mono and
	 * Unifont exist to stop tofu, and offering "GNU Unifont" in a picker
	 * invites someone to read a day of logs in it.
	 */
	bool selectable;

	/*
	 * Condensed faces fit noticeably more columns on a phone (sec 9.2),
	 * which is why the mobile default may differ from the desktop one --
	 * from the same set, never a different set.
	 */
	bool condensed;

	bssh_font_t() : selectable(true), condensed(false) {}
};

/*
 * Scale a UI font, whichever unit it is defined in.
 *
 * Not part of the catalog: this is for app chrome (a page heading), not for
 * the terminal grid, which sizes itself in whole cells. It lives here
 * because it is font handling and because the alternative was two copies of
 * it in two front doors.
 *
 * A QFont carries EITHER a point size or a pixel size, and the other one
 * reads back as -1. Qt's default UI font on Android is defined in pixels,
 * so the obvious `setPointSizeF(pointSizeF() * factor)` sets a negative
 * size there: Qt warns, ignores it, and the heading a page was designed
 * around comes out the same size as its body text. Desktop never shows
 * this, because its default font is defined in points.
 */
QFont bssh_scaled_font(const QFont &font, qreal factor);

class bssh_font_catalog {
public:
	/*
	 * One per process. The glyph-coverage cache below is shared across
	 * every pane for the same reason sec 9.5 wants one glyph atlas: four
	 * panes rendering the same shell prompt should ask the question once.
	 */
	static bssh_font_catalog &instance();

	bssh_font_catalog();

	/* Every family, fallback-only ones included. */
	const QList<bssh_font_t> &all() const { return m_fonts; }

	/* What a font picker shows -- identical on every platform. */
	QList<bssh_font_t> selectable() const;

	/* Null when the id is unknown, which is a caller error worth seeing. */
	const bssh_font_t *by_id(const QString &id) const;

	/*
	 * Accepts an id, and falls back to matching a display or family name
	 * case-insensitively. The tolerance is for profiles written before
	 * ids existed and for a human typing "Iosevka" into an import file;
	 * everything this project writes writes ids.
	 */
	QString canonical_id(const QString &name) const;

	/*
	 * The out-of-the-box choice. Same set both ways (sec 9.2): compact
	 * form factors get the condensed default, everything else the
	 * legibility-first one.
	 */
	QString default_id(bool compact) const;

	/*
	 * Loads whatever the bundle actually contains. Missing files are not
	 * an error: on Android most of the set arrives later through an asset
	 * pack, and on a developer's machine the fetch script may not have
	 * been run at all. Returns how many families were registered.
	 */
	int load_bundled(const QString &directory);

	/*
	 * True when this font can really be rendered -- either its file was
	 * loaded from the bundle or a family of that name is installed on the
	 * system. Everything else resolves down the chain.
	 */
	bool is_available(const QString &id) const;

	/*
	 * The family to actually hand to QFont, after substitution. Empty
	 * only if the entire chain is missing, which means the bundle is
	 * broken rather than that the user chose badly.
	 */
	QString family_for(const QString &id) const;

	/*
	 * sec 9.3's chain, resolved to available families and de-duplicated:
	 * selected -> DejaVu Sans Mono -> Noto Sans Mono -> Unifont.
	 *
	 * A proportional system font is never in it. Qt would happily merge
	 * one in to fill a gap, and it would break the grid the moment a
	 * single CJK character arrived, so the chain is walked explicitly and
	 * the fonts it produces have font merging switched off.
	 */
	QStringList fallback_chain(const QString &id) const;

	/*
	 * The family that actually carries this codepoint, walking the chain.
	 * Cached, because it is asked once per distinct character per repaint
	 * and the answer cannot change while the process runs.
	 *
	 * Returns the first chain entry when nothing has the glyph: something
	 * must be drawn, and a missing-glyph box in the selected font is
	 * honest and still one cell wide.
	 */
	QString family_for_glyph(const QString &id, char32_t ch) const;

	/*
	 * A ready-to-use font: fixed pitch, monospace hint, and font merging
	 * disabled so the explicit chain above is the only fallback there is.
	 */
	static QFont make_font(const QString &family, int point_size);

	/*
	 * Non-empty when the request could not be honoured, phrased for the
	 * user. sec 9.4 asks for "a visible note rather than silently
	 * substituting": the difference between a terminal that looks subtly
	 * wrong and one that tells you why.
	 */
	QString substitution_note(const QString &id) const;

private:
	bool family_installed(const QString &family) const;

	QList<bssh_font_t> m_fonts;

	/* Families successfully registered from bundled files. */
	QStringList m_loaded;

	mutable QHash<QString, QString> m_glyph_cache;
};

#endif /* BSSH_FONT_CATALOG_H */
