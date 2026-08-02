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
	 * Placements scrolled entirely off the top are dropped: there is no
	 * scrollback for images yet, and holding every image a long-running
	 * session ever drew is how a phone runs out of memory.
	 */
	void scrolled(int top_row, int bottom_row, int lines);

	/* Dropped when the grid shrinks past them. */
	void trim_to(int rows);

	const QList<bssh_image_placement_t> &placements() const { return m_placements; }
	int count() const { return m_placements.size(); }

private:
	QList<bssh_image_placement_t> m_placements;
	int m_next_id;
};

#endif /* BSSH_IMAGE_STORE_H */
