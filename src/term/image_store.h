#ifndef BSSH_IMAGE_STORE_H
#define BSSH_IMAGE_STORE_H

#include <QImage>
#include <QList>

/*
 * bssh_image_store -- images placed on the character grid (project.md
 * sec 7.1, tier 4).
 *
 * Three incompatible protocols put pixels in a terminal: sixel, kitty's
 * graphics protocol and iTerm2's inline images. They disagree about
 * transport, encoding and placement, but they agree on the *result* --
 * an image anchored at a cell position, occupying a rectangle of cells --
 * so that result is what this stores, and each protocol's parser is
 * reduced to producing one of these.
 *
 * Placements live beside the screen rather than inside a cell, because an
 * image is not a character: it spans cells, it survives the text under it
 * being rewritten, and on a phone it can be far larger than the grid.
 */
struct bssh_image_placement_t {
	/*
	 * The remote's own image id where the protocol has one (kitty), so a
	 * later "delete image 3" can find it. 0 means the protocol did not
	 * give one, which is the normal case for sixel and iTerm2.
	 */
	int id;

	QImage image;
	int row;
	int col;
	int cols;
	int rows;

	/*
	 * Negative draws under the text, which is how kitty lets a program
	 * put a background image behind a TUI.
	 */
	int z;

	bssh_image_placement_t() : id(0), row(0), col(0), cols(0), rows(0), z(0) {}
};

class bssh_image_store {
public:
	bssh_image_store();

	int place(const bssh_image_placement_t &placement);

	void remove(int id);
	void clear();

	/*
	 * Follows the text when the screen scrolls. Without this an image
	 * stays pinned to its original row while the output around it moves,
	 * which looks like the terminal has come apart.
	 *
	 * A placement scrolled off the top keeps going, into NEGATIVE rows:
	 * row -3 means three lines above the live screen, which is where the
	 * text it belongs to now is. That is image scrollback (sec 7), and it
	 * is what stops a picture vanishing the moment one more line of output
	 * arrives -- while the text it was drawn beside is still there to
	 * scroll back to.
	 *
	 * What bounds it is the memory budget below, not the scroll: holding
	 * every image a long-running session ever drew is how a phone runs
	 * out of memory, and a count of images is the wrong bound because one
	 * screenshot can outweigh a thousand sparklines.
	 */
	void scrolled(int top_row, int bottom_row, int lines);

	/*
	 * How many bytes of decoded image to keep, across the visible screen
	 * and the scrollback together. Once over it, the images furthest back
	 * are dropped first -- the ones the user would have to scroll longest
	 * to reach, and the ones least likely to be looked at again.
	 *
	 * Anything still ON the live screen is never dropped, however big: a
	 * budget that could blank the picture currently being looked at is a
	 * budget that has misunderstood its job.
	 */
	void set_memory_budget(qint64 bytes);

	/*
	 * Drop every placement that has gone entirely above the live screen
	 * -- the pictures that were in the scrollback (sec 6.4). Called when
	 * the history is dropped, because an image scrolled off the top is
	 * in the history as much as the text around it, and with the history
	 * gone there is no route left that could ever draw it.
	 *
	 * The same test enforce_budget uses for "above the fold", so a
	 * picture still touching the screen is as safe here as it is there.
	 */
	void drop_above_the_fold();
	qint64 memory_budget() const { return m_budget; }
	qint64 memory_used() const;

	/* Dropped when the grid shrinks past them. */
	/*
	 * Every placement moves up by `lines`, including the ones already
	 * above the fold. For a RESIZE, which pushes lines into the
	 * scrollback the way a scroll does but reports it through
	 * sb_pushline rather than moverect -- so scrolled() never hears
	 * about it.
	 *
	 * Unlike scrolled() this has no region: a resize moves the whole
	 * buffer past the screen, so an image already at row -3 is at -8
	 * after five more lines go by. scrolled() deliberately leaves those
	 * alone, because a scrolling REGION does not move what is outside
	 * it.
	 */
	void shift_rows(int lines);

	void trim_to(int rows);

	const QList<bssh_image_placement_t> &placements() const { return m_placements; }
	int count() const { return m_placements.size(); }

private:
	void enforce_budget();

	/* Removes every earlier placement the one at `index` paints over. */
	void drop_covered_by(int index);

	QList<bssh_image_placement_t> m_placements;
	qint64 m_budget;
};

#endif /* BSSH_IMAGE_STORE_H */
