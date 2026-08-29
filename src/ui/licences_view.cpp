#include "ui/touch_metrics.h"
#include "ui/licences_view.h"

#include "model/command_line.h"

#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QListWidget>
#include <QScroller>
#include <QPushButton>
#include <QTextBrowser>
#include <QLabel>
#include <QVBoxLayout>

#include "platform/platform.h"
#include "term/colour_scheme.h"
#include "term/font_catalog.h"

namespace {

/*
 * The libraries this app is built out of, and what each one asks for. Kept
 * here rather than generated, because the obligations differ in kind and a
 * generated list would flatten them into names: libssh's LGPL is the one
 * that constrains how the thing may be LINKED (sec 4), and saying so is
 * the point of listing it.
 */
struct library_t {
	const char *name;
	const char *licence;
	const char *note;
};

const library_t BSSH_LIBRARIES[] = {
	{ "Qt 6", "LGPL-3.0",
		"Linked dynamically on desktop. The Android build links the Qt "
		"libraries it ships as permitted by the same licence." },
	{ "libssh", "LGPL-2.1",
		"The SSH transport. Linked dynamically on desktop, which satisfies "
		"the LGPL without further obligation; the Android build must keep "
		"the relinking terms in view (project.md sec 4)." },
	{ "libvterm", "MIT",
		"The terminal emulation core, vendored as a submodule and built "
		"with the patches in vterm-patch/, which are ours." },
	{ "OpenSSL", "Apache-2.0",
		"The crypto backend libssh is built against. Cross-compiled for "
		"Android; the system copy is used on desktop." },
	{ "libsixel", "MIT",
		"Decodes sixel images (project.md sec 7.1). Desktop only." }
};

} /* namespace */

bssh_licences_view::bssh_licences_view(QWidget *parent)
    : QWidget(parent) {
	const bssh_platform *platform = bssh_platform::instance();
	const bool touch = platform && platform->is_touch_primary();

	QVBoxLayout *outer = new QVBoxLayout(this);

	/*
	 * What this program is, which version, and who wrote it -- and on
	 * touch this page is the ONLY place any of that appears.
	 *
	 * The desktop has an About box on its menu bar; a phone has no menu
	 * bar, so before this there was no surface on Android saying what
	 * version was running or who to attribute it to. sec 8.3 already
	 * argues that a bug report wants to name what it was running on,
	 * which is the same argument for naming the build.
	 *
	 * Here rather than as a second page, because this page is already
	 * "what this app is made of" and the app itself is the first entry
	 * in that answer. A separate About screen on touch would be one more
	 * thing to find (sec 8.5) for one paragraph.
	 *
	 * The copyright line is the constant --version and the About box
	 * both read, so the three surfaces cannot drift. Attribution, not
	 * licensing: it grants nothing and creates no obligation to add a
	 * LICENSE, which stays the copyright holder's alone.
	 */
#ifdef BSSH_VERSION_STRING
	const QString version = QStringLiteral(BSSH_VERSION_STRING);
#else
	const QString version = QStringLiteral("development build");
#endif

	QLabel *identity = new QLabel(
	        QStringLiteral("<b>BeerSSH</b> %1<br>%2")
	                .arg(version.toHtmlEscaped(),
	                      QString(QStringLiteral(BSSH_COPYRIGHT_LINE)).toHtmlEscaped()),
	        this);
	identity->setTextFormat(Qt::RichText);
	identity->setWordWrap(true);
	identity->setObjectName(QStringLiteral("licences_identity"));
	outer->addWidget(identity);

	m_list = new QListWidget(this);
	m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_list->setTextElideMode(Qt::ElideRight);

	/*
	 * Side by side on a desktop, stacked on a phone. A licence is a wall
	 * of prose, and in a shared-width column it arrives about ten
	 * characters wide -- which is not reading, it is scrolling. The same
	 * mistake the preset editor made when it was first put on a phone.
	 */
	QBoxLayout *columns = touch
	        ? static_cast<QBoxLayout *>(new QVBoxLayout())
	        : static_cast<QBoxLayout *>(new QHBoxLayout());
	outer->addLayout(columns, 1);

	if (touch) {
		m_list->setMaximumHeight(220);
		QScroller::grabGesture(m_list->viewport(), QScroller::LeftMouseButtonGesture);
	} else {
		m_list->setMaximumWidth(260);
	}

	columns->addWidget(m_list);

	m_text = new QTextBrowser(this);

	/*
	 * Read-only and plain: a licence is a document to be shown exactly as
	 * it was written, not reflowed into something that might read
	 * differently.
	 */
	m_text->setOpenExternalLinks(false);
	m_text->setLineWrapMode(QTextEdit::WidgetWidth);
	columns->addWidget(m_text, 1);

	if (touch) QScroller::grabGesture(m_text->viewport(), QScroller::LeftMouseButtonGesture);

	connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
		if (row < 0) return;
		m_text->setPlainText(m_list->item(row)->data(Qt::UserRole).toString());
	});

	load_fonts();
	load_schemes();
	load_libraries();

	if (m_list->count() > 0) m_list->setCurrentRow(0);

	if (touch) {
		/* A full-screen page needs its own way out (sec 8.5). */
		QPushButton *back = new QPushButton(QStringLiteral("Back"), this);
		connect(back, &QPushButton::clicked, this, &bssh_licences_view::back_requested);
		outer->addWidget(back);

		/*
		 * Including the LIST, whose rows are what a finger taps to pick
		 * a licence and which stood 17dp -- a third of sec 6.2's floor.
		 * Nothing caught it because rows are not widgets: every check
		 * that looks for child widgets walks straight past them, and it
		 * took rendering the page to see it.
		 */
		bssh_apply_touch_heights(this, true);
	}
}

