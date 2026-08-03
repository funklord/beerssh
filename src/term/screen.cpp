#include "term/screen.h"

#include <cstring>

namespace {

/* Returned for any out-of-bounds read, so callers need no null check. */
const bssh_cell_t &blank_cell() {
	static const bssh_cell_t blank;
	return blank;
}

} /* namespace */

bssh_screen::bssh_screen(int cols, int rows)
    : m_cols(qMax(1, cols)),
      m_rows(qMax(1, rows)),
      m_cursor_row(0),
      m_cursor_col(0),
      m_cursor_visible(true) {
	m_cells.resize(m_cols * m_rows);
}

bool bssh_screen::in_bounds(int row, int col) const {
	return row >= 0 && row < m_rows && col >= 0 && col < m_cols;
}

const bssh_cell_t &bssh_screen::cell(int row, int col) const {
	if (!in_bounds(row, col)) return blank_cell();
	return m_cells.at(row * m_cols + col);
}

void bssh_screen::set_cell(int row, int col, const bssh_cell_t &cell) {
	if (!in_bounds(row, col)) return;
	m_cells[row * m_cols + col] = cell;
}

void bssh_screen::resize(int cols, int rows) {
	cols = qMax(1, cols);
	rows = qMax(1, rows);
	if (cols == m_cols && rows == m_rows) return;

	/*
	 * Copied cell by cell into a fresh grid rather than resized in place:
	 * the backing store is row-major, so a change in width moves every
	 * row's start and an in-place resize would shear the contents.
	 */
	QList<bssh_cell_t> next;
	next.resize(cols * rows);

	const int copy_rows = qMin(rows, m_rows);
	const int copy_cols = qMin(cols, m_cols);
	for (int row = 0; row < copy_rows; ++row) {
		for (int col = 0; col < copy_cols; ++col) {
			next[row * cols + col] = m_cells.at(row * m_cols + col);
		}
	}

	m_cells = next;
	m_cols = cols;
	m_rows = rows;

	/* Keep the cursor on the grid it is now addressing. */
	set_cursor(m_cursor_row, m_cursor_col);
}

void bssh_screen::clear() {
	m_cells.fill(bssh_cell_t());
}

void bssh_screen::move_rect(int dest_row, int dest_col, int src_row, int src_col,
                             int rows, int cols) {
	if (rows <= 0 || cols <= 0) return;

	/*
	 * Row by row, in the direction that does not overwrite the source
	 * before it has been read: downwards when moving down, upwards when
	 * moving up. Scrolling text up the screen is the common case and it
	 * overlaps by every row but one.
	 */
	const bool downwards = dest_row > src_row;

	for (int i = 0; i < rows; ++i) {
		const int step = downwards ? rows - 1 - i : i;
		const int from = src_row + step;
		const int to = dest_row + step;

		if (from < 0 || from >= m_rows || to < 0 || to >= m_rows) continue;

		const int width = qMin(cols, m_cols - qMax(src_col, dest_col));
		if (width <= 0) continue;

		std::memmove(m_cells.data() + to * m_cols + dest_col,
		              m_cells.constData() + from * m_cols + src_col,
		              size_t(width) * sizeof(bssh_cell_t));
	}
}

void bssh_screen::clear_row(int row) {
	if (row < 0 || row >= m_rows) return;
	for (int col = 0; col < m_cols; ++col) {
		m_cells[row * m_cols + col] = bssh_cell_t();
	}
}

void bssh_screen::set_cursor(int row, int col) {
	m_cursor_row = qBound(0, row, m_rows - 1);
	m_cursor_col = qBound(0, col, m_cols - 1);
}

QString bssh_screen::to_text() const {
	QStringList lines;
	lines.reserve(m_rows);

	for (int row = 0; row < m_rows; ++row) {
		QString line;
		line.reserve(m_cols);
		for (int col = 0; col < m_cols; ++col) {
			const bssh_cell_t &c = cell(row, col);
			/* Skip the continuation half of a double-width glyph. */
			if (c.width == 0) continue;
			line.append(QString::fromUcs4(&c.ch, 1));
		}
		while (line.endsWith(QLatin1Char(' '))) {
			line.chop(1);
		}
		lines.append(line);
	}

	return lines.join(QLatin1Char('\n'));
}
