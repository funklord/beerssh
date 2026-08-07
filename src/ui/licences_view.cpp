#include "ui/touch_metrics.h"
#include "ui/licences_view.h"

#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QListWidget>
#include <QScroller>
#include <QPushButton>
#include <QTextBrowser>
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
		"with the patches in vterm-patches/, which are ours." },
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
		back->setMinimumHeight(BSSH_TOUCH_TARGET_DP);
		connect(back, &QPushButton::clicked, this, &bssh_licences_view::back_requested);
		outer->addWidget(back);
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
		 * not: on a checkout that never ran tools/fetch-fonts.sh, or an
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
	for (const bssh_colour_scheme_t &scheme : bssh_colour_scheme_catalog::instance().all()) {
		/*
		 * Sixteen numbers each, and still somebody's design. The
		 * attribution is short because there is no long version to give:
		 * these are palettes, published under the licence named.
		 */
		add_entry(QStringLiteral("Colours: %1").arg(scheme.display_name), scheme.licence,
		           QStringLiteral("The \"%1\" colour scheme.\n\nLicence: %2")
		                   .arg(scheme.display_name, scheme.licence));
	}
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