void bssh_licences_view::add_entry(const QString &title, const QString &subtitle,
                                    const QString &body) {
	QListWidgetItem *item = new QListWidgetItem(title, m_list);
	item->setData(Qt::UserRole, body);
	item->setToolTip(subtitle);
}

QString bssh_licences_view::licence_text(const QString &font_id) const {
	const bssh_platform *platform = bssh_platform::instance();
	if (!platform) return QString();

	const QString directory = platform->font_directory();
	if (directory.isEmpty()) return QString();

	/*
	 * By name from the catalog, not by guessing from the family. The
	 * first version of this matched filenames against the display name
	 * with its spaces removed, which found five of the eight: "Iosevka
	 * Term" ships under OFL-Iosevka.md and "Cascadia Code PL" under
	 * OFL-CascadiaCode.txt. Three fonts were credited with a line saying
	 * their licence was not installed, on the one screen whose entire job
	 * is to credit people.
	 */
	const bssh_font_t *font = bssh_font_catalog::instance().by_id(font_id);
	if (!font || font->licence_file.isEmpty()) return QString();

	QFile file(directory + QStringLiteral("/licences/") + font->licence_file);
	if (!file.open(QIODevice::ReadOnly)) return QString();

	return QString::fromUtf8(file.readAll());
}

void bssh_licences_view::load_fonts() {
	const bssh_font_catalog &catalog = bssh_font_catalog::instance();

	for (const bssh_font_t &font : catalog.all()) {
		const QString text = licence_text(font.id);

		/*
		 * The full text where it shipped, and an honest line where it did
		 * not: on a checkout that never ran tool/fetch-fonts.sh, or an
		 * Android build whose asset pack has not arrived, there is
		 * nothing to show and saying so beats an empty pane.
		 */
		const QString body = text.isEmpty()
		        ? QStringLiteral("%1 is licensed under %2.\n\n"
		                          "The full licence text ships beside the font "
		                          "and is not present in this installation.")
		                  .arg(font.display_name, font.licence)
		        : text;

		add_entry(QStringLiteral("Font: %1").arg(font.display_name), font.licence, body);
	}
}

void bssh_licences_view::load_schemes() {
	/*
	 * ONE entry, because there is now one palette.
	 *
	 * This used to add a row per scheme, and it was right to: the list
	 * carried Solarized, Gruvbox, Nord and Tango, each somebody's design
	 * under somebody's licence, and sec 9.4's rule is that a set which
	 * cannot say where each entry came from is a set nobody can check.
	 *
	 * Sec 9.3 then took the sixteen away from schemes entirely. What is
	 * left is the classic ANSI palette every scheme shares, so five rows
	 * would be one attribution repeated five times -- and one of them
	 * would be "Colours: Custom", attributing the user's own palette to
	 * the user, which is not a licence at all. A licences screen padded
	 * with entries that say nothing is a licences screen people stop
	 * reading, which is the opposite of what it is for.
	 */
	add_entry(QStringLiteral("Colours: Classic ANSI"),
	           QStringLiteral("Public domain"),
	           QStringLiteral("The sixteen ANSI colours, in the classic VGA text-mode "
	                           "values.\n\nThese are the standard's, not a design: no "
	                           "scheme in this app repaints them, so what a program "
	                           "means by red is what every other terminal means by "
	                           "it.\n\nA custom palette set on this device replaces "
	                           "them locally and belongs to whoever set it."));
}

void bssh_licences_view::load_libraries() {
	for (const library_t &library : BSSH_LIBRARIES) {
		add_entry(QStringLiteral("Library: %1").arg(QString::fromLatin1(library.name)),
		           QString::fromLatin1(library.licence),
		           QStringLiteral("%1\n\nLicence: %2\n\n%3")
		                   .arg(QString::fromLatin1(library.name),
		                        QString::fromLatin1(library.licence),
		                        QString::fromLatin1(library.note)));
	}
}
