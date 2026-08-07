#include "ui/colour_editor.h"

#include <QColorDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui/touch_metrics.h"
#include "ui/wrapped_note.h"

namespace {

/*
 * The names, in ANSI order. Named rather than numbered because "colour 12"
 * is not a thing anyone can check against what a program asked for, and
 * the whole difficulty of a palette editor is knowing which square is the
 * red that `ls` uses.
 */
const char *const BSSH_ANSI_NAMES[8] = {
	"Black", "Red", "Green", "Yellow", "Blue", "Magenta", "Cyan", "White"
};

/* The swatch, sized so the colour is the control rather than a hint on it. */
const int BSSH_SWATCH_PX = 28;

} /* namespace */

bssh_colour_editor::bssh_colour_editor(const bssh_colour_scheme_t &scheme, QWidget *parent)
    : QDialog(parent),
      m_scheme(scheme) {
	setWindowTitle(QStringLiteral("Custom colours"));

	QVBoxLayout *outer = new QVBoxLayout(this);

	QLabel *heading = new QLabel(QStringLiteral("<b>The sixteen</b>"), this);
	outer->addWidget(heading);

	QLabel *note = bssh_wrapped_note(
	        QStringLiteral("Normal on the top row, bright below. Programs choose from "
	                        "these by number, so changing one changes what every "
	                        "program on every host means by that colour."),
	        this);
	outer->addWidget(note);

	QGridLayout *grid = new QGridLayout();
	grid->setSpacing(4);

	/*
	 * Column headings, so the grid can be read without counting. Normal
	 * and bright are rows rather than two separate blocks because they
	 * are pairs -- bright red belongs beside red, not eight squares away.
	 */
	for (int col = 0; col < 8; ++col) {
		QLabel *name = new QLabel(QString::fromLatin1(BSSH_ANSI_NAMES[col]), this);
		name->setAlignment(Qt::AlignHCenter);
		grid->addWidget(name, 0, col + 1);
	}

	QLabel *normal_row = new QLabel(QStringLiteral("Normal"), this);
	grid->addWidget(normal_row, 1, 0);

	QLabel *bright_row = new QLabel(QStringLiteral("Bright"), this);
	grid->addWidget(bright_row, 2, 0);

	for (int i = 0; i < 8; ++i) {
		const QString normal_name = QString::fromLatin1(BSSH_ANSI_NAMES[i]);
		grid->addWidget(add_swatch(&m_scheme.ansi[i], QString(),
		                            QStringLiteral("Normal %1").arg(normal_name.toLower())),
		                 1, i + 1);
		grid->addWidget(add_swatch(&m_scheme.ansi[i + 8], QString(),
		                            QStringLiteral("Bright %1").arg(normal_name.toLower())),
		                 2, i + 1);
	}

	outer->addLayout(grid);

	QLabel *own_heading = new QLabel(QStringLiteral("<b>What the scheme itself sets</b>"),
	                                  this);
	outer->addWidget(own_heading);

	QLabel *own_note = bssh_wrapped_note(
	        QStringLiteral("The two colours the remote never chooses, and the cursor. "
	                        "These are what the shipped schemes differ on."),
	        this);
	outer->addWidget(own_note);

	QGridLayout *own = new QGridLayout();
	own->setSpacing(4);

	struct named_t {
		QRgb *target;
		const char *label;
	};

	const named_t owned[] = {
		{ &m_scheme.background, "Background" },
		{ &m_scheme.foreground, "Text" },
		{ &m_scheme.cursor, "Cursor" }
	};

	int column = 0;
	for (const named_t &entry : owned) {
		const QString label = QString::fromLatin1(entry.label);
		QLabel *name = new QLabel(label, this);
		own->addWidget(name, 0, column);
		own->addWidget(add_swatch(entry.target, label, label), 1, column);
		++column;
	}

	own->setColumnStretch(column, 1);
	outer->addLayout(own);

	/*
	 * Order and placement are the Windows Interface Guidelines': the
	 * destructive-but-recoverable reset on the left, away from the
	 * commit buttons, and OK/Cancel bottom right with Cancel the default
	 * so a stray Return does not accept a palette somebody was still
	 * looking at.
	 */
	QDialogButtonBox *buttons = new QDialogButtonBox(
	        QDialogButtonBox::Ok | QDialogButtonBox::Cancel | QDialogButtonBox::RestoreDefaults,
	        this);

	QPushButton *reset = buttons->button(QDialogButtonBox::RestoreDefaults);
	reset->setText(QStringLiteral("&Classic ANSI"));
	reset->setToolTip(QStringLiteral("Put the sixteen back to the standard values."));

	buttons->button(QDialogButtonBox::Ok)->setDefault(false);
	buttons->button(QDialogButtonBox::Cancel)->setDefault(true);

	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(reset, &QPushButton::clicked, this, &bssh_colour_editor::reset_to_classic);

	outer->addWidget(buttons);

	bssh_apply_touch_heights(this, false);
}

