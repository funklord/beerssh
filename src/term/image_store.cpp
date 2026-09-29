#include "term/image_store.h"
#include "term/picture_spill.h"

#include <QCoreApplication>
#include <QObject>
#include <QPointer>
#include <QThreadPool>

/*
 * 32 MB of decoded image, which is roughly a dozen full-screen phone
 * screenshots or a very great many plots. Chosen to be generous enough
 * that normal use never meets it and small enough that a session left
 * running all day cannot quietly become the reason the app is killed
 * (sec 5.1). It is a per-session budget: the count that matters to
 * Android is the process total, and sessions are what multiply.
 *
 * Only the starting value now: the user sets it per terminal on the
 * settings page ("Picture history", the holder's answer of 2026-09-26),
 * whose own default is this same 32.
 */
static const qint64 BSSH_DEFAULT_IMAGE_BUDGET = 32 * 1024 * 1024;

/*
 * 256 MB of sealed, compressed pictures on disk per terminal. Terminal
 * pictures compress well -- plots are mostly flat colour -- so this is
 * far more than 256 MB of what was drawn, and it is the ceiling a
 * session drawing all day reaches rather than a figure anyone meets.
 */
static const qint64 BSSH_DEFAULT_SPILL_BUDGET = 256 * 1024 * 1024;

bssh_image_store::bssh_image_store()
    : m_budget(BSSH_DEFAULT_IMAGE_BUDGET),
      m_disk_budget(BSSH_DEFAULT_SPILL_BUDGET),
      m_next_serial(0),
      m_spill_base(bssh_picture_spill::default_base()),
      m_alive(new QObject()) {
}

/* Out of line, where bssh_picture_spill is a complete type. */
bssh_image_store::~bssh_image_store() {
}

void bssh_image_store::set_spill_base(const QString &base) {
	m_spill_base = base;

	/* A spill that has written nothing yet has nothing to move. */
	if (m_spill && m_spill->directory().isEmpty()) m_spill.reset();
}

int bssh_image_store::place(const bssh_image_placement_t &given) {
	if (given.image.isNull()) return 0;

	bssh_image_placement_t placement = given;
	placement.serial = ++m_next_serial;

	/*
	 * An id is the REMOTE's id, and an anonymous placement stays
	 * anonymous -- which is what the field's own comment in the header
	 * has always said: "0 means the protocol did not give one, which is
	 * the normal case for sixel and iTerm2".
	 *
	 * This handed those an id anyway, out of a counter of its own, into
	 * the one namespace kitty's ids live in. The counter started at 1 and
	 * kitty's numbering starts at 1, so the two collided from the first
	 * image each side drew: in a session where both protocols are used --
	 * a plot from one tool, a picture from another -- `a=T,i=1` replaced
	 * a sixel frame the terminal had numbered 1, and `a=d,i=1` deleted
	 * it. Neither program had ever heard of the other, and neither has
	 * any way to find out: nothing in either protocol reports the number
	 * a terminal invented for somebody else's image.
	 *
	 * Nothing needed the invented number. The covering test, the budget,
	 * the scroll and the trim all work on geometry, and place()'s return
	 * value has no caller at all.
	 */
	if (placement.id != 0) {
		/*
		 * A protocol may re-transmit an existing id to replace the image
		 * rather than to add a second copy of it.
		 */
		for (int i = 0; i < m_placements.size(); ++i) {
			if (m_placements.at(i).id == placement.id) {
				/* The picture it replaces may have been spilled. */
				const quint64 replaced = m_placements.at(i).serial;
				if (m_spill) m_spill->remove(replaced);
				m_spilling.remove(replaced);
				m_restored.remove(replaced);
				m_restored_order.removeAll(replaced);

				m_placements[i] = placement;
				drop_covered_by(i);
				return placement.id;
			}
		}
	}

	m_placements.append(placement);
	drop_covered_by(m_placements.size() - 1);
	return placement.id;
}

