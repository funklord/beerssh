#include "ui/touch_metrics.h"
#include "ui/pane_container.h"

#include <QMetaType>
#include <QEvent>
#include <QSplitter>
#include <QSplitterHandle>
#include <QVBoxLayout>

#include "model/session.h"
#include "term/emulator.h"
#include "platform/platform.h"
#include "ui/layout_manager.h"
#include "ui/terminal_view.h"

bssh_pane_container::bssh_pane_container(bssh_layout_manager *layout, QWidget *parent)
    : QWidget(parent),
      m_layout(layout),
      m_focused(nullptr) {
	qRegisterMetaType<bssh_session *>();

	QVBoxLayout *outer = new QVBoxLayout(this);
	outer->setContentsMargins(0, 0, 0, 0);
	outer->setSpacing(0);

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

	outer->addWidget(m_splitter, 1);

	if (m_layout) {
		connect(m_layout, &bssh_layout_manager::layout_changed,
		         this, &bssh_pane_container::rebuild);
	}
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
			session->resize(view->cols(), view->rows());
		});

		session->emulator()->set_cell_pixels(view->cell_width(), view->cell_height());

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

		connect(view, &bssh_terminal_view::pane_drag_requested, this, [this, session] {
			emit pane_drag_requested(session);
		});

		connect(view, &bssh_terminal_view::focus_lost, this, [session] {
			if (session->emulator()) session->emulator()->set_focused(false);
		});

		connect(session->emulator(), &bssh_emulator::damaged,
		         view, &bssh_terminal_view::on_screen_damaged);
		connect(session->emulator(), &bssh_emulator::images_changed,
		         view, QOverload<>::of(&QWidget::update));

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
	if (view) view->setFocus();

	emit focus_changed(m_focused);
}

void bssh_pane_container::snap_divider_to_cells() {
	if (m_views.size() < 2) return;

	const int cell = m_views.first()->cell_width();
	if (cell <= 1) return;

	QList<int> sizes = m_splitter->sizes();
	bool changed = false;

	for (int i = 0; i < sizes.size() - 1; ++i) {
		const int snapped = qMax(cell, (sizes.at(i) / cell) * cell);
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
