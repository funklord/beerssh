#include "term/scrollback.h"

namespace {

const QList<bssh_cell_t> &empty_line() {
	static const QList<bssh_cell_t> empty;
	return empty;
}

} /* namespace */

bssh_scrollback::bssh_scrollback(int max_lines)
    : m_max_lines(qMax(0, max_lines)) {
}

void bssh_scrollback::set_max_lines(int max_lines) {
	m_max_lines = qMax(0, max_lines);

	/* Shrinking drops the oldest, which is what "keep the last N" means. */
	evict_beyond_cap();
}

void bssh_scrollback::evict_beyond_cap() {
	while (m_lines.size() > m_max_lines) {
		m_lines.removeFirst();
		m_continuations.removeFirst();
		++m_base;
	}
}

bool bssh_scrollback::is_blank(const bssh_cell_t &cell) {
	if (cell.ch != 0 && cell.ch != U' ') return false;
	if (cell.width != 1) return false;
	if ((cell.attrs & ~bssh_cell_t::UNWRITTEN) != bssh_cell_t::NONE) return false;
	if (cell.fg.kind != bssh_color_t::DEFAULT) return false;
	if (cell.bg.kind != bssh_color_t::DEFAULT) return false;
	if (cell.link != 0) return false;
	return true;
}

void bssh_scrollback::trim(QList<bssh_cell_t> &line) {
	int width = line.size();
	while (width > 0 && is_blank(line.at(width - 1))) --width;
	if (width < line.size()) line.resize(width);
}

void bssh_scrollback::push(const QList<bssh_cell_t> &line, bool continuation) {
	if (m_max_lines == 0) {
		/*
		 * Dropped the instant it arrives -- and dropping off the front is
		 * exactly what base() counts, so it counts this one too.
		 *
		 * Returning without counting froze the absolute numbering at
		 * zero for as long as scrollback was off, so `base() + count()`
		 * stopped being the number of lines that have gone past. The
		 * emulator measures a resize with that difference to keep images
		 * with their text (sec 7.1), and a frozen one is a difference of
		 * nought however far the screen scrolled.
		 *
		 * Unreachable until now, which is why it survived: the session
		 * discarded a scrollback size of 0 before it could arrive here.
		 */
		++m_base;
		return;
	}

	if (!continuation && !m_lines.isEmpty()) trim(m_lines.last());

	m_lines.append(line);
	m_continuations.append(continuation);
	evict_beyond_cap();
}

bool bssh_scrollback::is_continuation(int index) const {
	if (index < 0 || index >= m_continuations.size()) return false;
	return m_continuations.at(index);
}

bool bssh_scrollback::pop(QList<bssh_cell_t> &line, bool *continuation) {
	if (m_lines.isEmpty()) return false;

	line = m_lines.takeLast();
	const bool continued = m_continuations.takeLast();
	if (continuation) *continuation = continued;
	return true;
}

void bssh_scrollback::rewrap(int cols, bool keep_last) {
	if (cols <= 0 || m_lines.isEmpty()) return;

	int stop = m_lines.size();
	if (keep_last) {
		while (stop > 0 && m_continuations.at(stop - 1)) --stop;
		stop = qMax(0, stop - 1);
	}

	QList<QList<bssh_cell_t> > lines;
	QList<bool> continuations;
	lines.reserve(m_lines.size());
	continuations.reserve(m_lines.size());

	int index = 0;
	while (index < m_lines.size()) {
		if (index >= stop) {
			lines.append(m_lines.at(index));
			continuations.append(m_continuations.at(index));
			++index;
			continue;
		}
		int end = index + 1;
		while (end < m_lines.size() && m_continuations.at(end)) ++end;

		/* A line of one row that fits is kept as it is, shared, not copied. */
		if (end == index + 1 && m_lines.at(index).size() <= cols &&
		    (m_lines.at(index).isEmpty() || !is_blank(m_lines.at(index).last()))) {
			lines.append(m_lines.at(index));
			continuations.append(m_continuations.at(index));
			index = end;
			continue;
		}

		/*
		 * The empty cell a double-width glyph left at the edge when it
		 * wrapped is not text, and joined in it became a space: dropped
		 * where a row ends in an UNWRITTEN cell and the next starts with a
		 * wide glyph, as libvterm's reflow drops it on the screen
		 * (vterm-patch/0012). A space typed there is kept.
		 */
		QList<bssh_cell_t> joined;
		for (int row = index; row < end; ++row) {
			const QList<bssh_cell_t> &piece = m_lines.at(row);
			if (row > index && !joined.isEmpty() && (joined.last().attrs & bssh_cell_t::UNWRITTEN) &&
			    !piece.isEmpty() && piece.first().width == 2)
				joined.removeLast();
			/*
			 * Cell by cell, not `joined += piece`. Built -Os with
			 * QT_NO_DEBUG -- the app's flags -- GCC 14.2 left `joined`
			 * empty after that append, here and in no smaller program we
			 * could make, so every line longer than the new width went to
			 * the history as an empty row: the holder's "almost
			 * everything disappeared". The same file built with
			 * sanitizers, or at -O2 as the tests were, was correct. Not
			 * established whether it is a compiler fault or undefined
			 * behaviour neither sanitizer reports.
			 */
			joined.reserve(joined.size() + piece.size());
			for (const bssh_cell_t &cell : piece) joined.append(cell);
		}

		/* The blanks after the last row are not text; the ones before a
		 * continuation were (see push()). */
		trim(joined);

		bool first = true;
		int at = 0;
		do {
			int take = qMin(cols, int(joined.size()) - at);
			/*
			 * Never end a row on the first half of a wide glyph. The row
			 * is left a cell short rather than padded, because a blank
			 * stored there would be joined back into the text as a space.
			 */
			if (take > 1 && at + take < joined.size() && joined.at(at + take).width == 0) --take;
			lines.append(joined.mid(at, take));
			at += take;
			continuations.append(first ? m_continuations.at(index) : true);
			first = false;
		} while (at < joined.size());

		index = end;
	}

	m_lines = lines;
	m_continuations = continuations;
	evict_beyond_cap();
}

void bssh_scrollback::clear() {
	/*
	 * The base goes with them. A cleared line is gone rather than
	 * renumbered, so anything still holding a name for one must fail to
	 * find it rather than find a different line under the same name --
	 * which is exactly what resetting the base to zero would arrange.
	 */
	m_base += m_lines.size();
	m_lines.clear();
	m_continuations.clear();
}

const QList<bssh_cell_t> &bssh_scrollback::line(int index) const {
	if (index < 0 || index >= m_lines.size()) return empty_line();
	return m_lines.at(index);
}
