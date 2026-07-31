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

	/*
	 * Compute the grid up front rather than waiting for the first
	 * resizeEvent. Qt does not deliver one to a widget that has never
	 * been shown, so without this the view reports a 0x0 grid until it is
	 * mapped -- and anything that arrives in the meantime (an SSH banner
	 * beats the first paint comfortably) gets trimmed away against a row
	 * count of zero. The emission is a no-op here: nothing can be
	 * connected to a widget still inside its own constructor.
	 */
	recompute_grid();
}

void bssh_terminal_view::set_terminal_font(const QFont &font) {
	m_font = font;
	update_metrics();
	recompute_grid();
	update();
}

void bssh_terminal_view::feed(const QByteArray &bytes) {
	if (m_lines.isEmpty()) m_lines.append(QString());

	/*
	 * Latin-1, not UTF-8, and that is not an oversight to fix here: doing
	 * it properly means decoding across chunk boundaries (a multi-byte
	 * sequence split between two reads is the normal case, not the edge
	 * one) and knowing each character's cell width so CJK occupies two
	 * cells (sec 9.3). Both belong to the emulator at M2. Byte-per-cell
	 * is the honest placeholder until then -- it is obviously wrong for
	 * non-ASCII rather than subtly wrong.
	 */
	const QString text = QString::fromLatin1(bytes);
	for (const QChar &c : text) {
		if (c == QLatin1Char('\n')) {
			m_lines.append(QString());
		} else if (c == QLatin1Char('\r')) {
			/* Bare CR: start of line, no new line. */
			m_lines.last().clear();
		} else if (c.isPrint() || c == QLatin1Char('\t')) {
			m_lines.last().append(c);
		}
		/* Everything else -- escapes, control bytes -- is dropped on the
		 * floor at M1 rather than rendered as garbage. */
	}

	/*
	 * Keep only what can be shown. There is no scrollback until M2, and
	 * an unbounded buffer here would be a slow leak on a chatty session
	 * for no visible benefit.
	 */
	const int keep = qMax(1, m_rows);
	while (m_lines.size() > keep) {
		m_lines.removeFirst();
	}

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

	/*
	 * Drawn one cell at a time, which is how the emulator's row renderer
	 * will draw at M2 as well: a font whose advance width does not match
	 * the cell then shows up as visibly uneven spacing, rather than as a
	 * subtle misalignment nobody notices until a TUI is on screen.
	 *
	 * The per-glyph loop is also deliberately NOT the shape to optimise
	 * yet -- sec 9.5's shared glyph cache and per-row runs are M9, and
	 * guessing at them now would just be a cache to throw away.
	 */
	const QStringList rendered = m_lines.isEmpty()
	                              ? QStringList(QString::fromLatin1(BSSH_PLACEHOLDER))
	                              : m_lines;

	for (int row = 0; row < rendered.size() && row < m_rows; ++row) {
		const QString &line = rendered.at(row);
		const qreal baseline = m_baseline + row * m_cell_height;

		for (int col = 0; col < line.size() && col < m_cols; ++col) {
			painter.drawText(QPointF(col * m_cell_width, baseline), QString(line.at(col)));
		}
	}

	/* Block cursor after the last line, so the cell size is visible as a shape. */
	const int cursor_row = qMin(rendered.size() - 1, m_rows - 1);
	const int cursor_col = qMin(rendered.last().size(), m_cols - 1);
	painter.fillRect(cursor_col * m_cell_width, cursor_row * m_cell_height,
	                  m_cell_width, m_cell_height, m_foreground);
}