QToolButton *bssh_colour_editor::add_swatch(QRgb *target, const QString &label,
                                             const QString &tip) {
	QToolButton *swatch = new QToolButton(this);
	swatch->setToolTip(tip);

	/*
	 * The name goes to assistive technology whether or not it is painted:
	 * a grid of coloured squares is unusable to a screen reader otherwise,
	 * and "which red is this" is exactly the question it cannot answer by
	 * looking (sec 14).
	 */
	swatch->setAccessibleName(tip);
	if (!label.isEmpty()) swatch->setText(label);

	paint_swatch(swatch, *target);

	connect(swatch, &QToolButton::clicked, this, [this, target, swatch, tip] {
		pick(target, swatch, tip);
	});

	m_swatches.append(swatch);
	m_targets.append(target);

	return swatch;
}

void bssh_colour_editor::paint_swatch(QToolButton *swatch, QRgb colour) {
	QPixmap pixmap(BSSH_SWATCH_PX, BSSH_SWATCH_PX);
	pixmap.fill(QColor(colour));

	/*
	 * Outlined, or a black swatch on a dark theme and a white one on a
	 * light theme are both invisible -- and black and white are two of the
	 * sixteen, so that is not a corner case.
	 */
	QPainter painter(&pixmap);
	painter.setPen(QColor(0x80, 0x80, 0x80));
	painter.drawRect(0, 0, pixmap.width() - 1, pixmap.height() - 1);
	painter.end();

	swatch->setIcon(QIcon(pixmap));
	swatch->setIconSize(pixmap.size());
}

void bssh_colour_editor::pick(QRgb *target, QToolButton *swatch, const QString &label) {
	/*
	 * The system picker, not one written here. It is the control the user
	 * already knows, it remembers custom colours across the session, and
	 * on every platform it is the one place a colour is chosen.
	 */
	const QColor chosen = QColorDialog::getColor(
	        QColor(*target), this, QStringLiteral("Choose %1").arg(label.toLower()));

	if (!chosen.isValid()) return;

	*target = chosen.rgb();
	paint_swatch(swatch, *target);
}

void bssh_colour_editor::reset_to_classic() {
	/*
	 * The sixteen only. Background, text and cursor are what a person came
	 * here to change, and throwing those away with the palette would make
	 * the button too dangerous to press -- so it does the one thing its
	 * label says.
	 */
	const bssh_colour_scheme_t *classic =
	        bssh_colour_scheme_catalog::instance().by_id(QStringLiteral("default"));
	if (!classic) return;

	for (int i = 0; i < 16; ++i) m_scheme.ansi[i] = classic->ansi[i];

	for (int i = 0; i < m_swatches.size(); ++i) {
		paint_swatch(m_swatches.at(i), *m_targets.at(i));
	}
}
