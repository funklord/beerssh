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
	}
}

void bssh_scrollback::push(const QList<bssh_cell_t> &line) {
	if (m_max_lines == 0) return;

	m_lines.append(line);
	while (m_lines.size() > m_max_lines) {
		m_lines.removeFirst();
	}
}

bool bssh_scrollback::pop(QList<bssh_cell_t> &line) {
	if (m_lines.isEmpty()) return false;

	line = m_lines.takeLast();
	return true;
}

void bssh_scrollback::clear() {
	m_lines.clear();
}

const QList<bssh_cell_t> &bssh_scrollback::line(int index) const {
	if (index < 0 || index >= m_lines.size()) return empty_line();
	return m_lines.at(index);
}