/*
 * A placement drawn entirely over by a later one is dead: the renderer
 * walks this list in order and paints the later one on top, so not one
 * of its pixels can reach the screen again.
 *
 * Without this, sixel ANIMATION grows without limit. Sixel carries no
 * image id, so no frame can replace an earlier one and `place` simply
 * appends each -- and a program that redraws in place, which is what an
 * animation is, never scrolls. enforce_budget() cannot help: it drops only what is
 * above the fold, and each frame is on screen by construction, so the
 * whole stack of them is exactly the case it declines to touch. The
 * budget's own comment says it exists so "a session left running all day
 * cannot quietly become the reason the app is killed", and this is the
 * way that happened anyway.
 *
 * Dropping these is not the thing enforce_budget() refuses to do. That
 * refusal is about not deleting an image the user is reading; this one is
 * invisible by definition, and if it were not, the covering test would be
 * wrong rather than the policy.
 *
 * z is compared by SIDE, not by value: below-text images are painted in a
 * separate pass, so one of those is not covered by an image above the
 * text however exactly it overlaps.
 */
void bssh_image_store::drop_covered_by(int index) {
	const bssh_image_placement_t cover = m_placements.at(index);

	/* Nothing covers anything without area of its own. */
	if (cover.rows <= 0 || cover.cols <= 0) return;

	for (int i = index - 1; i >= 0; --i) {
		const bssh_image_placement_t &under = m_placements.at(i);

		if ((under.z < 0) != (cover.z < 0)) continue;

		if (under.row < cover.row) continue;
		if (under.col < cover.col) continue;
		if (under.row + under.rows > cover.row + cover.rows) continue;
		if (under.col + under.cols > cover.col + cover.cols) continue;

		discard(i);
	}
}

void bssh_image_store::remove(int id) {
	/*
	 * Anonymous placements answer to no id; see place(). Without this,
	 * "delete image 0" is a request every sixel frame in the buffer
	 * matches at once.
	 */
	if (id == 0) return;

	for (int i = m_placements.size() - 1; i >= 0; --i) {
		if (m_placements.at(i).id == id) discard(i);
	}
}

void bssh_image_store::clear() {
	for (int i = m_placements.size() - 1; i >= 0; --i) discard(i);
}

void bssh_image_store::discard(int index) {
	const quint64 serial = m_placements.at(index).serial;

	if (m_spill) m_spill->remove(serial);
	m_spilling.remove(serial);
	m_restored.remove(serial);
	m_restored_order.removeAll(serial);
	m_placements.removeAt(index);
}

void bssh_image_store::scrolled(int top_row, int bottom_row, int lines) {
	if (lines == 0) return;

	for (int i = m_placements.size() - 1; i >= 0; --i) {
		bssh_image_placement_t &placement = m_placements[i];

		/* Only what is inside the scrolling region moves. */
		if (placement.row + placement.rows <= top_row) continue;
		if (placement.row >= bottom_row) continue;

		placement.row -= lines;
	}

	/*
	 * Nothing is dropped for having scrolled off the top -- that is the
	 * scrollback. What drops images is the budget, applied here because a
	 * scroll is the only moment new ones arrive above the fold.
	 */
	enforce_budget();
}

bool bssh_image_store::pushed_into_history(int lines) {
	bool moved = false;

	for (int i = 0; i < m_placements.size(); ++i) {
		bssh_image_placement_t &placement = m_placements[i];
		if (placement.row + placement.rows > 0) continue;

		placement.row -= lines;
		moved = true;
	}

	return moved;
}

void bssh_image_store::set_memory_budget(qint64 bytes) {
	m_budget = qMax(qint64(0), bytes);
	enforce_budget();
}

void bssh_image_store::set_disk_budget(qint64 bytes) {
	m_disk_budget = qMax(qint64(0), bytes);
	enforce_disk_budget();
}

