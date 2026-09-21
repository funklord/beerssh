#include "ui/touch_metrics.h"
#include "model/preset.h"
#include "ui/font_picker.h"

#include <QComboBox>
#include <QPainter>
#include <QPixmap>
#include <QFormLayout>
#include <QLabel>
#include <QResizeEvent>

#include "ui/wrapped_note.h"
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


/* Tall enough to read as colour rather than as a line. */
static const int BSSH_SWATCH_HEIGHT = 18;

/*
 * Eight columns, each carrying a normal colour above its bright one, and
 * the floor is one pixel per column: below that there is no strip left to
 * look at. There is deliberately no PREFERRED width -- the size policy
 * below is Ignored, so the layout hands the strip whatever the form has
 * and a preferred number would have no reader.
 */
static const int BSSH_SWATCH_COLUMNS = 8;

/*
 * The scheme's sixteen ANSI colours as one strip: normal on the top half,
 * bright underneath, in ANSI order. Two rows rather than sixteen squares
 * in a line, because the bright half is what distinguishes two schemes
 * that look alike at a glance.
 */
/*
 * The strip is drawn to the width it is GIVEN, and that is the whole
 * point of the parameter.
 *
 * It used to be a fixed `16 * 18` pixmap, and a QLabel holding a pixmap
 * demands the pixmap's width -- so this one control asked for 288px and
 * carried the entire font picker, plus the host editor and the preset
 * editor that embed it, past the edge of a 320dp screen. Measured on a
 * Galaxy Z Fold 3 cover screen, which is 320dp: the picker's minimum was
 * 288 and its widest child was this label, to the pixel.
 *
 * Eight columns of two rows is what the strip IS, so a narrower strip is
 * narrower cells rather than fewer colours -- nothing is lost by fitting.
 * The previous width survives as the PREFERRED one rather than as a
 * floor, which is why sizeHint() still asks for it.
 */
static QPixmap bssh_swatch_strip(const bssh_colour_scheme_t &scheme, int width,
                                  int height, bool raise) {
	width = qMax(BSSH_SWATCH_COLUMNS, width);
	QPixmap strip(width, qMax(2, height));
	strip.fill(QColor(scheme.background));

	QPainter painter(&strip);
	const int cell = width / BSSH_SWATCH_COLUMNS;
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
	for (int i = 0; i < BSSH_SWATCH_COLUMNS; ++i) {
		const QRgb top = raise ? bssh_raise_contrast(scheme.ansi[i], scheme.background)
		                        : scheme.ansi[i];
		const QRgb bottom = raise ? bssh_raise_contrast(scheme.ansi[i + 8], scheme.background)
		                           : scheme.ansi[i + 8];

		painter.fillRect(i * cell, 0, cell, half, QColor(top));
		painter.fillRect(i * cell, half, cell, strip.height() - half, QColor(bottom));
	}

	return strip;
}

/*
 * The three label widgets QFormLayout made for us. Asked for by field
 * rather than by row index, so adding a row above them cannot silently
 * shift what this returns.
 */
QList<QWidget *> bssh_font_picker::label_widgets() const {
	QList<QWidget *> labels;
	if (!m_form) return labels;

	QWidget *const fields[] = { m_family, m_size, m_scheme };
	for (QWidget *field : fields) {
		if (QWidget *label = m_form->labelForField(field)) labels.append(label);
	}

	return labels;
}

int bssh_font_picker::label_column_width() const {
	int width = 0;
	for (QWidget *label : label_widgets()) {
		width = qMax(width, label->sizeHint().width());
	}
	return width;
}

void bssh_font_picker::set_label_column_width(int width) {
	for (QWidget *label : label_widgets()) {
		label->setMinimumWidth(width);
	}
}

void bssh_font_picker::align_labels(QFormLayout *host, bssh_font_picker *picker) {
	if (!host || !picker) return;

	int width = picker->label_column_width();
	QList<QWidget *> host_labels;

	for (int row = 0; row < host->rowCount(); ++row) {
		QLayoutItem *item = host->itemAt(row, QFormLayout::LabelRole);
		if (!item || !item->widget()) continue;

		host_labels.append(item->widget());
		width = qMax(width, item->widget()->sizeHint().width());
	}

	/*
	 * Both sides, not just the picker: the host form may be the narrower
	 * of the two, and a caller should not have to work out which.
	 */
	picker->set_label_column_width(width);
	for (QWidget *label : host_labels) {
		label->setMinimumWidth(width);
	}

	/*
	 * And the SPACING between the columns, which equal label widths do
	 * not settle. Two form layouts left to the style can resolve their
	 * horizontal spacing differently, and the host editor's fields
	 * started 6px right of the picker's with both label columns the same
	 * width -- close enough to read as aligned and measurably not.
	 *
	 * Taken from the host, because the host is the page and the picker is
	 * the guest.
	 */
	if (picker->m_form) picker->m_form->setHorizontalSpacing(host->horizontalSpacing());
}

