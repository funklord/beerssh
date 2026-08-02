#ifndef BSSH_SCROLLBACK_H
#define BSSH_SCROLLBACK_H

#include <QList>

#include "term/screen.h"

/*
 * bssh_scrollback -- lines that have scrolled off the top (project.md
 * sec 7).
 *
 * A ring with a hard line cap, not an unbounded list. A terminal left
 * open on a chatty log is the normal case, not the pathological one, and
 * on a phone the difference between a bounded buffer and an unbounded one
 * is whether the app survives the day.
 *
 * Lines are stored as cells rather than as text, so scrolled-back output
 * keeps its colours and attributes. The cost is real -- a cell is ~20
 * bytes against ~1 for a character -- which is exactly why the cap
 * matters and why sec 10.2 makes the size a preset-owned setting.
 */
class bssh_scrollback {
public:
	explicit bssh_scrollback(int max_lines = 5000);

	void set_max_lines(int max_lines);
	int max_lines() const { return m_max_lines; }

	void push(const QList<bssh_cell_t> &line);

	/*
	 * Takes the most recent line back off, for when the screen scrolls
	 * down again -- libvterm asks for it when the alternate screen exits
	 * or a resize pulls content back onto the grid. Returns false when
	 * there is nothing to give back.
	 */
	bool pop(QList<bssh_cell_t> &line);

	void clear();

	int count() const { return m_lines.size(); }
	const QList<bssh_cell_t> &line(int index) const;

private:
	QList<QList<bssh_cell_t> > m_lines;
	int m_max_lines;
};

#endif /* BSSH_SCROLLBACK_H */
