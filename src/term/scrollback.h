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
 *
 * Lines here are RAGGED, not grid-width: the emulator drops trailing
 * blanks before pushing, so a short line costs what it says rather than
 * what the window is wide (sec 9.5). Every reader must therefore bound
 * its column against the line's own size() -- line() returning an empty
 * list for an out-of-range index is the same contract one step up.
 */
class bssh_scrollback {
public:
	explicit bssh_scrollback(int max_lines = 5000);

	void set_max_lines(int max_lines);
	int max_lines() const { return m_max_lines; }

	/*
	 * `continuation` says this line is the tail of the one before it --
	 * a single logical line that ran past the right edge, not two lines.
	 * libvterm knows, and says so through sb_pushline4.
	 *
	 * Kept because COPY needs it: joining every row with a newline turns
	 * one wrapped command into two, and pasting that into a shell runs
	 * both halves. The trailing-blank trimming in selection_text() exists
	 * for the same reason and covers the smaller half of it.
	 */
	void push(const QList<bssh_cell_t> &line, bool continuation = false);

	/* Whether the line at `index` continues the one before it. */
	bool is_continuation(int index) const;

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

	/*
	 * How many lines this ring has dropped off the front, which makes
	 * `base() + index` a name for a line that does not change when the
	 * ring evicts.
	 *
	 * Without it "absolute line" was a lie the moment the buffer filled:
	 * every push renumbered every line by one, while a selection anchored
	 * against the old numbers did not move -- so a selection made in the
	 * history walked down the content, one line per line of output, and
	 * Copy yielded text the user never highlighted. It was invisible
	 * because the painting agreed with the anchor: what had moved was the
	 * highlight itself.
	 */
	qint64 base() const { return m_base; }

	/* How many lines have left the top, carried when a terminal moves to
	 * another process (bssh_emulator::import_state) so positions counted
	 * from it keep meaning the same lines. */
	void set_base(qint64 base) { m_base = base; }

private:
	QList<QList<bssh_cell_t> > m_lines;

	/* Parallel to m_lines; see push(). */
	QList<bool> m_continuations;
	int m_max_lines;

	/* Only ever grows; see base(). */
	qint64 m_base = 0;
};

#endif /* BSSH_SCROLLBACK_H */
