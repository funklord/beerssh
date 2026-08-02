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

		connect(session->emulator(), &bssh_emulator::damaged,
		         view, QOverload<>::of(&QWidget::update));
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
