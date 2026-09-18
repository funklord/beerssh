#include "ui/touch_metrics.h"
#include "ui/pane_container.h"

#include <QMetaType>
#include <QEvent>
#include <QFrame>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QSplitterHandle>
#include <QPointer>
#include <QVBoxLayout>

#include "model/session.h"
#include "term/emulator.h"
#include "platform/platform.h"
#include "ui/layout_manager.h"
#include "ui/pane_snap.h"
#include "ui/terminal_view.h"

/*
 * Every pointer member starts null. showEvent() calls
 * update_on_screen(), which reads m_splitter, and a show can arrive
 * before the constructor has finished building it.
 */
bssh_pane_container::bssh_pane_container(bssh_layout_manager *layout, QWidget *parent)
    : QWidget(parent),
      m_layout(layout),
      m_outer(nullptr),
      m_focused(nullptr),
      m_splitter(nullptr),
      m_scroll(nullptr) {
	qRegisterMetaType<bssh_session *>();

	m_outer = new QVBoxLayout(this);
	m_outer->setContentsMargins(0, 0, 0, 0);
	m_outer->setSpacing(0);

	/*
	 * The column's scroll area, built once and empty until a column
	 * rebuild moves the splitter into it (sec 8.7). Resizable so the
	 * splitter fills the viewport where the panes fit, and scrolls only
	 * when their minimum heights add up to more than the screen. No
	 * QScroller: a kinetic flick would fight the terminal body-drag the
	 * input selector routes, which is the gesture a device pass settles.
	 */
	m_scroll = new QScrollArea(this);
	m_scroll->setWidgetResizable(true);
	m_scroll->setFrameShape(QFrame::NoFrame);
	m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	m_scroll->hide();

	m_splitter = new QSplitter(Qt::Horizontal, this);

	/*
	 * sec 8.3's divider, sized for the input rather than for the look.
	 *
	 * This said it was giving touch a 48dp target and then set 8 for
	 * everything -- six times too small on a thumb, and the comment
	 * asserting compliance is what stops the next reader checking. A
	 * splitter's grab area is exactly its handle width; Qt has no
	 * separate hit region for it, so the number IS the target.
	 *
	 * Desktop stays narrow because sec 8.3 allows it to ("desktop can be
	 * tighter"), and a pointer lands on 8 pixels without trying. What
	 * desktop must keep is the resize cursor, which QSplitterHandle sets
	 * for itself -- nothing here overrides it.
	 */
	const bool thumb = bssh_platform::instance()
	                    && bssh_platform::instance()->is_touch_primary();
	m_splitter->setHandleWidth(thumb ? BSSH_TOUCH_TARGET_DP : 8);
	m_splitter->setChildrenCollapsible(false);

	/*
	 * sec 8.3's double-click/double-tap to equalize. One handler covers
	 * both: Qt synthesises a double-click from a double tap, so the
	 * gesture arrives the same way on either input.
	 */

	/*
	 * Dividers snap to whole character cells (sec 8.3): a pane that is
	 * two and a half columns wide renders a half column at its edge, and
	 * the half is not blank -- it is the left side of a glyph.
	 */
	connect(m_splitter, &QSplitter::splitterMoved, this, [this] {
		snap_divider_to_cells();
	});

	m_outer->addWidget(m_splitter, 1);

	if (m_layout) {
		connect(m_layout, &bssh_layout_manager::layout_changed,
		         this, &bssh_pane_container::rebuild);
	}
}

bssh_terminal_view *bssh_pane_container::view_for(bssh_session *session) const {
	const int index = m_sessions.indexOf(session);
	return index >= 0 ? view_at(index) : nullptr;
}

bssh_terminal_view *bssh_pane_container::view_at(int index) const {
	if (index < 0 || index >= m_views.size()) return nullptr;
	return m_views.at(index);
}

bssh_terminal_view *bssh_pane_container::focused_view() const {
	const int index = m_sessions.indexOf(m_focused);
	return view_at(index);
}

