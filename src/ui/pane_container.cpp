#include "ui/pane_container.h"

#include <QMetaType>
#include <QSplitter>
#include <QVBoxLayout>

#include "model/session.h"
#include "term/emulator.h"
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
	 * A generous divider: sec 8.3 asks for a 48dp effective touch target,
	 * and a handle thin enough to look elegant on a desktop is a handle
	 * nobody can grab with a thumb.
	 */
	m_splitter->setHandleWidth(8);
	m_splitter->setChildrenCollapsible(false);

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
	m_sessions = panes;

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
		});

		connect(session->emulator(), &bssh_emulator::damaged,
		         view, &bssh_terminal_view::on_screen_damaged);
		connect(session->emulator(), &bssh_emulator::images_changed,
		         view, QOverload<>::of(&QWidget::update));

		m_views.append(view);
		view->show();
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
