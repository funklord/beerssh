#include "ui/terminal_view.h"

#include <QFontDatabase>
#include <QFontMetricsF>
#include <QPainter>
#include <QResizeEvent>

/*
 * The placeholder drawn into the empty grid. It exists to prove the cell
 * metrics are right -- it is painted one cell at a time, exactly the way
 * the emulator's row renderer will paint at M2, so a wrong advance width
 * shows up as visibly uneven spacing rather than as a subtle bug later.
 */
static const char *const BSSH_PLACEHOLDER = "BeerSSH -- no session (M0 skeleton)";

bssh_terminal_view::bssh_terminal_view(QWidget *parent)
    : QWidget(parent),
      m_background(QColor(0x1c, 0x1c, 0x1c)),
      m_foreground(QColor(0xd0, 0xd0, 0xd0)),
      m_cell_width(1),
      m_cell_height(1),
      m_baseline(0),
      m_cols(0),
      m_rows(0) {
	/*
	 * The curated bundled set arrives at M9 (sec 9.2); until then the
	 * platform's own fixed-pitch font is the honest placeholder. Asking
	 * for the system fixed font rather than naming a family is what keeps
	 * this working identically on a phone and a workstation.
	 */
	m_font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
	m_font.setStyleHint(QFont::Monospace);
	m_font.setFixedPitch(true);

	setFocusPolicy(Qt::StrongFocus);
	setAttribute(Qt::WA_OpaquePaintEvent);
	update_metrics();
}

void bssh_terminal_view::set_terminal_font(const QFont &font) {
	m_font = font;
	update_metrics();
	recompute_grid();
	update();
}

void bssh_terminal_view::update_metrics() {
	const QFontMetricsF metrics(m_font);

	/*
	 * Rounded, never fractional: the grid has to land on whole pixels or
	 * the accumulated error across a wide window blurs the right-hand
	 * columns (sec 9.5). Fractional display scaling (125%, 150%) is
	 * exactly where this matters, which is a desktop case Android never
	 * exercises.
	 *
	 * 'W' is the measured glyph rather than the font's averageCharWidth,
	 * which is an average over a proportional-ish sample and is wrong for
	 * some fonts that ARE monospace. A font whose glyphs disagree about
	 * advance width is not a terminal font at all -- sec 9.6's CI check
	 * exists to keep any such font out of the bundled set.
	 */
	m_cell_width = qMax(1, qRound(metrics.horizontalAdvance(QLatin1Char('W'))));
	m_cell_height = qMax(1, qRound(metrics.height()));
	m_baseline = qRound(metrics.ascent());
}

void bssh_terminal_view::recompute_grid() {
	const int cols = qMax(1, width() / m_cell_width);
	const int rows = qMax(1, height() / m_cell_height);

	if (cols == m_cols && rows == m_rows) return;

	m_cols = cols;
	m_rows = rows;
	emit grid_resized(m_cols, m_rows);
}

void bssh_terminal_view::resizeEvent(QResizeEvent *event) {
	QWidget::resizeEvent(event);
	recompute_grid();
}

void bssh_terminal_view::paintEvent(QPaintEvent *event) {
	Q_UNUSED(event);

	QPainter painter(this);
	painter.fillRect(rect(), m_background);
	painter.setFont(m_font);
	painter.setPen(m_foreground);

	const QString placeholder = QString::fromLatin1(BSSH_PLACEHOLDER);
	for (int i = 0; i < placeholder.size() && i < m_cols; ++i) {
		painter.drawText(QPointF(i * m_cell_width, m_baseline), QString(placeholder.at(i)));
	}

	/* Block cursor on the next row, so the cell size is visible as a shape. */
	painter.fillRect(0, m_cell_height, m_cell_width, m_cell_height, m_foreground);
}
