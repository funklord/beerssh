#include "ui/touch_metrics.h"
#include "ui/pane_container.h"

#include <QMetaType>
#include <QMimeData>
#include <QEvent>
#include <QFrame>
#include <QScrollArea>
#include <QScrollBar>
#include <QHBoxLayout>
#include <QPointer>
#include <QSplitter>
#include <QSplitterHandle>
#include <QPointer>
#include <QTimer>
#include <QDateTime>
#include <QCoreApplication>
#include <QMouseEvent>
#include <QBoxLayout>
#include <QVBoxLayout>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <functional>
#include <utility>

#include "model/session.h"
#include "term/emulator.h"
#include "platform/platform.h"
#include "ui/file_panel.h"
#include "ui/layout_manager.h"
#include "model/app_settings.h"
#include "ui/pane_snap.h"
#include "ui/divider_links.h"
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
      m_splitter(nullptr),
      m_scroll(nullptr),
      m_focused(nullptr) {
	qRegisterMetaType<bssh_session *>();

	if (bssh_platform *platform = bssh_platform::instance()) {
		connect(platform, &bssh_platform::application_active_changed,
		        this, &bssh_pane_container::update_on_screen);
	}

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

	/* The column's height follows the viewport's; see apply_column_visible. */
	m_scroll->viewport()->installEventFilter(this);

	m_splitter = new QSplitter(Qt::Horizontal, this);
	configure_splitter(m_splitter);

	m_outer->addWidget(m_splitter, 1);

	if (m_layout) {
		connect(m_layout, &bssh_layout_manager::layout_changed,
		         this, &bssh_pane_container::refresh);
	}
}

void bssh_pane_container::set_bar_factory(
        std::function<QWidget *(bssh_session *, QWidget *)> factory) {
	m_bar_factory = std::move(factory);
}

void bssh_pane_container::set_bar_edge(Qt::Edge edge) {
	if (edge == m_bar_edge) return;
	m_bar_edge = edge;
	rebuild();
}

Qt::Edge bssh_pane_container::bar_edge_for(bssh_session *member) const {
	if (!m_layout || m_layout->arrangement() != bssh_layout_manager::TILED) return m_bar_edge;
	const Qt::Edges own = m_layout->bar_edge(member);
	if (own & Qt::LeftEdge) return Qt::LeftEdge;
	if (own & Qt::RightEdge) return Qt::RightEdge;
	if (own & Qt::BottomEdge) return Qt::BottomEdge;
	if (own & Qt::TopEdge) return Qt::TopEdge;
	return m_bar_edge;
}

QWidget *bssh_pane_container::frame_for(bssh_session *front) const {
	const int index = m_sessions.indexOf(front);
	return index >= 0 && index < m_frames.size() ? m_frames.at(index) : nullptr;
}