void bssh_pane_container::rebuild() {
	if (!m_layout) return;

	const QList<bssh_session *> panes = m_layout->visible_panes();

	/*
	 * The arrangement decides which way the splitter runs (sec 8.7):
	 * a column stacks its panes and drags a horizontal divider up and
	 * down, tiled keeps sec 8.4's side-by-side. Set before the new views
	 * are added so they arrive in the right orientation; the divider
	 * snapping keys off the same orientation below.
	 */
	const bool column = m_layout->arrangement() == bssh_layout_manager::COLUMN;
	m_splitter->setOrientation(column ? Qt::Vertical : Qt::Horizontal);

	/*
	 * A column scrolls when its panes overflow, so its splitter lives in
	 * the scroll area; a tile fills the container directly, exactly as it
	 * always has. Moved only when the arrangement's structure actually
	 * differs -- the checks are idempotent, so an ordinary pane change
	 * reparents nothing and a tiled window never touches the scroll area
	 * at all.
	 */
	if (column && m_scroll->widget() != m_splitter) {
		m_outer->removeWidget(m_splitter);
		m_scroll->setWidget(m_splitter);
		m_outer->insertWidget(0, m_scroll, 1);
		m_scroll->show();
	} else if (!column && m_scroll->widget() == m_splitter) {
		m_scroll->takeWidget();
		m_outer->removeWidget(m_scroll);
		m_scroll->hide();
		m_outer->insertWidget(0, m_splitter, 1);
	}

	/*
	 * Destroy and rebuild rather than diff. A pane view holds no state of
	 * its own -- the screen, scrollback, images and router all belong to
	 * the session -- so recreating one costs a widget allocation and
	 * loses precisely nothing. Diffing would be an optimisation of the
	 * cheap half at the cost of the subtle half.
	 */
	qDeleteAll(m_views);
	m_views.clear();

	/*
	 * Which sessions are ON SCREEN, which is this container's answer to
	 * give and nobody else's (sec 8.3's bell dot).
	 *
	 * It used to follow the FOCUSED session, set by the window as the
	 * focus moved -- which is the same thing only while there is one
	 * pane. With a split it was wrong twice over: a bell in the pane
	 * beside the one being typed in raised a you-missed-this dot on a
	 * pane the user was looking straight at, and the pane that lost focus
	 * was marked away while still visible.
	 *
	 * Marked before the new views exist, since the question is about the
	 * layout rather than about any widget: a session with a pane is on
	 * screen, one without is a tab.
	 */
	for (bssh_session *session : m_sessions) {
		if (!panes.contains(session)) session->set_on_screen(false);
	}

	m_sessions = panes;
	update_on_screen();

	for (bssh_session *session : panes) {
		bssh_terminal_view *view = new bssh_terminal_view(m_splitter);

		/*
		 * It is a pane, so it gets the drag handle (sec 8.4). A view
		 * built anywhere else is not something a window can be asked
		 * to hand over.
		 */
		view->set_pane_handle(true);


		view->attach_screen(&session->emulator()->screen());
		view->attach_scrollback(&session->emulator()->scrollback());
		view->attach_images(&session->emulator()->images());
		view->attach_emulator(session->emulator());
		view->attach_input_router(session->input_router());

		/*
		 * The host's resolved appearance (sec 9, sec 10.1), before the
		 * cell metrics are read below: the font decides the cell size,
		 * so applying it afterwards would tell the emulator a geometry
		 * computed for a font that is no longer in use, and the remote
		 * would lay out for the wrong width until the next resize.
		 */
		bssh_apply_appearance(view, session->settings());

		connect(session, &bssh_session::settings_changed, view, [view, session] {
			bssh_apply_appearance(view, session->settings());
			session->emulator()->set_cell_pixels(view->cell_width(), view->cell_height());
			session->emulator()->set_colours(view->terminal_foreground().rgb(),
			                                  view->terminal_background().rgb());
			session->emulator()->set_palette(view->terminal_palette());
			session->resize(view->cols(), view->rows());
		});

		session->emulator()->set_cell_pixels(view->cell_width(), view->cell_height());

		/*
		 * And what it is painted in, for OSC 10 and OSC 11. Set beside
		 * the cell metrics and on the same events, because it is the
		 * same kind of fact: something the VIEW knows that the emulator
		 * has to be able to answer for.
		 */
		session->emulator()->set_colours(view->terminal_foreground().rgb(),
		                                  view->terminal_background().rgb());
		session->emulator()->set_palette(view->terminal_palette());

		/*
		 * Per-pane geometry: each pane computes its own cols x rows from
		 * its own size, and sends window-change for ITS session only
		 * (sec 8.3, sec 7). Panes may legitimately differ, and a shared
		 * geometry would tell one remote about the other's window.
		 */
		connect(view, &bssh_terminal_view::grid_resized, session,
		         [session, view](int cols, int rows) {
			session->emulator()->set_cell_pixels(view->cell_width(), view->cell_height());
			session->resize(cols, rows);
		});

		/*
		 * sec 8.3's tap-to-focus. Qt moves the KEYBOARD to a clicked
		 * pane on its own -- the view takes StrongFocus -- and moved
		 * nothing else, so the focus border, the status line and what
		 * copy and paste acted on all stayed with the pane the user had
		 * left. That is the disagreement sec 8.3 calls non-negotiable,
		 * since the panes are otherwise identical and the wrong one
		 * taking a destructive command is a real outcome.
		 */
		connect(view, &bssh_terminal_view::focus_received, this, [this, session] {
			focus_session(session);

			/*
			 * sec 7.1's focus reporting, told per session because that
			 * is what the remote asked about: an editor in one pane
			 * wants to know when THAT pane is being looked at, not
			 * when some other one is.
			 */
			if (session->emulator()) session->emulator()->set_focused(true);
		});

		/*
		 * QUEUED, and that is the whole point of it.
		 *
		 * This signal is emitted from the view's own mouseMoveEvent,
		 * and what it reaches runs QDrag::exec() -- a nested event loop
		 * that does not return until the pointer is released. Whatever
		 * happens to the panes in the meantime comes back through
		 * rebuild(), which qDeleteAll()s these views. So a direct
		 * connection destroys the view whose event handler is still on
		 * the stack, inside its own signal emission, which is a thing
		 * Qt does not promise to survive.
		 *
		 * It does survive today, by an accident worth naming: nothing
		 * after the emit in mouseMoveEvent touches `this` -- the two
		 * remaining lines are event->accept() and return. That is a
		 * property of two lines rather than a design, and the next edit
		 * to that function has no way of knowing it is load-bearing.
		 *
		 * Queued, the emit returns at once and the drag begins from the
		 * event loop, with the view's handler long finished.
		 *
		 * A QPointer because deferring by a turn is exactly what makes
		 * a session able to disappear first -- the same guard, and for
		 * the same reason, as the biometric prompt's.
		 */
		/*
		 * Forwarded rather than connected straight to the window: views
		 * are destroyed and rebuilt whenever the panes change, so a
		 * window holding connections to them would be wiring itself to
		 * objects that stop existing. The container outlives them.
		 */
		connect(view, &bssh_terminal_view::input_panel_requested,
		         this, &bssh_pane_container::input_panel_requested);

		connect(view, &bssh_terminal_view::pane_drag_requested, this,
		         [this, alive = QPointer<bssh_session>(session)] {
			if (!alive) return;
			emit pane_drag_requested(alive.data());
		}, Qt::QueuedConnection);

		connect(view, &bssh_terminal_view::pane_move_requested, this,
		         [this, alive = QPointer<bssh_session>(session)](int direction) {
			if (!alive) return;
			emit pane_move_requested(alive.data(), direction);
		}, Qt::QueuedConnection);

		/*
		 * The Pan scroll is the container's own -- it owns the scroll area
		 * -- so it is handled here rather than forwarded to the window,
		 * and directly rather than queued: scrolling rebuilds nothing, so
		 * the view is still there when the next move arrives.
		 */
		connect(view, &bssh_terminal_view::column_scroll_requested,
		         this, &bssh_pane_container::scroll_column_by);

		connect(view, &bssh_terminal_view::focus_lost, this, [session] {
			if (session->emulator()) session->emulator()->set_focused(false);
		});

		connect(session->emulator(), &bssh_emulator::damaged,
		         view, &bssh_terminal_view::on_screen_damaged);
		connect(session->emulator(), &bssh_emulator::images_changed,
		         view, QOverload<>::of(&QWidget::update));

		/*
		 * The remote's DECSCUSR reaches the view that shows it (sec 6.4):
		 * vim asks for a bar in insert mode and a block in normal, and the
		 * cursor follows. Connected to the view, so a rebuild drops it with
		 * the view; the preset set the starting shape and the remote takes
		 * it from there.
		 */
		connect(session->emulator(), &bssh_emulator::cursor_shape_changed,
		         view, [view](const QString &style) {
			if (bssh_terminal_view::honour_remote_cursor_shape())
				view->set_cursor_style(style);
		});
		connect(session->emulator(), &bssh_emulator::cursor_blink_changed,
		         view, [view](bool blink) {
			if (bssh_terminal_view::honour_remote_cursor_shape())
				view->set_cursor_blink(blink);
		});

		/* DECSCNM reverse video (sec 6.4): the whole screen flips. */
		connect(session->emulator(), &bssh_emulator::reverse_video_changed,
		         view, &bssh_terminal_view::set_reverse_video);

		m_views.append(view);
		view->show();
	}

	/*
	 * The handles are QSplitter's own and are made and unmade with the
	 * panes, so the filter goes on after every rebuild rather than once.
	 * Handle 0 is a zero-width one before the first pane and is not a
	 * divider anybody can grab.
	 */
	for (int i = 1; i < m_splitter->count(); ++i) {
		if (QSplitterHandle *handle = m_splitter->handle(i)) {
			handle->installEventFilter(this);
		}
	}

	if (!panes.contains(m_focused)) m_focused = panes.isEmpty() ? nullptr : panes.first();

	apply_focus_styling();
	emit panes_rebuilt();
	emit focus_changed(m_focused);
}

