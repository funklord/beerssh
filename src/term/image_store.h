#ifndef BSSH_IMAGE_STORE_H
#define BSSH_IMAGE_STORE_H

#include <QHash>
#include <QImage>
#include <QList>
#include <QSet>

#include <memory>

class bssh_picture_spill;
class QObject;

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

	/*
	 * Null once the picture has been spilled (see set_memory_budget);
	 * bssh_image_store::image() is how a reader gets it either way.
	 */
	QImage image;

	/* The store's own name for this placement, never reused. */
	quint64 serial;

	int row;
	int col;
	int cols;
	int rows;

	/*
	 * Negative draws under the text, which is how kitty lets a program
	 * put a background image behind a TUI.
	 */
	int z;

	bssh_image_placement_t() : id(0), serial(0), row(0), col(0), cols(0), rows(0), z(0) {}
};

class bssh_image_store {
public:
	bssh_image_store();
	~bssh_image_store();

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
	 * `lines` more lines went into the scrollback: every placement wholly
	 * above the screen moves that much further back. scrolled() leaves
	 * those alone -- they are outside any scrolling region -- so without
	 * this a picture stopped at the row it first left the screen from.
	 * Called per push, BEFORE the scroll that follows it, so a picture
	 * straddling the top edge is moved by exactly one of the two.
	 * Returns whether anything moved.
	 */
	bool pushed_into_history(int lines);

	/*
	 * How many bytes of decoded image to keep IN MEMORY, across the
	 * visible screen and the scrollback together. Once over it, the
	 * images furthest back are spilled first -- written out, sealed, to
	 * bssh_picture_spill and dropped from memory -- the ones the user
	 * would have to scroll longest to reach. Scrolling to one reads it
	 * back (image()). 0 means no limit.
	 *
	 * Anything still ON the live screen is never spilled, however big: a
	 * budget that could blank the picture currently being looked at is a
	 * budget that has misunderstood its job.
	 *
	 * Where a spill cannot be made -- no cache directory, a full disk --
	 * the picture is dropped instead, which is what this did before
	 * there was anywhere to put it.
	 */
	void set_memory_budget(qint64 bytes);

	/*
	 * How many bytes of spilled pictures to keep on disk. Past it, the
	 * spilled ones furthest back are forgotten altogether. A disk is not
	 * a phone's memory, but it is not endless either, and a session
	 * drawing a plot a second for a day would otherwise find its end.
	 */
	void set_disk_budget(qint64 bytes);
	qint64 disk_budget() const { return m_disk_budget; }
	qint64 disk_used() const;

	/*
	 * Where spills go; bssh_picture_spill::default_base() unless a test
	 * says otherwise. Takes effect for the next spill only if none has
	 * been made yet.
	 */
	void set_spill_base(const QString &base);

	/*
	 * The picture, whether it is in memory or was spilled. A spilled one
	 * is read back and kept while it fits the budget beside what is
	 * resident, the most recently drawn kept longest -- so a picture
	 * scrolled to stays drawn while it is looked at, and scrolling on
	 * lets it go again. Null only when a spill could not be read back.
	 *
	 * const, because the renderer holds the store const: what it changes
	 * is a cache, not the placements.
	 */
	QImage image(const bssh_image_placement_t &placement) const;

	bool is_spilled(const bssh_image_placement_t &placement) const;

	/*
	 * Whether a spill is still being sealed on the worker. The picture
	 * stays in memory, and drawable, until it lands; a test waits on this
	 * rather than on time.
	 */
	bool spills_pending() const { return !m_spilling.isEmpty(); }

	/*
	 * Forget every placement that has scrolled back past `history_lines`
	 * lines above the screen -- the text it was drawn beside has left the
	 * scrollback, so no scroll can reach it again. A picture is not
	 * history the text is not.
	 */
	void forget_beyond(int history_lines);

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
	void enforce_disk_budget();

	/*
	 * Sends the picture at `index` to be sealed off the thread, or drops
	 * it where nothing can be spilled; finish_spill takes the result back.
	 */
	void begin_spill(int index);
	void finish_spill(quint64 serial, const QByteArray &sealed);

	/* Removes the placement at `index`, and its spill if it has one. */
	void discard(int index);

	/* Keeps restored pictures within what the budget leaves over. */
	void trim_restored(quint64 keep) const;
	qint64 resident_bytes() const;

	/* Removes every earlier placement the one at `index` paints over. */
	void drop_covered_by(int index);

	QList<bssh_image_placement_t> m_placements;
	qint64 m_budget;
	qint64 m_disk_budget;
	quint64 m_next_serial;
	QString m_spill_base;
	std::unique_ptr<bssh_picture_spill> m_spill;

	/* Serials being sealed on the worker; still resident meanwhile. */
	QSet<quint64> m_spilling;

	/*
	 * Lives exactly as long as this store, so a seal finishing after the
	 * terminal has closed finds it gone and delivers nothing.
	 */
	std::unique_ptr<QObject> m_alive;

	/* Spilled pictures read back, and the order they were last drawn. */
	mutable QHash<quint64, QImage> m_restored;
	mutable QList<quint64> m_restored_order;
};

#endif /* BSSH_IMAGE_STORE_H */