qint64 bssh_image_store::disk_used() const {
	return m_spill ? m_spill->bytes() : 0;
}

qint64 bssh_image_store::resident_bytes() const {
	qint64 total = 0;
	for (const bssh_image_placement_t &placement : m_placements) {
		total += placement.image.sizeInBytes();
	}
	return total;
}

qint64 bssh_image_store::memory_used() const {
	qint64 total = resident_bytes();
	for (const QImage &image : m_restored) total += image.sizeInBytes();
	return total;
}

bool bssh_image_store::is_spilled(const bssh_image_placement_t &placement) const {
	return placement.image.isNull() && m_spill && m_spill->contains(placement.serial);
}

QImage bssh_image_store::image(const bssh_image_placement_t &placement) const {
	if (!placement.image.isNull()) return placement.image;

	const auto found = m_restored.constFind(placement.serial);
	if (found != m_restored.constEnd()) {
		m_restored_order.removeAll(placement.serial);
		m_restored_order.append(placement.serial);
		return found.value();
	}

	if (!m_spill) return QImage();

	const QImage image = m_spill->read(placement.serial);
	if (image.isNull()) return QImage();

	m_restored.insert(placement.serial, image);
	m_restored_order.append(placement.serial);
	trim_restored(placement.serial);
	return image;
}

/*
 * Least recently drawn first, and never `keep` -- the one being drawn
 * now, which is held while it is on screen however the budget stands,
 * for the same reason enforce_budget never spills what is visible.
 */
void bssh_image_store::trim_restored(quint64 keep) const {
	if (m_budget <= 0) return;

	qint64 used = memory_used();
	int next = 0;

	while (used > m_budget && next < m_restored_order.size()) {
		const quint64 serial = m_restored_order.at(next);
		if (serial == keep) {
			++next;
			continue;
		}

		used -= m_restored.value(serial).sizeInBytes();
		m_restored.remove(serial);
		m_restored_order.removeAt(next);
	}
}

void bssh_image_store::forget_beyond(int history_lines) {
	if (history_lines < 0) return;

	for (int i = m_placements.size() - 1; i >= 0; --i) {
		const bssh_image_placement_t &placement = m_placements.at(i);
		if (placement.row + placement.rows <= -history_lines) discard(i);
	}
}

void bssh_image_store::drop_above_the_fold() {
	for (int i = m_placements.size() - 1; i >= 0; --i) {
		const bssh_image_placement_t &placement = m_placements.at(i);
		if (placement.row + placement.rows > 0) continue;
		discard(i);
	}
}

void bssh_image_store::enforce_budget() {
	if (m_budget <= 0) return;

	/* What was read back is on disk already, so it goes first and free. */
	trim_restored(0);

	/*
	 * What is being sealed already counts as gone: it will be, and
	 * counting it again would send the next picture after it for the same
	 * bytes.
	 */
	qint64 staying = 0;
	for (const bssh_image_placement_t &placement : m_placements) {
		if (!m_spilling.contains(placement.serial)) staying += placement.image.sizeInBytes();
	}

	while (staying > m_budget) {
		/*
		 * The one furthest above the screen goes first: it is the oldest
		 * and the one a user would have to scroll longest to reach.
		 */
		int oldest = -1;
		int oldest_row = 0;

		for (int i = 0; i < m_placements.size(); ++i) {
			const bssh_image_placement_t &placement = m_placements.at(i);

			/* Already out of memory, or on its way. */
			if (placement.image.isNull() || m_spilling.contains(placement.serial)) continue;

			/* Never the one being looked at, whatever it weighs. */
			if (placement.row + placement.rows > 0) continue;

			if (oldest < 0 || placement.row < oldest_row) {
				oldest = i;
				oldest_row = placement.row;
			}
		}

		/*
		 * Everything left is on screen. Over budget and unable to help
		 * it is the honest state: dropping a visible image to satisfy a
		 * number would be a terminal deleting what the user is reading.
		 */
		if (oldest < 0) break;

		staying -= m_placements.at(oldest).image.sizeInBytes();
		begin_spill(oldest);
	}

	enforce_disk_budget();
}