QWidget *bssh_pane_container::bar_for(bssh_session *front) const {
	const int index = m_sessions.indexOf(front);
	return index >= 0 && index < m_bars.size() ? m_bars.at(index) : nullptr;
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

void bssh_pane_container::build_panes(const QList<bssh_session *> &panes) {
	/*
	 * A column stacks every pane in order and has no shape to follow
	 * (sec 8.7): the tree is a tiled window's, and a phone's column is the
	 * same sessions one after another whatever they were beside.
	 */
	const bssh_pane_node tree = m_layout->visible_tree();
	const bool column = m_layout->arrangement() == bssh_layout_manager::COLUMN;

	if (column || tree.is_empty() || tree.is_leaf()) {
		for (bssh_session *session : panes) add_pane_view(session, m_splitter);
		return;
	}

	m_splitter->setOrientation(tree.orientation);
	add_split_children(tree, m_splitter, QStringLiteral("r"));
}

void bssh_pane_container::add_split_children(const bssh_pane_node &node, QSplitter *into,
                                             const QString &path) {
	m_splitter_paths.insert(into, path);
	int index = 0;
	/*
	 * In order, and that is what places them: a QSplitter lays out its
	 * children in the order they were created in it, so building depth
	 * first puts every pane and every nested split where the tree says.
	 */
	for (const bssh_pane_node &child : node.children) {
		const QString here = path + QLatin1Char('.') + QString::number(index++);
		if (child.is_leaf()) {
			add_pane_view(child.session, into);
			continue;
		}

		/*
		 * Added explicitly, as a frame is in add_pane_view and for its
		 * reason: a splitter shows a new child only once it is polished,
		 * and a nested one built into a splitter already on screen could
		 * stay hidden with every pane in it -- terminals that were in the
		 * layout and nowhere on screen (the holder: "terminals disappear",
		 * 2026-10-30).
		 */
		QSplitter *nested = new QSplitter(child.orientation, into);
		into->addWidget(nested);
		configure_splitter(nested);
		add_split_children(child, nested, here);
	}
}

QList<QSplitter *> bssh_pane_container::all_splitters() const {
	QList<QSplitter *> splitters{m_splitter};
	splitters += m_splitter->findChildren<QSplitter *>();
	return splitters;
}

/*
 * Every splitter the panes use, the root and any nested in it, gets the
 * same divider: a split made by dropping a pane on another's edge is a
 * divider like any other, and one that a thumb could not grab or that
 * cut a glyph in half would be a worse one.
 */
void bssh_pane_container::configure_splitter(QSplitter *splitter) {
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
	splitter->setHandleWidth(thumb ? BSSH_TOUCH_TARGET_DP : 8);
	splitter->setChildrenCollapsible(false);

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
	connect(splitter, &QSplitter::splitterMoved, this, [this, splitter](int, int index) {
		snap_splitter(splitter);
		follow_divider(splitter, index);
	});
}

bssh_terminal_view *bssh_pane_container::add_pane_view(bssh_session *session,
                                                    QSplitter *parent, QWidget *replacing) {
	/*
	 * A frame per pane, its bar on the set edge and the view taking the
	 * rest (sec 8.3's bar per pane). The splitter holds frames, so a
	 * divider moves a pane and its bar together.
	 */
	/* Unparented when it replaces one: QSplitter::replaceWidget() refuses
	 * a widget that is already the splitter's child. */
	QWidget *frame = new QWidget(replacing ? nullptr : parent);
	frame->setObjectName(QStringLiteral("pane-frame"));
	QBoxLayout::Direction direction = QBoxLayout::TopToBottom;
	switch (bar_edge_for(session)) {
	case Qt::BottomEdge: direction = QBoxLayout::BottomToTop; break;
	case Qt::LeftEdge:   direction = QBoxLayout::LeftToRight; break;
	case Qt::RightEdge:  direction = QBoxLayout::RightToLeft; break;
	default:             break;
	}
	QBoxLayout *box = new QBoxLayout(direction, frame);
	box->setContentsMargins(0, 0, 0, 0);
	box->setSpacing(0);

	QWidget *bar = m_bar_factory ? m_bar_factory(session, frame) : nullptr;
	if (bar) box->addWidget(bar, 0);

	/*
	 * The view, and beside it the session's file panel when it is open
	 * (sec 6.4) -- in a row of its own whichever edge the bar is on, and a
	 * layout rather than a splitter: every QSplitter under this container
	 * is taken for a pane divider (all_splitters()), and would be snapped
	 * and evened out as one.
	 */
	QHBoxLayout *row = new QHBoxLayout;
	row->setContentsMargins(0, 0, 0, 0);
	row->setSpacing(0);
	box->addLayout(row, 1);
	bssh_terminal_view *view = new bssh_terminal_view(frame);
	row->addWidget(view, 1);
	if (session->file_panel_open()) {
		/* Over the terminal on touch, where beside it would leave it
		 * nothing; beside it elsewhere. */
		const bool touch = bssh_platform::instance() && bssh_platform::instance()->is_touch_primary();
		bssh_file_panel *panel = new bssh_file_panel(session, touch ? static_cast<QWidget *>(view) : frame);
		if (touch) panel->set_overlay(true);
		else row->addWidget(panel, 0);
		m_file_panels.removeAll(QPointer<QWidget>());
		m_file_panels.append(panel);
		const QPointer<bssh_session> held = session;
		connect(panel, &bssh_file_panel::close_requested, this, [this, held] {
			if (held) set_file_panel_open(held, false);
		});
		if (!touch && session->file_panel_width() > 0) panel->set_panel_width(session->file_panel_width());
		connect(panel, &bssh_file_panel::width_chosen, this, [held](int width) {
			if (held) held->set_file_panel_width(width);
		});
		connect(panel, &bssh_file_panel::listed, this, [held, panel] {
			if (held && !panel->path().isEmpty()) held->set_file_panel_path(panel->path());
		});
		if (session->file_panel_path().isEmpty()) panel->open_shells_directory();
		else panel->open_directory(session->file_panel_path());
	}

	/*
	 * Added explicitly, which shows it. A splitter otherwise shows a new
	 * child when the child is POLISHED, and nothing polishes a plain
	 * frame -- the view did it for itself, measuring its font -- so the
	 * first pane of a rebuild came up hidden and zero wide.
	 */
	if (replacing) parent->replaceWidget(parent->indexOf(replacing), frame);
	else parent->addWidget(frame);
	m_frames.append(frame);
	m_frame_tabs.insert(frame, m_layout ? m_layout->tabs(session) : QList<bssh_session *>{ session });
	m_bars.append(bar);

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

	/*
	 * And the zoom on top of it (sec 9.5), which is why the session
	 * holds it: this view is new -- every view here is, on every
	 * layout change -- and a zoom the user set before the split
	 * would otherwise be gone.
	 */
	view->set_font_zoom(session->font_zoom());
	session->set_font_zoom(view->font_zoom());

	connect(session, &bssh_session::font_zoom_changed, view, [view, session] {
		view->set_font_zoom(session->font_zoom());

		/*
		 * And BACK, which is what keeps the two from disagreeing
		 * once the zoom reaches its limit. The view is the only
		 * one that knows the size the host resolved to, so it is
		 * the only one that can say which steps are legal; without
		 * this the session goes on counting presses the view has
		 * refused, and the first several steps back then change
		 * nothing on screen.
		 *
		 * It terminates: set_font_zoom returns without emitting
		 * when the value is unchanged, which it now is.
		 */
		session->set_font_zoom(view->font_zoom());

		/*
		 * The same three the resolve path below sends, and for
		 * the same reason: a zoom is a new cell size, so the
		 * emulator's pixel geometry and the remote's idea of the
		 * window both move with it. grid_resized carries the
		 * resize on its own only while the pane is visible.
		 */
		session->emulator()->set_cell_pixels(view->cell_width(),
		                                      view->cell_height());
		session->resize(view->cols(), view->rows());
	});

	/*
	 * Ctrl+wheel, named. The view says the gesture happened; which
	 * session it happened to is this container's answer, and the
	 * zoom is stored on the session rather than on the widget that
	 * reported it.
	 */
	connect(view, &bssh_terminal_view::zoom_requested, session,
	         [session](int steps) {
		session->set_font_zoom(session->font_zoom() + steps);
	});

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
	 * And what it missed. Adding the frame above shows the view and, on
	 * Qt 6.4, lays it out there and then, so its one grid_resized went
	 * out before this connection existed: the session stayed 80x24 in a
	 * pane of 47, drawing its first columns and the rest off the edge,
	 * until the window next changed size. Later Qts lay it out afterwards.
	 */
	if (view->isVisible() && view->width() > 0 && view->height() > 0) {
		session->emulator()->set_cell_pixels(view->cell_width(), view->cell_height());
		session->resize(view->cols(), view->rows());
	}

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
		/*
		 * Not while the panes are being rebuilt: deleting a frame hands
		 * Qt's focus to whichever view is next in the chain, and that is
		 * not the user choosing a pane.
		 */
		if (m_rebuilding) return;
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
	connect(view, &bssh_terminal_view::input_mode_chosen,
	         this, &bssh_pane_container::input_mode_chosen);
	connect(view, &bssh_terminal_view::corner_tabs_requested, this,
	         [this, session](const QPoint &global) { emit corner_tabs_requested(session, global); });
	connect(view, &bssh_terminal_view::corner_menu_requested,
	         this, &bssh_pane_container::corner_menu_requested);

	connect(view, &bssh_terminal_view::pane_drag_requested, this,
	         [this, alive = QPointer<bssh_session>(session)] {
		if (!alive) return;
		emit pane_drag_requested(alive.data());
	}, Qt::QueuedConnection);

	connect(view, &bssh_terminal_view::session_dropped, this,
	         [this, session](const QMimeData *mime, int edge) {
		emit session_dropped(mime, session, edge);
	});
	apply_drop_planner(view, session);

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
	connect(view, &bssh_terminal_view::column_scroll_settled,
	         this, &bssh_pane_container::settle_column_scroll);

	connect(view, &bssh_terminal_view::find_matches_changed,
	         this, &bssh_pane_container::find_matches_changed);

	connect(view, &bssh_terminal_view::focus_lost, this, [session] {
		if (session->emulator()) session->emulator()->set_focused(false);
	});

	connect(session->emulator(), &bssh_emulator::rows_scrolled,
	         view, &bssh_terminal_view::on_rows_scrolled);
	connect(session->emulator(), &bssh_emulator::rows_damaged,
	         view, &bssh_terminal_view::on_rows_damaged);

	/*
	 * The tmux and screen drawer (sec 10.2a). The session holds the offer
	 * and this view is new on every layout change, so it is handed what
	 * is still pending as well as told of what is found later; the answer
	 * goes to the session, and the session settling takes the drawer away
	 * from whichever view is showing it.
	 */
	view->offer_multiplexers(session->pending_multiplexers());
	connect(session, &bssh_session::multiplexers_found,
	         view, &bssh_terminal_view::offer_multiplexers);
	connect(session, &bssh_session::multiplexers_settled,
	         view, &bssh_terminal_view::withdraw_multiplexers);
	connect(view, &bssh_terminal_view::multiplexer_chosen,
	         session, &bssh_session::choose_multiplexer);
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

	return view;
}

QSize bssh_pane_container::least_pane() const {
	QSize least;
	for (const QWidget *frame : m_frames) {
		if (frame) least = least.expandedTo(frame->minimumSizeHint());
	}
	return least;
}

int bssh_pane_container::divider() const {
	return m_splitter->handleWidth();
}

namespace {

/* A tree's shape without its sessions: which way each split runs and how
 * many it holds. Two builds with the same signature have their dividers
 * in the same places. */
QString division_signature(const bssh_pane_node &node) {
	if (node.is_leaf()) return QStringLiteral("p");
	QString out = node.orientation == Qt::Horizontal ? QStringLiteral("H(") : QStringLiteral("V(");
	bool first = true;
	for (const bssh_pane_node &child : node.children) {
		if (!first) out += QLatin1Char(',');
		first = false;
		out += division_signature(child);
	}
	return out + QLatin1Char(')');
}

} /* namespace */

void bssh_pane_container::remember_divisions() {
	/*
	 * Only what was laid out: a window not yet shown has splitters of
	 * nothing, and remembering those would forget the real ones.
	 */
	QHash<QString, QList<int>> divisions;
	for (auto it = m_splitter_paths.cbegin(); it != m_splitter_paths.cend(); ++it) {
		const QList<int> sizes = it.key()->sizes();
		int total = 0;
		for (const int size : sizes) total += size;
		if (total > 0) divisions.insert(it.value(), sizes);
	}
	if (divisions.isEmpty()) return;
	m_divisions = divisions;

	QSet<bssh_session *> fronts;
	for (int i = 0; i < m_frames.size() && i < m_sessions.size(); ++i) {
		QWidget *frame = m_frames.at(i);
		if (!frame || frame->size().isEmpty()) continue;
		m_pane_sizes.insert(m_sessions.at(i), frame->size());
		fronts.insert(m_sessions.at(i));
	}
	m_last_fronts = fronts;
}

int bssh_pane_container::extent_of(QWidget *child, Qt::Orientation axis) const {
	if (QSplitter *splitter = qobject_cast<QSplitter *>(child)) {
		/* Across its own direction a split is as wide as its widest
		 * part; along it, its parts added up. */
		int widest = -1;
		int sum = 0;
		int known = 0;
		for (int i = 0; i < splitter->count(); ++i) {
			const int part = extent_of(splitter->widget(i), axis);
			if (part < 0) continue;
			widest = qMax(widest, part);
			sum += part;
			++known;
		}
		if (known == 0) return -1;
		if (splitter->orientation() != axis) return widest;
		return sum + (splitter->count() - known) * (sum / known);
	}

	/* A pane: the room its own front had last time, found through any of
	 * its tabs that was in front, since picking a tab changes the front
	 * of the same pane. */
	for (bssh_session *session : m_frame_tabs.value(child)) {
		if (!m_last_fronts.contains(session)) continue;
		const QSize size = m_pane_sizes.value(session);
		if (size.isEmpty()) continue;
		return axis == Qt::Horizontal ? size.width() : size.height();
	}
	return -1;
}

void bssh_pane_container::restore_divisions(const bssh_pane_node &tree) {
	const QString shape = division_signature(tree);
	const bool same_shape = shape == m_division_shape;
	m_division_shape = shape;
	if (tree.is_leaf() || tree.is_empty()) return;

	for (auto it = m_splitter_paths.cbegin(); it != m_splitter_paths.cend(); ++it) {
		QSplitter *splitter = it.key();

		/* The same shape: every divider exactly where it was. */
		if (same_shape) {
			const QList<int> was = m_divisions.value(it.value());
			if (was.size() == splitter->count()) splitter->setSizes(was);
			continue;
		}

		/*
		 * A new shape: each part as big as its panes were. A part with
		 * no history -- a new pane -- takes its neighbour's, which for a
		 * pane just split off halves the one it came from. setSizes()
		 * shares the room out in these proportions.
		 */
		QList<int> sizes;
		for (int i = 0; i < splitter->count(); ++i)
			sizes << extent_of(splitter->widget(i), splitter->orientation());
		bool any = false;
		for (const int size : sizes) any = any || size >= 0;
		/*
		 * Nothing known of any part: even shares. Left to QSplitter, a
		 * split of panes it had not laid out gave every one but the last
		 * its narrowest and the last the rest -- a grid built before its
		 * window knew its room showed its top row that way, was
		 * remembered so, and kept it when the other rows arrived (the
		 * holder's 4x4, 2026-11-02).
		 */
		if (!any) {
			splitter->setSizes(QList<int>(splitter->count(), 1));
			continue;
		}
		for (int i = 0; i < sizes.size(); ++i) {
			if (sizes.at(i) >= 0) continue;
			int near = -1;
			for (int step = 1; near < 0 && step < sizes.size(); ++step) {
				if (i - step >= 0 && sizes.at(i - step) >= 0) near = sizes.at(i - step);
				else if (i + step < sizes.size() && sizes.at(i + step) >= 0) near = sizes.at(i + step);
			}
			sizes[i] = near;
		}
		splitter->setSizes(sizes);
	}
}

QString bssh_pane_container::visible_shape(const QList<bssh_session *> &panes) const {
	/* One mark per pane, whatever its terminal: the splits and their
	 * order, and nothing about which tab is in front. */
	return bssh_layout_manager::shape_text(m_layout->visible_tree(), [&panes](bssh_session *each) {
		return panes.contains(each) ? QStringLiteral("p") : QString();
	});
}

bssh_pane_container::~bssh_pane_container() {
	release_sizes();
}

/*
 * The panes' sessions tell their remotes nothing until the layout has
 * been quiet a moment: Qt lays a new nested split out in steps, the first
 * of which gives a pane its minimum, and a shell told that redraws its
 * prompt for it and erases what was above (bssh_session::hold_size).
 */
void bssh_pane_container::hold_sizes(const QList<bssh_session *> &panes) {
	for (bssh_session *session : panes) {
		if (!session || m_size_held.contains(session)) continue;
		session->hold_size(true);
		m_size_held.append(session);
	}
	if (!m_settle) {
		m_settle = new QTimer(this);
		m_settle->setSingleShot(true);
		m_settle->setInterval(50);
		connect(m_settle, &QTimer::timeout, this, &bssh_pane_container::release_sizes);
	}
	m_settle->start();
}

void bssh_pane_container::release_sizes() {
	const QList<QPointer<bssh_session>> held = std::exchange(m_size_held, {});
	for (const QPointer<bssh_session> &session : held) {
		if (session) session->hold_size(false);
	}
}

void bssh_pane_container::refresh() {
	if (!m_layout) return;
	if (!swap_one_pane(m_layout->visible_panes())) rebuild();
}

/* Every pane but `except` holds the tabs the layout gives it now. */
bool bssh_pane_container::tabs_as_built(int except) const {
	for (int i = 0; i < m_frames.size(); ++i) {
		if (i == except) continue;
		if (m_frame_tabs.value(m_frames.at(i)) != m_layout->tabs(m_sessions.at(i))) return false;
	}
	return true;
}

/*
 * The holder, 2026-10-31: "when I created a tab bar in one of the
 * terminals, switching between them flickers the whole window". A tab
 * picked in one pane's bar went through rebuild(), which takes down every
 * pane's frame, bar and view and builds them again -- so every pane on
 * screen was new for a click that changed one, and the window repainted
 * whole. When the panes differ from what is built in exactly one place,
 * with the same splits, the same bar edge and the same arrangement, only
 * that pane's frame is replaced, in its slot in its splitter, which
 * keeps the divider where it was; everything else stays as built.
 */
bool bssh_pane_container::swap_one_pane(const QList<bssh_session *> &panes) {
	if (m_rebuilding || panes.size() != m_sessions.size() || m_frames.size() != m_sessions.size())
		return false;
	const bool column = m_layout->arrangement() == bssh_layout_manager::COLUMN;
	if (column != m_built_column || m_bar_edge != m_built_edge) return false;
	int at = -1;
	for (int i = 0; i < panes.size(); ++i) {
		if (panes.at(i) == m_sessions.at(i)) continue;
		if (at >= 0) return false;
		at = i;
	}
	if (visible_shape(panes) != m_built_shape) return false;
	/*
	 * Nothing at all to change -- the layout announcing what is already
	 * built, as show_session() does after bring_to_front() -- is done
	 * here too, rather than rebuilt for nothing.
	 */
	if (at < 0) return !m_rebuilding && tabs_as_built(-1);
	if (m_sessions.contains(panes.at(at)) || !tabs_as_built(at)) return false;

	QWidget *old_frame = m_frames.at(at);
	QSplitter *parent = qobject_cast<QSplitter *>(old_frame->parentWidget());
	if (!parent) return false;
	bssh_session *was = m_sessions.at(at);
	bssh_session *now = panes.at(at);

	m_rebuilding = true;
	hold_sizes({ now });
	const QList<int> sizes = parent->sizes();
	add_pane_view(now, parent, old_frame);
	/* Built at the end of the lists; moved into the slot it took. */
	m_frames[at] = m_frames.takeLast();
	m_bars[at] = m_bars.takeLast();
	m_views[at] = m_views.takeLast();
	m_frame_tabs.remove(old_frame);
	delete old_frame;
	parent->setSizes(sizes);

	was->set_on_screen(false);
	m_sessions[at] = now;
	update_on_screen();
	m_rebuilding = false;
	if (m_focused == was) m_focused = now;

	apply_focus_styling();
	apply_column_visible();
	emit panes_rebuilt();
	emit focus_changed(m_focused);
	return true;
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
	/*
	 * What the dividers were, before anything moves them: turning the root
	 * re-lays it out at once, so a grid -- rows one above the other, a
	 * root that runs top to bottom -- had its rows side by side for a
	 * moment and was remembered so, every pane a ninth of the width and
	 * the whole height. Each rebuild of a grid then came back wrong; it
	 * was found when breaking a row divider lost the columns' widths
	 * (2026-11-13).
	 */
	/*
	 * Not from a build folded to fit (sec 8.2): its sizes are the squeeze
	 * of a window not yet its size, not anybody's choice. `--grid 3x3`
	 * comes up folded in the window before it is sized -- a row of three
	 * over one pane -- and that build, remembered, gave the unfolded grid
	 * a short top row and two tall ones (the holder, 2026-11-13). With
	 * nothing remembered the unfolded grid shares evenly -- and so does a
	 * build that stays folded, as a new shape: kept as the same shape, it
	 * looked up dividers remembered for an earlier one, found none that
	 * fitted, and was left to QSplitter's first-narrowest default.
	 */
	if (!column && !m_built_collapsed) remember_divisions();
	else if (!column) m_division_shape.clear();
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
	m_splitter_paths.clear();
	m_frame_tabs.clear();

	/* The frames, which take their bars and views with them. */
	m_rebuilding = true;
	qDeleteAll(m_frames);
	m_frames.clear();
	m_bars.clear();
	m_views.clear();

	/*
	 * And the nested splitters a tiled tree was built from. Only the
	 * root's direct children: deleting one takes everything nested in it,
	 * and the views inside are already gone above.
	 */
	qDeleteAll(m_splitter->findChildren<QSplitter *>(QString(), Qt::FindDirectChildrenOnly));

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

	hold_sizes(panes);
	build_panes(panes);
	if (!column) restore_divisions(m_layout->visible_tree());
	m_built_shape = visible_shape(panes);
	m_built_edge = m_bar_edge;
	m_built_column = column;
	m_built_collapsed = m_layout->is_collapsed();

	/*
	 * The handles are QSplitter's own and are made and unmade with the
	 * panes, so the filter goes on after every rebuild rather than once.
	 * Handle 0 is a zero-width one before the first pane and is not a
	 * divider anybody can grab.
	 */
	for (QSplitter *splitter : all_splitters()) {
		for (int i = 1; i < splitter->count(); ++i) {
			if (QSplitterHandle *handle = splitter->handle(i)) {
				handle->installEventFilter(this);
			}
		}
	}

	m_rebuilding = false;
	if (!panes.contains(m_focused)) m_focused = panes.isEmpty() ? nullptr : panes.first();

	/*
	 * Tiled, the panes ask the window for one pane's room, not all of
	 * theirs: the sum made a window with many panes impossible to shrink,
	 * where the layout now turns the ones that no longer fit into tabs
	 * (sec 8.2). A column's height is apply_column_visible's.
	 */
	if (!column) m_splitter->setMinimumSize(least_pane().expandedTo(QSize(1, 1)));
	else m_splitter->setMinimumWidth(0);

	apply_focus_styling();
	apply_column_visible();
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
	for (QSplitter *splitter : all_splitters()) snap_splitter(splitter);
}

void bssh_pane_container::snap_splitter(QSplitter *splitter) {
	if (m_views.size() < 2 || splitter->count() < 2) return;

	/*
	 * Snap along the axis the panes actually divide: a column's dividers
	 * run across the width and cut the HEIGHT, so a half-column at a
	 * pane's edge (sec 8.3) is a half-ROW there, and the cell to round to
	 * is its height rather than its width.
	 */
	const bool vertical = splitter->orientation() == Qt::Vertical;
	const int cell = vertical ? m_views.first()->cell_height()
	                          : m_views.first()->cell_width();
	if (cell <= 1) return;

	QList<int> sizes = splitter->sizes();

	/*
	 * A two-pane column is sec 8.7's case: one divider, magnetically
	 * pulled to the halfway line ("two half-screen") and otherwise on a
	 * whole row. The math is pure and lives in pane_snap.h so a headless
	 * test asserts it; the gesture that moves the handle is what needs a
	 * device.
	 */
	if (vertical && sizes.size() == 2) {
		const int total = sizes.at(0) + sizes.at(1);
		/*
		 * The radius is the user's (sec 8.7 asks for it to be
		 * configurable), read here rather than cached: a drag is a rare
		 * event beside anything this class does per frame, and a cached
		 * copy is one more thing for a settings change to have to reach.
		 */
		const int first = bssh_snap_column_divider(
		        sizes.at(0), total, cell, bssh_app_settings().column_snap_radius());
		if (first == sizes.at(0)) return;

		sizes[0] = first;
		sizes[1] = total - first;
		splitter->setSizes(sizes);
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

	if (changed) splitter->setSizes(sizes);
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

		/*
		 * The corner -- grip, mode and multiplexer -- only on the
		 * focused pane: "hidden while that window is inactive" (sec
		 * 8.7a). Every other pane shows its text whole.
		 */
		view->set_corner_active(focused);
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

void bssh_pane_container::settle_column_scroll() {
	if (!m_scroll || m_scroll->widget() != m_splitter) return;

	QScrollBar *bar = m_scroll->verticalScrollBar();
	if (!bar) return;

	/*
	 * The pane tops in the splitter's own coordinates, which is the space
	 * the scrollbar's value is in -- the splitter IS the scroll area's
	 * widget, so a pane's y() and a scroll position are the same number.
	 */
	QList<int> tops;
	for (bssh_terminal_view *view : m_views) {
		if (view) tops.append(view->y());
	}

	bar->setValue(bssh_settle_column_scroll(bar->value(), tops.constData(),
	                                         int(tops.size()), bar->maximum()));

	/*
	 * Panned so the focused pane is out of view: focus goes to the pane
	 * most on screen, so the corner, and the mode drum in it, stays where
	 * it can be reached (sec 8.7a). Panning there is choosing to look at
	 * it, and the pane looked at is the one typed in, as sec 8.7 already
	 * has it the other way round for moving focus.
	 */
	if (focused_in_view()) return;
	const QRect seen(QPoint(0, bar->value()), m_scroll->viewport()->size());
	int best = -1;
	int most = 0;
	for (int i = 0; i < m_views.size(); ++i) {
		bssh_terminal_view *view = m_views.at(i);
		if (!view) continue;
		const QRect shown = seen.intersected(QRect(view->mapTo(m_splitter, QPoint(0, 0)), view->size()));
		const int area = shown.isEmpty() ? 0 : shown.width() * shown.height();
		if (area > most) {
			most = area;
			best = i;
		}
	}
	if (best >= 0 && best < m_sessions.size()) focus_session(m_sessions.at(best));
}

void bssh_pane_container::equalize_panes() {
	for (QSplitter *splitter : all_splitters()) equalize_splitter(splitter);
}

void bssh_pane_container::equalize_splitter(QSplitter *splitter) {
	const int count = splitter->count();
	if (count < 2) return;

	/*
	 * Equal values rather than computed widths: setSizes treats them
	 * proportionally when they do not add up to the space available, so
	 * this stays right whatever the handles are eating.
	 */
	QList<int> sizes;
	for (int i = 0; i < count; ++i) sizes.append(1);
	splitter->setSizes(sizes);

	/*
	 * And then back onto whole cells, because equal halves of an odd
	 * width are not both a whole number of columns -- equalizing without
	 * this would undo the snap the drag handler exists to apply.
	 */
	snap_splitter(splitter);
}

QList<bssh_pane_container::divider_ref_t> bssh_pane_container::all_dividers() const {
	QList<divider_ref_t> dividers;
	for (QSplitter *splitter : all_splitters()) {
		/* Handle 0 is the zero-width one before the first part. */
		for (int i = 1; i < splitter->count(); ++i) {
			QSplitterHandle *handle = splitter->handle(i);
			if (handle && handle->isVisible()) dividers.append({ splitter, i });
		}
	}
	return dividers;
}

/* Where a divider's line is across, in `in`'s coordinates: its middle. */
int bssh_pane_container::divider_at(QSplitter *splitter, int index, const QWidget *in) {
	QSplitterHandle *handle = splitter->handle(index);
	const QPoint corner = handle->mapTo(in, QPoint(0, 0));
	return splitter->orientation() == Qt::Horizontal ? corner.x() + handle->width() / 2
	                                                  : corner.y() + handle->height() / 2;
}

namespace {

/* The same dividers as plain geometry, for divider_links.h. */
template <typename refs_t>
QList<bssh_divider_t> divider_geometry(const refs_t &refs, const QWidget *in) {
	QList<bssh_divider_t> out;
	for (const auto &ref : refs) {
		QSplitterHandle *handle = ref.splitter->handle(ref.index);
		const QPoint corner = handle->mapTo(in, QPoint(0, 0));
		bssh_divider_t each;
		each.vertical = ref.splitter->orientation() == Qt::Horizontal;
		each.at = each.vertical ? corner.x() + handle->width() / 2 : corner.y() + handle->height() / 2;
		each.from = each.vertical ? corner.y() : corner.x();
		each.to = each.from + (each.vertical ? handle->height() : handle->width()) - 1;
		out.append(each);
	}
	return out;
}

} /* namespace */

void bssh_pane_container::move_divider_to(const divider_ref_t &divider, int at) {
	QSplitter *splitter = divider.splitter;
	if (!splitter || divider.index < 1 || divider.index >= splitter->count()) return;
	int delta = at - divider_at(splitter, divider.index, this);
	if (delta == 0) return;

	QList<int> sizes = splitter->sizes();
	const int before = divider.index - 1;
	const int after = divider.index;
	const bool across = splitter->orientation() == Qt::Horizontal;
	const auto least = [splitter, across](int i) {
		const QSize hint = splitter->widget(i)->minimumSizeHint().expandedTo(splitter->widget(i)->minimumSize());
		return across ? hint.width() : hint.height();
	};
	/* No part below its least: the line stops where a pane would. */
	if (sizes.at(before) + delta < least(before)) delta = least(before) - sizes.at(before);
	if (sizes.at(after) - delta < least(after)) delta = sizes.at(after) - least(after);
	if (delta == 0) return;
	sizes[before] += delta;
	sizes[after] -= delta;
	splitter->setSizes(sizes);
}

void bssh_pane_container::begin_divider_drag(QSplitterHandle *handle, bool alone) {
	m_drag = {};
	m_drag_linked.clear();
	QSplitter *splitter = handle->splitter();
	for (int i = 1; i < splitter->count(); ++i) {
		if (splitter->handle(i) == handle) m_drag = { splitter, i };
	}
	if (!m_drag.splitter || alone) return;

	const QList<divider_ref_t> dividers = all_dividers();
	int dragged = -1;
	for (int i = 0; i < dividers.size(); ++i) {
		if (dividers.at(i).splitter == splitter && dividers.at(i).index == m_drag.index) dragged = i;
	}
	/*
	 * In line within two pixels: dividers snap to whole cells, so a grid's
	 * columns land on the same pixel or one apart. The gap end to end is a
	 * crossing divider's width, which is what lies between two rows'.
	 */
	const QList<int> linked = bssh_linked_dividers(divider_geometry(dividers, this), dragged, 2,
	                                               splitter->handleWidth() + 2);
	for (const int i : linked) {
		if (i != dragged) m_drag_linked.append(dividers.at(i));
	}
}

void bssh_pane_container::follow_divider(QSplitter *splitter, int index) {
	if (m_drag.splitter != splitter || m_drag.index != index) return;
	int at = divider_at(splitter, index, this);

	/*
	 * Snapped into line with a divider that continues this one: within
	 * the column snap's reach (sec 8.7's setting, the same magnet), held
	 * there. Not joined for the rest of the drag -- passing one by takes
	 * nothing along -- but in line once let go, and so linked next time.
	 */
	const QList<divider_ref_t> dividers = all_dividers();
	QList<int> moving;
	for (int i = 0; i < dividers.size(); ++i) {
		const divider_ref_t &each = dividers.at(i);
		bool ours = each.splitter == splitter && each.index == index;
		for (const divider_ref_t &link : m_drag_linked)
			ours = ours || (link.splitter == each.splitter && link.index == each.index);
		if (ours) moving.append(i);
	}
	const QList<bssh_divider_t> geometry = divider_geometry(dividers, this);
	const int target = bssh_snap_divider(geometry, moving, at, bssh_app_settings().column_snap_radius(),
	                                     splitter->handleWidth() + 2);
	if (target >= 0 && geometry.at(target).at != at) {
		move_divider_to(m_drag, geometry.at(target).at);
		at = divider_at(splitter, index, this);
	}

	/*
	 * No further than the most hemmed-in of them can go: one row whose
	 * pane reached its least stopped while the rest went on, and the
	 * grid came out of line -- and no longer linked. Seen on macOS, whose
	 * panes are wider at their least (2026-11-13).
	 */
	if (!m_drag_linked.isEmpty()) {
		QPair<int, int> reach = divider_reach(m_drag);
		for (const divider_ref_t &link : m_drag_linked) {
			const QPair<int, int> each = divider_reach(link);
			reach.first = std::max(reach.first, each.first);
			reach.second = std::min(reach.second, each.second);
		}
		const int held = std::clamp(at, reach.first, std::max(reach.first, reach.second));
		if (held != at) {
			move_divider_to(m_drag, held);
			at = divider_at(splitter, index, this);
		}
	}
	for (const divider_ref_t &link : m_drag_linked) move_divider_to(link, at);
}

/* How far a divider can go either way before a pane beside it is at its
 * least, in this container's coordinates. */
QPair<int, int> bssh_pane_container::divider_reach(const divider_ref_t &divider) const {
	QSplitter *splitter = divider.splitter;
	if (!splitter || divider.index < 1 || divider.index >= splitter->count()) return { INT_MIN, INT_MAX };
	const bool across = splitter->orientation() == Qt::Horizontal;
	const auto least = [splitter, across](int i) {
		const QSize hint = splitter->widget(i)->minimumSizeHint().expandedTo(splitter->widget(i)->minimumSize());
		return across ? hint.width() : hint.height();
	};
	const QList<int> sizes = splitter->sizes();
	const int at = divider_at(splitter, divider.index, this);
	return { at - (sizes.at(divider.index - 1) - least(divider.index - 1)),
	         at + (sizes.at(divider.index) - least(divider.index)) };
}

bool bssh_pane_container::renest_for(QSplitterHandle *handle, const QPoint &global) {
	/*
	 * Only with every pane on screen: the tree rebuilt here is the one
	 * shown, and panes folded away into tabs (sec 8.2) are not in it.
	 */
	if (!m_layout || m_layout->arrangement() != bssh_layout_manager::TILED || m_layout->is_collapsed())
		return false;
	QSplitter *splitter = handle->splitter();
	int index = -1;
	for (int i = 1; i < splitter->count(); ++i) {
		if (splitter->handle(i) == handle) index = i;
	}
	if (index < 1) return false;

	/* Nothing to break unless it runs past a split the other way. */
	const auto split_across = [splitter](QWidget *part) {
		QSplitter *nested = qobject_cast<QSplitter *>(part);
		return nested && nested->orientation() != splitter->orientation() && nested->count() > 1;
	};
	if (!split_across(splitter->widget(index - 1)) && !split_across(splitter->widget(index))) return false;

	/* Each pane where it is, with its own node from the layout. */
	const std::function<const bssh_pane_node *(const bssh_pane_node &, bssh_session *)> leaf_of =
	        [&leaf_of](const bssh_pane_node &node, bssh_session *front) -> const bssh_pane_node * {
		if (node.is_leaf()) return node.session == front ? &node : nullptr;
		for (const bssh_pane_node &child : node.children) {
			if (const bssh_pane_node *found = leaf_of(child, front)) return found;
		}
		return nullptr;
	};
	std::vector<bssh_placed_pane_t> placed;
	for (int i = 0; i < m_frames.size() && i < m_sessions.size(); ++i) {
		const bssh_pane_node *leaf = leaf_of(m_layout->tree(), m_sessions.at(i));
		if (!leaf) return false;
		placed.push_back({ *leaf, QRect(m_frames.at(i)->mapTo(this, QPoint(0, 0)), m_frames.at(i)->size()) });
	}
	const Qt::Orientation first = splitter->orientation() == Qt::Vertical ? Qt::Horizontal : Qt::Vertical;
	bssh_pane_node tree;
	/* No tree cut the other way first: this layout cannot break here, and
	 * the divider moves whole, as it did. */
	if (!bssh_tree_from_rects(placed, first, &tree) || tree.orientation != first) return false;

	m_renest_from = mapFromGlobal(global);
	m_renest_vertical_line = splitter->orientation() == Qt::Horizontal;
	m_drag = {};
	m_drag_linked.clear();
	/*
	 * Later, not here: this runs inside the handle's own event, and the
	 * rebuild deletes the handle. The container takes the pointer from
	 * then on, since the handle that had it is gone.
	 */
	QTimer::singleShot(0, this, [this, tree] {
		if (!m_layout) return;
		m_layout->set_tree(tree);
		m_renest = {};
		m_renest_dragging = true;
		grabMouse();
	});
	return true;
}

void bssh_pane_container::mouseMoveEvent(QMouseEvent *event) {
	if (!m_renest_dragging) return QWidget::mouseMoveEvent(event);
	/*
	 * The piece under the pointer, found on the first move rather than
	 * straight after the rebuild: the new splitters have no geometry
	 * until they are laid out, and all of them read as being at 0.
	 */
	if (!m_renest.splitter) {
		QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
		const QList<divider_ref_t> dividers = all_dividers();
		const QList<bssh_divider_t> geometry = divider_geometry(dividers, this);
		const int across = m_renest_vertical_line ? m_renest_from.x() : m_renest_from.y();
		const int along = m_renest_vertical_line ? m_renest_from.y() : m_renest_from.x();
		for (int i = 0; i < dividers.size(); ++i) {
			const bssh_divider_t &each = geometry.at(i);
			if (each.vertical != m_renest_vertical_line || along < each.from || along > each.to) continue;
			if (std::abs(each.at - across) > dividers.at(i).splitter->handleWidth()) continue;
			m_renest = dividers.at(i);
		}
		if (!m_renest.splitter) return;
	}
	const QPoint at = event->position().toPoint();
	move_divider_to(m_renest, m_renest_vertical_line ? at.x() : at.y());

	/*
	 * Onto a whole cell, the pane before it alone: rounding the whole
	 * split, as a drag's own does, nudged the column's next divider by up
	 * to a cell and out of line with the rest.
	 */
	QSplitter *splitter = m_renest.splitter;
	if (m_views.isEmpty() || !splitter) return;
	const int cell = m_renest_vertical_line ? m_views.first()->cell_width() : m_views.first()->cell_height();
	QList<int> sizes = splitter->sizes();
	const int before = m_renest.index - 1;
	const int snapped = bssh_snap_pane_extent(sizes.at(before), cell);
	if (snapped == sizes.at(before)) return;
	sizes[m_renest.index] += sizes.at(before) - snapped;
	sizes[before] = snapped;
	splitter->setSizes(sizes);
}

void bssh_pane_container::mouseReleaseEvent(QMouseEvent *event) {
	if (!m_renest_dragging) return QWidget::mouseReleaseEvent(event);
	releaseMouse();
	m_renest_dragging = false;
	m_renest = {};
}

void bssh_pane_container::set_column_visible(int count) {
	if (count == m_column_visible) return;
	m_column_visible = count;
	apply_column_visible();
}

void bssh_pane_container::apply_column_visible() {
	const bool column = m_layout && m_layout->arrangement() == bssh_layout_manager::COLUMN;
	const int panes = m_views.size();

	/*
	 * Only when there are more panes than are meant to be seen: fewer, and
	 * they share the viewport as they always have, so two panes held
	 * upright are two halves and one is the whole screen.
	 */
	if (!column) return;
	if (m_column_visible <= 0 || panes <= m_column_visible) {
		m_splitter->setMinimumHeight(0);
		return;
	}

	const int viewport = m_scroll->viewport()->height();
	m_splitter->setMinimumHeight(viewport * panes / m_column_visible);

	/*
	 * Fewer panes on screen than before -- the window turned or shrank --
	 * may have left the focused one out of view; once the layout has
	 * settled, bring it back, as moving focus does.
	 */
	QMetaObject::invokeMethod(this, [this] { bring_focus_into_view(); }, Qt::QueuedConnection);
}

bool bssh_pane_container::focused_in_view() const {
	bssh_terminal_view *view = focused_view();
	if (!view || !m_scroll || m_scroll->widget() != m_splitter) return true;
	const QRect seen(QPoint(0, m_scroll->verticalScrollBar()->value()), m_scroll->viewport()->size());
	return seen.intersects(QRect(view->mapTo(m_splitter, QPoint(0, 0)), view->size()));
}

void bssh_pane_container::bring_focus_into_view() {
	bssh_terminal_view *view = focused_view();
	if (!view || !m_scroll || m_scroll->widget() != m_splitter) return;
	if (!focused_in_view()) m_scroll->ensureWidgetVisible(view);
}

bool bssh_pane_container::eventFilter(QObject *watched, QEvent *event) {
	if (watched == m_scroll->viewport() && event->type() == QEvent::Resize) {
		apply_column_visible();
		return false;
	}

	QSplitterHandle *handle = qobject_cast<QSplitterHandle *>(watched);
	if (!handle || !all_splitters().contains(handle->splitter())) return QWidget::eventFilter(watched, event);

	if (event->type() == QEvent::MouseButtonDblClick) {
		/* Its splitter and those of the dividers in line with it: equal
		 * shares in each, so a grid evened out stays a grid. */
		begin_divider_drag(handle, false);
		QList<QSplitter *> splitters{ handle->splitter() };
		for (const divider_ref_t &link : m_drag_linked) {
			if (link.splitter && !splitters.contains(link.splitter.data())) splitters.append(link.splitter);
		}
		m_drag = {};
		m_drag_linked.clear();
		for (QSplitter *each : splitters) equalize_splitter(each);
		return true;
	}

	if (event->type() == QEvent::MouseButtonPress) {
		/*
		 * Shift moves this one alone. Not Alt, which TDE's and KDE's window
		 * managers take for moving the window, so it never reaches here.
		 */
		const auto *press = static_cast<QMouseEvent *>(event);
		m_drag_pressed_ms = QDateTime::currentMSecsSinceEpoch();
		m_drag_moved = false;
		const bool alone = press->modifiers() & Qt::ShiftModifier;
		/* Alone, a divider the whole way across breaks at the pointer. */
		if (alone && renest_for(handle, press->globalPosition().toPoint())) return true;
		begin_divider_drag(handle, alone);
	} else if (event->type() == QEvent::MouseMove && !m_drag_moved && m_drag.splitter) {
		/*
		 * Held still half a second, then moved: alone too. A phone has no
		 * Shift, and a way out only one input can produce is none on the
		 * other (sec 6.2).
		 */
		m_drag_moved = true;
		if (QDateTime::currentMSecsSinceEpoch() - m_drag_pressed_ms >= 500) {
			const auto *move = static_cast<QMouseEvent *>(event);
			if (renest_for(handle, move->globalPosition().toPoint())) return true;
			m_drag_linked.clear();
		}
	} else if (event->type() == QEvent::MouseButtonRelease) {
		m_drag = {};
		m_drag_linked.clear();
	}
	return QWidget::eventFilter(watched, event);

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
	 *
	 * And the application has to be the one in front: a window that can
	 * be seen behind a browser is not being watched, which the holder
	 * settled on 2026-09-29 (sec 8.3). Coming back counts as having
	 * seen it, the same as coming back from the tray.
	 */
	const bssh_platform *platform = bssh_platform::instance();
	const bool showing = isVisible() && (!platform || platform->is_application_active());
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

void bssh_pane_container::set_drop_planner(
        std::function<int(const QMimeData *, bssh_session *, int)> planner) {
	m_drop_planner = std::move(planner);
	for (int i = 0; i < m_views.size() && i < m_sessions.size(); ++i) {
		apply_drop_planner(m_views.at(i), m_sessions.at(i));
	}
}

void bssh_pane_container::apply_drop_planner(bssh_terminal_view *view, bssh_session *session) {
	if (!view) return;
	if (!m_drop_planner) {
		view->set_drop_planner(nullptr);
		return;
	}
	view->set_drop_planner([this, alive = QPointer<bssh_session>(session)](const QMimeData *mime,
	                                                                      int edge) {
		return alive && m_drop_planner ? m_drop_planner(mime, alive.data(), edge) : BSSH_DROP_CANCEL;
	});
}

bool bssh_pane_container::file_panel_shown() const {
	for (const QPointer<QWidget> &panel : m_file_panels)
		if (panel && panel->isVisible()) return true;
	return false;
}

void bssh_pane_container::set_file_panel_open(bssh_session *session, bool open) {
	if (!session || session->file_panel_open() == open) return;
	session->set_file_panel_open(open);
	/*
	 * Rebuilt from the event loop: this is reached from the panel's own
	 * close button, and the rebuild deletes the panel.
	 */
	QMetaObject::invokeMethod(this, [this] { rebuild(); }, Qt::QueuedConnection);
}