void bssh_pane_container::focus_session(bssh_session *session) {
	if (!m_sessions.contains(session) || m_focused == session) return;

	m_focused = session;
	apply_focus_styling();

	bssh_terminal_view *view = focused_view();
	if (view) {
		view->setFocus();

		/*
		 * In a scrolling column the focused pane may be off screen (sec
		 * 8.7), so bring it into view -- a tap, Tab or a chord that moves
		 * focus should not leave you looking at a pane you are no longer
		 * typing in. Only when the splitter is actually in the scroll
		 * area, so a tiled window does nothing.
		 */
		if (m_scroll && m_scroll->widget() == m_splitter) {
			m_scroll->ensureWidgetVisible(view);
		}
	}

	emit focus_changed(m_focused);
}

void bssh_pane_container::snap_divider_to_cells() {
	if (m_views.size() < 2) return;

	/*
	 * Snap along the axis the panes actually divide: a column's dividers
	 * run across the width and cut the HEIGHT, so a half-column at a
	 * pane's edge (sec 8.3) is a half-ROW there, and the cell to round to
	 * is its height rather than its width.
	 */
	const bool vertical = m_splitter->orientation() == Qt::Vertical;
	const int cell = vertical ? m_views.first()->cell_height()
	                          : m_views.first()->cell_width();
	if (cell <= 1) return;

	QList<int> sizes = m_splitter->sizes();

	/*
	 * A two-pane column is sec 8.7's case: one divider, magnetically
	 * pulled to the halfway line ("two half-screen") and otherwise on a
	 * whole row. The math is pure and lives in pane_snap.h so a headless
	 * test asserts it; the gesture that moves the handle is what needs a
	 * device.
	 */
	if (vertical && sizes.size() == 2) {
		const int total = sizes.at(0) + sizes.at(1);
		const int first = bssh_snap_column_divider(sizes.at(0), total, cell,
		                                            BSSH_COLUMN_SNAP_RADIUS);
		if (first == sizes.at(0)) return;

		sizes[0] = first;
		sizes[1] = total - first;
		m_splitter->setSizes(sizes);
		return;
	}

	bool changed = false;

	for (int i = 0; i < sizes.size() - 1; ++i) {
		const int snapped = bssh_snap_pane_extent(sizes.at(i), cell);
		if (snapped == sizes.at(i)) continue;

		/*
		 * Whatever is trimmed off goes to the neighbour rather than
		 * vanishing: the splitter's sizes must still add up to the
		 * width it was given, or it reflows them itself and undoes this.
		 */
		sizes[i + 1] += sizes.at(i) - snapped;
		sizes[i] = snapped;
		changed = true;
	}

	if (changed) m_splitter->setSizes(sizes);
}

