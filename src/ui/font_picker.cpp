#include "ui/font_picker.h"

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include "platform/platform.h"
#include "term/font_catalog.h"

/*
 * Everything a selectable font is REQUIRED to have (sec 9.1): the
 * disambiguation set, box drawing and block elements. Chosen so the
 * preview never needs the fallback chain -- a preview that quietly
 * borrowed a glyph from DejaVu would be showing the user a font other
 * than the one they are looking at the name of.
 *
 * What the chain does instead is said in words below it, because a CJK
 * sample here would render as tofu in most of the set and look like a
 * defect rather than like the fallback working.
 */
static const char *const BSSH_PREVIEW_TEXT =
        "ABCDEfgh 0O1lI5S2Z {}[]()  \342\224\214\342\224\200\342\224\254\342\224\200\342\224\220  \342\226\221\342\226\222\342\226\223\342\226\210";

static const int BSSH_MIN_POINT_SIZE = 6;
static const int BSSH_MAX_POINT_SIZE = 32;

/* sec 6.2's floor, applied to the controls rather than only to the key row. */
static const int BSSH_TOUCH_HEIGHT_DP = 48;

bssh_font_picker::bssh_font_picker(QWidget *parent)
    : QWidget(parent) {
	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);

	QFormLayout *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	m_family = new QComboBox(this);

	/*
	 * Expanding, or QFormLayout::ExpandingFieldsGrow leaves it at its
	 * natural width -- the policy grows fields that ask to be grown and
	 * nothing else. On the Android host page the surrounding fields are
	 * full width, and a font combo two-fifths as wide next to them reads
	 * as a control that did not finish laying out.
	 */
	m_family->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	m_size = new QSpinBox(this);
	m_size->setRange(BSSH_MIN_POINT_SIZE, BSSH_MAX_POINT_SIZE);
	m_size->setSuffix(QStringLiteral(" pt"));

	const bssh_font_catalog &catalog = bssh_font_catalog::instance();
	for (const bssh_font_t &font : catalog.selectable()) {
		/*
		 * Marked, not hidden. An entry that disappears between devices
		 * is how a user concludes their font choice was lost, when in
		 * fact it is intact and waiting for a download (sec 9.4).
		 */
		const QString label = catalog.is_available(font.id)
		        ? font.display_name
		        : font.display_name + QStringLiteral("  (not installed)");

		m_family->addItem(label, font.id);
	}

	form->addRow(QStringLiteral("Font"), m_family);
	form->addRow(QStringLiteral("Size"), m_size);
	outer->addLayout(form);

	m_preview = new QLabel(QString::fromUtf8(BSSH_PREVIEW_TEXT), this);
	m_preview->setFrameShape(QFrame::StyledPanel);
	m_preview->setMargin(8);
	m_preview->setMinimumHeight(48);
	/*
	 * The terminal's colours, not the dialog's: a font is being judged for
	 * how it reads as terminal output, and light-on-dark is a different
	 * judgement from dark-on-light at the same size.
	 */
	m_preview->setAutoFillBackground(true);
	m_preview->setStyleSheet(QStringLiteral("background: #1c1c1c; color: #d0d0d0;"));
	outer->addWidget(m_preview);

	m_note = new QLabel(this);
	m_note->setWordWrap(true);
	outer->addWidget(m_note);

	connect(m_family, &QComboBox::currentIndexChanged,
	         this, &bssh_font_picker::on_choice_changed);
	connect(m_size, QOverload<int>::of(&QSpinBox::valueChanged),
	         this, &bssh_font_picker::on_choice_changed);

	const bssh_platform *platform = bssh_platform::instance();
	set_font_choice(QString(), 0);
	set_touch_sized(platform && platform->is_touch_primary());
}

QString bssh_font_picker::font_id() const {
	return m_family->currentData().toString();
}

int bssh_font_picker::point_size() const {
	return m_size->value();
}

void bssh_font_picker::set_font_choice(const QString &id, int point_size) {
	bssh_font_catalog &catalog = bssh_font_catalog::instance();

	const bssh_platform *platform = bssh_platform::instance();
	const QString wanted =
	        catalog.effective_id(id, platform && platform->is_touch_primary());

	const int index = m_family->findData(wanted);

	/*
	 * A font that is not in the picker: an id from a newer build, or a
	 * family name an imported profile invented. Added as its own entry
	 * rather than silently reset to the default, so the user can SEE what
	 * their profile asks for -- and the note under the preview then says
	 * what is actually being drawn.
	 */
	if (index < 0) {
		m_family->addItem(wanted + QStringLiteral("  (not in this build)"), wanted);
		m_family->setCurrentIndex(m_family->count() - 1);
	} else {
		m_family->setCurrentIndex(index);
	}

	m_size->setValue(point_size > 0 ? point_size : 11);
	refresh_preview();
}

void bssh_font_picker::set_touch_sized(bool touch) {
	const int height = touch ? BSSH_TOUCH_HEIGHT_DP : 0;
	m_family->setMinimumHeight(height);
	m_size->setMinimumHeight(height);
}

void bssh_font_picker::on_choice_changed() {
	refresh_preview();
	emit font_chosen(font_id(), point_size());
}

void bssh_font_picker::refresh_preview() {
	const bssh_font_catalog &catalog = bssh_font_catalog::instance();
	const QString id = font_id();

	/*
	 * Drawn in what the terminal would actually draw in, which for an
	 * unavailable font is the head of its fallback chain rather than the
	 * font named above it. A preview showing a font the terminal will not
	 * use is worse than no preview.
	 */
	m_preview->setFont(bssh_font_catalog::make_font(catalog.family_for(id), point_size()));

	const QString note = catalog.substitution_note(id);
	if (note.isEmpty()) {
		m_note->setText(QStringLiteral(
		        "Characters this font does not have are drawn from "
		        "DejaVu Sans Mono, then Noto Sans Mono, then Unifont."));
	} else {
		m_note->setText(note);
	}
}
