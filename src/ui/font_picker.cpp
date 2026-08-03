#include "ui/font_picker.h"

#include <QComboBox>
#include <QPainter>
#include <QPixmap>
#include <QFormLayout>
#include <QLabel>
#include <QSpinBox>
#include <QVBoxLayout>

#include "platform/platform.h"
#include "model/app_settings.h"
#include "term/colour_scheme.h"
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

/* Tall enough to read as colour rather than as a line. */
static const int BSSH_SWATCH_HEIGHT = 18;

/*
 * The scheme's sixteen ANSI colours as one strip: normal on the top half,
 * bright underneath, in ANSI order. Two rows rather than sixteen squares
 * in a line, because the bright half is what distinguishes two schemes
 * that look alike at a glance.
 */
static QPixmap bssh_swatch_strip(const bssh_colour_scheme_t &scheme, int height, bool raise) {
	const int width = 16 * 18;
	QPixmap strip(width, qMax(2, height));
	strip.fill(QColor(scheme.background));

	QPainter painter(&strip);
	const int cell = width / 8;
	const int half = strip.height() / 2;

	/*
	 * Drawn through the same contrast floor the terminal draws through
	 * (sec 9.3), because this strip exists to answer "what will this look
	 * like" and the answer changed when the floor arrived. Showing the
	 * published values while the terminal shows lifted ones makes the one
	 * control whose entire job is judging contrast the one place that
	 * reports it wrongly -- a nearly invisible blue block beside a
	 * perfectly readable blue prompt.
	 */
	for (int i = 0; i < 8; ++i) {
		const QRgb top = raise ? bssh_raise_contrast(scheme.ansi[i], scheme.background)
		                        : scheme.ansi[i];
		const QRgb bottom = raise ? bssh_raise_contrast(scheme.ansi[i + 8], scheme.background)
		                           : scheme.ansi[i + 8];

		painter.fillRect(i * cell, 0, cell, half, QColor(top));
		painter.fillRect(i * cell, half, cell, strip.height() - half, QColor(bottom));
	}

	return strip;
}

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

	m_scheme = new QComboBox(this);
	m_scheme->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

	for (const bssh_colour_scheme_t &scheme : bssh_colour_scheme_catalog::instance().all()) {
		m_scheme->addItem(scheme.display_name, scheme.id);
	}

	form->addRow(QStringLiteral("Font"), m_family);
	form->addRow(QStringLiteral("Size"), m_size);
	form->addRow(QStringLiteral("Colours"), m_scheme);
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

	/*
	 * The sixteen colours, drawn. A scheme's NAME tells a user nothing --
	 * "Nord" and "Gruvbox Dark" are both "a dark one" until you see them
	 * -- and the sample text only ever exercises two of the sixteen.
	 */
	m_swatches = new QLabel(this);
	m_swatches->setFixedHeight(BSSH_SWATCH_HEIGHT);
	outer->addWidget(m_swatches);

	m_note = new QLabel(this);
	m_note->setWordWrap(true);
	outer->addWidget(m_note);

	connect(m_family, &QComboBox::currentIndexChanged,
	         this, &bssh_font_picker::on_choice_changed);
	connect(m_size, QOverload<int>::of(&QSpinBox::valueChanged),
	         this, &bssh_font_picker::on_choice_changed);
	connect(m_scheme, &QComboBox::currentIndexChanged, this, [this] {
		refresh_preview();
		emit scheme_chosen(scheme_id());
	});

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
	m_scheme->setMinimumHeight(height);
}

QString bssh_font_picker::scheme_id() const {
	return m_scheme->currentData().toString();
}

void bssh_font_picker::set_scheme(const QString &id) {
	const bssh_colour_scheme_catalog &catalog = bssh_colour_scheme_catalog::instance();
	const QString wanted = catalog.effective_id(id);

	const int index = m_scheme->findData(wanted);
	if (index < 0) {
		/* Same rule as an unknown font: shown, not swallowed. */
		m_scheme->addItem(wanted + QStringLiteral("  (not in this build)"), wanted);
		m_scheme->setCurrentIndex(m_scheme->count() - 1);
	} else {
		m_scheme->setCurrentIndex(index);
	}

	refresh_preview();
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

	/*
	 * The preview wears the scheme's own colours, so what is on screen is
	 * what the terminal will look like rather than a font sample on an
	 * unrelated background. A scheme judged against the wrong background
	 * is a scheme judged wrongly -- contrast is the whole decision.
	 */
	const bssh_colour_scheme_catalog &schemes = bssh_colour_scheme_catalog::instance();
	const bssh_colour_scheme_t *scheme = schemes.by_id(scheme_id());
	if (!scheme) scheme = schemes.by_id(schemes.default_id());

	if (scheme) {
		const bool raise = bssh_app_settings().raise_contrast();
		const QRgb foreground = raise
		        ? bssh_raise_contrast(scheme->foreground, scheme->background)
		        : scheme->foreground;

		m_preview->setStyleSheet(QStringLiteral("background: %1; color: %2;")
		                                 .arg(QColor(scheme->background).name(),
		                                      QColor(foreground).name()));
		m_swatches->setPixmap(bssh_swatch_strip(*scheme, m_swatches->height(), raise));
	}

	const QString note = catalog.substitution_note(id);
	if (note.isEmpty()) {
		m_note->setText(QStringLiteral(
		        "Characters this font does not have are drawn from "
		        "DejaVu Sans Mono, then Noto Sans Mono, then Unifont."));
	} else {
		m_note->setText(note);
	}
}