void bssh_pane_container::focus_adjacent(int direction) {
	if (m_sessions.size() < 2) return;

	const int current = m_sessions.indexOf(m_focused);
	if (current < 0) return;

	const int next = qBound(0, current + (direction < 0 ? -1 : 1), m_sessions.size() - 1);
	if (next == current) return;

	focus_session(m_sessions.at(next));
}

void bssh_pane_container::apply_focus_styling() {
	/*
	 * Non-negotiable, per sec 8.3: with more than one pane the focused
	 * one must be unmistakable, because the panes are otherwise identical
	 * and the wrong one receiving a destructive command is a real
	 * outcome, not a theoretical one.
	 *
	 * With a single pane there is nothing to disambiguate, so the border
	 * is omitted rather than drawn around the whole terminal for no
	 * reason.
	 */
	const bool many = m_views.size() > 1;

	for (int i = 0; i < m_views.size(); ++i) {
		bssh_terminal_view *view = m_views.at(i);
		const bool focused = m_sessions.value(i) == m_focused;

		view->setProperty("bssh_focused", many && focused);
		view->set_focus_border(many && focused);
	}
}

void bssh_pane_container::remeasure_panes() {
	for (int i = 0; i < m_views.size(); ++i) {
		bssh_terminal_view *view = m_views.at(i);
		if (!view) continue;

		view->remeasure_cell();

		/*
		 * The emulator is told the cell in pixels too, because inline
		 * images (sec 7.1) are placed and scaled in cells and a stale
		 * cell size draws them at the old monitor's scale. The column
		 * count reaches the session on its own, through the
		 * grid_resized connection made when the pane was built.
		 */
		bssh_session *session = m_sessions.value(i, nullptr);
		if (session && session->emulator()) {
			session->emulator()->set_cell_pixels(view->cell_width(), view->cell_height());
		}
	}
}

