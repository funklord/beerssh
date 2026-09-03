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
	while (m_lines.size() > m_max_lines) {
		m_lines.removeFirst();
		m_continuations.removeFirst();
		++m_base;
	}
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

	m_lines.append(line);
	m_continuations.append(continuation);
	while (m_lines.size() > m_max_lines) {
		m_lines.removeFirst();
		m_continuations.removeFirst();
		++m_base;
	}
}

bool bssh_scrollback::is_continuation(int index) const {
	if (index < 0 || index >= m_continuations.size()) return false;
	return m_continuations.at(index);
}

bool bssh_scrollback::pop(QList<bssh_cell_t> &line) {
	if (m_lines.isEmpty()) return false;

	line = m_lines.takeLast();
	m_continuations.removeLast();
	return true;
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