/*
 * Spilled rather than dropped, so scrolling back still finds it -- and
 * sealed on a worker, because compressing and encrypting a large picture
 * took 160 ms on the emulator, on the thread the output arrives on. The
 * file is written back on this thread (store() is quick); only the pure
 * half leaves it.
 *
 * Where it cannot be spilled, dropped as it always was: a budget is a
 * promise about memory, and keeping the picture because the disk said no
 * would break it.
 */
void bssh_image_store::begin_spill(int index) {
	const bssh_image_placement_t &placement = m_placements.at(index);
	const quint64 serial = placement.serial;

	if (!m_spill && !m_spill_base.isEmpty())
		m_spill.reset(new bssh_picture_spill(m_spill_base));

	const std::function<QByteArray()> job =
	        m_spill ? m_spill->prepare(serial, placement.image) : nullptr;
	if (!job) {
		discard(index);
		return;
	}

	m_spilling.insert(serial);

	/* No event loop to come back on: seal it here. */
	if (!QCoreApplication::instance()) {
		finish_spill(serial, job());
		return;
	}

	/*
	 * One worker for every terminal's spills, so they queue rather than
	 * take every core while output is arriving -- and each queued one is a
	 * picture that is still resident, so the queue is bounded by the same
	 * memory it is freeing.
	 */
	static QThreadPool *pool = [] {
		QThreadPool *made = new QThreadPool();
		made->setMaxThreadCount(1);
		return made;
	}();

	const QPointer<QObject> alive(m_alive.get());
	bssh_image_store *store = this;
	pool->start([job, serial, alive, store]() {
		const QByteArray sealed = job();

		/* Gone at exit, with a seal still queued: nothing to deliver to. */
		QCoreApplication *app = QCoreApplication::instance();
		if (!app) return;

		QMetaObject::invokeMethod(app, [alive, store, serial, sealed]() {
			if (alive) store->finish_spill(serial, sealed);
		}, Qt::QueuedConnection);
	});
}

void bssh_image_store::finish_spill(quint64 serial, const QByteArray &sealed) {
	/* Dropped, replaced or cleared while it was being sealed. */
	if (!m_spilling.remove(serial)) return;

	int index = -1;
	for (int i = 0; i < m_placements.size(); ++i) {
		if (m_placements.at(i).serial == serial) index = i;
	}
	if (index < 0) return;

	/*
	 * Scrolled back onto the screen while it was away: it stays, by the
	 * rule enforce_budget chose it under, and is chosen again if it goes.
	 */
	bssh_image_placement_t &placement = m_placements[index];
	if (placement.row + placement.rows > 0) return;

	if (m_spill && m_spill->store(serial, sealed)) {
		placement.image = QImage();
	} else {
		discard(index);
	}

	enforce_disk_budget();
}

void bssh_image_store::enforce_disk_budget() {
	if (m_disk_budget <= 0 || !m_spill) return;

	while (m_spill->bytes() > m_disk_budget) {
		int oldest = -1;

		for (int i = 0; i < m_placements.size(); ++i) {
			if (!m_spill->contains(m_placements.at(i).serial)) continue;
			if (oldest < 0 || m_placements.at(i).row < m_placements.at(oldest).row) oldest = i;
		}

		if (oldest < 0) return;
		discard(oldest);
	}
}

void bssh_image_store::shift_rows(int lines) {
	if (lines == 0) return;

	for (int i = 0; i < m_placements.size(); ++i) {
		m_placements[i].row -= lines;
	}

	/* Some of them are above the fold now that were not. */
	enforce_budget();
}

void bssh_image_store::trim_to(int rows) {
	for (int i = m_placements.size() - 1; i >= 0; --i) {
		if (m_placements.at(i).row >= rows) discard(i);
	}
}