void bssh_pane_container::scroll_column_by(int delta) {
	if (!m_scroll || m_scroll->widget() != m_splitter) return;

	QScrollBar *bar = m_scroll->verticalScrollBar();
	if (!bar) return;

	/*
	 * Content follows the finger: a downward drag (positive delta) reveals
	 * the panes above, which is a smaller scroll position. setValue clamps
	 * to the range on its own, so a drag past either end simply stops.
	 */
	bar->setValue(bar->value() - delta);
}

void bssh_pane_container::equalize_panes() {
	const int count = m_splitter->count();
	if (count < 2) return;

	/*
	 * Equal values rather than computed widths: setSizes treats them
	 * proportionally when they do not add up to the space available, so
	 * this stays right whatever the handles are eating.
	 */
	QList<int> sizes;
	for (int i = 0; i < count; ++i) sizes.append(1);
	m_splitter->setSizes(sizes);

	/*
	 * And then back onto whole cells, because equal halves of an odd
	 * width are not both a whole number of columns -- equalizing without
	 * this would undo the snap the drag handler exists to apply.
	 */
	snap_divider_to_cells();
}

bool bssh_pane_container::eventFilter(QObject *watched, QEvent *event) {
	if (event->type() == QEvent::MouseButtonDblClick) {
		QSplitterHandle *handle = qobject_cast<QSplitterHandle *>(watched);
		if (handle && handle->splitter() == m_splitter) {
			equalize_panes();
			return true;
		}
	}

	return QWidget::eventFilter(watched, event);
}

void bssh_pane_container::update_on_screen() {
	/*
	 * A pane is NECESSARY and not sufficient: the window showing it has
	 * to be up as well.
	 *
	 * Marking on-screen from the layout alone left every pane "on screen"
	 * while the whole app was hidden -- the tray on desktop (sec 5.5),
	 * the task moved to the back on Android -- so the bell that rang
	 * while the user was in another program, which is the one they most
	 * want to know about, raised nothing at all. isVisible() answers for
	 * this widget and every ancestor, which is exactly the question.
	 */
	const bool showing = isVisible();
	for (bssh_session *session : m_sessions) session->set_on_screen(showing);
}

void bssh_pane_container::showEvent(QShowEvent *event) {
	QWidget::showEvent(event);
	update_on_screen();
}

void bssh_pane_container::hideEvent(QHideEvent *event) {
	QWidget::hideEvent(event);
	update_on_screen();
}