/*
 * Every pointer member starts null, and that is load-bearing rather than
 * tidy: resizeEvent() below runs while this constructor is still building
 * them -- adding a widget to a layout can lay the parent out again -- and
 * it guards on these being null. Only m_form was initialised, so that
 * guard would have read uninitialised memory and passed on garbage.
 */
bssh_font_picker::bssh_font_picker(QWidget *parent)
    : QWidget(parent),
      m_form(nullptr),
      m_family(nullptr),
      m_scheme(nullptr),
      m_size(nullptr),
      m_preview(nullptr),
      m_swatches(nullptr),
      m_note(nullptr) {
	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);

	QFormLayout *form = new QFormLayout();
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	/*
	 * Label above field where the row will not fit beside it, exactly as
	 * the settings page does -- and here the reason is portability rather
	 * than one screen.
	 *
	 * This picker's minimum is the sum of a label column and a combo
	 * whose items are FONT FAMILY NAMES, so it is a function of the UI
	 * font's metrics: 222px on the machine this was written on and 285
	 * to 302 in the CI container, which draws in a different face. A
	 * two-column form that only just fits is one that stops fitting on
	 * somebody else's fonts, and check-ci caught exactly that after the
	 * local suite went green.
	 *
	 * Wrapping makes the row's minimum max(label, field) rather than
	 * their sum, which is what removes the dependency instead of leaving
	 * it with a little more headroom.
	 */
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);

	m_form = form;

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
	m_size->setRange(BSSH_PRESET_MIN_POINT_SIZE, BSSH_PRESET_MAX_POINT_SIZE);
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
	m_scheme->setObjectName(QStringLiteral("font-picker-scheme"));
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
	m_preview->setMinimumHeight(BSSH_TOUCH_TARGET_DP);

	/*
	 * Allowed to wrap, which is what keeps this picker off a phone's
	 * right-hand edge.
	 *
	 * The sample is one long monospace line, and a QLabel asks for room
	 * for all of it -- 360px of it, exactly a narrow phone, so the picker
	 * alone consumed the whole width and both editors that EMBED it
	 * demanded more than the screen once their label column was added.
	 * Wrapping costs a second line at a narrow width and keeps every
	 * glyph visible, which clipping would not.
	 */
	bssh_wrap_note(m_preview);
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

	/*
	 * Ignored horizontally, so the pixmap this label is about to hold
	 * cannot decide the form's width. A QLabel takes its minimum from
	 * its pixmap, and the pixmap is drawn from the label's width -- left
	 * to itself that is a widget arguing with the layout about a number
	 * it made up. Ignored means the layout decides and the strip follows,
	 * which is the direction that terminates.
	 */
	m_swatches->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	m_swatches->setMinimumWidth(BSSH_SWATCH_COLUMNS);
	m_swatches->setToolTip(QStringLiteral(
	        "The scheme's sixteen colours: normal above, bright below."));
	outer->addWidget(m_swatches);

	m_note = new QLabel(this);
	bssh_wrap_note(m_note);
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
	/*
	 * The walk rather than three named controls, so that this picker
	 * grows a fourth without also having to be told about it -- and so
	 * that the rows of the popups these combo boxes drop down get the
	 * floor as well. Those stood 25dp: what a finger taps when choosing
	 * a font is a row in a list, not the closed box.
	 */
	bssh_apply_touch_heights(this, touch);

	/* Back to the style's own size when touch is turned off again. */
	if (touch) return;

	m_family->setMinimumHeight(0);
	m_size->setMinimumHeight(0);
	m_scheme->setMinimumHeight(0);
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

/*
 * The strip follows the width it is given, so a resize redraws it.
 *
 * refresh_preview() is the one place that renders it, and calling the
 * whole function rather than the strip alone is deliberate: the preview
 * font, the note and the strip are one picture of one scheme, and a
 * resize path that refreshed only part of it is how two of them come to
 * disagree.
 */
void bssh_font_picker::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);

	/*
	 * Guarded, because a resize can arrive while the constructor is still
	 * building these -- adding a widget to a layout can lay the parent
	 * out again, and refresh_preview() dereferences every one of them.
	 */
	if (!m_preview || !m_swatches || !m_note) return;

	refresh_preview();
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
		m_swatches->setPixmap(bssh_swatch_strip(*scheme, m_swatches->width(),
		                                         m_swatches->height(), raise));
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
