#include <QApplication>
#include <QAbstractButton>
#include <QPushButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QLineEdit>
#include <QKeySequenceEdit>
#include <QSlider>
#include <QAction>
#include <QComboBox>
#include <QFormLayout>
#include <QPair>
#include <QClipboard>
#include <QJsonObject>
#include <QAbstractScrollArea>
#include <QScrollArea>
#include <QKeyCombination>
#include <QKeyEvent>
#include <QKeySequence>
#include <QStackedWidget>
#include <QSet>
#include <QTreeWidget>
#include <QSignalSpy>
#include <QWheelEvent>
#include <QScrollBar>
#include <QMimeData>
#include <QWindow>
#include <QPointer>
#include <QEnterEvent>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QtTest>

#include <QAccessible>
#include <QAbstractSlider>
#include <QTimer>
#include "model/command_line.h"
#include "model/profile.h"
#include "platform/platform.h"
#include <QTemporaryDir>
#include "model/profile_store.h"
#include "keyboard/key_cap.h"
#include "keyboard/soft_keyboard.h"
#include "keyboard/soft_keyboard_layout.h"
#include "input/input_router.h"
#include "term/emulator.h"
#include "ui/host_list.h"
#include "ui/host_editor.h"
#include "ui/form_wrap.h"
#include "ui/preset_editor.h"
#include "ui/app_settings_view.h"
#include "ui/licences_view.h"
#include "ui/font_picker.h"
#include "ui/password_prompt.h"
#include "ui/session_dialog.h"
#include "ui/ssh_config_import.h"
#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QSplitter>
#include <QVBoxLayout>
#include <QSplitterHandle>
#include <QTabBar>
#include <QMetaMethod>
#include "ui/long_press.h"
#include "ui/main_window.h"
#include "fake_ssh_session.h"
#include "phone_metrics.h"
#include "layout_test.h"
#include "input/input_router.h"
#include "model/app_settings.h"
#include "model/session.h"
#include "model/session_registry.h"
#include "term/emulator.h"
#include "keyboard/key_row.h"
#include "keyboard/key_row_layout.h"
#include "term/colour_scheme.h"
#include "ui/layout_manager.h"
#include "ui/touch_metrics.h"
#include "ui/input_selector.h"
#include "ui/pane_container.h"
#include "ui/pane_snap.h"
#include "ui/find_bar.h"
#include "ui/terminal_view.h"

/*
 * Forces the touch idiom for as long as it is in scope, and puts it back.
 *
 * A guard rather than a call and a matching reset at the bottom: QVERIFY
 * returns from the function it fails in, so the reset at the bottom is
 * exactly the line that does not run on the day it matters -- and every
 * test after it would then be measuring a phone, reporting a failure
 * somewhere else entirely.
 */
struct forced_touch_t {
	explicit forced_touch_t(int touch) { bssh_platform::force_touch_primary(touch); }
	~forced_touch_t() { bssh_platform::force_touch_primary(-1); }
};

/*
 * Closes any session a window test left in the shared registry, so a
 * later test does not count a window this one built -- a leak surfaces as
 * a failure somewhere else entirely.
 */
struct registry_left_as_found_t {
	bssh_session_registry *registry;

	explicit registry_left_as_found_t(bssh_session_registry *r) : registry(r) {}
	~registry_left_as_found_t() {
		while (registry && registry->count() > 0) registry->close(registry->at(0));
	}
};

static bssh_profile_t named(const QString &name) {
	bssh_profile_t profile;
	profile.name = name;
	profile.host = name + QStringLiteral(".example");
	return profile;
}

void layout_test::size_classes_follow_the_window_not_the_platform() {
	/* Android's thresholds, applied to whatever window is in front of us. */
	QCOMPARE(bssh_layout_manager::size_class_for(360), bssh_layout_manager::COMPACT);
	QCOMPARE(bssh_layout_manager::size_class_for(599), bssh_layout_manager::COMPACT);
	QCOMPARE(bssh_layout_manager::size_class_for(600), bssh_layout_manager::MEDIUM);
	QCOMPARE(bssh_layout_manager::size_class_for(839), bssh_layout_manager::MEDIUM);
	QCOMPARE(bssh_layout_manager::size_class_for(840), bssh_layout_manager::EXPANDED);
	QCOMPARE(bssh_layout_manager::size_class_for(2560), bssh_layout_manager::EXPANDED);
}

void layout_test::pane_limits_match_the_size_class() {
	/*
	 * One pane on a phone is the decision sec 8 exists to make: an
	 * 80-column shell needs room, and two 40-column panes are worse than
	 * useless.
	 */
	QCOMPARE(bssh_layout_manager::max_panes_for(bssh_layout_manager::COMPACT), 1);
	QCOMPARE(bssh_layout_manager::max_panes_for(bssh_layout_manager::MEDIUM), 2);
	QCOMPARE(bssh_layout_manager::max_panes_for(bssh_layout_manager::EXPANDED), 4);
}

void layout_test::shrinking_collapses_panes_to_tabs_without_closing_them() {
	bssh_session_registry registry;
	bssh_layout_manager layout;

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));
	bssh_session *c = registry.create(named(QStringLiteral("c")));

	layout.set_width(1200);
	layout.set_intent({a, b, c});
	QCOMPARE(layout.visible_panes().size(), 3);
	QVERIFY(!layout.is_collapsed());

	/* Something arrives on the pane that is about to be collapsed away. */
	c->emulator()->feed(QByteArrayLiteral("work in progress"));

	/* Fold the foldable / drag the window narrow. */
	layout.set_width(400);

	QCOMPARE(layout.visible_panes().size(), 1);
	QVERIFY(layout.is_collapsed());

	/*
	 * The rule sec 8.2 is built around: never destroy a session because a
	 * view went away. All three are still registered, still connected in
	 * principle, and the collapsed one still has its screen.
	 */
	QCOMPARE(registry.count(), 3);
	QVERIFY(c->emulator()->screen().to_text().contains(QStringLiteral("work in progress")));
}

void layout_test::widening_restores_the_arrangement() {
	bssh_session_registry registry;
	bssh_layout_manager layout;

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));

	layout.set_width(900);
	layout.set_intent({a, b});
	QCOMPARE(layout.visible_panes().size(), 2);

	layout.set_width(400);
	QCOMPARE(layout.visible_panes().size(), 1);

	/*
	 * Unfold, or rotate back: the intent was remembered, so the split
	 * returns without the user rebuilding it. Forgetting it here is what
	 * makes a rotation feel like losing your workspace.
	 */
	layout.set_width(900);
	QCOMPARE(layout.visible_panes().size(), 2);
	QCOMPARE(layout.visible_panes().at(1), b);
	QVERIFY(!layout.is_collapsed());
}

void layout_test::layout_only_changes_on_a_class_boundary() {
	bssh_layout_manager layout;
	layout.set_width(1000);

	QSignalSpy spy(&layout, &bssh_layout_manager::layout_changed);

	/*
	 * A desktop window drag is a continuous stream of widths. Rebuilding
	 * the pane tree per pixel would be the same self-inflicted flood the
	 * grid avoids by only reporting real size changes (sec 8.1).
	 */
	for (int width = 1000; width < 1200; ++width) layout.set_width(width);
	QCOMPARE(spy.count(), 0);

	layout.set_width(700);   /* EXPANDED -> MEDIUM */
	QCOMPARE(spy.count(), 1);
	layout.set_width(500);   /* MEDIUM -> COMPACT */
	QCOMPARE(spy.count(), 2);
}

void layout_test::panes_get_their_own_geometry() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(1200, 400);
	layout.set_intent({a, b});
	container.rebuild();

	QCOMPARE(container.pane_count(), 2);

	bssh_terminal_view *first = container.view_at(0);
	bssh_terminal_view *second = container.view_at(1);
	QVERIFY(first && second);

	/*
	 * Each pane computes cols x rows from its OWN size and reports
	 * window-change for its own session (sec 8.3). A shared geometry
	 * would tell one remote about the other's window.
	 */
	/*
	 * The splitter sizes its children on the next layout pass, not
	 * inside rebuild(), so the geometry is only real after the event
	 * loop has run -- which is also true of the running app.
	 */
	QCoreApplication::processEvents();

	const int total_capacity = container.width() / qMax(1, first->cell_width());

	QVERIFY(first->cols() > 0);
	QVERIFY(second->cols() > 0);

	/* Each pane is about half the window, not all of it. */
	QVERIFY(first->cols() < total_capacity);
	QVERIFY(second->cols() < total_capacity);
	QVERIFY(first->cols() + second->cols() <= total_capacity);
	QVERIFY(qAbs(first->cols() - second->cols()) <= 1);
}

void layout_test::focus_is_visible_only_when_there_is_a_choice() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(1200, 400);

	/* One pane: nothing to disambiguate, so no border. */
	layout.set_intent({a});
	QCOMPARE(container.pane_count(), 1);
	QCOMPARE(container.view_at(0)->property("bssh_focused").toBool(), false);

	/*
	 * Two identical shells side by side is the case sec 8.3 calls
	 * non-negotiable: the user has to be able to tell which one is about
	 * to receive what they type.
	 */
	layout.set_intent({a, b});
	QCOMPARE(container.pane_count(), 2);
	QCOMPARE(container.focused_session(), a);
	QCOMPARE(container.view_at(0)->property("bssh_focused").toBool(), true);
	QCOMPARE(container.view_at(1)->property("bssh_focused").toBool(), false);

	QSignalSpy focus(&container, &bssh_pane_container::focus_changed);
	container.focus_session(b);

	QCOMPARE(focus.count(), 1);
	QCOMPARE(container.focused_session(), b);
	QCOMPARE(container.view_at(0)->property("bssh_focused").toBool(), false);
	QCOMPARE(container.view_at(1)->property("bssh_focused").toBool(), true);
}

void layout_test::pane_focus_moves_with_the_keyboard() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(1200, 400);
	layout.set_intent({a, b});

	QCOMPARE(container.focused_session(), a);

	container.focus_adjacent(1);
	QCOMPARE(container.focused_session(), b);

	/*
	 * Stops at the end rather than wrapping. With two panes, wrapping
	 * makes left and right do the same thing and the user loses track of
	 * where focus is -- which matters because the focused pane is the one
	 * about to receive whatever they type (sec 6.3).
	 */
	container.focus_adjacent(1);
	QCOMPARE(container.focused_session(), b);

	container.focus_adjacent(-1);
	QCOMPARE(container.focused_session(), a);
	container.focus_adjacent(-1);
	QCOMPARE(container.focused_session(), a);
}

void layout_test::dividers_snap_to_whole_cells() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(1200, 400);
	layout.set_intent({a, b});
	QCoreApplication::processEvents();

	QCOMPARE(container.pane_count(), 2);

	container.snap_divider_to_cells();
	QCoreApplication::processEvents();

	/*
	 * A pane two and a half columns wide renders a half column at its
	 * edge, and the half is not blank -- it is the left side of a glyph
	 * (sec 8.3).
	 */
	const int cell = container.view_at(0)->cell_width();
	QVERIFY(cell > 1);

	const int width = container.view_at(0)->width();
	QCOMPARE(width % cell, 0);
}

void layout_test::a_column_caps_where_a_tile_cannot() {
	bssh_layout_manager layout;

	/* Default is tiled -- sec 8.4, and what a desktop opens on. */
	QCOMPARE(layout.arrangement(), bssh_layout_manager::TILED);

	/*
	 * A phone-width window. Tiled, it holds one pane (COMPACT); a column
	 * holds every session it is given, because stacking is not what a
	 * narrow window runs out of room for. That difference at THIS width is
	 * the whole feature -- the two caps agreeing here would mean the
	 * arrangement changed nothing.
	 */
	layout.set_width(400);
	QCOMPARE(layout.max_panes(), 1);

	QSignalSpy spy(&layout, &bssh_layout_manager::layout_changed);
	layout.set_arrangement(bssh_layout_manager::COLUMN);
	QCOMPARE(spy.count(), 1);

	QCOMPARE(layout.arrangement(), bssh_layout_manager::COLUMN);
	QCOMPARE(layout.max_panes(), bssh_layout_manager::BSSH_COLUMN_PANE_LIMIT);
	QVERIFY(layout.max_panes() > layout.max_panes_for(bssh_layout_manager::EXPANDED));

	/* Idempotent: setting the same arrangement is not a layout change. */
	layout.set_arrangement(bssh_layout_manager::COLUMN);
	QCOMPARE(spy.count(), 1);
}

void layout_test::a_column_holds_the_terminal_a_tile_would_tab() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(400);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));

	/*
	 * Two sessions the user asked to see together, on a phone-width
	 * window. Tiled, the second collapses to a tab (sec 8.2).
	 */
	layout.set_intent({a, b});
	QCOMPARE(layout.visible_panes().size(), 1);
	QVERIFY(layout.is_collapsed());

	/*
	 * The column shows both -- the second terminal is on screen, not
	 * behind a tab. That is sec 8.7's "one full-screen or two
	 * half-screen" delivering the pane the tile hid.
	 */
	layout.set_arrangement(bssh_layout_manager::COLUMN);
	QCOMPARE(layout.visible_panes().size(), 2);
	QVERIFY(!layout.is_collapsed());
	QVERIFY(layout.visible_panes().contains(a));
	QVERIFY(layout.visible_panes().contains(b));
}

void layout_test::a_column_stacks_its_panes() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(400);
	layout.set_arrangement(bssh_layout_manager::COLUMN);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(400, 800);
	layout.set_intent({a, b});
	QCoreApplication::processEvents();

	QCOMPARE(container.pane_count(), 2);

	bssh_terminal_view *top = container.view_at(0);
	bssh_terminal_view *bottom = container.view_at(1);

	/*
	 * Stacked, not tiled: each pane spans the width and the second sits
	 * below the first. A horizontal tile would put them at one height and
	 * two x-positions, so this is the assertion that tells the two
	 * arrangements apart from the outside.
	 */
	const QPoint top_tl = top->mapTo(&container, QPoint(0, 0));
	const QPoint bottom_tl = bottom->mapTo(&container, QPoint(0, 0));

	QCOMPARE(top_tl.x(), bottom_tl.x());
	QVERIFY(bottom_tl.y() > top_tl.y());
	QVERIFY(top->width() > container.width() / 2);
	QVERIFY(bottom->width() > container.width() / 2);
}

void layout_test::a_column_divider_is_magnetic_to_the_halfway_line() {
	const int total = 800;
	const int cell = 20;
	const int radius = BSSH_COLUMN_SNAP_RADIUS;

	/*
	 * Near the halfway line, the divider is pulled onto it (sec 8.7's
	 * "two half-screen"). The value is chosen BELOW half by less than the
	 * radius, where plain row-flooring lands on the row before it -- so
	 * without the magnet the answer is a whole cell short of half, and the
	 * test can tell the pull from the flooring rather than passing on
	 * either.
	 */
	const int near = total / 2 - (radius - 3);
	QVERIFY(bssh_snap_pane_extent(near, cell) != bssh_snap_pane_extent(total / 2, cell));
	QCOMPARE(bssh_snap_column_divider(near, total, cell, radius),
	         bssh_snap_pane_extent(total / 2, cell));

	/*
	 * Beyond the magnet's reach it snaps to a whole row instead, leaving
	 * the halfway line alone -- one terminal genuinely larger than the
	 * other.
	 */
	const int away = 592;
	QCOMPARE(bssh_snap_column_divider(away, total, cell, radius),
	         bssh_snap_pane_extent(away, cell));
	QVERIFY(bssh_snap_column_divider(away, total, cell, radius) != total / 2);

	/*
	 * And never onto the neighbour's last row: dragged to the floor, the
	 * bottom pane still keeps a whole cell.
	 */
	const int floored = bssh_snap_column_divider(total - 1, total, cell, radius);
	QVERIFY(total - floored >= cell);
}

void layout_test::a_column_holds_every_session_and_dissolves_the_tabs() {
	const forced_touch_t column(1);

	bssh_main_window window;
	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);

	window.adopt_session(registry->create(named(QStringLiteral("a"))));
	window.adopt_session(registry->create(named(QStringLiteral("b"))));
	window.adopt_session(registry->create(named(QStringLiteral("c"))));

	/*
	 * All three are panes in the column (sec 8.7). Before this, a phone
	 * showed one or two and made the rest tabs -- and the session on
	 * screen had no tab at all, which is the off-by-one that was reported.
	 * With every session a pane there is nothing to convert, so the count
	 * is the reproduction of the bug turned into an assertion.
	 */
	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	QCOMPARE(panes->pane_count(), 3);

	QTabBar *tabs = window.findChild<QTabBar *>();
	QVERIFY(tabs);
	QCOMPARE(tabs->count(), 0);
}

void layout_test::an_overflowing_column_keeps_every_pane_reachable() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(400);
	layout.set_arrangement(bssh_layout_manager::COLUMN);

	QList<bssh_session *> many;
	for (int i = 0; i < 8; ++i)
		many.append(registry.create(named(QStringLiteral("s%1").arg(i))));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(400, 400);
	layout.set_intent(many);
	QCoreApplication::processEvents();

	/*
	 * Eight four-row panes cannot fit a 400px column, and every one is
	 * still built rather than truncated to a tab (sec 8.7) -- "holds every
	 * session" surviving the overflow it creates.
	 */
	QCOMPARE(container.pane_count(), 8);

	/*
	 * And the last, past the bottom of the viewport, is reachable: focusing
	 * it scrolls the column to it rather than leaving it clipped off the
	 * end, which is the whole point of the scroll area.
	 */
	/*
	 * The panes keep their real height and the column is taller than the
	 * viewport -- which is what the scroll area is for. Without it the
	 * splitter is trapped at the container's height and crams eight panes
	 * below the four-row minimum into 400px, so the total equals the
	 * viewport rather than overflowing it. With it, each pane sits at its
	 * minimum and the total runs past the screen, scrollbar and all.
	 */
	int total = 0;
	for (int i = 0; i < container.pane_count(); ++i) {
		bssh_terminal_view *view = container.view_at(i);
		QVERIFY(view);
		total += view->height();
	}
	QVERIFY2(total > container.height(),
	          "an overflowing column was crammed into the viewport instead of "
	          "scrolling, so its panes are below a usable size");
}

void layout_test::the_input_selector_defaults_to_scroll_and_reports_changes() {
	bssh_input_selector selector;

	/* Scroll is sec 8.7's default, and the control opens on it. */
	QCOMPARE(selector.mode(), BSSH_INPUT_SCROLL);

	QSignalSpy spy(&selector, &bssh_input_selector::mode_changed);

	QAbstractButton *select =
	        selector.findChild<QAbstractButton *>(QStringLiteral("input_select"));
	QVERIFY(select);
	select->click();

	QCOMPARE(selector.mode(), BSSH_INPUT_SELECT);
	QCOMPARE(spy.count(), 1);

	/*
	 * set_mode reflects a mode changed elsewhere WITHOUT emitting -- a
	 * control that only mirrored its own clicks would lie the first time
	 * something else moved the mode, and a re-emit here would loop back
	 * through the window that set it.
	 */
	selector.set_mode(BSSH_INPUT_CLICK);
	QCOMPARE(selector.mode(), BSSH_INPUT_CLICK);
	QCOMPARE(spy.count(), 1);

	QAbstractButton *click =
	        selector.findChild<QAbstractButton *>(QStringLiteral("input_click"));
	QVERIFY(click);
	QVERIFY(click->isChecked());
}

void layout_test::the_selector_pushes_its_mode_to_every_pane() {
	forced_touch_t touch(1);

	bssh_main_window window;
	window.adopt_session(window.registry()->create(named(QStringLiteral("a"))));

	bssh_input_selector *selector = window.findChild<bssh_input_selector *>();
	QVERIFY(selector);
	bssh_terminal_view *view = window.findChild<bssh_terminal_view *>();
	QVERIFY(view);

	/*
	 * The default reached the pane without anyone touching the selector:
	 * a freshly built view starts from its own default, so the window has
	 * to tell it the window's mode after every rebuild (sec 8.7).
	 */
	QCOMPARE(view->input_mode(), BSSH_INPUT_SCROLL);

	QAbstractButton *select =
	        selector->findChild<QAbstractButton *>(QStringLiteral("input_select"));
	QVERIFY(select);
	select->click();

	QCOMPARE(selector->mode(), BSSH_INPUT_SELECT);
	QVERIFY2(view->input_mode() == BSSH_INPUT_SELECT,
	          "the selector's mode never reached the pane");

	/* Shared registry: leave it as it was found (sec 8.4). */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::the_move_state_is_offered_only_in_a_column() {
	bssh_input_selector selector;

	QAbstractButton *move =
	        selector.findChild<QAbstractButton *>(QStringLiteral("input_move"));
	QVERIFY(move);

	/* No column to reorder yet, so the state is not offered. */
	QVERIFY2(move->isHidden(),
	          "Move is offered before there is a column to reorder");

	selector.set_column_available(true);
	QVERIFY2(!move->isHidden(), "Move is not offered once a column exists");

	/*
	 * Select Move, then lose the column. The mode must not be stranded on
	 * a control the user can no longer see: it falls back to Scroll and
	 * says so, because a body drag now has to scroll scrollback again
	 * rather than reorder a column that is gone.
	 */
	move->click();
	QCOMPARE(selector.mode(), BSSH_INPUT_MOVE);

	QSignalSpy spy(&selector, &bssh_input_selector::mode_changed);
	selector.set_column_available(false);
	QVERIFY2(move->isHidden(), "the Move button stayed after its column went");
	QCOMPARE(selector.mode(), BSSH_INPUT_SCROLL);
	QCOMPARE(spy.count(), 1);
}

void layout_test::a_column_reorder_survives_a_new_session() {
	const forced_touch_t column(1);

	bssh_main_window window;
	bssh_session_registry *reg = window.registry();
	QVERIFY(reg);

	bssh_session *a = reg->create(named(QStringLiteral("a")));
	bssh_session *b = reg->create(named(QStringLiteral("b")));
	bssh_session *c = reg->create(named(QStringLiteral("c")));
	window.adopt_session(a);
	window.adopt_session(b);
	window.adopt_session(c);

	bssh_layout_manager *layout = window.findChild<bssh_layout_manager *>();
	QVERIFY(layout);
	QCOMPARE(layout->arrangement(), bssh_layout_manager::COLUMN);
	QCOMPARE(layout->intent(), (QList<bssh_session *>{ a, b, c }));

	/* Move the last pane up one place: a, c, b. */
	window.move_pane(c, -1);
	QCOMPARE(layout->intent(), (QList<bssh_session *>{ a, c, b }));

	/*
	 * Opening a new session resyncs the column's intent from the shown
	 * list. The reorder has to survive that -- it moved m_shown, the
	 * column's order of record, not just the intent -- or every reorder
	 * would be silently undone the next time a terminal was opened.
	 */
	bssh_session *d = reg->create(named(QStringLiteral("d")));
	window.adopt_session(d);
	QCOMPARE(layout->intent(), (QList<bssh_session *>{ a, c, b, d }));

	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_move_gesture_reorders_the_column() {
	const forced_touch_t column(1);

	bssh_main_window window;
	window.resize(360, 800);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *reg = window.registry();
	bssh_session *a = reg->create(named(QStringLiteral("a")));
	bssh_session *b = reg->create(named(QStringLiteral("b")));
	window.adopt_session(a);
	window.adopt_session(b);

	bssh_layout_manager *layout = window.findChild<bssh_layout_manager *>();
	QVERIFY(layout);
	QCOMPARE(layout->intent(), (QList<bssh_session *>{ a, b }));

	/* Put the window in Move mode through the selector, as a user would. */
	bssh_input_selector *selector = window.findChild<bssh_input_selector *>();
	QVERIFY(selector);
	QAbstractButton *move =
	        selector->findChild<QAbstractButton *>(QStringLiteral("input_move"));
	QVERIFY(move);
	move->click();
	QCOMPARE(selector->mode(), BSSH_INPUT_MOVE);

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	bssh_terminal_view *view_b = panes->view_for(b);
	QVERIFY(view_b);

	/*
	 * Drag the second pane up the screen. The gesture has to travel the
	 * whole route -- view to container to window to move_pane -- and leave
	 * the column as b, a. Anything unwired anywhere on that path leaves it
	 * a, b, which every earlier assertion here would still pass.
	 */
	const QPoint from(view_b->cell_width() + 1, 9 * view_b->cell_height() + 1);
	const QPoint to(view_b->cell_width() + 1, 1 * view_b->cell_height() + 1);
	QMouseEvent press(QEvent::MouseButtonPress, from, view_b->mapToGlobal(from),
	                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QMouseEvent moved(QEvent::MouseMove, to, view_b->mapToGlobal(to),
	                   Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
	QMouseEvent release(QEvent::MouseButtonRelease, to, view_b->mapToGlobal(to),
	                     Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QCoreApplication::sendEvent(view_b, &press);
	QCoreApplication::sendEvent(view_b, &moved);
	QCoreApplication::sendEvent(view_b, &release);

	/*
	 * pane_move_requested is queued -- acting on it rebuilds the panes and
	 * destroys the view that asked -- so deliver the posted events before
	 * reading the result.
	 */
	QCoreApplication::sendPostedEvents();
	QCoreApplication::processEvents();

	QCOMPARE(layout->intent(), (QList<bssh_session *>{ b, a }));

	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::the_pan_state_is_offered_only_in_a_column() {
	bssh_input_selector selector;

	QAbstractButton *pan =
	        selector.findChild<QAbstractButton *>(QStringLiteral("input_pan"));
	QVERIFY(pan);

	/* Like Move, Pan acts on the column and is offered only where one is. */
	QVERIFY2(pan->isHidden(), "Pan is offered before there is a column");
	selector.set_column_available(true);
	QVERIFY2(!pan->isHidden(), "Pan is not offered once a column exists");

	pan->click();
	QCOMPARE(selector.mode(), BSSH_INPUT_COLUMN_SCROLL);

	QSignalSpy spy(&selector, &bssh_input_selector::mode_changed);
	selector.set_column_available(false);
	QVERIFY2(pan->isHidden(), "the Pan button stayed after its column went");
	QCOMPARE(selector.mode(), BSSH_INPUT_SCROLL);
	QCOMPARE(spy.count(), 1);
}

void layout_test::a_pan_gesture_scrolls_the_column() {
	/* The body-drag path is the touch one, so the gesture needs a thumb. */
	const forced_touch_t touch(1);

	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(400);
	layout.set_arrangement(bssh_layout_manager::COLUMN);

	QList<bssh_session *> many;
	for (int i = 0; i < 8; ++i)
		many.append(registry.create(named(QStringLiteral("s%1").arg(i))));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(400, 400);
	layout.set_intent(many);
	QCoreApplication::processEvents();
	QVERIFY(QTest::qWaitForWindowExposed(&container));

	/*
	 * Eight four-row panes overflow a 400px column, so its scroll area has
	 * somewhere to go -- the precondition the Pan gesture acts on.
	 */
	QScrollArea *scroll = container.findChild<QScrollArea *>();
	QVERIFY(scroll);
	QScrollBar *bar = scroll->verticalScrollBar();
	QVERIFY(bar);
	QVERIFY2(bar->maximum() > 0, "the column does not overflow, so there is nothing to pan");

	/* Start in the middle, so a drag either way has room. */
	bar->setValue(bar->maximum() / 2);
	const int before = bar->value();

	bssh_terminal_view *view = container.view_at(0);
	QVERIFY(view);
	view->set_input_mode(BSSH_INPUT_COLUMN_SCROLL);

	/*
	 * Drag up the screen. Content follows the finger, so the viewport moves
	 * DOWN the column to a larger scroll position -- and the whole route,
	 * view emit to container scroll, has to run for that to happen.
	 */
	const QPoint from(view->cell_width() + 1, 9 * view->cell_height() + 1);
	const QPoint to(view->cell_width() + 1, 1 * view->cell_height() + 1);
	QMouseEvent press(QEvent::MouseButtonPress, from, view->mapToGlobal(from),
	                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QMouseEvent moved(QEvent::MouseMove, to, view->mapToGlobal(to),
	                   Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
	QMouseEvent release(QEvent::MouseButtonRelease, to, view->mapToGlobal(to),
	                     Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QCoreApplication::sendEvent(view, &press);
	QCoreApplication::sendEvent(view, &moved);
	QCoreApplication::sendEvent(view, &release);

	QVERIFY2(bar->value() > before,
	          "a Pan drag up the screen did not scroll the column down");
}

void layout_test::a_remote_cursor_shape_reaches_the_pane() {
	bssh_main_window window;
	bssh_session *s = window.registry()->create(named(QStringLiteral("a")));
	window.adopt_session(s);

	bssh_terminal_view *view = window.findChild<bssh_terminal_view *>();
	QVERIFY(view);
	QVERIFY(s->emulator());

	/*
	 * The remote asks for a blinking bar, the way vim does on entering
	 * insert mode. feed() processes synchronously and the connection is
	 * direct, so the pane has followed by the time it returns -- shape and
	 * blink both, where the preset had left a steady block.
	 */
	s->emulator()->feed(QByteArrayLiteral("\x1b[5 q"));
	QCOMPARE(view->cursor_style(), QStringLiteral("bar"));
	QVERIFY2(view->cursor_blink_wanted(), "the remote's blink never reached the pane");

	/* And back to a steady block on leaving insert mode. */
	s->emulator()->feed(QByteArrayLiteral("\x1b[2 q"));
	QCOMPARE(view->cursor_style(), QStringLiteral("block"));
	QVERIFY2(!view->cursor_blink_wanted(),
	          "the remote's steady cursor never reached the pane");

	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::the_remote_cursor_shape_can_be_switched_off() {
	/*
	 * Turn the override off in the settings BEFORE the window reads them:
	 * the window's apply_device_settings carries the preference to the
	 * process-wide flag the pane's DECSCUSR handler checks (sec 6.4).
	 */
	bssh_app_settings before_settings;
	const bool original = before_settings.honour_remote_cursor_shape();
	before_settings.set_honour_remote_cursor_shape(false);

	bssh_main_window window;
	bssh_session *s = window.registry()->create(named(QStringLiteral("a")));
	window.adopt_session(s);

	bssh_terminal_view *view = window.findChild<bssh_terminal_view *>();
	QVERIFY(view);
	QVERIFY(s->emulator());
	const QString kept = view->cursor_style();

	/*
	 * The remote asks for a bar, and is ignored: the preset's cursor stays
	 * exactly where it was, because the user turned the override off.
	 */
	s->emulator()->feed(QByteArrayLiteral("\x1b[5 q"));
	QCOMPARE(view->cursor_style(), kept);
	QVERIFY2(!view->cursor_blink_wanted(),
	          "a blink slipped through with the override off");

	/* Put the preference and the process-wide flag back for the next test. */
	bssh_app_settings after_settings;
	after_settings.set_honour_remote_cursor_shape(original);
	bssh_terminal_view::set_honour_remote_cursor_shape(true);

	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::reverse_video_reaches_the_pane_and_flips_it() {
	bssh_main_window window;
	bssh_session *s = window.registry()->create(named(QStringLiteral("a")));
	window.adopt_session(s);

	bssh_terminal_view *view = window.findChild<bssh_terminal_view *>();
	QVERIFY(view);
	QVERIFY(s->emulator());

	const QColor fg = view->terminal_foreground();
	const QColor bg = view->terminal_background();
	QVERIFY2(fg != bg, "a scheme whose fg and bg match cannot show a flip");

	/* Off to begin with: what is painted is the scheme as configured. */
	QVERIFY(!view->reverse_video());
	QCOMPARE(view->effective_background(), bg);
	QCOMPARE(view->effective_foreground(), fg);

	/*
	 * The remote turns reverse video on. The pane flips -- the effective
	 * pair the paint path reads trade places -- while the scheme colours
	 * stay put, so an OSC 10/11 query still answers what the user chose.
	 */
	s->emulator()->feed(QByteArrayLiteral("\x1b[?5h"));
	QVERIFY(view->reverse_video());
	QCOMPARE(view->effective_background(), fg);
	QCOMPARE(view->effective_foreground(), bg);
	QCOMPARE(view->terminal_background(), bg);
	QCOMPARE(view->terminal_foreground(), fg);

	/* And off again, back to the scheme. */
	s->emulator()->feed(QByteArrayLiteral("\x1b[?5l"));
	QVERIFY(!view->reverse_video());
	QCOMPARE(view->effective_background(), bg);

	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::the_find_chord_opens_a_bar_that_searches_the_pane() {
	bssh_main_window window;
	window.resize(800, 500);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session *s = window.registry()->create(named(QStringLiteral("a")));
	window.adopt_session(s);

	/* A sized, shown window, so the pane -- and the emulator it drives --
	 * has real columns for the fed text to land in. */
	for (int i = 0; i < 40; ++i) s->emulator()->feed(QByteArrayLiteral("filler line\r\n"));
	s->emulator()->feed(QByteArrayLiteral("find the treasure here\r\n"));
	s->emulator()->feed(QByteArrayLiteral("and more filler"));

	bssh_find_bar *bar = window.findChild<bssh_find_bar *>();
	QVERIFY(bar);
	QVERIFY2(bar->isHidden(), "the find bar is shown before it is asked for");

	/* The find chord (Ctrl+Shift+F) reaches the router, which the window
	 * turns into open_find. */
	QKeyEvent chord(QEvent::KeyPress, Qt::Key_F,
	                Qt::ControlModifier | Qt::ShiftModifier);
	s->input_router()->handle_key(&chord);
	QVERIFY2(!bar->isHidden(), "the find chord did not open the bar");

	/* Typing a query searches the focused pane. */
	QLineEdit *field = bar->findChild<QLineEdit *>(QStringLiteral("find_field"));
	QVERIFY(field);
	field->setText(QStringLiteral("treasure"));

	bssh_terminal_view *view = window.findChild<bssh_terminal_view *>();
	QVERIFY(view);
	QCOMPARE(view->find_match_count(), 1);
	QCOMPARE(view->current_find_index(), 1);

	/* And the count came back to the bar. */
	QLabel *status = bar->findChild<QLabel *>(QStringLiteral("find_status"));
	QVERIFY(status);
	QCOMPARE(status->text(), QStringLiteral("1/1"));

	/* Escape closes the bar and forgets the matches. */
	QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
	QCoreApplication::sendEvent(bar, &escape);
	QVERIFY2(bar->isHidden(), "Escape did not close the find bar");
	QCOMPARE(view->find_match_count(), 0);

	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::panes_collapse_to_tabs_and_come_back() {
	/*
	 * sec 8.2's hard case, through the real container rather than the
	 * layout manager alone: the user has a two-pane split, then folds the
	 * device or tiles the window into a narrow column. The panes become
	 * tabs, and unfolding restores the split -- the intent is remembered,
	 * not discarded.
	 *
	 * Worth testing at this level because the two halves were each
	 * correct and unconnected for several milestones: nothing in the app
	 * built a container or a layout manager, so none of this ran outside
	 * a unit test of one piece.
	 */
	bssh_layout_manager layout;
	bssh_pane_container panes(&layout);

	bssh_profile_t a;
	a.host = QStringLiteral("one.example.com");
	bssh_profile_t b;
	b.host = QStringLiteral("two.example.com");

	bssh_session first(a);
	bssh_session second(b);

	layout.set_width(1200);
	layout.set_intent({ &first, &second });
	panes.rebuild();

	QCOMPARE(panes.pane_count(), 2);
	QVERIFY(!layout.is_collapsed());

	/* Folded, or tiled into a phone-width column. */
	layout.set_width(400);
	panes.rebuild();

	QCOMPARE(panes.pane_count(), 1);
	QVERIFY2(layout.is_collapsed(), "a narrow window must report its splits as collapsed");

	/*
	 * The session that vanished is still in the intent -- it became a
	 * tab, it was not closed. Losing it here would mean folding a phone
	 * silently dropped a connection.
	 */
	QCOMPARE(layout.intent().size(), 2);
	QVERIFY(layout.intent().contains(&second));

	layout.set_width(1200);
	panes.rebuild();

	QCOMPARE(panes.pane_count(), 2);
	QVERIFY(!layout.is_collapsed());
}

namespace {

/*
 * The narrow phone width sec 8.2 designs to. Asked of the MINIMUM size
 * hint: that is the width below which a layout refuses to go, so anything
 * larger is content the user cannot reach on a screen this size.
 */
/*
 * Forces the arrangement for as long as it is in scope.
 *
 * A guard rather than a call and a matching reset at the bottom: QVERIFY
 * returns from the function it fails in, so the reset at the bottom is
 * exactly the line that does not run on the day it matters -- and every
 * test after it would then be measuring the wrong arrangement, reporting
 * a failure somewhere else entirely.
 */

QString too_wide(const QString &name, const QWidget *widget)
{
	return QStringLiteral("%1 demands %2px on a %3px screen")
	        .arg(name).arg(widget->minimumSizeHint().width()).arg(BSSH_PHONE_DP);
}

/*
 * Anything that ends up beyond the right edge once the surface is squeezed
 * to a phone. Reported by name, because "something is off-screen" is not
 * a thing anybody can act on.
 */
QString overflowing_child(const QWidget *root)
{
	for (const QWidget *child : root->findChildren<QWidget *>()) {
		if (!child->isVisible() || child->width() == 0) continue;

		/*
		 * Content inside a scroll area is ALLOWED to be wider than the
		 * screen -- that is what a scroll area is for, and flagging it
		 * would fail the fix for an overflow rather than the overflow.
		 * What matters is that the scrolling widget itself fits, which
		 * the minimum-width check above already asks.
		 *
		 * Found by adding the key row to this list: its keys legitimately
		 * run to 652px inside a viewport 360px wide.
		 */
		bool inside_scroll_area = false;
		const QWidget *up = child->parentWidget();
		while (up && up != root) {
			if (qobject_cast<const QAbstractScrollArea *>(up)) {
				inside_scroll_area = true;
				break;
			}
			up = up->parentWidget();
		}
		if (inside_scroll_area) continue;

		const QPoint corner = child->mapTo(root, QPoint(child->width() - 1, 0));
		if (corner.x() > BSSH_PHONE_DP) {
			return QStringLiteral("%1 \"%2\" ends at %3px")
			        .arg(QString::fromLatin1(child->metaObject()->className()),
			              child->accessibleName().isEmpty() ? child->objectName()
			                                                 : child->accessibleName())
			        .arg(corner.x());
		}
	}
	return QString();
}

/*
 * `sideways` is for the one surface whose design IS horizontal scrolling:
 * sec 6.2's key row, whose keys must run past the edge rather than shrink
 * under the touch floor. Every other surface is a form, and a form that
 * scrolls sideways on a phone is a form nobody reads.
 */
void check_fits(const QString &name, QWidget *widget, bool sideways = false)
{
	widget->resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	widget->show();
	QVERIFY2(QTest::qWaitForWindowExposed(widget), qPrintable(name));

	/*
	 * Height is the other half, and it fails differently: too WIDE puts
	 * content off the right edge, too TALL makes Qt squeeze rows past
	 * their minimums until they draw on top of each other. The host
	 * editor wanted 821px of a 640px screen and the preset editor 1063,
	 * and both overlapped on a device rather than clipping.
	 *
	 * Asked as one question -- does the MINIMUM fit -- rather than "does
	 * it fit or is there a scroll area somewhere". The first version
	 * asked the second, and passed a deliberately broken preset editor:
	 * the scroll area was still a child of the page, just no longer the
	 * thing the fields were inside, and findChildren cannot tell the
	 * difference. A working scroll area drops the page's own minimum to
	 * what its frame needs -- 1063px became 305 -- so the simpler
	 * question is also the stronger one.
	 */
	QVERIFY2(widget->minimumSizeHint().height() <= BSSH_PHONE_TALL_DP,
	          qPrintable(QStringLiteral("%1 needs %2px of a %3px screen, so its rows "
	                                     "will be squeezed until they overlap")
	                             .arg(name).arg(widget->minimumSizeHint().height())
	                             .arg(BSSH_PHONE_TALL_DP)));

	QVERIFY2(widget->minimumSizeHint().width() <= BSSH_PHONE_DP,
	          qPrintable(too_wide(name, widget)));

	const QString overflow = overflowing_child(widget);
	QVERIFY2(overflow.isEmpty(), qPrintable(QStringLiteral("%1: %2").arg(name, overflow)));

	/*
	 * A page may scroll DOWN. It may not scroll sideways.
	 *
	 * The overflow walk above skips whatever is inside a scroll area,
	 * because a scroll area's content legitimately IS taller than its
	 * viewport (sec 6.2) -- and that exemption was hiding a form wider
	 * than the phone it is drawn on. The settings page came out with its
	 * notes cut mid-word and its combo boxes running off the right edge,
	 * and every width assertion here passed while it did.
	 *
	 * So the exemption is for HEIGHT only: a scroll area's content has to
	 * fit the width it was given, or the user is reading a form through a
	 * letterbox.
	 */
	/*
	 * A list's ROWS are tapped, and they are not widgets -- so the walk
	 * above cannot see them and the licences list stood at 17dp rows,
	 * a third of the floor, with every check passing.
	 */
	int checked = 0;

	for (QAbstractItemView *view : widget->findChildren<QAbstractItemView *>()) {
		if (view->model() == nullptr || view->model()->rowCount() < 1) continue;

		++checked;

		QString complaint = QStringLiteral(
		  "%1: a %2 has %3dp rows, and sec 6.2's floor is %4 -- "
		  "a row is tapped, so it is a target like any other");
		complaint = complaint.arg(name);
		complaint = complaint.arg(QString::fromLatin1(view->metaObject()->className()));
		complaint = complaint.arg(view->sizeHintForRow(0)).arg(BSSH_TOUCH_TARGET_DP);

		QVERIFY2(view->sizeHintForRow(0) >= BSSH_TOUCH_TARGET_DP, qPrintable(complaint));
	}

	for (QScrollArea *area : widget->findChildren<QScrollArea *>()) {
		QWidget *content = area->widget();
		if (!content || sideways) continue;

		const int room = area->viewport()->width();
		const int wanted = content->minimumSizeHint().width();

		/*
		 * Name the widest thing on the page, because the number alone
		 * sends the reader hunting: a form is one column of labels and
		 * one of fields, and which widget is asking is the whole answer.
		 */
		QString widest;
		int widest_px = 0;

		for (QWidget *child : content->findChildren<QWidget *>()) {
			if (child->isHidden()) continue;

			const int needs = child->minimumSizeHint().width();
			if (needs <= widest_px) continue;

			widest_px = needs;
			widest = QString::fromLatin1(child->metaObject()->className());

			const QString label = child->property("text").toString();
			if (!label.isEmpty()) {
				widest += QStringLiteral(" \"") + label.left(40) + QLatin1Char('"');
			}
		}

		QString complaint = QStringLiteral(
		  "%1: the page inside its scroll area wants %2px of %3 -- "
		  "a form that scrolls sideways on a phone is a form nobody "
		  "reads. The widest thing on it is %4, asking %5px");
		complaint = complaint.arg(name).arg(wanted).arg(room);
		complaint = complaint.arg(widest).arg(widest_px);

		QVERIFY2(wanted <= room, qPrintable(complaint));
	}

	/*
	 * And every control on it can be hit (sec 6.2's floor).
	 *
	 * Fitting a phone is not the same as being usable on one: a form that
	 * fits perfectly out of 24px rows is a form nobody can tap. The floor
	 * was a static in seven files under four names and a bare 48 fifteen
	 * times over, so nothing could have checked it -- there was no "it" to
	 * check. Now there is one number, and this is what holds every touch
	 * surface to it at once rather than each surface remembering.
	 *
	 * The things a finger AIMS at: a text field, a button, a box you pick
	 * from, a switch. Labels and notes are read rather than hit and have
	 * no floor. Anything hidden is skipped -- a control on a page this
	 * surface is not showing is not on screen to be missed.
	 */
	for (QWidget *control : widget->findChildren<QWidget *>()) {
		/*
		 * This test's OWN list, deliberately not the one the code sizes
		 * from. Sharing it made the check unfalsifiable: deleting a
		 * type removed the requirement and the check in one edit, so
		 * the one failure it exists for could never be reported. The
		 * sabotage runner is what keeps the two honest -- a spec that
		 * drops sliders from the code must fail here.
		 *
		 * Asking "does it take keyboard focus" instead was tried and is
		 * wrong in BOTH directions, which is worth writing down because
		 * it is such a tidy-looking idea. A QLabel with selectable text
		 * takes focus and is read rather than tapped, so the settings
		 * page failed on a paragraph. And the key row's buttons take
		 * NO focus, deliberately -- tapping a key must not steal focus
		 * from the terminal -- so the most important tap targets in the
		 * app were skipped entirely.
		 */
		bool operable = qobject_cast<QAbstractButton *>(control) != nullptr;
		if (qobject_cast<QLineEdit *>(control)) operable = true;
		if (qobject_cast<QComboBox *>(control)) operable = true;
		if (qobject_cast<QAbstractSpinBox *>(control)) operable = true;
		if (qobject_cast<QKeySequenceEdit *>(control)) operable = true;
		if (qobject_cast<QSlider *>(control)) operable = true;

		if (!operable || !control->isVisible()) continue;

		/*
		 * A control INSIDE another control is that one's business, and
		 * this is the check being wrong rather than the code: a
		 * QSpinBox that stands a correct 48 tall holds a QLineEdit of
		 * 42 -- the difference is its own frame -- and a combo box and
		 * a key-sequence editor are built the same way. What a thumb
		 * aims at is the outermost of them, so an ANCESTOR that is
		 * itself a target takes responsibility for the whole thing.
		 *
		 * Written as a walk rather than one parentWidget() test,
		 * because a first version tested only the immediate parent and
		 * would have gone on reporting whatever nests two deep.
		 */
		bool inside_a_target = false;
		for (QWidget *up = control->parentWidget(); up; up = up->parentWidget()) {
			/*
			 * The type list here, deliberately, where the test above
			 * refuses it.
			 *
			 * The two uses are not symmetric. Above, the list decides
			 * what MUST be checked, so a type missing from it is a
			 * control nobody checks -- silent. Here it decides what
			 * may be SKIPPED, so a type missing from it means the
			 * inner widget gets checked too, which at worst fails
			 * loudly and is corrected. A list is safe when the cost
			 * of it being incomplete is noise rather than silence.
			 *
			 * Asking "is this ancestor focusable" instead looked
			 * neater and was much worse: QScrollArea takes focus, so
			 * every control on a scrolling page was exempted and this
			 * check quietly stopped examining the settings page
			 * altogether.
			 */
			if (bssh_is_touch_target(up)) {
				inside_a_target = true;
				break;
			}
		}
		if (inside_a_target) continue;

		const QString what = QString::fromLatin1(control->metaObject()->className());
		QString complaint = QStringLiteral(
		  "%1: a %2 is %3dp tall, and sec 6.2's floor is %4 -- "
		  "a thumb cannot reliably hit it");
		complaint = complaint.arg(name, what).arg(control->height());
		complaint = complaint.arg(BSSH_TOUCH_TARGET_DP);

		QVERIFY2(control->height() >= BSSH_TOUCH_TARGET_DP, qPrintable(complaint));
		++checked;
	}

	/*
	 * And it looked at SOMETHING. A loop that skipped everything reports
	 * success exactly as loudly as one that examined forty, and this
	 * check has been vacuous twice -- once because its type list was
	 * short, once because an exemption swallowed everything inside a
	 * scroll area. Both times the suite stayed green.
	 *
	 * One, not some larger number: a surface whose content is a LIST has
	 * almost no controls of its own -- the licences view has a single
	 * Back button and the host list two -- and its rows are counted here
	 * as well, because they are what the row check above examined. A
	 * threshold picked to feel thorough fails those honestly-small
	 * surfaces and teaches whoever meets it to raise the number.
	 */
	QVERIFY2(checked > 0, qPrintable(QStringLiteral(
	          "%1: the touch floor examined nothing at all, so it is not checking "
	          "this surface").arg(name)));
}

} /* namespace */

void layout_test::every_touch_surface_fits_a_phone()
{
	/*
	 * In the TOUCH arrangement, which this was not asking for.
	 *
	 * Most of these widgets decide their shape from is_touch_primary(),
	 * and on the desktop build that runs the suite it is false -- so this
	 * had been holding the DESKTOP arrangement to a phone's width and
	 * calling it a touch test. The touch forms, which are the ones with
	 * 48dp controls and stacked columns, were never measured at all.
	 */
	const forced_touch_t arrangement(1);

	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));

	/*
	 * A host with the kind of length people really have. An empty store
	 * would let a list that cannot fit a real entry pass, which is the
	 * mistake the password prompt's first test nearly made.
	 */
	bssh_profile_t profile;
	profile.name = QStringLiteral("build server (europe)");
	profile.host = QStringLiteral("build.internal.example.com");
	profile.user = QStringLiteral("nabbe.testing");
	profile.port = 2299;
	profile.identity_file = QStringLiteral("~/.ssh/id_ed25519_work");
	profile.proxy_jump = QStringLiteral("nabbe@bastion.internal.example.com:2222");
	QVERIFY(store.save(profile) != 0);

	{
		bssh_host_list list(&store);
		check_fits(QStringLiteral("host list"), &list);
	}
	{
		bssh_host_editor editor(&store);
		editor.set_profile(profile);
		check_fits(QStringLiteral("host editor"), &editor);
	}
	{
		bssh_preset_editor editor(&store, bssh_preset_editor::TOUCH);
		check_fits(QStringLiteral("preset editor"), &editor);
	}
	{
		bssh_app_settings_view settings;
		check_fits(QStringLiteral("settings"), &settings);
	}
	{
		bssh_licences_view licences;
		check_fits(QStringLiteral("licences"), &licences);
	}
	{
		bssh_font_picker picker;
		check_fits(QStringLiteral("font picker"), &picker);
	}
	{
		bssh_password_prompt prompt(QStringLiteral("nabbe.testing@build.internal.example.com:2299"),
		                             true);
		check_fits(QStringLiteral("password prompt"), &prompt);
	}

}

namespace {

/*
 * Every visible control on one scrolling page, measured against its own
 * text, at the width the page's scroll area actually gives it.
 *
 * A helper rather than one page's test body, because the fudge that
 * prompted this covered THREE pages and the check covered one. The
 * settings page's viewport comes out 306 inside 320dp; the preset
 * editor's is 284, and 284 was never measured by anything -- `make
 * shots` drew it at BSSH_PHONE_W - 20 = 300, sixteen pixels WIDER than
 * the device, so a control clipping between 284 and 300 was invisible
 * in the one instrument that could have shown it.
 *
 * That is the more dangerous direction of the same error that made the
 * combo look clipped on the settings page: there the picture was six
 * pixels narrow and manufactured a defect, here it was sixteen wide and
 * would have hidden one.
 */
/*
 * Whether a line edit shows text the APPLICATION chose.
 *
 * The distinction decides what may be asserted about it, and getting it
 * wrong is how this check nearly acquired an unsatisfiable arm. A chord,
 * a port number, a scrollback count and a combo's current item all come
 * from a bounded vocabulary, so there is a width that fits them and a
 * field narrower than that is a defect. A hostname, a key path or a
 * ProxyJump is whatever somebody typed: no width fits it, and scrolling
 * horizontally is what a QLineEdit is FOR.
 *
 * So the free-form fields get a different question asked of them, below,
 * rather than an assertion no layout could ever satisfy. An assertion
 * nothing can satisfy is not a guard -- it is a finding waiting to be
 * waived, and the waiver is what switches the check off.
 *
 * Qt makes the test structural rather than a list of names: a spin box
 * and a key-sequence editor each own a QLineEdit, and findChildren
 * reaches it, so what is left after those and combo boxes are excluded
 * is exactly the free-form set. It earns its place on the settings page
 * alone: that page's window-title format is free-form, was being
 * measured, and passed only because "%t - BeerSSH" happens to be short.
 */
bool bssh_shows_a_chosen_value(const QLineEdit *edit) {
	if (edit->isReadOnly()) return true;
	for (const QWidget *up = edit->parentWidget(); up; up = up->parentWidget()) {
		if (qobject_cast<const QAbstractSpinBox *>(up)) return true;
		if (qobject_cast<const QKeySequenceEdit *>(up)) return true;
		if (qobject_cast<const QComboBox *>(up)) return true;
	}
	return false;
}

QStringList bssh_clipped_controls(QWidget *surface, int *checked) {
	QStringList clipped;
	*checked = 0;

	QScrollArea *scroll = surface->findChild<QScrollArea *>();
	if (!scroll) return clipped;
	QWidget *page = scroll->widget();
	if (!page) return clipped;

	/*
	 * The page is inside a scroll area, so it has to be given its real
	 * height before anything is measured -- a widget still at its
	 * default size reports widths nobody will ever see.
	 */
	page->resize(scroll->viewport()->width(), page->sizeHint().height());
	QCoreApplication::processEvents();

	for (const QLineEdit *edit : page->findChildren<QLineEdit *>()) {
		if (!edit->isVisible() || edit->text().isEmpty()) continue;
		if (!bssh_shows_a_chosen_value(edit)) continue;
		/*
		 * A combo's own line edit is measured by the combo arm below,
		 * against the combo's width -- counting it here as well would
		 * inflate the population the floor is checked against, which
		 * is the number standing in for "this measured something".
		 */
		bool inside_a_combo = false;
		for (const QWidget *up = edit->parentWidget(); up && !inside_a_combo;
		      up = up->parentWidget())
			if (qobject_cast<const QComboBox *>(up)) inside_a_combo = true;
		if (inside_a_combo) continue;

		++*checked;
		const int needs = edit->fontMetrics().horizontalAdvance(edit->text());
		if (edit->contentsRect().width() < needs) {
			clipped.append(QStringLiteral("%1 shows \"%2\" in %3px of %4 needed")
			                       .arg(QString::fromLatin1(edit->metaObject()->className()),
			                             edit->text())
			                       .arg(edit->contentsRect().width()).arg(needs));
		}
	}

	for (const QComboBox *box : page->findChildren<QComboBox *>()) {
		if (!box->isVisible() || box->currentText().isEmpty()) continue;
		++*checked;
		const int arrow = box->style()->pixelMetric(QStyle::PM_ScrollBarExtent);
		const int needs = box->fontMetrics().horizontalAdvance(box->currentText()) + arrow;
		if (box->width() < needs) {
			clipped.append(QStringLiteral("QComboBox shows \"%1\" in %2px of %3 needed")
			                       .arg(box->currentText())
			                       .arg(box->width()).arg(needs));
		}
	}

	return clipped;
}

/*
 * The other half of the question, for the fields no width can satisfy.
 *
 * A free-form field cannot be asserted to fit its text, so what IS
 * assertable is that it was given every pixel the page has: on a phone
 * its label sits ABOVE it rather than beside it, and the two share a
 * left edge. That is the outcome QFormLayout::WrapAllRows produces and
 * the thing a reader can check against the picture.
 *
 * Asked of the label's geometry rather than of the policy, because the
 * policy is the mechanism and a form can be given one and still lay out
 * two columns -- WrapLongRows is exactly that case, wrapping only rows
 * whose minimum will not fit, which a field willing to scroll never has.
 */
void check_free_form_fields_own_their_row(const QString &name, QWidget *surface,
                                            int floor) {
	/*
	 * At the phone floor and laid out, or every geometry read below is
	 * the widget's default -- which is a size nobody will ever see, and
	 * would answer the question about a form that was never arranged.
	 */
	surface->resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	surface->show();
	QVERIFY(QTest::qWaitForWindowExposed(surface));
	QCoreApplication::processEvents();

	QFormLayout *form = surface->findChild<QFormLayout *>();
	QVERIFY2(form, qPrintable(QStringLiteral("%1 is a form, and this found none")
	                                  .arg(name)));

	int checked = 0;
	QStringList beside;

	for (int row = 0; row < form->rowCount(); ++row) {
		QLayoutItem *label_item = form->itemAt(row, QFormLayout::LabelRole);
		QLayoutItem *field_item = form->itemAt(row, QFormLayout::FieldRole);
		if (!label_item || !field_item) continue;

		QWidget *label = label_item->widget();
		QLineEdit *field = qobject_cast<QLineEdit *>(field_item->widget());
		if (!label || !field || bssh_shows_a_chosen_value(field)) continue;

		++checked;
		if (label->x() != field->x())
			beside.append(QStringLiteral("\"%1\" starts at x=%2 with its label at "
			                              "x=%3, so it is sharing the row -- %4px wide")
			                      .arg(field->text()).arg(field->x())
			                      .arg(label->x()).arg(field->width()));
	}

	QTextStream(stdout) << QStringLiteral("  %1  %2 free-form fields\n")
	                               .arg(name, -16).arg(checked);

	QVERIFY2(checked >= floor,
	          qPrintable(QStringLiteral("%1: only %2 free-form fields were found of %3 "
	                                     "expected, so this checked almost nothing")
	                             .arg(name).arg(checked).arg(floor)));

	QVERIFY2(beside.isEmpty(),
	          qPrintable(QStringLiteral("%1 at %2dp: %3 free-form field(s) still share a "
	                                     "row with their label:\n  %4")
	                             .arg(name).arg(BSSH_PHONE_DP).arg(beside.size())
	                             .arg(beside.join(QStringLiteral("\n  ")))));
}

/*
 * One page, shown at the phone floor and measured.
 *
 * The floor is passed in rather than shared, because a page that laid
 * out none of its controls would otherwise report nothing clipped and
 * read as a pass -- the gate over an empty file list, in a form this
 * project has met before. It is the count this page actually measures,
 * so adding a control keeps it green and losing one does not.
 */
void check_no_control_clips(const QString &name, QWidget *surface, int floor) {
	surface->resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	surface->show();
	QVERIFY(QTest::qWaitForWindowExposed(surface));

	int checked = 0;
	const QStringList clipped = bssh_clipped_controls(surface, &checked);

	QTextStream(stdout) << QStringLiteral("  %1  %2 controls measured\n")
	                               .arg(name, -16).arg(checked);

	QVERIFY2(checked >= floor,
	          qPrintable(QStringLiteral("%1: only %2 controls were measured of %3 "
	                                     "expected, so this checked almost nothing")
	                             .arg(name).arg(checked).arg(floor)));

	QVERIFY2(clipped.isEmpty(),
	          qPrintable(QStringLiteral("%1 at %2dp: %3 control(s) are drawn narrower "
	                                     "than their own text:\n  %4")
	                             .arg(name).arg(BSSH_PHONE_DP).arg(clipped.size())
	                             .arg(clipped.join(QStringLiteral("\n  ")))));
}

}

/*
 * Every setting is on exactly one section page, and every section is
 * shorter than the page it came out of (project.md sec 8.5).
 *
 * The first half is what makes splitting a page safe: a row whose
 * section nobody assigned would simply stop existing on touch, and
 * nothing else here would notice -- the desktop still builds EVERYTHING,
 * so its own tests would stay green while the phone quietly lost a
 * setting.
 *
 * The second is asserted as a RELATIONSHIP rather than against a number.
 * A pixel bound would be a constant somebody has to maintain, and worse,
 * it could be met by shrinking the content -- which sec 12.3b is
 * explicit about, since trimming the descriptions to reach a figure
 * deforms the thing being measured. Half the whole page cannot be
 * satisfied that way.
 */
/*
 * A section page shows nothing belonging to another section (sec 8.5).
 *
 * The split hides the widgets a page leaves out, and hiding is all it
 * can do: they stay children of the view, out of its form. So anything
 * that SHOWS one again puts it back unlaid-out at the top-left corner,
 * over whatever is drawn there.
 *
 * refresh_keyboard_fallback() did exactly that. It ends by showing the
 * note, and it is called from resizeEvent, so on the device "This screen
 * is too narrow for the full-size keyboard" was painted across the
 * Shortcuts heading. Every assertion was green: the note is a legitimate
 * widget, correctly built, and no width or height was wrong.
 *
 * Asserted STRUCTURALLY rather than against that one note, because the
 * next slot to re-show a member would land in exactly the same corner
 * and a test naming the keyboard fallback could not see it. A widget
 * that is visible and not in the view's own layout is the fault, whoever
 * put it there.
 *
 * It needs the device's combination to appear at all -- the built-in
 * FULL keyboard chosen and a screen too narrow to draw it -- so the
 * setting is forced here rather than left to whatever the machine has.
 */
void layout_test::no_section_page_shows_a_widget_from_another_section() {
	const forced_touch_t thumb(1);

	bssh_app_settings settings;
	const bssh_app_settings::keyboard_t before = settings.keyboard();
	settings.set_keyboard(bssh_app_settings::KEYBOARD_BUILT_IN_FULL);

	const bssh_app_settings_view::section_t order[] = {
		bssh_app_settings_view::SECTION_APPEARANCE,
		bssh_app_settings_view::SECTION_TERMINAL,
		bssh_app_settings_view::SECTION_PASSWORDS,
		bssh_app_settings_view::SECTION_SHORTCUTS,
	};

	for (bssh_app_settings_view::section_t which : order) {
		bssh_app_settings_view page(which, nullptr);
		page.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
		page.show();
		QVERIFY(QTest::qWaitForWindowExposed(&page));

		/* A second resize, because the escape happens in resizeEvent. */
		page.resize(BSSH_PHONE_DP - 4, BSSH_PHONE_TALL_DP);
		QCoreApplication::processEvents();

		QLayout *own = page.layout();
		QVERIFY2(own, "the settings view must have a layout of its own");

		QStringList stray;
		for (QWidget *child : page.findChildren<QWidget *>(
		             QString(), Qt::FindDirectChildrenOnly)) {
			if (!child->isVisible()) continue;
			if (own->indexOf(child) >= 0) continue;
			stray.append(QStringLiteral("%1 \"%2\" at %3,%4")
			                     .arg(QString::fromLatin1(child->metaObject()->className()),
			                           child->property("text").toString().left(40))
			                     .arg(child->x()).arg(child->y()));
		}

		QVERIFY2(stray.isEmpty(),
		          qPrintable(QStringLiteral("the %1 page shows %2 widget(s) that are in "
		                                     "no layout of its own:\n  %3")
		                             .arg(bssh_app_settings_view::section_name(which))
		                             .arg(stray.size())
		                             .arg(stray.join(QStringLiteral("\n  ")))));
		page.hide();
	}

	settings.set_keyboard(before);
}

void layout_test::every_setting_is_on_exactly_one_section() {
	const forced_touch_t thumb(1);

	const bssh_app_settings_view::section_t order[] = {
		bssh_app_settings_view::SECTION_KEYBOARD,
		bssh_app_settings_view::SECTION_APPEARANCE,
		bssh_app_settings_view::SECTION_TERMINAL,
		bssh_app_settings_view::SECTION_PASSWORDS,
		bssh_app_settings_view::SECTION_SHORTCUTS,
	};

	const auto controls_on = [](bssh_app_settings_view *page) {
		int seen = 0;
		for (QWidget *w : page->findChildren<QWidget *>()) {
			if (!w->isVisible()) continue;
			if (qobject_cast<QComboBox *>(w) || qobject_cast<QCheckBox *>(w)
			     || qobject_cast<QLineEdit *>(w) || qobject_cast<QSlider *>(w)
			     || qobject_cast<QKeySequenceEdit *>(w))
				++seen;
		}
		return seen;
	};

	const auto natural = [](bssh_app_settings_view *page) {
		QScrollArea *area = page->findChild<QScrollArea *>();
		QWidget *content = area ? area->widget() : nullptr;
		if (!content) return 0;
		content->resize(area->viewport()->width(), 1);
		QCoreApplication::processEvents();
		return content->sizeHint().height();
	};

	bssh_app_settings_view whole(bssh_app_settings_view::EVERYTHING, nullptr);
	whole.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	whole.show();
	QVERIFY(QTest::qWaitForWindowExposed(&whole));
	const int all_controls = controls_on(&whole);
	const int all_height = natural(&whole);
	QVERIFY2(all_controls > 10,
	          qPrintable(QStringLiteral("only %1 controls on the whole page, so this "
	                                     "compares almost nothing").arg(all_controls)));

	int summed = 0;
	for (bssh_app_settings_view::section_t which : order) {
		bssh_app_settings_view page(which, nullptr);
		page.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
		page.show();
		QVERIFY(QTest::qWaitForWindowExposed(&page));

		summed += controls_on(&page);

		const int tall = natural(&page);
		QVERIFY2(tall * 2 <= all_height,
		          qPrintable(QStringLiteral("the %1 section is %2px against the whole "
		                                     "page's %3 -- splitting it bought nothing")
		                             .arg(bssh_app_settings_view::section_name(which))
		                             .arg(tall).arg(all_height)));
		page.hide();
	}

	QVERIFY2(summed == all_controls,
	          qPrintable(QStringLiteral("the sections carry %1 controls and the whole "
	                                     "page has %2 -- a setting is on no section, or "
	                                     "on two").arg(summed).arg(all_controls)));
}

void layout_test::a_form_stacks_on_width_not_on_touch() {
	/*
	 * The threshold first, so a failure below reads as a wiring fault
	 * rather than an argument about where narrow starts -- and asserted
	 * from both sides of sec 8.2's COMPACT boundary rather than sampled
	 * from one, since a rule that only ever says "stack" passes the phone
	 * arm and says nothing.
	 */
	QVERIFY(bssh_form_stacks(BSSH_PHONE_DP));
	QVERIFY(bssh_form_stacks(599));
	QVERIFY(!bssh_form_stacks(600));
	QVERIFY(!bssh_form_stacks(BSSH_SMALL_DESKTOP_W));

	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));

	/*
	 * Asked of the forms that SAY they key off width, rather than of a
	 * list written here: a hand-written population keeps passing about
	 * the forms somebody remembered. Each editor also holds a font
	 * picker with a form of its own, which is not watched -- so this
	 * finding exactly one per page is itself the check that the name
	 * selects rather than sweeps.
	 */
	const auto watched = [](QWidget *page) {
		QList<QFormLayout *> forms;
		for (QFormLayout *form : page->findChildren<QFormLayout *>()) {
			if (form->objectName() == BSSH_WIDTH_KEYED_FORM) forms.append(form);
		}

		return forms;
	};

	for (int touch = 0; touch <= 1; ++touch) {
		const forced_touch_t device(touch);

		bssh_host_editor host(&store);
		bssh_preset_editor presets(&store, bssh_preset_editor::TOUCH);

		QList<QPair<QString, QWidget *> > pages;
		pages.append(qMakePair(QStringLiteral("host editor"),
		                        static_cast<QWidget *>(&host)));
		pages.append(qMakePair(QStringLiteral("preset editor"),
		                        static_cast<QWidget *>(&presets)));

		for (const QPair<QString, QWidget *> &page : pages) {
			const QList<QFormLayout *> forms = watched(page.second);
			QVERIFY2(forms.size() == 1,
			          qPrintable(QStringLiteral("%1 has %2 width-keyed forms, "
			                                     "expected exactly one")
			                             .arg(page.first).arg(forms.size())));

			QFormLayout *form = forms.first();

			page.second->resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
			page.second->show();
			QCoreApplication::processEvents();
			QVERIFY2(form->rowWrapPolicy() == QFormLayout::WrapAllRows,
			          qPrintable(QStringLiteral("%1 at %2dp with touch=%3 kept its "
			                                     "labels beside its fields")
			                             .arg(page.first).arg(BSSH_PHONE_DP).arg(touch)));

			page.second->resize(BSSH_SMALL_DESKTOP_W, BSSH_PHONE_TALL_DP);
			QCoreApplication::processEvents();
			QVERIFY2(form->rowWrapPolicy() == QFormLayout::DontWrapRows,
			          qPrintable(QStringLiteral("%1 at %2px with touch=%3 stacked its "
			                                     "labels above fields that had room")
			                             .arg(page.first).arg(BSSH_SMALL_DESKTOP_W).arg(touch)));

			page.second->hide();
		}
	}
}

void layout_test::no_control_is_narrower_than_the_text_it_shows() {
	const forced_touch_t thumb(1);

	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));

	bssh_profile_t profile;
	profile.name = QStringLiteral("build server (europe)");
	profile.host = QStringLiteral("build.internal.example.com");
	profile.user = QStringLiteral("nabbe.testing");
	profile.port = 2299;
	profile.identity_file = QStringLiteral("~/.ssh/id_ed25519_work");
	profile.proxy_jump = QStringLiteral("nabbe@bastion.internal.example.com:2222");
	QVERIFY(store.save(profile) != 0);

	/*
	 * WHAT THIS HAS AND HAS NOT BEEN SEEN TO CATCH, because the arms are
	 * not equally proved and a pass reads as though they were.
	 *
	 * The QLineEdit arm HAS: removing the chord editors' minimum width
	 * makes the settings page fail naming four of them -- "Shift+PgDown"
	 * in 67px of 80 needed, and three more. That is the fix it guards.
	 *
	 * The QComboBox arm has NOT, and not because it cannot fire.
	 * Measured with the combo fix disabled at the settings page's width:
	 * "BeerSSH's own, compact" is 161px wide needing 152, so it FITS
	 * with nine to spare and there is nothing to report. The clipping
	 * once seen in `make shots` was at that tool's old page width of 300
	 * -- six pixels narrower than the 306 this page really gets -- and
	 * was reported as a device defect it is not. That fudge is gone.
	 *
	 * Nine pixels is not a margin worth trusting: the font picker was
	 * 222px here and 285 to 302 in the CI container on the same code,
	 * because a form's width is a function of the UI font. So the combo
	 * fix stays, justified by that measurement rather than by this test,
	 * and this comment stands instead of a claim that it was proved.
	 */
	/*
	 * THE CLIPPING ARM STAYS ON ONE PAGE, and that is sec 12.3i's
	 * decision rather than an oversight -- it measured the combos on
	 * every other touch surface and found the narrowest at roughly
	 * double what it needs, against the nine pixels the settings page
	 * had. A guard that cannot fail on three of the four pages it
	 * covered reads as broader coverage than it has.
	 *
	 * What sec 12.3i did NOT ask is why the host editor's field was
	 * 218px wide in the first place. It read the clipping there as
	 * legitimate, which it is -- a line edit scrolls, and no width fits
	 * text nobody chose -- and stopped, because the question it was
	 * asking was whether to assert. The layout underneath was still
	 * two columns on a phone, and that is what the second arm is for.
	 */
	{
		bssh_app_settings_view settings;
		check_no_control_clips(QStringLiteral("settings"), &settings, 15);
	}

	{
		bssh_host_editor editor(&store);
		editor.set_profile(profile);
		check_free_form_fields_own_their_row(QStringLiteral("host editor"), &editor, 5);
	}
	{
		/*
		 * The narrowest page in the app -- 284px of scroll-area width
		 * against the other two pages' 306 -- and three of its fields
		 * are free-form: Name, TERM and On connect. The last one holds
		 * a shell command, so it is the field most likely to carry a
		 * long string and it had the least room of any in the app.
		 */
		bssh_preset_editor editor(&store, bssh_preset_editor::TOUCH);
		check_free_form_fields_own_their_row(QStringLiteral("preset editor"),
		                                      &editor, 3);
	}
}

namespace {

/*
 * One keyboard's shape: same rows in every group, every row exactly the
 * group's own width, and no group wider than the widget clears stretch
 * for. Called once per style rather than wrapped in a loop, so the body
 * keeps its indentation and the failure message names which keyboard.
 */
void check_keyboard_shape(bssh_soft_keyboard_catalog::style_t style) {
	const QList<bssh_soft_key_group_t> &groups =
	        bssh_soft_keyboard_catalog::instance().all(style);

	QVERIFY2(groups.size() >= 2,
	          "a side button that walks one group is a button with nothing to do");

	const int rows = groups.first().rows.size();
	QVERIFY2(rows > 0, "a group with no rows is a keyboard with no keys");

	for (const bssh_soft_key_group_t &group : groups) {
		/*
		 * Same number of ROWS in every group.
		 *
		 * Not tidiness. The keyboard is docked under the terminal, so a
		 * group one row taller than another shrinks the terminal when
		 * the side button is pressed -- a resize on the wire (sec 6.3)
		 * and a reflow of whatever the remote is drawing, on every tap
		 * of the button whose whole purpose is to change which keys are
		 * shown.
		 */
		QString complaint = QStringLiteral(
		  "the %1 group has %2 rows and %3 has %4 -- switching between them "
		  "would resize the terminal and reflow the remote's output");
		complaint = complaint.arg(group.display_name).arg(group.rows.size());
		complaint = complaint.arg(groups.first().display_name).arg(rows);
		QVERIFY2(group.rows.size() == rows, qPrintable(complaint));

		for (int r = 0; r < group.rows.size(); ++r) {
			int columns = 0;
			for (const bssh_key_t &key : group.rows.at(r)) {
				QVERIFY2(key.span > 0, "a key of no width cannot be pressed");
				columns += key.span;
			}

			/*
			 * And every row is exactly the grid's width. A short row
			 * leaves its last key stretched across the gap, so the keys
			 * above and below it stop lining up -- which on a keyboard
			 * is not a cosmetic fault: a thumb aims at where the key was
			 * on the previous row.
			 */
			QString wrong = QStringLiteral(
			  "%1 row %2 adds to %3 columns, not %4 -- its keys will not "
			  "line up with the rows above and below");
			wrong = wrong.arg(group.display_name).arg(r);
			wrong = wrong.arg(columns).arg(group.columns);
			QVERIFY2(columns == group.columns, qPrintable(wrong));
		}

		/*
		 * And no group is wider than the widget is prepared to clear
		 * stretch for, or switching to a narrower one leaves stretch on
		 * columns nothing occupies.
		 */
		QVERIFY2(group.columns <= BSSH_KEYBOARD_MAX_COLUMNS,
		          "a group wider than BSSH_KEYBOARD_MAX_COLUMNS is drawn clipped");
	}
}

} /* namespace */

/*
 * The full keyboard answers about ITSELF, which for an hour it did not.
 *
 * bssh_soft_keyboard's catalogue lookups took a style that defaulted to
 * COMPACT, so with the full board on screen group_id() named a compact
 * group and set_group() clamped indices against the compact list's three
 * when the full one has two. set_group(2) was therefore accepted, rebuild()
 * found it past the end, and the keyboard came up EMPTY -- which is why the
 * button count below is the assertion that matters and the ids are the
 * cheaper half.
 *
 * The defaults are gone, so this cannot recur without a compile error. The
 * test stays because the next person to add a style will not know that.
 */
/*
 * The full keyboard gives way on a narrow screen, and comes back.
 *
 * Asked of the rule AND of the window, because they fail differently: the
 * rule can be right while nothing calls it -- which is this tree's most
 * repeated defect -- and the window can call it on the setting only, which
 * would never fire on a folding phone, where the width changes and no
 * setting does.
 */
/*
 * Shared registry: leave it as it was found (sec 8.4), and by a DESTRUCTOR
 * rather than by a line at the end.
 *
 * A QVERIFY2 that fails returns from the test function, so a cleanup
 * written at the bottom is exactly the cleanup that does not run on the
 * day it is needed -- the session leaks and the next test to count the
 * registry fails too. Measured: the sabotage run for this test reported
 * two failures for one cause, and the second was a middle-click test
 * counting a window this one left behind.
 */

/*
 * One layout, two geometries: the slot split (sec 6.2).
 *
 * Geometry and legends used to be declared in one call --
 * full_literal_run("qwertyuiop") said both "ten keys here" and "these
 * characters" -- so the compact keyboard and the full one carried two
 * copies of the alphabet with nothing holding them together. A letter
 * corrected on one would have stayed wrong on the other, and no test
 * could have seen it, because each was right about itself.
 *
 * Now both name XKB slots and the characters come from one table. This
 * asserts the property that makes that worth having: the same slot sends
 * the same thing on both keyboards, at every level.
 */
void layout_test::a_soft_keyboard_letter_chord_is_kept_by_the_app() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);
	bssh_soft_keyboard keyboard(&router);
	keyboard.set_style(bssh_soft_keyboard_catalog::STYLE_FULL);

	/*
	 * The 'c' key, found by its label before any Shift relabels it. Its
	 * click handler carries the base letter, so pressing it drives
	 * steal_char_chord('c') whatever the cap then reads.
	 */
	QAbstractButton *c_key = nullptr;
	for (QAbstractButton *button : keyboard.findChildren<QAbstractButton *>()) {
		if (button->text() == QStringLiteral("c")) { c_key = button; break; }
	}
	QVERIFY2(c_key, "the full keyboard has no 'c' key to press");

	QSignalSpy fired(&router, &bssh_input_router::shortcut_pressed);

	/* Arm Ctrl+Shift the way the modifier keys do, then press 'c'. */
	router.cycle_modifier(bssh_emulator::MOD_CTRL);
	router.cycle_modifier(bssh_emulator::MOD_SHIFT);
	c_key->click();

	QCOMPARE(fired.count(), 1);
	QCOMPARE(fired.first().first().toInt(), int(bssh_input_router::SHORTCUT_COPY));
	/* Kept by the app, so nothing reached the shell. */
	QCOMPARE(emulator.take_output(), QByteArray());

	/* And a bare 'c' still types: no chord, and it produces the letter. */
	c_key->click();
	QCOMPARE(fired.count(), 1);
	QCOMPARE(emulator.take_output(), QByteArrayLiteral("c"));
}

void layout_test::a_scroll_chord_scrolls_the_focused_terminal() {
	bssh_main_window window;
	window.resize(BSSH_SMALL_DESKTOP_W, BSSH_SMALL_DESKTOP_H);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);

	bssh_session *session = registry->create(named(QStringLiteral("scrolls")));
	QVERIFY(session && session->input_router() && session->emulator());
	window.adopt_session(session);
	QCoreApplication::processEvents();

	/*
	 * More lines than the screen holds, so the scrollback has somewhere
	 * to scroll TO. Fed straight into the emulator, which is where a
	 * remote's output lands.
	 */
	QByteArray output;
	for (int i = 0; i < 300; ++i) output += QByteArrayLiteral("line\r\n");
	session->emulator()->feed(output);
	QCoreApplication::processEvents();

	bssh_terminal_view *view = window.findChild<bssh_terminal_view *>();
	QVERIFY2(view, "the window built no terminal view for its session");
	QCOMPARE(view->scroll_offset(), 0);

	/*
	 * Exactly the on-screen keyboard's path: arm a sticky Shift, then
	 * press PgUp, which reaches the router as send_key(KEY_PAGE_UP, NONE)
	 * carrying the armed modifier. It must steal the scroll chord and,
	 * through the connection wire_session() makes, move the terminal.
	 */
	bssh_input_router *router = session->input_router();
	router->cycle_modifier(bssh_emulator::MOD_SHIFT);
	router->send_key(bssh_emulator::KEY_PAGE_UP, bssh_emulator::MOD_NONE);
	QCoreApplication::processEvents();

	QVERIFY2(view->scroll_offset() > 0,
	          qPrintable(QStringLiteral("a Shift+PgUp from the session router left the "
	                                     "terminal at offset %1").arg(view->scroll_offset())));
}

void layout_test::the_front_door_overflow_menu_reaches_the_touch_floor() {
	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));

	/*
	 * The floor is applied at menu-construction time from
	 * is_touch_primary(), so the host list is built under each forced
	 * value rather than one list re-read: that is the seam the real
	 * callers cross, and the arm that could silently pass is the one
	 * where a pointer keeps its own metrics.
	 */
	const QString floor = QStringLiteral("min-height: %1px").arg(BSSH_TOUCH_TARGET_DP);

	{
		const forced_touch_t thumb(1);
		bssh_host_list list(&store);
		QMenu *overflow = list.findChild<QMenu *>();
		QVERIFY2(overflow, "the front door must carry an overflow menu");
		QVERIFY2(overflow->styleSheet().contains(floor),
		          qPrintable(QStringLiteral("touch rows must reach the %1dp floor, "
		                                     "stylesheet is: \"%2\"")
		                             .arg(BSSH_TOUCH_TARGET_DP)
		                             .arg(overflow->styleSheet())));
	}

	{
		const forced_touch_t pointer(0);
		bssh_host_list list(&store);
		QMenu *overflow = list.findChild<QMenu *>();
		QVERIFY2(overflow, "the front door must carry an overflow menu");
		QVERIFY2(!overflow->styleSheet().contains(floor),
		          qPrintable(QStringLiteral("a pointer keeps its own metrics, but the "
		                                     "menu carries the touch floor: \"%1\"")
		                             .arg(overflow->styleSheet())));
	}
}

void layout_test::every_config_page_sits_on_the_one_index() {
	const forced_touch_t thumb(1);

	bssh_main_window window;
	window.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	const registry_left_as_found_t tidy(window.registry());

	QStackedWidget *pages = window.findChild<QStackedWidget *>();
	QWidget *index = window.findChild<QWidget *>(QStringLiteral("settings-index"));
	QVERIFY2(pages && index, "the touch window must have a page stack and a config index");

	QListWidget *rows = index->findChild<QListWidget *>();
	QVERIFY2(rows && rows->count() > 0, "the config index must be a list of rows");

	QStringList row_names;
	for (int row = 0; row < rows->count(); ++row)
		row_names.append(rows->item(row)->text());

	/*
	 * HALF ONE: nothing is named in both places.
	 *
	 * Asserted as an intersection rather than by pinning either list,
	 * because the report was about a DUPLICATE road and not about any
	 * particular entry -- so this stays true as either list grows, and
	 * fails the moment a page acquires a second way in.
	 */
	bssh_host_list *hosts = window.findChild<bssh_host_list *>();
	QVERIFY2(hosts, "the touch front door must be a host list");

	QMenu *overflow = hosts->findChild<QMenu *>();
	QVERIFY2(overflow, "the host list must carry an overflow menu");

	QStringList both;
	QAction *into_config = nullptr;
	for (QAction *entry : overflow->actions()) {
		if (row_names.contains(entry->text())) both.append(entry->text());
		if (entry->text() == QStringLiteral("Settings")) into_config = entry;
	}

	QVERIFY2(both.isEmpty(),
	          qPrintable(QStringLiteral("%1 reachable from the overflow menu AND "
	                                     "from the config index: %2")
	                             .arg(both.size()).arg(both.join(QStringLiteral(", ")))));

	/*
	 * And the one road works, which is what makes the rest of this
	 * reachable at all: the rows below have no geometry to click until
	 * the index has been shown.
	 */
	QVERIFY2(into_config, "the overflow menu must offer a way into config");
	into_config->trigger();
	QCoreApplication::processEvents();
	QCOMPARE(pages->currentWidget(), index);

	/*
	 * HALF TWO: each row lands on the page it names.
	 *
	 * Clicked rather than reasoned about, because the index maps rows to
	 * pages by POSITION and the two lists are built by separate loops --
	 * nothing in the code makes them agree, so nothing but this can say
	 * they do.
	 */
	QList<QWidget *> landed;
	for (int row = 0; row < rows->count(); ++row) {
		const QString name = rows->item(row)->text();

		rows->scrollToItem(rows->item(row));
		QCoreApplication::processEvents();
		QTest::mouseClick(rows->viewport(), Qt::LeftButton, Qt::NoModifier,
		                   rows->visualItemRect(rows->item(row)).center());
		QCoreApplication::processEvents();

		QWidget *page = pages->currentWidget();

		QVERIFY2(page && page != index,
		          qPrintable(QStringLiteral("tapping \"%1\" left the index showing")
		                             .arg(name)));

		QString arrived;
		if (qobject_cast<bssh_preset_editor *>(page)) {
			arrived = QStringLiteral("Presets");
		} else if (qobject_cast<bssh_licences_view *>(page)) {
			arrived = QStringLiteral("Licences");
		} else if (bssh_app_settings_view *view =
		                   qobject_cast<bssh_app_settings_view *>(page)) {
			arrived = bssh_app_settings_view::section_name(view->section());
		} else {
			arrived = QStringLiteral("a page of no config kind");
		}

		QVERIFY2(arrived == name,
		          qPrintable(QStringLiteral("row %1 says \"%2\" and opens \"%3\"")
		                             .arg(row).arg(name, arrived)));

		QVERIFY2(!landed.contains(page),
		          qPrintable(QStringLiteral("\"%1\" opens a page another row "
		                                     "already opened").arg(name)));
		landed.append(page);

		/*
		 * The system Back, which is how a person leaves one of these --
		 * and the path sec 8.5 already has a test for, rather than a
		 * second way in for the suite's convenience.
		 */
		QKeyEvent back(QEvent::KeyPress, Qt::Key_Back, Qt::NoModifier);
		QApplication::sendEvent(&window, &back);
		QCoreApplication::processEvents();

		QCOMPARE(pages->currentWidget(), index);
	}

	QCOMPARE(landed.size(), row_names.size());
}

void layout_test::both_keyboards_read_one_layout() {
	bssh_soft_keyboard_catalog &catalog = bssh_soft_keyboard_catalog::instance();

	QHash<int, bssh_key_t> compact;
	for (const bssh_soft_key_group_t &group :
	     catalog.all(bssh_soft_keyboard_catalog::STYLE_COMPACT)) {
		for (const QList<bssh_key_t> &row : group.rows) {
			for (const bssh_key_t &key : row) {
				if (key.kind == bssh_key_t::TEXT && !compact.contains(key.value)) {
					compact.insert(key.value, key);
				}
			}
		}
	}

	/*
	 * The MAIN block only. The numeric keypad is not a layout position:
	 * KP7 is KP7 in every language, and a national layout moves the
	 * letters above it and not the digits on it. Comparing it here would
	 * be asking the wrong population -- and it says so loudly, because
	 * the keypad's 0 and the digit row's 0 are the same character and
	 * only one of them has ) on its top half.
	 */
	int compared = 0;
	for (const bssh_soft_key_group_t &group :
	     catalog.all(bssh_soft_keyboard_catalog::STYLE_FULL)) {
		if (!group.id.contains(QStringLiteral("main"))) continue;

		for (const QList<bssh_key_t> &row : group.rows) {
			for (const bssh_key_t &key : row) {
				if (key.kind != bssh_key_t::TEXT) continue;
				if (!compact.contains(key.value)) continue;

				const bssh_key_t &twin = compact.value(key.value);
				QCOMPARE(twin.shifted_value, key.shifted_value);
				QCOMPARE(twin.altgr_value, key.altgr_value);
				QCOMPARE(twin.shift_altgr_value, key.shift_altgr_value);
				++compared;
			}
		}
	}

	/*
	 * And the comparison was over something. A loop that agrees about
	 * nothing passes exactly as loudly as one that agrees about the
	 * alphabet, which is this suite's own standing lesson.
	 */
	QVERIFY2(compared >= 26, qPrintable(QStringLiteral("compared %1 keys").arg(compared)));
}

/*
 * The ampersand key draws an ampersand (sec 6.2).
 *
 * Qt reads & in a button's text as a mnemonic marker and eats it, so
 * the & on the compact symbol page drew a BLANK cap. It sent the right
 * character, it was the right size, it was in the right cell, and every
 * test about the catalogue passed -- because the catalogue was right.
 * What was wrong was the drawing, and only a picture of the phone
 * showed it.
 *
 * Asserted through the predicate rather than through pixels, because
 * pixels are what the shots are for and a unit test that rasterises a
 * button is a test of Qt. What this pins is the decision: a cap whose
 * label contains an ampersand must not be left to the style.
 */
void layout_test::an_ampersand_cap_is_not_left_to_the_style() {
	QVERIFY(bssh_key_cap::needs_own_paint(QStringLiteral("&"), QString()));
	QVERIFY(bssh_key_cap::needs_own_paint(QStringLiteral("q"),
	                                       QString(QChar(0x00e4))));

	/*
	 * And the ordinary caps are still the style's, which is the half
	 * that keeps a keyboard looking like its own buttons -- and keeps
	 * the eliding the full board's narrow keys rely on.
	 */
	QVERIFY(!bssh_key_cap::needs_own_paint(QStringLiteral("Enter"), QString()));
	QVERIFY(!bssh_key_cap::needs_own_paint(QStringLiteral("g"), QString()));

	/*
	 * And the keyboard really does carry one, or the two lines above are
	 * a rule about nothing.
	 */
	bool found = false;
	for (const bssh_soft_key_group_t &group :
	     bssh_soft_keyboard_catalog::instance().all(
	             bssh_soft_keyboard_catalog::STYLE_COMPACT)) {
		for (const QList<bssh_key_t> &row : group.rows) {
			for (const bssh_key_t &key : row) {
				if (key.label == QStringLiteral("&")) found = true;
			}
		}
	}
	QVERIFY2(found, "no cap on the compact keyboard is labelled &");
}

namespace {

/*
 * Leave the layout as it was found, by a destructor.
 *
 * The layout is process-wide and the catalog is a singleton, so a test
 * that selects one and returns early on a failed QVERIFY would leave
 * every later test reading a Swedish keyboard. Same reason
 * registry_left_as_found_t exists, and the same failure it was written
 * for: a cleanup at the bottom of a function is the cleanup that does
 * not run on the day it is needed.
 */
struct layout_left_as_found_t {
	QString id;

	layout_left_as_found_t() : id(bssh_keyboard_layout()) {}
	~layout_left_as_found_t() { bssh_set_keyboard_layout(id); }
};

/*
 * Every character a style can send, over every modifier combination.
 *
 * Through bssh_keyboard_char rather than by reading the stored levels,
 * which is the difference between what a key SENDS and what it holds: a
 * capital comes from toUpper at send time, so a probe reading levels
 * reports every capital as unreachable. One did, during this work, and
 * the false gap looked exactly like a real one.
 */
QSet<uint> every_character(bssh_soft_keyboard_catalog::style_t style) {
	QSet<uint> sent;
	for (const bssh_soft_key_group_t &group :
	     bssh_soft_keyboard_catalog::instance().all(style)) {
		for (const QList<bssh_key_t> &row : group.rows) {
			for (const bssh_key_t &key : row) {
				if (key.kind != bssh_key_t::TEXT) continue;

				for (int modifiers = 0; modifiers < 4; ++modifiers) {
					const QChar character = bssh_keyboard_char(
					        key, modifiers & 1, modifiers & 2);
					if (!character.isNull() && character.unicode()) {
						sent.insert(character.unicode());
					}
				}
			}
		}
	}
	return sent;
}

/* The character a TEXT key sends, for a key that sends this one. */
QChar slot_character(char32_t wanted) {
	for (const bssh_soft_key_group_t &group :
	     bssh_soft_keyboard_catalog::instance().all(
	             bssh_soft_keyboard_catalog::STYLE_FULL)) {
		for (const QList<bssh_key_t> &row : group.rows) {
			for (const bssh_key_t &key : row) {
				if (key.kind == bssh_key_t::TEXT
				     && char32_t(key.value) == wanted) {
					return QChar(uint(key.value));
				}
			}
		}
	}
	return QChar();
}

void check_every_printable_character(const QString &layout_id);

} /* namespace */

/*
 * Every printable ASCII character is reachable on both keyboards
 * (sec 6.2).
 *
 * The compact one could not type + = [ \\ ] { } -- so no ${VAR}, no
 * [ -f x ], no escaping and no line continuation, on the board a phone
 * gets and in a program that is a shell. Nothing said so: every group
 * was the right shape, every row added up, and every key sent what its
 * cap showed. A keyboard is not a set of correct keys, it is a claim
 * that you can type, and only asking what it CANNOT reach can check it.
 *
 * Capitals are excluded because they are not a level: bssh_shifted_char
 * falls back to toUpper, so a letter key covers its capital without
 * storing one, and a layout in a caseless script gets the right answer
 * by not having one to store.
 *
 * This is also what makes the qwertz sabotage catchable.
 * both_keyboards_read_one_layout compares only characters both boards
 * carry, so a letter going MISSING from one of them is invisible to it
 * -- an absence, not a disagreement. Found by running the sabotage and
 * reading why it was missed, which is the argument for running them.
 */
void layout_test::both_keyboards_reach_every_printable_character() {
	/*
	 * EVERY layout, not just the one that happens to be current. This is
	 * the contract a layout has to satisfy to be in the catalog at all:
	 * a Swedish letter block has no bar on it, because a Swedish board
	 * puts one on the ISO key ANSI does not have, and what covers it is
	 * the symbol page being ours and ASCII. That is a claim about the
	 * whole keyboard, so only asking it of the whole keyboard, under
	 * each layout in turn, can check it.
	 */
	const layout_left_as_found_t restore;

	for (const bssh_keyboard_layout_t &layout : bssh_keyboard_layouts()) {
		QVERIFY2(bssh_set_keyboard_layout(layout.id),
		          qPrintable(QStringLiteral("the catalog lists %1 and will not "
		                                     "select it").arg(layout.id)));
		check_every_printable_character(layout.id);
	}
}

/*
 * The layout setting reaches the keyboard (sec 6.2).
 *
 * The catalog can be right about every layout and the user still have no
 * way to choose one, which is this tree's most repeated defect and the
 * reason this test exists rather than a second catalog test. It goes
 * through the settings PAGE -- the control a thumb touches -- and then
 * asks the window's keyboard what its keys send.
 */
void layout_test::the_layout_setting_reaches_the_keyboard() {
	const layout_left_as_found_t restore_layout;

	bssh_app_settings settings;
	const QString before = settings.keyboard_layout();
	const bssh_app_settings::keyboard_t style_before = settings.keyboard();
	settings.set_keyboard(bssh_app_settings::KEYBOARD_BUILT_IN);
	settings.set_keyboard_layout(QStringLiteral("us-intl"));

	bssh_main_window window;
	window.resize(400, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	const registry_left_as_found_t tidy(window.registry());

	bssh_app_settings_view *page = window.findChild<bssh_app_settings_view *>();
	QVERIFY2(page, "the window has no settings page");
	QComboBox *chooser = page->findChild<QComboBox *>(QStringLiteral("keyboard-layout"));
	QVERIFY2(chooser, "the settings page offers no layout control");

	/*
	 * And it is on the page rather than merely constructed. findChild
	 * finds a widget that was built and never added to a layout, or one
	 * that was explicitly hidden -- both of which are a control the user
	 * cannot reach, and both of which this test passed until a sabotage
	 * replaced the addRow() with a hide().
	 *
	 * isHidden() rather than isVisible(), because the settings page is
	 * one card of a stack and need not be the visible one for the
	 * control to be properly on it.
	 */
	QVERIFY2(!chooser->isHidden(), "the layout control is on the page but hidden");
	QVERIFY2(chooser->parentWidget() != nullptr,
	          "the layout control was never given a parent");

	const int swedish = chooser->findData(QStringLiteral("se"));
	QVERIFY2(swedish >= 0, "the control does not list the Swedish layout");
	chooser->setCurrentIndex(swedish);

	QCOMPARE(bssh_app_settings().keyboard_layout(), QStringLiteral("se"));

	/*
	 * And the keys followed. Asked of the CATALOG the widget draws from,
	 * because that is what the setting is supposed to move; the widget
	 * rebuilding from it is covered by rebuild_keys() having a caller.
	 */
	QVERIFY(bssh_set_keyboard_layout(bssh_app_settings().keyboard_layout()));
	QCOMPARE(slot_character(0x00e5), QChar(0x00e5));

	settings.set_keyboard_layout(before);
	settings.set_keyboard(style_before);
}

/*
 * A small screen loses no LETTER (sec 6.2).
 *
 * Selecting Swedish used to make Swedish worse. The compact board showed
 * ten keys a row, so it stopped at AD10 and AC10 -- and Swedish puts
 * a-ring at AD11 and a-diaeresis at AC11, which fell off the board. Only
 * o-diaeresis survived, because AC10 is on the row. Under US
 * International all three are reachable through AltGr, so **the US
 * layout was a better Swedish keyboard than the Swedish one**, on the
 * device it was asked for.
 *
 * The ASCII contract passed throughout and was right to: those letters
 * are not ASCII, and a keyboard that can type a shell is what it
 * promises. This is the promise it does not make -- that choosing a
 * layout gets you the letters you chose it for.
 *
 * Stated against the FULL keyboard rather than against a list of
 * letters, so it holds for a layout nobody has added yet: the full board
 * carries every slot, so what it can send is what the layout has, and a
 * smaller screen may drop symbols but must not drop letters. Measured,
 * Swedish loses only AltGr curiosities from TLDE, AE12 and BKSL -- a
 * square root, a section sign, an acute, a middle dot, a plus-minus, an
 * ellipsis and a per-mille -- and no shell wants any of them.
 */
void layout_test::a_small_screen_loses_no_letter() {
	const layout_left_as_found_t restore;

	for (const bssh_keyboard_layout_t &layout : bssh_keyboard_layouts()) {
		QVERIFY(bssh_set_keyboard_layout(layout.id));

		const QSet<uint> full =
		        every_character(bssh_soft_keyboard_catalog::STYLE_FULL);
		const QSet<uint> compact =
		        every_character(bssh_soft_keyboard_catalog::STYLE_COMPACT);

		QString lost;
		for (uint code : full) {
			if (!compact.contains(code) && QChar(code).isLetter()) {
				lost.append(QChar(code));
			}
		}
		QVERIFY2(lost.isEmpty(),
		          qPrintable(QStringLiteral("layout %1: the compact keyboard "
		                                     "cannot type %2").arg(layout.id, lost)));

		/*
		 * And it compared something. Two empty sets satisfy the loop
		 * above exactly as loudly as two full ones.
		 */
		QVERIFY2(full.size() > 40,
		          qPrintable(QStringLiteral("layout %1 sends %2 characters")
		                             .arg(layout.id).arg(full.size())));
	}
}

/*
 * The fourth compact page exists only where a letter needs it (sec 6.2).
 *
 * The page carries the slots the other three omit -- the digit row, TLDE,
 * AD12, BKSL, LSGT and AB08-AB10 -- and under most layouts every one of
 * those holds punctuation the ASCII palette already has. Giving those
 * layouts a fourth page would be a tap every user pays and nobody spends,
 * so the page is admitted by a set difference against the pages already
 * built rather than by naming the layouts that get one.
 *
 * Asserted in both directions, because only one of them is the change:
 * `a_small_screen_loses_no_letter` already proves German can type eszett
 * somewhere, and would go on passing if every layout grew a page it did
 * not need. What that test cannot see is the tap, so this one counts the
 * pages.
 */
void layout_test::a_fourth_page_appears_only_where_a_letter_needs_it() {
	const layout_left_as_found_t restore;

	QVERIFY(bssh_set_keyboard_layout(QStringLiteral("us-intl")));
	QCOMPARE(int(bssh_soft_keyboard_catalog::instance()
	                     .all(bssh_soft_keyboard_catalog::STYLE_COMPACT).size()), 3);

	/*
	 * Swedish is the sharper half of the control: it has letters the
	 * ASCII palette does not carry, and they are on AD11 and AC11 where
	 * the letters page already reaches them.
	 */
	QVERIFY(bssh_set_keyboard_layout(QStringLiteral("se")));
	QCOMPARE(int(bssh_soft_keyboard_catalog::instance()
	                     .all(bssh_soft_keyboard_catalog::STYLE_COMPACT).size()), 3);

	QVERIFY(bssh_set_keyboard_layout(QStringLiteral("de")));
	const QList<bssh_soft_key_group_t> german =
	        bssh_soft_keyboard_catalog::instance().all(
	                bssh_soft_keyboard_catalog::STYLE_COMPACT);
	QCOMPARE(int(german.size()), 4);

	/*
	 * And it is the page that carries the letter rather than merely a
	 * fourth page: the button says what it is for, and the keys send it.
	 */
	QCOMPARE(german.last().button_label, QString(QChar(0x00df)));

	bool eszett = false;
	for (const QList<bssh_key_t> &row : german.last().rows) {
		for (const bssh_key_t &key : row) {
			if (key.kind != bssh_key_t::TEXT) continue;
			for (int modifiers = 0; modifiers < 4; ++modifiers) {
				if (bssh_keyboard_char(key, modifiers & 1, modifiers & 2)
				     == QChar(0x00df)) {
					eszett = true;
				}
			}
		}
	}
	QVERIFY2(eszett, "the German fourth page does not carry eszett");

	/*
	 * French, which adds three letters where German adds one -- and
	 * the interesting part is the fourth letter it does NOT add.
	 *
	 * AZERTY keeps a-grave, c-cedilla, e-grave and e-acute on the
	 * digit row, so all four appear on this page. Only three of them
	 * are new: US International puts c-cedilla at AltGr+comma, the
	 * palette rows carry US International's levels by construction so
	 * that they stay ASCII whatever layout is chosen, and the comma
	 * is on the numbers page. C-cedilla is therefore already typeable
	 * in EVERY layout, and a page that offered it again would be
	 * adding nothing.
	 *
	 * Sorted, the three are a-grave, e-grave, e-acute, so the button
	 * takes the first two. This was read off a phone as a suspected
	 * defect before it was understood as the rule working.
	 */
	QVERIFY(bssh_set_keyboard_layout(QStringLiteral("fr")));
	const QList<bssh_soft_key_group_t> french =
	        bssh_soft_keyboard_catalog::instance().all(
	                bssh_soft_keyboard_catalog::STYLE_COMPACT);
	QCOMPARE(int(french.size()), 4);
	QCOMPARE(french.last().button_label,
	          QString(QChar(0x00e0)) + QChar(0x00e8));
}

/*
 * A non-Latin layout opens in Latin and can still reach its own script
 * (sec 6.2).
 *
 * Russian, Greek and Ukrainian put Cyrillic or Greek on the whole letter
 * block, and the ASCII palette carries punctuation only, so choosing one
 * of them cost the alphabet a shell is written in. They were refused on
 * that until there was something to switch with.
 *
 * Both halves are asserted because each alone would pass over a broken
 * toggle: a keyboard stuck in Latin satisfies the ASCII contract and is
 * not a Russian keyboard, and one stuck in Cyrillic reaches the script
 * and is not a terminal.
 */
void layout_test::a_non_latin_layout_opens_in_latin_and_reaches_its_script() {
	const layout_left_as_found_t restore;

	QVERIFY(bssh_set_keyboard_layout(QStringLiteral("ru")));
	QVERIFY2(!bssh_layout_has_latin(), "Russian is being read as a Latin layout");
	QVERIFY2(bssh_latin_mode(), "a non-Latin layout did not open in Latin");

	const QSet<uint> opened =
	        every_character(bssh_soft_keyboard_catalog::STYLE_COMPACT);
	for (char letter = 'a'; letter <= 'z'; ++letter) {
		QVERIFY2(opened.contains(uint(letter)),
		          qPrintable(QStringLiteral("Russian opens without %1")
		                             .arg(QChar(letter))));
	}

	/* And the script it was chosen for is one tap away. */
	bssh_set_latin_mode(false);
	const QSet<uint> script =
	        every_character(bssh_soft_keyboard_catalog::STYLE_COMPACT);
	QVERIFY2(script.contains(0x0439),
	          "Russian cannot type short-i with Latin mode off");
	QVERIFY2(!script.contains(uint('q')),
	          "Latin mode off still draws a Latin letter block");

	bssh_set_latin_mode(true);
	QVERIFY(every_character(bssh_soft_keyboard_catalog::STYLE_COMPACT)
	                .contains(uint('q')));

	/*
	 * A layout change resets it. Carrying the mode across would mean
	 * choosing Greek and getting whichever script the last layout was
	 * left showing.
	 */
	bssh_set_latin_mode(false);
	QVERIFY(bssh_set_keyboard_layout(QStringLiteral("gr")));
	QVERIFY2(bssh_latin_mode(), "the toggle survived a layout change");
}

/*
 * The script toggle is offered only where there is a second script
 * (sec 6.2, sec 11).
 *
 * A Latin layout has nothing to switch to, and a control that does
 * nothing on 27 of 30 layouts is worse than an absent one -- which is
 * absent-not-faked applied to a button rather than to a capability.
 */
void layout_test::the_script_toggle_is_offered_only_where_it_does_something() {
	const layout_left_as_found_t restore;
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	QVERIFY(bssh_set_keyboard_layout(QStringLiteral("se")));
	{
		bssh_soft_keyboard keyboard(&router);
		keyboard.set_style(bssh_soft_keyboard_catalog::STYLE_COMPACT);
		QToolButton *toggle =
		        keyboard.findChild<QToolButton *>(QStringLiteral("keyboard-script"));
		QVERIFY2(toggle, "the toggle was never built");
		QVERIFY2(toggle->isHidden(), "Swedish is offered a script toggle");
	}

	QVERIFY(bssh_set_keyboard_layout(QStringLiteral("ru")));
	{
		bssh_soft_keyboard keyboard(&router);
		keyboard.set_style(bssh_soft_keyboard_catalog::STYLE_COMPACT);
		QToolButton *toggle =
		        keyboard.findChild<QToolButton *>(QStringLiteral("keyboard-script"));
		QVERIFY2(toggle, "the toggle was never built");
		QVERIFY2(!toggle->isHidden(), "Russian is not offered a script toggle");

		/* The label says what pressing it will GIVE you. */
		QCOMPARE(toggle->text(), QStringLiteral("RU"));
		QVERIFY2(!toggle->accessibleName().isEmpty(),
		          "the toggle draws a label and says nothing");

		toggle->click();
		QCOMPARE(toggle->text(), QStringLiteral("abc"));
		QVERIFY2(!bssh_latin_mode(), "pressing the toggle changed nothing");
	}
}

namespace {

/* Windows of ours that are currently alive, for a test that makes one. */
int bssh_main_window_count() {
	int found = 0;
	for (QWidget *widget : QApplication::topLevelWidgets()) {
		if (qobject_cast<bssh_main_window *>(widget)) ++found;
	}
	return found;
}

} /* namespace */

/*
 * "Detach Tab to a Window" produces a window (sec 8.4).
 *
 * Reported as doing nothing. It is guarded -- a window will not empty
 * itself, so the action is disabled while this window shows one session
 * -- and this asks the question that guard makes easy to skip: with
 * enough sessions to be allowed, does it actually tear one out.
 *
 * Driven through the QAction the menu holds rather than by calling
 * tear_out_tab(), because the menu item is what was reported and an
 * action that is disabled, or wired to nothing, is exactly the failure
 * being chased.
 */
/*
 * The LAST tab does not tear out (sec 8.4).
 *
 * A window will not empty itself: tearing out the only session would
 * leave a window with nothing in it and, on desktop, no front door
 * showing either. The menu item is disabled for that case, and
 * the_detach_action_makes_a_window asserts as much -- but the menu is
 * not the only caller, and the guard that matters lives in tear_out()
 * itself, where a drag to the tab bar and any future caller arrive.
 *
 * Asked of tear_out() directly for that reason. Deleting its guard left
 * the suite green: every existing test either drives the disabled menu
 * item, which never reaches the guard, or tears out with two sessions,
 * where the guard does not fire. So the one line stopping a window from
 * emptying itself was covered by nothing.
 *
 * Both halves are asserted, because a refusal that still built a window
 * and returned nullptr would satisfy the first alone.
 */
/*
 * Touch can REORDER panes, which is the one arrangement it could not
 * reach (sec 8.4).
 *
 * Split adds a pane and Show as Tab removes one; neither moves a pane
 * past its neighbour. A grip drawn for touch would have, and it cannot:
 * QDrag hands the gesture to the platform, Android makes the whole
 * window the payload, and this device's window manager claims it as
 * drag-to-split-screen -- the app is halved, an app picker appears, and
 * dropEvent never runs. Measured on a Fold at two drag distances and
 * confirmed by hand, which is why the route is a menu item that nothing
 * can intercept.
 *
 * Asserted on the INTENT rather than on the visible panes: the intent is
 * what the layout rebuilds from and what survives a rotation, so a
 * reorder of the visible subset alone would be undone by the next
 * resize.
 */
/*
 * A tab moves along the BAR, not along the registry (sec 8.3).
 *
 * The drag that does this is desktop-only -- the bar scrolls
 * horizontally on a phone and cannot both scroll and rearrange under one
 * drag -- so touch reaches it through the menu, and both routes go
 * through move_tab().
 *
 * The registry is the wrong list twice over, and the first version of
 * this used it. It holds the sessions shown as PANES, which are not in
 * the bar, so a registry step past one of those moves nothing a user can
 * see: measured on a phone, the item fired, the session went from
 * registry index 0 to 1, and the bar did not move. And the registry is
 * shared, so reordering it rearranges every other window's tabs.
 *
 * The last tab is what separates the two. With three sessions -- one on
 * screen as a pane, two in the bar -- the second tab is LAST in the bar
 * and not last in the registry, so a registry-based Move Right is
 * offered and does nothing, while a bar-based one is correctly refused.
 */
void layout_test::a_tab_can_be_moved_along_the_bar() {
	/*
	 * Tiled, not a column: a column holds every session as a pane and has
	 * no tabs to move (sec 8.7), so tab reordering is a tiled question
	 * now. Forced off-touch keeps the window tiled, and its default
	 * COMPACT width fits one pane so the other two sessions are tabs.
	 */
	const forced_touch_t arrangement(0);

	bssh_main_window window;
	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);

	window.adopt_session(registry->create(named(QStringLiteral("first"))));
	window.adopt_session(registry->create(named(QStringLiteral("second"))));
	window.adopt_session(registry->create(named(QStringLiteral("shown"))));

	QMenu *at_first = window.build_tab_menu(0, &window);
	QVERIFY2(at_first, "the first tab has no menu");
	QAction *first_left = at_first->findChild<QAction *>(QStringLiteral("tab_move_left"));
	QAction *first_right = at_first->findChild<QAction *>(QStringLiteral("tab_move_right"));
	QVERIFY2(first_left && first_right, "the tab menu offers no way to reorder");
	QVERIFY2(!first_left->isEnabled(), "the first tab was offered a move left");
	QVERIFY2(first_right->isEnabled(), "a tab with a tab beside it was refused");
	delete at_first;

	/*
	 * The discriminating half. This session is not last in the registry
	 * -- the on-screen one follows it -- so a registry-based enablement
	 * offers a move that would do nothing.
	 */
	QMenu *at_last = window.build_tab_menu(1, &window);
	QVERIFY2(at_last, "the second tab has no menu, so there is no second tab");
	QVERIFY(at_last->findChild<QAction *>(QStringLiteral("tab_move_left"))->isEnabled());
	QVERIFY2(!at_last->findChild<QAction *>(QStringLiteral("tab_move_right"))->isEnabled(),
	          "the last TAB was offered a move right, so the bar is being read "
	          "as the registry");
	delete at_last;

	/*
	 * And the move itself lands, both ways, without touching the
	 * registry's own order -- which is what keeps another window's bar
	 * out of it.
	 */
	const QList<bssh_session *> registry_before = registry->sessions();

	window.move_tab(0, 1);
	QMenu *moved = window.build_tab_menu(0, &window);
	QVERIFY(moved);
	QVERIFY2(!moved->findChild<QAction *>(QStringLiteral("tab_move_left"))->isEnabled(),
	          "the first tab must still refuse a move left after a swap");
	delete moved;

	QCOMPARE(registry->sessions(), registry_before);

	window.move_tab(1, 0);
	QCOMPARE(registry->sessions(), registry_before);
}

void layout_test::a_pane_can_be_moved_along_the_arrangement() {
	bssh_main_window window;
	window.resize(1400, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);

	bssh_session *first = registry->create(named(QStringLiteral("first")));
	window.adopt_session(first);
	bssh_session *second = registry->create(named(QStringLiteral("second")));
	window.adopt_session(second);
	window.split_beside_current();

	bssh_layout_manager *layout = window.findChild<bssh_layout_manager *>();
	QVERIFY2(layout, "the window has no layout manager");
	QVERIFY2(layout->intent().size() > 1, "nothing split, so this tests nothing");

	const QList<bssh_session *> before = layout->intent();
	bssh_session *last = before.last();

	/* Later, then back, so the move is shown to go both ways. */
	window.move_pane(last, -1);
	QVERIFY2(layout->intent() != before, "moving a pane left changed nothing");
	QCOMPARE(layout->intent().indexOf(last), before.indexOf(last) - 1);

	window.move_pane(last, 1);
	QCOMPARE(layout->intent(), before);

	/*
	 * And it stops at the ends rather than wrapping, because a wrap makes
	 * "left" mean "far right" at the moment somebody is looking at the
	 * leftmost pane.
	 */
	window.move_pane(layout->intent().first(), -1);
	QCOMPARE(layout->intent(), before);
	window.move_pane(layout->intent().last(), 1);
	QCOMPARE(layout->intent(), before);
}

/*
 * And touch still gets NO grip, which is the other half (sec 8.4).
 *
 * It was given one and taken back: the grip is a drag handle, the drag
 * cannot complete on Android, and a control that hands the window to the
 * platform's split-screen is worse than an absent one. sec 11's
 * absent-not-faked rule, applied to a control rather than a capability.
 */
void layout_test::touch_gets_no_pane_grip() {
	const forced_touch_t arrangement(1);

	bssh_main_window window;
	window.resize(1400, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);

	window.adopt_session(registry->create(named(QStringLiteral("first"))));
	window.adopt_session(registry->create(named(QStringLiteral("second"))));
	window.split_beside_current();

	const QList<bssh_terminal_view *> views =
	        window.findChildren<bssh_terminal_view *>();
	QVERIFY2(views.size() > 1, "the window did not split, so this tests nothing");
	for (bssh_terminal_view *view : views) {
		QVERIFY2(!view->grip_visible(),
		          "touch has a grip again, and its drag becomes the platform's");
	}
}

void layout_test::the_last_tab_does_not_tear_out() {
	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);

	bssh_session *only = registry->create(named(QStringLiteral("only")));
	window.adopt_session(only);

	const int before = bssh_main_window_count();
	QCOMPARE(window.tear_out(only), static_cast<bssh_main_window *>(nullptr));
	QCOMPARE(bssh_main_window_count(), before);
}

void layout_test::the_detach_action_makes_a_window() {
	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);

	QAction *detach = nullptr;
	for (QAction *action : window.findChildren<QAction *>()) {
		if (action->text().contains(QStringLiteral("Detach"))) detach = action;
	}
	QVERIFY2(detach, "the View menu has no Detach item");

	/* One session: refused, and the menu says so rather than no-opping. */
	window.adopt_session(registry->create(named(QStringLiteral("only"))));
	QVERIFY2(!detach->isEnabled(),
	          "detaching the only session would leave an empty window, and the "
	          "menu must say so rather than accept the click and do nothing");

	/*
	 * TWO sessions, SPLIT, which is the reported case and the one the
	 * guard does not cover.
	 *
	 * A second session is a tab until somebody splits, and a split moves
	 * it into a pane -- so both sessions are panes and the tab bar is
	 * empty. The action stays enabled, because two sessions are shown;
	 * it reads the current TAB, of which there are none; and it does
	 * nothing, silently, which is exactly what was reported.
	 */
	window.adopt_session(registry->create(named(QStringLiteral("second"))));
	window.split_beside_current();
	QVERIFY2(detach->isEnabled(), "two sessions and detaching is refused");

	QTabBar *bar = window.findChild<QTabBar *>();
	const int tabs = bar && bar->isVisible() ? bar->count() : 0;
	const int windows_before = bssh_main_window_count();
	detach->trigger();
	QVERIFY2(bssh_main_window_count() > windows_before,
	          qPrintable(QStringLiteral("with %1 tab(s) showing, Detach did "
	                                     "nothing").arg(tabs)));

	window.adopt_session(registry->create(named(QStringLiteral("third"))));
	QVERIFY2(detach->isEnabled(), "three sessions and detaching is still refused");

	const int before = bssh_main_window_count();
	detach->trigger();
	QVERIFY2(bssh_main_window_count() > before,
	          qPrintable(QStringLiteral("triggering Detach left %1 window(s)")
	                             .arg(bssh_main_window_count())));

	/*
	 * DELETED rather than closed, and that distinction cost an hour.
	 *
	 * closeEvent() asks before closing a window with a live session in
	 * it, and a modal QDialog::exec() offscreen waits for an answer that
	 * never comes. The test hung there -- in its own cleanup, after the
	 * assertions had passed -- and the hang was read as a fault in the
	 * code under test, because reverting the one line being tested made
	 * it go away. It made it go away by changing which session was torn
	 * out, and so whether the closing window still had one.
	 *
	 * A backtrace settled it in one look: QDialog::exec under
	 * closeEvent, under this loop. Deleting skips the confirmation,
	 * which is what a test wants -- the prompt has its own coverage.
	 *
	 * COLLECTED FIRST, then deleted, which is the second thing this
	 * loop got wrong. topLevelWidgets() is a snapshot of raw pointers
	 * and a window owns top-level children of its own -- menus -- so
	 * deleting one while walking the snapshot leaves entries in it
	 * pointing at freed memory, and the next qobject_cast reads them.
	 * That crashed in QMetaObject::cast, which again looked like a
	 * fault in the code under test and again was not.
	 */
	QList<QPointer<bssh_main_window> > extras;
	for (QWidget *widget : QApplication::topLevelWidgets()) {
		bssh_main_window *other = qobject_cast<bssh_main_window *>(widget);
		if (other && other != &window) extras.append(other);
	}
	for (const QPointer<bssh_main_window> &extra : extras) {
		if (extra) delete extra.data();
	}
}

/*
 * Nothing the brightness slider can be set to is unreadable (sec 9.3).
 *
 * The dim is applied AFTER the contrast floor on purpose -- a slider the
 * floor could veto is a slider that stops working where somebody reached
 * for it -- so the floor cannot protect this and the RANGE has to.
 * Nobody had decided how far down it went: the minimum was 10, which is
 * 1.19:1 white on black against a 3:1 floor, and one of these configs
 * was sitting at exactly that.
 *
 * Asserted against pure black because that is what a dark scheme's
 * background is, and because the dim is never applied to a background --
 * so black is what the text is always dimmed against.
 */
void layout_test::the_brightness_slider_cannot_reach_unreadable() {
	const QRgb black = qRgb(0, 0, 0);
	const QRgb white = qRgb(255, 255, 255);

	for (int percent = BSSH_TEXT_BRIGHTNESS_MIN; percent <= 100; ++percent) {
		const QRgb dimmed = bssh_dim(white, percent);
		const double ratio = bssh_contrast_ratio(dimmed, black);
		QVERIFY2(ratio >= BSSH_CONTRAST_FLOOR,
		          qPrintable(QStringLiteral("%1%% gives %2:1")
		                             .arg(percent).arg(ratio, 0, 'f', 2)));
	}

	/*
	 * And one step below it does NOT clear the floor, or the minimum is
	 * higher than it needs to be and the slider has lost range for
	 * nothing. A bound wants both halves checked.
	 */
	const double below = bssh_contrast_ratio(
	        bssh_dim(white, BSSH_TEXT_BRIGHTNESS_MIN - 5), black);
	QVERIFY2(below < BSSH_CONTRAST_FLOOR,
	          qPrintable(QStringLiteral("the floor could be lower: %1%% gives %2:1")
	                             .arg(BSSH_TEXT_BRIGHTNESS_MIN - 5)
	                             .arg(below, 0, 'f', 2)));

	/* A stored value from before the floor moved comes back clamped. */
	bssh_app_settings settings;
	const int before = settings.text_brightness();
	settings.set_text_brightness(10);
	QCOMPARE(settings.text_brightness(), BSSH_TEXT_BRIGHTNESS_MIN);
	settings.set_text_brightness(before);
}

/*
 * The side-column icons are still NAMED (sec 6.2).
 *
 * Cfg and Hide became pictures, and a button whose label becomes a
 * picture loses the text a screen reader was reading. The word moves to
 * the accessible name rather than being dropped -- the same trap the
 * AltGr hint had, where what is drawn and what is announced are two
 * different things and only one of them is checked by looking.
 *
 * Also asserts the icon is THERE, since an accessible name on a blank
 * button would satisfy the first half while leaving nothing to press.
 */
void layout_test::the_keyboard_side_icons_are_still_named() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	bssh_soft_keyboard keyboard(&router);
	keyboard.set_style(bssh_soft_keyboard_catalog::STYLE_COMPACT);

	const char *names[] = { "keyboard-settings", "keyboard-hide" };
	for (const char *name : names) {
		QToolButton *button =
		        keyboard.findChild<QToolButton *>(QLatin1String(name));
		QVERIFY2(button, name);
		QVERIFY2(!button->accessibleName().isEmpty(),
		          qPrintable(QStringLiteral("%1 draws a picture and says nothing")
		                             .arg(QLatin1String(name))));
		QVERIFY2(!button->icon().isNull(),
		          qPrintable(QStringLiteral("%1 has no icon").arg(QLatin1String(name))));
		QVERIFY2(button->text().isEmpty(),
		          qPrintable(QStringLiteral("%1 still carries a label beside its icon")
		                             .arg(QLatin1String(name))));

		/*
		 * And it has INK in it. A QIcon built from a fully transparent
		 * pixmap is not null, so the check above passes over a button
		 * that draws nothing -- which is a live risk for a filled path
		 * with an odd-even hole, where getting the fill rule backwards
		 * paints either everything or nothing.
		 */
		const QImage drawn = button->icon().pixmap(64, 64).toImage()
		                             .convertToFormat(QImage::Format_ARGB32);
		QVERIFY2(!drawn.isNull(), name);

		int painted = 0;
		for (int y = 0; y < drawn.height(); ++y) {
			for (int x = 0; x < drawn.width(); ++x) {
				if (qAlpha(drawn.pixel(x, y)) > 0) ++painted;
			}
		}
		const int area = drawn.width() * drawn.height();
		QVERIFY2(painted > area / 50,
		          qPrintable(QStringLiteral("%1 draws almost nothing: %2 of %3 px")
		                             .arg(QLatin1String(name)).arg(painted).arg(area)));
		QVERIFY2(painted < area * 9 / 10,
		          qPrintable(QStringLiteral("%1 is a solid block: %2 of %3 px")
		                             .arg(QLatin1String(name)).arg(painted).arg(area)));
	}

	/*
	 * The cog has a HOLE, which is what separates it from a blob at the
	 * size this is drawn. Asserted only for the settings icon: the hide
	 * chevron passes through its own centre.
	 */
	QToolButton *cog =
	        keyboard.findChild<QToolButton *>(QStringLiteral("keyboard-settings"));
	QVERIFY(cog);
	const QImage face = cog->icon().pixmap(64, 64).toImage()
	                            .convertToFormat(QImage::Format_ARGB32);
	QCOMPARE(qAlpha(face.pixel(face.width() / 2, face.height() / 2)), 0);
}

/*
 * Pressing the grip and moving asks for the drag (sec 8.4).
 *
 * This is as far as a test can drive the gesture: the next thing that
 * happens is QDrag::exec(), a nested loop waiting on a real pointer,
 * which nothing headless can answer. So the assertion is on the
 * REQUEST -- the view deciding the press was a drag rather than a
 * selection -- which is the part the handle actually added.
 *
 * Driven against a bare view rather than through a window, deliberately.
 * Through a window the signal reaches start_session_drag() and the test
 * would hang in exec(), which is the same trap that made an earlier
 * cleanup look like a fault in the code under test.
 *
 * And it asserts the press is NOT a drag until the pointer moves, since
 * a press that rearranged the window on its own is what the Alt gesture
 * was careful to avoid and the handle must not reintroduce.
 */
void layout_test::the_grip_asks_for_a_drag_once_the_pointer_moves() {
	bssh_terminal_view view;
	view.set_pane_handle(true);
	view.resize(600, 400);
	view.show();
	QVERIFY(QTest::qWaitForWindowExposed(&view));

	const QRect grip = view.pane_grip_rect();
	QVERIFY2(!grip.isNull(), "a 600x400 pane should have room for a handle");

	QSignalSpy asked(&view, &bssh_terminal_view::pane_drag_requested);

	QTest::mousePress(&view, Qt::LeftButton, Qt::NoModifier, grip.center());
	QCOMPARE(asked.count(), 0);

	/* Short of the threshold: still not a drag. */
	QTest::mouseMove(&view, grip.center() + QPoint(2, 0));
	QCOMPARE(asked.count(), 0);

	QTest::mouseMove(&view, grip.center()
	                         + QPoint(QApplication::startDragDistance() * 3, 40));
	QCOMPARE(asked.count(), 1);

	QTest::mouseRelease(&view, Qt::LeftButton, Qt::NoModifier,
	                     grip.center() + QPoint(60, 40));
}

/*
 * A window that gives up its last session closes (sec 8.4).
 *
 * Found by driving the real gesture with a real pointer: tearing a pane
 * out and dragging it back left the torn-out window on screen, empty,
 * titled "BeerSSH" with "no session" in its status line.
 *
 * The rule is not new -- tear_out() already refuses to empty a window,
 * because the result is useless -- and a drop into ANOTHER window
 * empties one just as thoroughly with nothing enforcing it there.
 *
 * The last window is exempt: closing it would quit the program on a
 * gesture that was about rearranging it.
 */
void layout_test::a_window_that_loses_its_last_session_closes() {
	bssh_main_window keeper;
	keeper.resize(900, 600);
	keeper.show();
	QVERIFY(QTest::qWaitForWindowExposed(&keeper));

	bssh_session_registry *registry = keeper.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);

	keeper.adopt_session(registry->create(named(QStringLiteral("stays"))));

	QPointer<bssh_main_window> giver = new bssh_main_window;
	giver->setAttribute(Qt::WA_DeleteOnClose);
	giver->resize(900, 600);
	giver->show();
	QVERIFY(QTest::qWaitForWindowExposed(giver));

	bssh_session *moving = registry->create(named(QStringLiteral("moves")));
	giver->adopt_session(moving);
	QVERIFY2(giver->isVisible(), "the second window did not come up");

	/* The keeper takes it, which is what a drop onto it does. */
	keeper.adopt_session(moving);

	/*
	 * deleteLater, so the window goes on the next turn of the loop
	 * rather than underneath the take_session() that emptied it.
	 */
	QTest::qWait(50);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

	QVERIFY2(giver.isNull() || !giver->isVisible(),
	          "the window that gave up its last session is still on screen");

	/*
	 * And the keeper is untouched: the exemption is about the LAST
	 * window, so a window that still has something must never take
	 * itself down.
	 */
	QVERIFY2(keeper.isVisible(), "the window that took the session closed itself");
}

/*
 * The whole tab-bar ROW is the drop target, not just the tabs (sec 8.3).
 *
 * A QTabBar is only as wide as its tabs -- with one tab open, about 123
 * pixels of a 1000-pixel window -- and the hit test asked the widget.
 * Measured with a real pointer under Xvfb: a drop at x=60 un-split
 * correctly and the identical drop at x=340, the same row and visually
 * the same strip, did nothing at all.
 *
 * Asserted on the DECISION rather than by dropping, because a
 * synthesised QDropEvent never reaches dropEvent() -- Qt's drop
 * delivery wants a real drag, which a headless test has not got. That
 * was established by instrumenting the handler and watching other
 * tests' drops appear while this one's did not, so the geometry is what
 * a test can hold and the gesture itself is held by the live run.
 */
void layout_test::the_tab_bar_row_takes_a_drop_beyond_the_tabs() {
	bssh_main_window window;
	window.resize(1000, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);

	window.adopt_session(registry->create(named(QStringLiteral("first"))));
	window.adopt_session(registry->create(named(QStringLiteral("second"))));
	QTest::qWait(50);

	QTabBar *bar = window.findChild<QTabBar *>();
	QVERIFY2(bar && bar->isVisible(), "no tab bar to aim at");

	const QPoint on_a_tab = bar->mapTo(&window, QPoint(4, bar->height() / 2));
	const QPoint same_row = bar->mapTo(&window,
	                                    QPoint(bar->width() + 250, bar->height() / 2));
	const QPoint below = bar->mapTo(&window, QPoint(4, bar->height() + 80));

	/*
	 * The gap is the whole point: if the bar spanned the window these
	 * two points would be the same test.
	 */
	QVERIFY2(same_row.x() > bar->width(), "the bar already spans the window");
	QVERIFY2(same_row.x() < window.width(), "aimed off the end of the window");

	QVERIFY2(window.point_is_on_tab_row(on_a_tab), "a tab is not on the tab row");
	QVERIFY2(window.point_is_on_tab_row(same_row),
	          "the row beyond the last tab is not treated as the tab bar");
	QVERIFY2(!window.point_is_on_tab_row(below),
	          "a point below the row counts as the tab bar, so the panes would "
	          "never receive a drop");
}

/*
 * A pane dropped back where it came from does nothing (sec 8.4).
 *
 * Asked for as "the ability to drop it back with no effect", and it was
 * not a no-op before: the drop went through take-and-show, and on a
 * window with no room for another pane that means show_session(), which
 * replaces the intent with the dropped session ALONE. So putting a pane
 * back exactly where it was collapsed the split it belonged to -- a
 * gesture whose undo is destructive, which is worse than one that
 * cannot be undone.
 */
void layout_test::a_pane_dropped_back_changes_nothing() {
	bssh_main_window window;
	window.resize(1000, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);

	bssh_session *first = registry->create(named(QStringLiteral("first")));
	bssh_session *second = registry->create(named(QStringLiteral("second")));
	window.adopt_session(first);
	window.adopt_session(second);
	window.split_beside_current();

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	const QList<bssh_terminal_view *> before = panes->views();
	QVERIFY2(before.size() >= 2, "the split produced fewer than two panes");

	/* Dropped onto the panes it is already one of. */
	window.drop_session(panes->views().size() >= 2 ? second : first, true);

	QCOMPARE(panes->views().size(), before.size());
}

/*
 * The pane's drag handle arms the drag without a modifier (sec 8.4).
 *
 * Alt and a drag has always worked and nobody discovers it, which is why
 * the handle exists. Pressing where it is drawn must do what Alt did:
 * arm, and wait for the pointer to move, so that a press on its own is
 * not a gesture.
 */
void layout_test::the_pane_grip_arms_a_drag() {
	bssh_main_window window;
	window.resize(1000, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);
	window.adopt_session(registry->create(named(QStringLiteral("only"))));

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	QVERIFY2(!panes->views().isEmpty(), "no pane was built");

	bssh_terminal_view *view = panes->views().first();

	/*
	 * After the splitter has given it a width. Asked too early the pane
	 * is 0 wide, which is not a fact about the handle.
	 */
	QTest::qWait(50);
	QVERIFY2(view->width() > 100, "the pane was never laid out");

	/*
	 * A pane, so it has a handle -- and the handle is inside the view,
	 * which is the half a rect can get wrong.
	 */
	const QRect grip = view->pane_grip_rect();
	QVERIFY2(view->rect().contains(grip),
	          qPrintable(QStringLiteral("grip %1,%2 %3x%4 is outside a %5x%6 view")
	                             .arg(grip.x()).arg(grip.y())
	                             .arg(grip.width()).arg(grip.height())
	                             .arg(view->width()).arg(view->height())));
	QVERIFY2(grip.width() >= 14, "the grip is too small to hit");
}

/*
 * A layout that is not in the catalog changes nothing.
 *
 * A settings file written by a newer version, or edited by hand, must
 * not quietly produce a keyboard whose letters are somewhere else. The
 * refusal is reported rather than swallowed, so the window knows not to
 * rebuild.
 */
void layout_test::an_unknown_layout_is_refused_and_changes_nothing() {
	const layout_left_as_found_t restore;

	QVERIFY(bssh_set_keyboard_layout(QStringLiteral("us-intl")));
	QVERIFY(!bssh_set_keyboard_layout(QStringLiteral("klingon")));
	QCOMPARE(bssh_keyboard_layout(), QStringLiteral("us-intl"));

	/* And the keyboard it left behind is still the one that was asked for. */
	QCOMPARE(slot_character(U'q'), QChar(U'q'));
}

/*
 * Selecting a layout changes the letters (sec 6.2).
 *
 * The point of the split, asserted at the only place it is visible: the
 * geometry says AD11 and the layout says what AD11 sends, so a Swedish
 * keyboard puts an a-ring where the US one puts a bracket -- with no
 * change to a single span, row or group.
 */
void layout_test::choosing_a_layout_changes_what_the_keys_send() {
	const layout_left_as_found_t restore;

	QVERIFY(bssh_set_keyboard_layout(QStringLiteral("us-intl")));
	QCOMPARE(slot_character(U'['), QChar(U'['));

	QVERIFY(bssh_set_keyboard_layout(QStringLiteral("se")));
	QCOMPARE(slot_character(0x00e5), QChar(0x00e5));   /* a-ring, AD11 */
	QCOMPARE(slot_character(0x00f6), QChar(0x00f6));   /* o-diaeresis, AC10 */
	QCOMPARE(slot_character(0x00e4), QChar(0x00e4));   /* a-diaeresis, AC11 */

	/*
	 * And the geometry did not move. Same groups, same rows, same
	 * columns -- which is the whole claim the slot split makes.
	 */
	const QList<bssh_soft_key_group_t> &swedish =
	        bssh_soft_keyboard_catalog::instance().all(
	                bssh_soft_keyboard_catalog::STYLE_FULL);
	QVERIFY(bssh_set_keyboard_layout(QStringLiteral("us-intl")));
	const QList<bssh_soft_key_group_t> &american =
	        bssh_soft_keyboard_catalog::instance().all(
	                bssh_soft_keyboard_catalog::STYLE_FULL);
	QCOMPARE(swedish.size(), american.size());
}

namespace {

void check_every_printable_character(const QString &layout_id) {
	for (bssh_soft_keyboard_catalog::style_t style :
	     { bssh_soft_keyboard_catalog::STYLE_COMPACT,
	       bssh_soft_keyboard_catalog::STYLE_FULL }) {
		QSet<uint> reachable;
		for (const bssh_soft_key_group_t &group :
		     bssh_soft_keyboard_catalog::instance().all(style)) {
			for (const QList<bssh_key_t> &row : group.rows) {
				for (const bssh_key_t &key : row) {
					if (key.kind != bssh_key_t::TEXT) continue;

					const int levels[4] = { key.value, key.shifted_value,
					                         key.altgr_value, key.shift_altgr_value };
					for (int level : levels) {
						if (level > 0) reachable.insert(uint(level));
					}
				}
			}
		}

		QString missing;
		for (uint ch = '!'; ch <= '~'; ++ch) {
			if (ch >= 'A' && ch <= 'Z') continue;
			if (!reachable.contains(ch)) missing.append(QChar(ch));
		}

		QVERIFY2(missing.isEmpty(),
		          qPrintable(QStringLiteral("layout %1, style %2 cannot type: %3")
		                             .arg(layout_id).arg(int(style)).arg(missing)));
	}
}

} /* namespace */

/*
 * A keyboard has a key for every level it advertises (sec 6.2).
 *
 * The compact board did not. Every letter on it carries a third legend
 * and draws it in the corner of the cap, and it had Shift, Ctrl and Alt
 * and no AltGr -- so on a phone, which is the device that gets this
 * keyboard and the device the accented characters were asked for, not
 * one of the 78 hints could be reached.
 *
 * Every existing test passed. The catalogue reaches the characters, the
 * router sends them, the cap draws them, the two boards agree about
 * them. Each was true and none of them asked whether the board the user
 * is looking at has the key that gets there -- which is this tree's most
 * repeated defect, a correct function with nothing wired to it, arriving
 * one level up: a correct KEYBOARD with no way in.
 *
 * Written as a relationship rather than as "the compact board has an
 * AltGr key", so it holds for a style added later and for a layout that
 * puts its third level somewhere else.
 */
void layout_test::a_keyboard_can_reach_every_level_it_shows() {
	for (bssh_soft_keyboard_catalog::style_t style :
	     { bssh_soft_keyboard_catalog::STYLE_COMPACT,
	       bssh_soft_keyboard_catalog::STYLE_FULL }) {
		const QList<bssh_soft_key_group_t> &groups =
		        bssh_soft_keyboard_catalog::instance().all(style);

		quint8 offered = 0;
		quint8 advertised = 0;
		for (const bssh_soft_key_group_t &group : groups) {
			for (const QList<bssh_key_t> &row : group.rows) {
				for (const bssh_key_t &key : row) {
					if (key.kind == bssh_key_t::MODIFIER) offered |= key.modifier;
					if (key.kind != bssh_key_t::TEXT) continue;

					if (key.shifted_value != 0 || QChar::isLetter(uint(key.value))) {
						advertised |= quint8(bssh_emulator::MOD_SHIFT);
					}
					if (key.altgr_value != 0 || key.shift_altgr_value != 0) {
						advertised |= bssh_input_router::MOD_ALTGR;
					}
				}
			}
		}

		QVERIFY2(advertised != 0, "no group offered a second level to check");
		QVERIFY2((advertised & ~offered) == 0,
		          qPrintable(QStringLiteral("style %1 shows levels %2 and offers %3")
		                             .arg(int(style)).arg(advertised).arg(offered)));
	}
}

/*
 * A keypad key sends its digit and nothing else (sec 6.2).
 *
 * It did not. Every key on the numeric keypad was built with the helper
 * that consults the layout, and the layout is keyed by character -- so
 * the keypad inherited the DIGIT ROW's legends: Shift on its 4 sent $,
 * AltGr on its 5 sent a euro sign, Shift on its . sent >. A real keypad
 * with NumLock on sends its digit, and that is all.
 *
 * The double-width 0 was the one key in the group that behaved, purely
 * because its width made somebody reach for a different helper. So the
 * group disagreed with itself and the correct member was the accident.
 */
void layout_test::a_keypad_key_sends_only_its_digit() {
	int checked = 0;

	for (const bssh_soft_key_group_t &group :
	     bssh_soft_keyboard_catalog::instance().all(
	             bssh_soft_keyboard_catalog::STYLE_FULL)) {
		if (!group.id.contains(QStringLiteral("keypad"))) continue;

		for (const QList<bssh_key_t> &row : group.rows) {
			for (const bssh_key_t &key : row) {
				if (key.kind != bssh_key_t::TEXT) continue;

				const QString where = QStringLiteral("keypad %1").arg(key.label);
				QVERIFY2(key.shifted_value == 0, qPrintable(where));
				QVERIFY2(key.altgr_value == 0, qPrintable(where));
				QVERIFY2(key.shift_altgr_value == 0, qPrintable(where));
				++checked;
			}
		}
	}

	/*
	 * And it looked at the keypad rather than at an empty list -- the
	 * assertion above is satisfied by a group with no keys in it.
	 */
	QVERIFY2(checked >= 15, qPrintable(QStringLiteral("checked %1 keys").arg(checked)));
}

/*
 * Every slot the geometry names is one the layout has.
 *
 * slot_key() cannot fail loudly at build time -- it is handed a string --
 * so a mistyped position produces a key that sends nothing and wears the
 * id it could not find. That is deliberate and it needs a reader: this
 * is it. A keyboard drawn with a hole in it is exactly the fault a test
 * over a green catalogue would otherwise miss.
 */
void layout_test::every_slot_the_geometry_names_exists() {
	bssh_soft_keyboard_catalog &catalog = bssh_soft_keyboard_catalog::instance();

	for (bssh_soft_keyboard_catalog::style_t style :
	     { bssh_soft_keyboard_catalog::STYLE_COMPACT,
	       bssh_soft_keyboard_catalog::STYLE_FULL }) {
		for (const bssh_soft_key_group_t &group : catalog.all(style)) {
			for (const QList<bssh_key_t> &row : group.rows) {
				for (const bssh_key_t &key : row) {
					if (key.kind != bssh_key_t::TEXT) continue;
					QVERIFY2(key.value != 0,
					          qPrintable(QStringLiteral("%1 has a key sending nothing, labelled %2")
					                             .arg(group.id, key.label)));
				}
			}
		}
	}
}

/*
 * AltGr reaches US International's third level, and the Swedish three in
 * particular.
 *
 * The holder's phone types Svenska and neither built-in keyboard could
 * produce a single accented character -- 53 characters, all ASCII. That
 * is the same family as the Shift defect below: a keyboard that cannot
 * reach what its user types.
 *
 * The table is generated from the system's own xkb us(intl) rather than
 * recalled, so what this test guards is that it is WIRED -- that the
 * levels reach the key, and that asking for level 3 gets level 3.
 */
void layout_test::altgr_reaches_the_us_international_level() {
	QSet<char32_t> plain_only;
	QSet<char32_t> with_altgr;
	QSet<char32_t> with_shift_altgr;

	for (bssh_soft_keyboard_catalog::style_t style :
	     { bssh_soft_keyboard_catalog::STYLE_COMPACT,
	       bssh_soft_keyboard_catalog::STYLE_FULL }) {
		for (const bssh_soft_key_group_t &group :
		     bssh_soft_keyboard_catalog::instance().all(style)) {
			for (const QList<bssh_key_t> &row : group.rows) {
				for (const bssh_key_t &key : row) {
					if (key.kind != bssh_key_t::TEXT) continue;
					plain_only.insert(bssh_keyboard_char(key, false, false).unicode());
					with_altgr.insert(bssh_keyboard_char(key, false, true).unicode());
					with_shift_altgr.insert(bssh_keyboard_char(key, true, true).unicode());
				}
			}
		}
	}

	/* The three this was asked for, and their capitals. */
	const QList<QPair<QChar, QChar> > nordic = {
		{ QChar(0x00e5), QChar(0x00c5) },   /* a-ring   */
		{ QChar(0x00e4), QChar(0x00c4) },   /* a-umlaut */
		{ QChar(0x00f6), QChar(0x00d6) }    /* o-umlaut */
	};
	for (const QPair<QChar, QChar> &pair : nordic) {
		QVERIFY2(with_altgr.contains(pair.first.unicode()),
		          qPrintable(QStringLiteral("AltGr cannot reach %1").arg(pair.first)));
		QVERIFY2(with_shift_altgr.contains(pair.second.unicode()),
		          qPrintable(QStringLiteral("Shift+AltGr cannot reach %1").arg(pair.second)));
	}

	/*
	 * And AltGr is HARMLESS where a key has no third legend, which is
	 * most of them: it must fall through to what the key already sent
	 * rather than swallowing the press. Space is the case that would hurt
	 * most if it did not.
	 */
	QVERIFY2(with_altgr.contains(U' '), "AltGr must leave space alone");
	QVERIFY2(with_altgr.contains(U'/'), "AltGr must leave a key with no third legend alone");

	/*
	 * The plain keyboard is unchanged by any of this -- a regression here
	 * would mean AltGr had leaked into the unmodified level.
	 */
	QVERIFY2(plain_only.contains(U'q') && plain_only.contains(U'1'),
	          "the unmodified level must still be plain US");
	QVERIFY2(!plain_only.contains(0x00e4),
	          "an accented character must not appear without AltGr");
}

/*
 * Shift picks the OTHER LEGEND, not the capital.
 *
 * This is the defect a device found and no test could: Shift was
 * QChar::toUpper(), which is right for letters and does nothing at all to
 * a digit or a bracket -- so on the full-size board's real number row,
 * 21 characters were unreachable. In a terminal that is the pipe, the
 * home directory and most of a shell's punctuation, which is why the
 * report was "the full keyb isn't full".
 *
 * Asserted over the catalogue rather than by clicking, because the claim
 * is about EVERY key on the keyboard and a test that pressed a few would
 * be back to sampling. The list below is US ANSI's, written out rather
 * than computed, since there is no rule to compute -- which is the whole
 * reason toUpper was the wrong model.
 */
void layout_test::shift_reaches_every_character_a_real_keyboard_has() {
	struct pair_t { char plain; char shifted; };
	const pair_t ansi[] = {
		{'`','~'}, {'1','!'}, {'2','@'}, {'3','#'}, {'4','$'}, {'5','%'},
		{'6','^'}, {'7','&'}, {'8','*'}, {'9','('}, {'0',')'}, {'-','_'},
		{'=','+'}, {'[','{'}, {']','}'}, {'\\','|'}, {';',':'}, {'\'','"'},
		{',','<'}, {'.','>'}, {'/','?'}
	};

	/*
	 * What the keyboard can PRODUCE, over every key -- not a map keyed by
	 * the plain character.
	 *
	 * Keyed that way the numpad's 0 overwrote the number row's 0 and the
	 * test reported ")" unreachable, which was the test's error and not
	 * the keyboard's: a real numpad 0 is Insert and correctly has no
	 * second legend. Two distinct keys carry the same plain character and
	 * only one of them is shifted, so the question is "does SOME key
	 * produce this", which is also what a user means by "can I type it".
	 */
	QSet<char32_t> producible;
	const QList<bssh_soft_key_group_t> &full =
	        bssh_soft_keyboard_catalog::instance().all(
	                bssh_soft_keyboard_catalog::STYLE_FULL);
	for (const bssh_soft_key_group_t &group : full) {
		for (const QList<bssh_key_t> &row : group.rows) {
			for (const bssh_key_t &key : row) {
				if (key.kind != bssh_key_t::TEXT) continue;
				producible.insert(bssh_shifted_char(char32_t(key.value),
				                                     char32_t(key.shifted_value),
				                                     true).unicode());
			}
		}
	}

	QStringList unreachable;
	for (const pair_t &p : ansi) {
		if (!producible.contains(char32_t(p.shifted))) {
			unreachable << QStringLiteral("%1 (shift+%2)")
			        .arg(QChar(p.shifted)).arg(QChar(p.plain));
		}
	}
	QVERIFY2(unreachable.isEmpty(),
	          qPrintable(QStringLiteral("unreachable: %1").arg(unreachable.join(QStringLiteral(", ")))));

	/* And a letter still takes its capital, which IS the rule for letters. */
	QCOMPARE(bssh_shifted_char(U'a', 0, true), QChar('A'));
	QCOMPARE(bssh_shifted_char(U'a', 0, false), QChar('a'));

	/*
	 * The keycap and the character come from one function, so a key
	 * cannot show one thing and send another. Asserted by asking the same
	 * question the widget asks for its label.
	 */
	QCOMPARE(bssh_shifted_char(U'1', U'!', true), QChar('!'));
	QCOMPARE(bssh_shifted_char(U'1', U'!', false), QChar('1'));
}

/*
 * The keyboard's own two keys, and the way back (sec 6.2).
 *
 * Driven through the window rather than the widget, because every part of
 * this that can be wrong is a connection: the keyboard does not hide
 * itself, the window remembers that the user asked, and the way back is a
 * gesture in a DIFFERENT widget that has to reach the window through the
 * pane container. A test of bssh_soft_keyboard alone would pass with none
 * of that wired.
 */
void layout_test::the_hide_key_is_remembered_and_a_tap_undoes_it() {
	const forced_touch_t arrangement(1);

	bssh_app_settings settings;
	const bssh_app_settings::keyboard_t before = settings.keyboard();
	settings.set_keyboard(bssh_app_settings::KEYBOARD_BUILT_IN);

	bssh_ssh_session::set_factory([](QObject *parent) -> bssh_ssh_session * {
		bssh_fake_ssh_session *fake = new bssh_fake_ssh_session(parent);
		fake->set_fail_on_connect(QStringLiteral("no network in a test"));
		return fake;
	});

	bssh_main_window window(bssh_profile_t::from_target(QStringLiteral("nowhere.example")));
	window.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	const registry_left_as_found_t tidy(window.registry());

	bssh_soft_keyboard *keys = window.findChild<bssh_soft_keyboard *>();
	QVERIFY2(keys, "a touch window with the built-in keyboard chosen must build one");
	QVERIFY2(keys->isVisible(), "and must show it");

	QAbstractButton *hide = window.findChild<QAbstractButton *>(
	        QStringLiteral("keyboard-hide"));
	QVERIFY2(hide, "the keyboard must carry a hide key");
	hide->click();
	QVERIFY2(!keys->isVisible(), "the hide key must put it away");

	/*
	 * And it must STAY away across a pane rebuild, which is what makes
	 * this a remembered choice rather than a momentary one.
	 * apply_device_settings() recomputes visibility on every rebuild, so
	 * without the flag the keyboard returns a moment after the user asked
	 * for it to go -- a key that visibly does not work.
	 */
	window.resize(BSSH_FULL_KEYBOARD_MIN_DP + 100, BSSH_PHONE_TALL_DP);
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	QVERIFY2(!keys->isVisible(),
	          "a pane rebuild must not overrule the hide key");

	/*
	 * The way back: a tap in the terminal. Driven as a real click on the
	 * view rather than by emitting anything, because the path under test
	 * IS the plumbing -- mouseReleaseEvent to request_input_panel to the
	 * container to the window -- and emitting the signal by hand would
	 * test none of it.
	 */
	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	bssh_terminal_view *view = panes->view_at(0);
	QVERIFY2(view, "there must be a pane to tap");

	QTest::mouseClick(view, Qt::LeftButton, Qt::KeyboardModifiers(),
	                   view->rect().center());
	QVERIFY2(keys->isVisible(),
	          "one tap in the terminal must bring the keyboard back");

	settings.set_keyboard(before);
	bssh_ssh_session::set_factory(nullptr);
}

/*
 * The configuration key reaches the SAME settings page the front door's
 * menu opens, and not a second one. sec 10 records what two front doors to
 * one setting cost when only one of them was kept in step.
 */
void layout_test::the_keyboard_config_key_opens_the_settings_page() {
	const forced_touch_t arrangement(1);

	bssh_app_settings settings;
	const bssh_app_settings::keyboard_t before = settings.keyboard();
	settings.set_keyboard(bssh_app_settings::KEYBOARD_BUILT_IN);

	bssh_ssh_session::set_factory([](QObject *parent) -> bssh_ssh_session * {
		bssh_fake_ssh_session *fake = new bssh_fake_ssh_session(parent);
		fake->set_fail_on_connect(QStringLiteral("no network in a test"));
		return fake;
	});

	bssh_main_window window(bssh_profile_t::from_target(QStringLiteral("nowhere.example")));
	window.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	const registry_left_as_found_t tidy(window.registry());

	QStackedWidget *pages = window.findChild<QStackedWidget *>();
	/*
	 * The INDEX, not a settings view: sec 8.5's settings are a list of
	 * sections now, each its own page, so the key opens the list and
	 * findChild<bssh_app_settings_view *> would return whichever section
	 * happens to be first in the stack.
	 */
	QWidget *device = window.findChild<QWidget *>(QStringLiteral("settings-index"));
	QVERIFY2(pages && device, "the touch window must have a page stack and a settings index");
	QVERIFY2(pages->currentWidget() != device,
	          "the settings page must not be what a session opens on");

	QAbstractButton *config = window.findChild<QAbstractButton *>(
	        QStringLiteral("keyboard-settings"));
	QVERIFY2(config, "the keyboard must carry a configuration key");
	config->click();

	QCOMPARE(pages->currentWidget(), device);

	settings.set_keyboard(before);
	bssh_ssh_session::set_factory(nullptr);
}

/*
 * Android's Back leaves the PAGE, not the app (sec 8.5).
 *
 * Reported from a phone: open the settings from a session's keyboard,
 * press the system Back, and the whole app went to the background with
 * the settings page still showing underneath when it came forward
 * again. Qt delivers the system Back as Qt::Key_Back and a window that
 * does not handle it lets Qt close the window, so the app left while
 * the page it should have left stayed put.
 *
 * Asserted on the EVENT rather than only on the page, because at the
 * bottom of the stack "the page did not change" is true whether Back
 * was handled or ignored, and those are opposite behaviours: ignoring
 * it is what lets Android background the app, and swallowing it would
 * strand a user pressing a key that does nothing.
 */
void layout_test::the_back_key_leaves_the_page_not_the_app() {
	const forced_touch_t arrangement(1);

	bssh_app_settings settings;
	const bssh_app_settings::keyboard_t before = settings.keyboard();
	settings.set_keyboard(bssh_app_settings::KEYBOARD_BUILT_IN);

	bssh_ssh_session::set_factory([](QObject *parent) -> bssh_ssh_session * {
		bssh_fake_ssh_session *fake = new bssh_fake_ssh_session(parent);
		fake->set_fail_on_connect(QStringLiteral("no network in a test"));
		return fake;
	});

	bssh_main_window window(bssh_profile_t::from_target(QStringLiteral("nowhere.example")));
	window.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	const registry_left_as_found_t tidy(window.registry());

	QStackedWidget *pages = window.findChild<QStackedWidget *>();
	/*
	 * The INDEX, not a settings view: sec 8.5's settings are a list of
	 * sections now, each its own page, so the key opens the list and
	 * findChild<bssh_app_settings_view *> would return whichever section
	 * happens to be first in the stack.
	 */
	QWidget *device = window.findChild<QWidget *>(QStringLiteral("settings-index"));
	QVERIFY2(pages && device, "the touch window must have a page stack and a settings index");

	QWidget *came_from = pages->currentWidget();
	QVERIFY2(came_from != device, "the settings page must not be what a session opens on");

	QAbstractButton *config = window.findChild<QAbstractButton *>(
	        QStringLiteral("keyboard-settings"));
	QVERIFY2(config, "the keyboard must carry a configuration key");
	config->click();
	QCOMPARE(pages->currentWidget(), device);

	/*
	 * Back returns to the page it was opened FROM, not to the host
	 * list. Opened from a live session's keyboard, returning to the
	 * host list puts the session out of sight.
	 */
	QKeyEvent leaving(QEvent::KeyPress, Qt::Key_Back, Qt::NoModifier);
	QApplication::sendEvent(&window, &leaving);
	QVERIFY2(leaving.isAccepted(), "Back on the settings page was not handled");
	QCOMPARE(pages->currentWidget(), came_from);

	/* At the bottom of the stack it must be left alone. */
	QKeyEvent at_root(QEvent::KeyPress, Qt::Key_Back, Qt::NoModifier);
	QApplication::sendEvent(&window, &at_root);
	QVERIFY2(!at_root.isAccepted(),
	          "Back was swallowed with no page to leave, so nothing can background the app");
	QCOMPARE(pages->currentWidget(), came_from);

	settings.set_keyboard(before);
	bssh_ssh_session::set_factory(nullptr);
}

void layout_test::a_narrow_screen_falls_back_to_the_compact_keyboard() {
	typedef bssh_soft_keyboard_catalog catalog;

	/* The rule, at the boundary and either side of it. */
	QCOMPARE(bssh_keyboard_style_for(catalog::STYLE_FULL, BSSH_PHONE_DP),
	          catalog::STYLE_COMPACT);
	QCOMPARE(bssh_keyboard_style_for(catalog::STYLE_FULL,
	                                  BSSH_FULL_KEYBOARD_MIN_DP - 1),
	          catalog::STYLE_COMPACT);
	QCOMPARE(bssh_keyboard_style_for(catalog::STYLE_FULL,
	                                  BSSH_FULL_KEYBOARD_MIN_DP),
	          catalog::STYLE_FULL);

	/*
	 * And the compact choice is never widened INTO the full one. The
	 * fallback narrows what was asked for; it does not overrule someone
	 * who asked for the thumb keyboard on a tablet.
	 */
	QCOMPARE(bssh_keyboard_style_for(catalog::STYLE_COMPACT, 2000),
	          catalog::STYLE_COMPACT);

	/* Now the wiring. */
	const forced_touch_t arrangement(1);

	bssh_app_settings settings;
	const bssh_app_settings::keyboard_t before = settings.keyboard();
	settings.set_keyboard(bssh_app_settings::KEYBOARD_BUILT_IN_FULL);

	bssh_main_window window;
	window.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_soft_keyboard *keys = window.findChild<bssh_soft_keyboard *>();
	QVERIFY2(keys, "the window must build the keyboard it was asked for");

	const QString narrow = keys->group_id();
	QVERIFY2(narrow.startsWith(QStringLiteral("full-")) == false,
	          qPrintable(QStringLiteral("a %1dp window drew the full keyboard's "
	                                     "group '%2'").arg(BSSH_PHONE_DP).arg(narrow)));

	/*
	 * Widen past the threshold and the full keyboard returns -- WITHOUT
	 * the setting being touched, which is the half that says the choice
	 * was remembered rather than rewritten.
	 */
	window.resize(BSSH_FULL_KEYBOARD_MIN_DP + 100, BSSH_PHONE_TALL_DP);
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	QVERIFY2(keys->group_id().startsWith(QStringLiteral("full-")),
	          qPrintable(QStringLiteral("a wide window drew '%1'").arg(keys->group_id())));
	QCOMPARE(bssh_app_settings().keyboard(), bssh_app_settings::KEYBOARD_BUILT_IN_FULL);

	settings.set_keyboard(before);
}

void layout_test::the_full_keyboard_answers_about_itself() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	bssh_soft_keyboard keyboard(&router);
	keyboard.set_style(bssh_soft_keyboard_catalog::STYLE_FULL);

	const QList<bssh_soft_key_group_t> &full =
	        bssh_soft_keyboard_catalog::instance().all(
	                bssh_soft_keyboard_catalog::STYLE_FULL);
	QVERIFY2(full.size() >= 2, "the full keyboard needs a second page to walk to");

	QCOMPARE(keyboard.group_id(), full.first().id);

	/*
	 * Every group, and a wrap past the end. The wrap is the case that was
	 * broken: an index the compact list accepts and the full one does not.
	 */
	for (int i = 0; i <= full.size(); ++i) {
		keyboard.set_group(i);

		const bssh_soft_key_group_t &expected = full.at(i % full.size());
		QCOMPARE(keyboard.group_id(), expected.id);

		/*
		 * And it drew something. An out-of-range group leaves rebuild()
		 * returning before it adds a single key, so the widget is a
		 * keyboard with no keys -- present, sized, and useless, which is
		 * exactly what no id comparison can see.
		 */
		const int drawn = keyboard.findChildren<QAbstractButton *>().size();
		QVERIFY2(drawn > 10,
		          qPrintable(QStringLiteral("group %1 drew %2 buttons")
		                             .arg(expected.id).arg(drawn)));
	}
}

/*
 * The third legend is drawn, and it goes away when it has been acted on.
 *
 * The catalogue test above proves AltGr REACHES the accented characters;
 * this proves the user can find out that it does. Those are different
 * claims and the tree has been caught out by the difference before -- a
 * correct function is not a working feature, and a legend nobody can see
 * is exactly the shape of that fault.
 *
 * Asserted through the cap's own hint rather than through its text,
 * because the two now say different things on purpose: the text is what
 * the key sends, which is what a screen reader reads, and the hint is
 * the annotation. A test reading a concatenated label would have passed
 * against the string version this replaced and could not tell the two
 * apart.
 */
void layout_test::the_altgr_legend_is_shown_and_then_stands_aside() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	bssh_soft_keyboard keyboard(&router);
	keyboard.set_style(bssh_soft_keyboard_catalog::STYLE_FULL);

	bssh_key_cap *q = nullptr;
	for (bssh_key_cap *cap : keyboard.findChildren<bssh_key_cap *>()) {
		if (cap->text() == QStringLiteral("q")) q = cap;
	}
	QVERIFY2(q, "the full keyboard drew no q");

	/* Idle: the cap sends q and advertises what AltGr would give. */
	QCOMPARE(q->text(), QStringLiteral("q"));
	QCOMPARE(q->hint(), QString(QChar(0x00e4)));
	QVERIFY2(!q->accessibleDescription().isEmpty(),
	          "the hint is drawn but not said, so a screen reader cannot find it");

	/*
	 * Armed: the cap sends a and stops advertising, because the user has
	 * already done the thing the advertisement was for.
	 */
	router.cycle_modifier(bssh_input_router::MOD_ALTGR);
	QCOMPARE(q->text(), QString(QChar(0x00e4)));
	QCOMPARE(q->hint(), QString());

	/* And released, it goes back to saying what it says at rest. */
	router.cycle_modifier(bssh_input_router::MOD_ALTGR);
	router.cycle_modifier(bssh_input_router::MOD_ALTGR);
	QCOMPARE(q->text(), QStringLiteral("q"));
	QCOMPARE(q->hint(), QString(QChar(0x00e4)));
}

void layout_test::every_keyboard_group_is_the_same_shape() {
	/*
	 * BOTH keyboards, because there are two now (sec 6.2) and the rule is
	 * about neither one in particular. The full-size board is where this
	 * earns its keep: its rows are built from real key widths in quarter
	 * units, so one wrong span puts every key after it in the wrong
	 * place, and the arithmetic is nobody's idea of readable.
	 */
	/*
	 * EVERY layout, because a row's width can depend on one. An ISO
	 * layout carries LSGT and the full board spends a column of the left
	 * Shift on it; get that arithmetic wrong and the row is 56 or 64
	 * rather than 60.
	 *
	 * It checked only the current layout until a sabotage showed why
	 * that is not enough: dropping the ISO key left the Swedish shift
	 * row four columns short and this test saw nothing, because under
	 * US the row it was looking at had not changed.
	 */
	const layout_left_as_found_t restore;

	for (const bssh_keyboard_layout_t &layout : bssh_keyboard_layouts()) {
		QVERIFY(bssh_set_keyboard_layout(layout.id));
		check_keyboard_shape(bssh_soft_keyboard_catalog::STYLE_COMPACT);
		check_keyboard_shape(bssh_soft_keyboard_catalog::STYLE_FULL);
	}
}

void layout_test::the_built_in_keyboard_fits_a_phone() {
	const forced_touch_t arrangement(1);

	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	bssh_soft_keyboard keyboard(&router);
	check_fits(QStringLiteral("built-in keyboard"), &keyboard);

	/*
	 * And it fits in EVERY group, not just the one it opens on. The
	 * terminal group carries the longest labels -- F11, PgU, Bksp -- so a
	 * keyboard checked only on its letters is a keyboard checked where it
	 * is least likely to fail.
	 */
	/*
	 * The COMPACT catalogue by name: this test is the_built_in_keyboard
	 * _fits_a_phone, and the full-size one deliberately does not (sec 6.2
	 * -- it wants about 770dp). Naming the style is what keeps this test
	 * about the keyboard it is about.
	 */
	const int groups = bssh_soft_keyboard_catalog::instance()
	        .all(bssh_soft_keyboard_catalog::STYLE_COMPACT).size();
	for (int i = 1; i < groups; ++i) {
		keyboard.set_group(i);
		check_fits(QStringLiteral("built-in keyboard (%1)").arg(keyboard.group_id()),
		            &keyboard);
	}
}

void layout_test::only_one_on_screen_keyboard_is_ever_shown() {
	/*
	 * The key row and the built-in keyboard carry the same modifiers over
	 * the same router (sec 6.2). Two Ctrl buttons is two answers to what
	 * is armed, and on a phone it is also two keyboards stacked where
	 * there was barely room for one.
	 *
	 * Asserted with the row's own setting on ALWAYS, which is the state
	 * where the two rules actually disagree. On AUTO with touch off the
	 * row is hidden anyway, and the check would pass without the code
	 * doing anything -- the shape this project keeps recording, where a
	 * fixture that cannot reach the distinguishing case reports agreement
	 * whatever the code says.
	 *
	 * A window per setting, because the window reads app.ini when it is
	 * built and the function that re-reads it is private. Two windows is
	 * also the honest test: this is a device-level choice, and what has
	 * to be right is what a user sees after making it.
	 */
	const forced_touch_t arrangement(1);

	{
		bssh_app_settings settings;
		settings.set_key_row_visibility(bssh_app_settings::KEY_ROW_ALWAYS);
		settings.set_keyboard(bssh_app_settings::KEYBOARD_BUILT_IN);
	}
	{
		bssh_main_window window;
		window.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));

		bssh_key_row *row = window.findChild<bssh_key_row *>();
		bssh_soft_keyboard *keyboard = window.findChild<bssh_soft_keyboard *>();
		QVERIFY2(row && keyboard, "the window builds both and shows one");

		QVERIFY2(!keyboard->isHidden(),
		          "the built-in keyboard was chosen and must be the one on screen");
		QVERIFY2(row->isHidden(),
		          "the key row is set to ALWAYS and the built-in keyboard must "
		          "still replace it -- two Ctrl buttons over one router is two "
		          "answers to what is armed");
	}

	/* And the other way round, which is what every existing install has. */
	{
		bssh_app_settings settings;
		settings.set_keyboard(bssh_app_settings::KEYBOARD_SYSTEM);
	}
	{
		bssh_main_window window;
		window.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));

		bssh_key_row *row = window.findChild<bssh_key_row *>();
		bssh_soft_keyboard *keyboard = window.findChild<bssh_soft_keyboard *>();
		QVERIFY(row && keyboard);

		QVERIFY2(keyboard->isHidden(),
		          "choosing this device's keyboard must put ours away");
		QVERIFY2(!row->isHidden(),
		          "and bring the key row back, which is what ALWAYS asked for");
	}

	/* Left as found, so the next test does not inherit this one's choice. */
	{
		bssh_app_settings settings;
		settings.set_key_row_visibility(bssh_app_settings::KEY_ROW_AUTO);
	}
}

namespace {

/*
 * The small desktop window sec 8.2 already contemplates: it names tiling
 * a window into a half-screen column, which on a 1366px laptop is about
 * 680 and lands in the MEDIUM size class. 800x600 is the round number
 * safely inside that, and the classic floor besides.
 */

bool has_button(const QWidget *root, const QString &text)
{
	for (const QAbstractButton *button : root->findChildren<QAbstractButton *>()) {
		if (button->text() == text) return true;
	}
	return false;
}

QWidget *by_accessible_name(const QWidget *root, const QString &name)
{
	for (QWidget *child : root->findChildren<QWidget *>()) {
		if (child->accessibleName() == name) return child;
	}
	return nullptr;
}

} /* namespace */

void layout_test::every_desktop_surface_fits_a_small_window()
{
	/* The desktop arrangement, said rather than assumed. */
	const forced_touch_t arrangement(0);

	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));

	bssh_profile_t profile;
	profile.name = QStringLiteral("build server (europe)");
	profile.host = QStringLiteral("build.internal.example.com");
	profile.user = QStringLiteral("nabbe.testing");
	profile.port = 2299;
	QVERIFY(store.save(profile) != 0);

	struct surface_t {
		QString name;
		QWidget *widget;
	};

	bssh_session_dialog front_door(&store);
	bssh_preset_editor presets(&store, bssh_preset_editor::DESKTOP);
	bssh_ssh_config_import import(&store);

	const surface_t surfaces[] = {
		{QStringLiteral("session dialog"), &front_door},
		{QStringLiteral("preset editor (desktop)"), &presets},
		{QStringLiteral("SSH config import"), &import},
	};

	for (const surface_t &surface : surfaces) {
		surface.widget->resize(BSSH_SMALL_DESKTOP_W, BSSH_SMALL_DESKTOP_H);
		surface.widget->show();
		QVERIFY2(QTest::qWaitForWindowExposed(surface.widget), qPrintable(surface.name));

		const QSize needed = surface.widget->minimumSizeHint();
		QVERIFY2(needed.width() <= BSSH_SMALL_DESKTOP_W && needed.height() <= BSSH_SMALL_DESKTOP_H,
		          qPrintable(QStringLiteral("%1 needs %2x%3 in a %4x%5 window")
		                             .arg(surface.name).arg(needed.width()).arg(needed.height())
		                             .arg(BSSH_SMALL_DESKTOP_W).arg(BSSH_SMALL_DESKTOP_H)));
	}

}

void layout_test::the_desktop_front_door_lines_its_fields_up()
{
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));

	bssh_session_dialog dialog(&store);
	dialog.resize(BSSH_SMALL_DESKTOP_W, BSSH_SMALL_DESKTOP_H);
	dialog.show();
	QVERIFY(QTest::qWaitForWindowExposed(&dialog));

	/*
	 * The four destination fields are one column to the eye, and they
	 * were four: each row was its own box layout, so every field began
	 * wherever its own label happened to end. "Host Name (or IP address)"
	 * is twice the width of "User name", so the gap was not subtle.
	 *
	 * It matters here more than it would elsewhere. This is the front
	 * door, and sec 8.5's whole argument for it is that it borrows an
	 * interface people already have in their fingers -- PuTTY's, which
	 * aligns them.
	 */
	const QStringList fields = {
		QStringLiteral("Host name or IP address"),
		QStringLiteral("User name"),
		QStringLiteral("Private key file"),
		QStringLiteral("Connect via jump host"),
	};

	int column = -1;
	for (const QString &name : fields) {
		const QWidget *field = by_accessible_name(&dialog, name);
		QVERIFY2(field, qPrintable(QStringLiteral("no field named \"%1\"").arg(name)));

		const int left = field->mapTo(&dialog, QPoint(0, 0)).x();
		if (column < 0) column = left;

		QCOMPARE(left, column);
	}

	QVERIFY2(column > 0, "the fields must be beside their labels, not on top of them");
}

void layout_test::the_desktop_front_door_uses_the_desktop_preset_editor()
{
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));

	/*
	 * The two arrangements, pinned by the one control that only makes
	 * sense in one of them. A touch page has to carry its own way out --
	 * the system back gesture is not wired to that stack -- and a dialog
	 * with a category tree down its left-hand side does not.
	 */
	const QString exit_button = QStringLiteral("Back to hosts");

	bssh_preset_editor touch(&store, bssh_preset_editor::TOUCH);
	bssh_preset_editor desktop(&store, bssh_preset_editor::DESKTOP);

	QVERIFY2(has_button(&touch, exit_button), "the touch page must carry its own way out");
	QVERIFY2(!has_button(&desktop, exit_button),
	          "the desktop page is navigated by the category tree");

	/*
	 * And the front door builds the desktop one. It did not: the call
	 * read
	 *   new bssh_preset_editor(m_store, this)
	 * with the QWidget* landing in what was then a bool `touch`,
	 * converting to true. The desktop dialog got the thumb-sized editor,
	 * stacked in one column, with a "Back to hosts" button inside a
	 * dialog that navigates by a tree. It compiled and it ran; it took
	 * looking at the thing to notice.
	 */
	bssh_session_dialog front_door(&store);
	QVERIFY2(!has_button(&front_door, exit_button),
	          "the front door embedded the TOUCH preset editor");
}

void layout_test::the_tab_menu_is_reachable_by_both_gestures() {
	bssh_main_window window;

	QTabBar *tabs = window.findChild<QTabBar *>();
	QVERIFY2(tabs, "the window must have a tab bar");

	/*
	 * The right-click route: a custom context menu policy plus something
	 * listening.
	 *
	 * The listening half is asserted with disconnect(), which returns
	 * whether there was anything to disconnect. It is a destructive way
	 * to ask a question -- and harmless here, because this window is
	 * local and discarded -- but it is the only PUBLIC way to ask it:
	 * isSignalConnected and receivers() are both protected. The question
	 * is worth asking, since the failure guarded against is a connect
	 * lost while moving the handler out of a lambda, which is exactly
	 * what happened to the link prompt once before.
	 */
	QCOMPARE(tabs->contextMenuPolicy(), Qt::CustomContextMenu);
	QVERIFY2(QObject::disconnect(tabs, &QWidget::customContextMenuRequested, nullptr, nullptr),
	          "nothing listens for a right-click on the tab bar");

	/*
	 * And the press-and-hold route, which Qt does not provide: an event
	 * filter of ours must be watching the bar itself.
	 */
	const QList<bssh_long_press *> holds = window.findChildren<bssh_long_press *>();
	bool watches_tabs = false;
	for (const bssh_long_press *hold : holds) {
		if (hold->parent() == &window) watches_tabs = true;
	}
	QVERIFY2(watches_tabs, "no long-press gesture belongs to the window");
}

void layout_test::backing_out_of_a_session_reaches_the_host_list() {
	/*
	 * The tab bar can only appear if a SECOND session can be opened, and
	 * on the touch front door the only place to open one from is the host
	 * list. Opening a host was a one-way door: Android's back button
	 * offered to quit, and nothing else led back -- so the tab bar, its
	 * context menu and every split were unreachable on the one device
	 * they were designed for.
	 */
	const forced_touch_t arrangement(1);

	/*
	 * The connection is made to FAIL, which is not incidental. With a live
	 * session the close event this test sends reaches the "keep it running
	 * in the background?" prompt -- a modal nobody is there to answer --
	 * so a regression in the rule below would HANG the suite instead of
	 * failing it. Nothing here is about that prompt.
	 */
	bssh_ssh_session::set_factory([](QObject *parent) -> bssh_ssh_session * {
		bssh_fake_ssh_session *fake = new bssh_fake_ssh_session(parent);
		fake->set_fail_on_connect(QStringLiteral("no network in a test"));
		return fake;
	});

	/* A command-line target opens straight into the terminal (sec 8.5). */
	bssh_main_window window(bssh_profile_t::from_target(QStringLiteral("nowhere.example")));
	QVERIFY2(!window.showing_front_door(), "a target must open into the terminal");

	QCloseEvent back;
	QApplication::sendEvent(&window, &back);

	QVERIFY2(window.showing_front_door(),
	          "backing out of a session must go up to the host list");

	/*
	 * And the close is REFUSED, which is the other half: an accepted one
	 * takes the window with it, so going up would be indistinguishable
	 * from quitting for everything downstream of the event.
	 */
	QVERIFY2(!back.isAccepted(), "going up must not also close the window");

	bssh_ssh_session::set_factory(nullptr);
}


/*
 * The Android keyboard defect, held as a desktop test because the property
 * is not Android's: a resize that does not change the size class must not
 * destroy the pane views.
 *
 * resizeEvent called m_panes->rebuild() unconditionally, and rebuild() is
 * qDeleteAll(m_views). On a phone the on-screen keyboard's appearance IS a
 * window resize, delivered repeatedly as it animates -- so every frame of
 * the keyboard opening destroyed the view that had just asked for it, and
 * Qt answers a focus object that no longer accepts input by calling
 * QtInputDelegate.hideSoftwareKeyboard(). The keyboard flashed up and
 * vanished on every tap, which on a phone is the whole of typing.
 *
 * A keyboard changes the HEIGHT, so the first assertion is the one that
 * matters; the width case is here because a desktop window drag was
 * rebuilding every view per pixel by the same route.
 */
void layout_test::a_resize_that_keeps_the_size_class_keeps_the_views() {
	bssh_ssh_session::set_factory([](QObject *parent) -> bssh_ssh_session * {
		bssh_fake_ssh_session *fake = new bssh_fake_ssh_session(parent);
		fake->set_fail_on_connect(QStringLiteral("no network in a test"));
		return fake;
	});

	bssh_main_window window(bssh_profile_t::from_target(QStringLiteral("nowhere.example")));
	window.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);

	/*
	 * Shown, because a hidden widget is not laid out and delivers no
	 * resizeEvent -- so every assertion below would hold for the reason
	 * that nothing happened at all. The control at the end is what caught
	 * that when this test was first written without it.
	 */
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	const registry_left_as_found_t tidy(window.registry());

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY2(panes, "the window must have a pane container");

	/*
	 * Held by QPointer rather than by the raw pointer: rebuild() deletes
	 * the view, and asking a deleted object anything is how a test that
	 * means to catch this crashes instead of failing.
	 */
	QPointer<bssh_terminal_view> view = panes->view_at(0);
	QVERIFY2(!view.isNull(), "there must be a view for the resize to lose");

	/* Exactly what a keyboard opening does: height only. */
	window.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP / 2);
	QVERIFY2(!view.isNull(),
	          "a height-only resize must not destroy the pane views -- this is "
	          "the on-screen keyboard dismissing itself");

	/* And a width change that stays inside the same class. */
	window.resize(BSSH_PHONE_DP - 20, BSSH_PHONE_TALL_DP / 2);
	QVERIFY2(!view.isNull(),
	          "a width change within one size class must not rebuild either");

	/*
	 * The positive control, and without it the three assertions above are
	 * satisfied by a window that never rebuilds at all. Crossing into a
	 * class that fits more panes MUST still rebuild, which is what the
	 * layout_changed signal is for.
	 */
	window.resize(BSSH_SMALL_DESKTOP_W, BSSH_SMALL_DESKTOP_H);
	QVERIFY2(view.isNull(),
	          "crossing the size class must still rebuild, or this test would "
	          "pass against a window that had stopped laying out entirely");

	bssh_ssh_session::set_factory(nullptr);
}

void layout_test::reordering_tabs_reorders_the_sessions_behind_them() {
	/*
	 * sec 8.3's drag-to-reorder, and the invariant it could quietly
	 * break. Tab index and registry index are ONE thing here: every tab
	 * action -- close, the context menu, switching -- turns an index into
	 * a session with at(). A bar that reorders itself without telling the
	 * registry leaves Close pointing at whichever session used to be in
	 * that slot, and the next rebuild reads the registry and puts the
	 * tabs back, so the drag appears to have been ignored as well.
	 */
	bssh_session_registry registry;

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));
	bssh_session *c = registry.create(named(QStringLiteral("c")));

	QCOMPARE(registry.at(0), a);
	QCOMPARE(registry.at(2), c);

	QSignalSpy changed(&registry, &bssh_session_registry::changed);

	/* The last tab dragged to the front. */
	registry.move(2, 0);

	QCOMPARE(registry.at(0), c);
	QCOMPARE(registry.at(1), a);
	QCOMPARE(registry.at(2), b);

	/* index_of is the same answer read backwards, and must agree. */
	QCOMPARE(registry.index_of(c), 0);
	QCOMPARE(registry.index_of(b), 2);

	/*
	 * And it announces itself, because the tab bar is rebuilt from
	 * changed() and nothing else -- a silent move would leave the labels
	 * and the status dots one session out.
	 */
	QCOMPARE(changed.count(), 1);

	/* Nothing was created or lost by moving. */
	QCOMPARE(registry.count(), 3);

	/*
	 * Out of range does nothing rather than something surprising, and a
	 * move to where it already is says nothing at all -- Qt emits
	 * tabMoved for a drag that ends where it started.
	 */
	registry.move(0, 0);
	registry.move(-1, 1);
	registry.move(1, 99);
	QCOMPARE(changed.count(), 1);
	QCOMPARE(registry.at(0), c);
}

void layout_test::a_dragged_tab_bar_is_a_pointers_affordance() {
	/*
	 * sec 8.3 gives drag-to-reorder to the desktop only. The tab bar
	 * scrolls horizontally where touch is the input, and a bar that both
	 * scrolls and rearranges under one drag picks between them by
	 * accident.
	 */
	{
		const forced_touch_t pointer(0);
		bssh_main_window window;
		QTabBar *tabs = window.findChild<QTabBar *>();
		QVERIFY(tabs);
		QVERIFY2(tabs->isMovable(), "a pointer must be able to drag tabs");
	}

	const forced_touch_t thumb(1);
	bssh_main_window window;
	QTabBar *tabs = window.findChild<QTabBar *>();
	QVERIFY(tabs);
	QVERIFY2(!tabs->isMovable(), "a dragged tab must scroll the bar on touch, not reorder it");
}

void layout_test::middle_click_closes_the_tab_under_it() {
	/*
	 * sec 8.3's middle-click close, which QTabBar raises no signal for:
	 * tabCloseRequested is its close button's, so this is an event filter
	 * and an event filter is exactly the thing that goes missing without
	 * anything failing to compile.
	 */
	const forced_touch_t pointer(0);

	bssh_main_window window;
	bssh_session_registry *registry = window.registry();
	QVERIFY2(registry, "the window must own a session registry");

	/*
	 * Created in the registry and then SHOWN by this window: since
	 * sec 8.4's per-window tab set, a session existing in the process is
	 * not the same as a window displaying it.
	 */
	bssh_session *a = registry->create(named(QStringLiteral("a")));
	bssh_session *b = registry->create(named(QStringLiteral("b")));

	/*
	 * A third, adopted last, to be the pane. The two under test are then
	 * the bar's two tabs -- a pane is not in the bar (sec 8.3), so two
	 * sessions would give one tab and there would be no "second" to
	 * middle-click.
	 */
	bssh_session *shown = registry->create(named(QStringLiteral("shown")));
	window.adopt_session(a);
	window.adopt_session(b);
	window.adopt_session(shown);
	QCOMPARE(registry->count(), 3);

	QTabBar *tabs = window.findChild<QTabBar *>();
	QVERIFY(tabs);
	QCOMPARE(tabs->count(), 2);

	const QPoint on_second = tabs->tabRect(1).center();

	/*
	 * The LEFT button over the same tab must not close anything -- it
	 * selects, which is what a tab bar is for. Asserted first, because a
	 * filter that closes on any button would pass the middle-click half
	 * and destroy a session on every click.
	 */
	QMouseEvent left(QEvent::MouseButtonRelease, on_second, tabs->mapToGlobal(on_second),
	                  Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(tabs, &left);
	QCOMPARE(registry->count(), 3);

	/*
	 * And the left click SELECTED it, which is the other half of "a tab
	 * bar is for selecting": tab 1 is now the pane, so the bar has
	 * rebuilt and `b` has left it. Re-read rather than assumed, because
	 * the middle click below is aimed at a rectangle.
	 */
	QCoreApplication::processEvents();
	QCOMPARE(tabs->count(), 2);

	const QPoint on_first = tabs->tabRect(0).center();
	QMouseEvent middle(QEvent::MouseButtonRelease, on_first, tabs->mapToGlobal(on_first),
	                    Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(tabs, &middle);

	/* The one under the pointer went, and the others did not. */
	QCOMPARE(registry->count(), 2);
	QVERIFY(registry->index_of(a) < 0);
	QVERIFY(registry->index_of(b) >= 0);
	QVERIFY(registry->index_of(shown) >= 0);

	/*
	 * The registry is process-wide now (sec 8.4), so a session left here
	 * is one the next test inherits. Cleaned up explicitly rather than by
	 * the window going away, which no longer owns it.
	 */
	registry->close(b);
	registry->close(shown);
	QCOMPARE(registry->count(), 0);
}

void layout_test::the_tab_menu_offers_sec_8_3s_three_verbs() {
	/*
	 * sec 8.3 asks for close, duplicate and move-to-split. Duplicate was
	 * missing for as long as that sentence has existed, and nothing said
	 * so: the menu was built inside the function that exec()s it, so its
	 * contents were unreachable -- the same reason build_terminal_menu()
	 * was separated out next door.
	 */
	const forced_touch_t pointer(0);

	bssh_main_window window;
	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);

	/*
	 * Two sessions, and the one under test is adopted FIRST. The second
	 * takes the screen, which leaves the first as the window's only tab
	 * -- a window holding one terminal has no tab bar at all now, since
	 * a pane and a tab are different states (sec 8.3).
	 */
	window.adopt_session(registry->create(named(QStringLiteral("a"))));
	window.adopt_session(registry->create(named(QStringLiteral("on-screen"))));

	QMenu *menu = window.build_tab_menu(0, &window);
	QVERIFY2(menu, "a tab with a session behind it must have a menu");

	const QList<QAction *> actions = menu->actions();

	/*
	 * Five, not four: this session has never connected, and a restore
	 * reopens every remembered host exactly like that (sec 8.4), so the
	 * menu leads with Connect. Without it a restored tab would be a dead
	 * end -- the one-way door sec 8.3 already had to fix once.
	 */
	QCOMPARE(actions.size(), 7);

	QCOMPARE(actions.at(0)->text(), QStringLiteral("Co&nnect"));
	QCOMPARE(actions.at(1)->text(), QStringLiteral("&Duplicate"));
	QCOMPARE(actions.at(2)->text(), QStringLiteral("Show &beside current"));

	/*
	 * Then reordering, above the separator because it is not
	 * destructive. The drag that does this is desktop-only -- the bar
	 * scrolls horizontally on a phone and cannot both scroll and
	 * rearrange under one drag -- so touch reaches it here, as panes do.
	 */
	QCOMPARE(actions.at(3)->text(), QStringLiteral("Move &Left"));
	QCOMPARE(actions.at(4)->text(), QStringLiteral("Move &Right"));

	/*
	 * The separator earns its place: Close is the one verb here that
	 * cannot be undone, and a menu that puts it flush against the others
	 * is one a slipped thumb closes a session with.
	 */
	QVERIFY2(actions.at(5)->isSeparator(), "the destructive verb needs separating");
	QCOMPARE(actions.at(6)->text(), QStringLiteral("&Close"));

	for (const QAction *action : actions) {
		if (action->isSeparator()) continue;
		QVERIFY2(action->text().contains(QLatin1Char('&')),
		          qPrintable(action->text() + QStringLiteral(" has no access key")));
	}

	/*
	 * Duplicate does not depend on room for a pane, which the split does
	 * -- it opens a tab. A window with no room must still offer it.
	 */
	QVERIFY2(actions.at(1)->isEnabled(), "Duplicate must not depend on the window's width");

	delete menu;

	/* An index with nothing behind it gets no menu rather than an empty one. */
	QVERIFY2(!window.build_tab_menu(7, &window), "a menu was built for a tab that is not there");

	/* Shared registry: leave it as it was found (sec 8.4). */
	while (registry->count() > 0) registry->close(registry->at(0));
}

void layout_test::clicking_a_pane_focuses_it() {
	/*
	 * sec 8.3's tap-to-focus, and it is a SAFETY property rather than a
	 * convenience: the panes are identical, the focus border is what says
	 * which one is live, and that section calls it non-negotiable because
	 * the wrong pane taking a destructive command is a real outcome.
	 *
	 * Two ideas of "focused" existed and only one of them moved. Qt gives
	 * a clicked pane the keyboard by itself -- the view takes StrongFocus
	 * -- while the container's own idea, which drives the border, the
	 * status line and what copy and paste act on, was moved by the
	 * keyboard chords and by nothing else. So a click sent the typing to
	 * one pane and left everything else pointing at another.
	 *
	 * Focus is moved with setFocus(MouseFocusReason) rather than by
	 * sending a QMouseEvent, and that is a finding rather than a
	 * shortcut: Qt applies focus-on-click for SPONTANEOUS events only, so
	 * a sent press moves no focus at all. Checked against a bare
	 * StrongFocus QWidget in a scratch program to be sure it was the
	 * synthetic event and not this widget -- the plain widget did not take
	 * focus either. A test written the obvious way fails whether or not
	 * the app works.
	 */
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(1200, 400);
	layout.set_intent({a, b});
	QCoreApplication::processEvents();

	QCOMPARE(container.pane_count(), 2);
	QCOMPARE(container.focused_session(), a);

	bssh_terminal_view *second = container.view_at(1);
	QVERIFY(second);

	QSignalSpy focus(&container, &bssh_pane_container::focus_changed);

	/* What Qt does to the second pane when a user clicks in it. */
	second->setFocus(Qt::MouseFocusReason);
	QCoreApplication::processEvents();

	QCOMPARE(container.focused_session(), b);
	QCOMPARE(focus.count(), 1);

	/* And the border moved with it, which is the half a user can see. */
	QCOMPARE(container.view_at(0)->property("bssh_focused").toBool(), false);
	QCOMPARE(container.view_at(1)->property("bssh_focused").toBool(), true);

	/*
	 * Tab between panes is the same hook, which is why this hangs off
	 * focusInEvent rather than off the mouse press: handling the click
	 * alone would move the keyboard by Tab and leave the border behind.
	 */
	container.view_at(0)->setFocus(Qt::TabFocusReason);
	QCoreApplication::processEvents();
	QCOMPARE(container.focused_session(), a);
	QCOMPARE(focus.count(), 2);
}

void layout_test::a_divider_equalizes_on_a_double_click() {
	/*
	 * sec 8.3 asks for double-click/double-tap to equalize, and nothing
	 * implemented it -- QSplitter raises no signal for a double click on
	 * a handle, so the gesture went to the handle and stopped there.
	 *
	 * One handler covers both inputs: Qt synthesises a double-click from
	 * a double tap, so the gesture arrives the same way either way.
	 */
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(1200, 400);
	layout.set_intent({a, b});
	QCoreApplication::processEvents();
	QCOMPARE(container.pane_count(), 2);

	QSplitter *splitter = container.findChild<QSplitter *>();
	QVERIFY(splitter);

	/* Dragged well off centre, as a user would leave it. */
	splitter->setSizes({900, 300});
	QCoreApplication::processEvents();
	QVERIFY2(qAbs(splitter->sizes().at(0) - splitter->sizes().at(1)) > 200,
	          "the panes were meant to start uneven");

	QSplitterHandle *handle = splitter->handle(1);
	QVERIFY(handle);

	const QPoint at(2, 20);
	QMouseEvent click(QEvent::MouseButtonDblClick, at, handle->mapToGlobal(at),
	                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(handle, &click);
	QCoreApplication::processEvents();

	/*
	 * Equal to within a character cell, not to the pixel: the dividers
	 * snap to whole columns (sec 8.3), so equal halves of an odd width
	 * land one cell apart and that is the snap working, not a failure.
	 */
	const int cell = qMax(1, container.view_at(0)->cell_width());
	const int difference = qAbs(splitter->sizes().at(0) - splitter->sizes().at(1));
	QVERIFY2(difference <= cell,
	          qPrintable(QStringLiteral("panes differ by %1px, one cell is %2px")
	                             .arg(difference).arg(cell)));
}

void layout_test::a_divider_is_thumb_sized_only_where_a_thumb_uses_it() {
	/*
	 * sec 8.3 wants a 48dp effective target on touch and allows the
	 * desktop to be tighter. A splitter's grab area IS its handle width
	 * -- Qt has no separate hit region for it -- so the number is the
	 * target, and this was 8 on both for as long as it existed, under a
	 * comment claiming it honoured the 48dp rule.
	 */
	{
		const forced_touch_t thumb(1);
		bssh_layout_manager layout;
		bssh_pane_container container(&layout);
		QSplitter *splitter = container.findChild<QSplitter *>();
		QVERIFY(splitter);
		QVERIFY2(splitter->handleWidth() >= 48,
		          qPrintable(QStringLiteral("a thumb gets %1dp, sec 6.2's floor is 48")
		                             .arg(splitter->handleWidth())));
	}

	const forced_touch_t pointer(0);
	bssh_layout_manager layout;
	bssh_pane_container container(&layout);
	QSplitter *splitter = container.findChild<QSplitter *>();
	QVERIFY(splitter);

	/*
	 * And the desktop is NOT given the thumb-sized one: 48px of dead
	 * space between two panes on a monitor is a gap, not a divider.
	 */
	QVERIFY2(splitter->handleWidth() < 48, "a pointer does not need a thumb's target");
	QVERIFY2(splitter->handleWidth() > 0, "a divider nobody can grab is not a divider");
}

void layout_test::a_bell_in_a_visible_pane_is_not_a_missed_one() {
	/*
	 * sec 8.3's fourth dot state is "bell-rang-while-AWAY", and the flag
	 * behind it documents itself as a bell that rang "while this session
	 * was not the one on screen".
	 *
	 * With a split, more than one session is on screen at once. Only the
	 * FOCUSED one was ever marked so, so a bell in the pane beside the
	 * one being typed in raised a you-missed-this dot on a pane the user
	 * was looking at -- and the same rule marked a still-visible pane as
	 * off screen the moment focus left it.
	 *
	 * Asserted on the FLAG rather than on the dot: these sessions never
	 * connected, and state_for() answers DISCONNECTED for them whatever
	 * the bell says -- deliberately, since a bell on a dead tab would
	 * send someone to read output that stopped arriving. Which state wins
	 * is tab_status_test's question; this one is about which sessions
	 * count as on screen.
	 */
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(1200, 400);
	layout.set_intent({a, b});
	QCoreApplication::processEvents();
	QCOMPARE(container.pane_count(), 2);

	/* Focus is on the first; the second is beside it and plainly visible. */
	QCOMPARE(container.focused_session(), a);

	b->emulator()->feed(QByteArrayLiteral("\a"));
	QVERIFY2(!b->has_unseen_bell(),
	          "a bell in a pane on screen beside the focused one is not missed");

	/*
	 * And a session the layout is NOT showing is away, which is the case
	 * the state exists for: it is a tab, not a pane.
	 */
	layout.set_intent({a});
	container.rebuild();
	QCoreApplication::processEvents();
	QCOMPARE(container.pane_count(), 1);

	b->emulator()->feed(QByteArrayLiteral("\a"));
	QVERIFY2(b->has_unseen_bell(), "a bell in a session with no pane is exactly what the dot is for");

	/*
	 * Bringing it back on screen clears it, because the point of the dot
	 * is that you have not looked yet.
	 */
	layout.set_intent({a, b});
	container.rebuild();
	QCoreApplication::processEvents();
	QVERIFY2(!b->has_unseen_bell(), "showing the pane again means it has been seen");
}

void layout_test::a_bell_with_the_window_away_is_a_missed_one() {
	/*
	 * The case sec 8.3's dot most exists for, and the one having a pane
	 * does not answer: the whole window is gone. On desktop that is
	 * sec 5.5's hide-to-tray, on Android sec 5.5's move-to-back, and in
	 * both the sessions keep running with nobody watching -- which is
	 * precisely "away".
	 *
	 * A pane is necessary and not sufficient. Marking on-screen from the
	 * layout alone left every pane on screen while the app was in the
	 * background, so the bell that rang while the user was in another
	 * app -- the one they most want to know about -- raised nothing.
	 */
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));

	/*
	 * Parented, like the real one: the container lives inside the window,
	 * so hiding the WINDOW is what has to reach it, not hiding itself.
	 */
	QWidget window;
	QVBoxLayout *box = new QVBoxLayout(&window);
	bssh_pane_container *container = new bssh_pane_container(&layout, &window);
	box->addWidget(container);

	window.show();
	window.resize(1200, 400);
	layout.set_intent({a});
	QCoreApplication::processEvents();
	QCOMPARE(container->pane_count(), 1);

	a->emulator()->feed(QByteArrayLiteral("\a"));
	QVERIFY2(!a->has_unseen_bell(), "the window is up and the pane is on it");

	window.hide();
	QCoreApplication::processEvents();

	a->emulator()->feed(QByteArrayLiteral("\a"));
	QVERIFY2(a->has_unseen_bell(), "a bell with the window away is exactly what the dot is for");

	/* And coming back is seeing it. */
	window.show();
	QCoreApplication::processEvents();
	QVERIFY2(!a->has_unseen_bell(), "the window came back, so it has been seen");
}

void layout_test::the_caps_setting_reaches_a_sessions_router() {
	/*
	 * The half that this project keeps getting wrong: a setting that is
	 * stored, read and applied to nothing.
	 *
	 * sec 6.2 records it happening to the key row -- three visibility
	 * modes supported since M3 with nothing ever setting one, so AUTO was
	 * the only reachable state -- and sec 10.1 records it happening to the
	 * override badge, built on one editor while the other resolved against
	 * the app defaults. A router test and a persistence test would both
	 * pass with the window never calling the setter.
	 *
	 * Safe to write because the suite sandboxes HOME and XDG_CONFIG_HOME
	 * (test/main.cpp); restored afterwards regardless, since it is a
	 * process-wide file and the next test should not inherit it.
	 */
	const bool original = bssh_app_settings().caps_as_ctrl();
	bssh_app_settings().set_caps_as_ctrl(true);

	{
		bssh_main_window window;
		bssh_session_registry *registry = window.registry();
		QVERIFY(registry);

		/* A session opened AFTER the setting was stored. */
		bssh_session *session = registry->create(named(QStringLiteral("a")));
		QVERIFY(session->input_router());
		QVERIFY2(session->input_router()->caps_as_ctrl(),
		          "the stored setting never reached the session's router");
	}

	bssh_app_settings().set_caps_as_ctrl(original);
	QCOMPARE(bssh_app_settings().caps_as_ctrl(), original);

	/* Shared registry: leave it as it was found (sec 8.4). */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_key_remap_setting_reaches_a_sessions_router() {
	const auto seq = [](Qt::Key key) {
		return QKeySequence(QKeyCombination(Qt::NoModifier, key))
		        .toString(QKeySequence::PortableText);
	};

	const QList<QPair<QString, QString>> original = bssh_app_settings().key_remaps();

	QList<QPair<QString, QString>> remaps(bssh_app_settings::BSSH_KEY_REMAP_SLOTS);
	remaps[0] = qMakePair(seq(Qt::Key_F13), seq(Qt::Key_Escape));
	bssh_app_settings().set_key_remaps(remaps);

	/*
	 * Clear the process-wide table first, so the only thing that can arm
	 * it is the window's own apply_device_settings. Without this a remap
	 * left by an earlier test would let F13 speak even with the wiring
	 * cut, and the sabotage that cuts it would read green.
	 */
	bssh_input_router::set_key_remaps(QHash<QString, QString>());

	{
		/*
		 * A window built AFTER the remap is stored applies it through
		 * apply_device_settings, exactly as it does the chords. A session
		 * opened in that window then speaks the mapped key.
		 */
		bssh_main_window window;
		bssh_session *session =
		        window.registry()->create(named(QStringLiteral("a")));
		QVERIFY(session->input_router());
		QVERIFY(session->emulator());

		QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
		session->input_router()->handle_key(&escape);
		const QByteArray want = session->emulator()->take_output();
		QVERIFY(!want.isEmpty());

		QKeyEvent f13(QEvent::KeyPress, Qt::Key_F13, Qt::NoModifier);
		session->input_router()->handle_key(&f13);
		const QByteArray got = session->emulator()->take_output();
		QVERIFY2(got == want, "the stored remap never reached the session's router");
	}

	bssh_app_settings().set_key_remaps(original);
	bssh_input_router::set_key_remaps(QHash<QString, QString>());

	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_tiled_session_leaves_the_tab_bar() {
	/*
	 * Reported from use: "they become tabs. tabs, splits and windows are
	 * supposed to be different things". They were not -- the bar was
	 * built from m_shown outright, so a session tiled into a pane was
	 * listed in the bar directly above itself, and splitting a window in
	 * two produced two tabs for two terminals both already on screen.
	 *
	 * sec 8.3 had said otherwise since M8 ("a session with a pane is on
	 * screen, one without is a tab") and nothing had ever read it that
	 * way, which is why this is asserted rather than left to the
	 * document.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session *tiled = window.registry()->create(named(QStringLiteral("tiled")));
	bssh_session *waiting = window.registry()->create(named(QStringLiteral("waiting")));
	bssh_session *shown = window.registry()->create(named(QStringLiteral("shown")));
	window.adopt_session(tiled);
	window.adopt_session(waiting);
	window.adopt_session(shown);
	QCoreApplication::processEvents();

	QTabBar *bar = window.findChild<QTabBar *>();
	QVERIFY(bar);

	/*
	 * Three terminals, one on screen: two tabs. The window holds all
	 * three either way, which is the pair of facts the old model could
	 * not tell apart.
	 */
	QCOMPARE(window.shown_sessions().size(), 3);
	QCOMPARE(bar->count(), 2);
	QCOMPARE(window.panes()->views().size(), 1);

	/* Tiled beside what is already there -- the drop-on-the-panes half. */
	window.drop_session(tiled, true);
	QCoreApplication::processEvents();

	QCOMPARE(window.panes()->views().size(), 2);
	QCOMPARE(window.shown_sessions().size(), 3);

	/*
	 * And it is gone from the bar. This is the assertion the whole change
	 * is about: same three terminals, one more of them on screen, one
	 * fewer in the bar.
	 */
	QCOMPARE(bar->count(), 1);
	QCOMPARE(bar->tabText(0), QStringLiteral("waiting"));

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_pane_dropped_on_the_tab_bar_becomes_a_tab() {
	/*
	 * The other direction, and the one that makes a split escapable: a
	 * pane dropped on the tab bar goes back to being a tab (sec 8.3).
	 * Without it a tiled terminal could only be un-tiled by clicking some
	 * OTHER tab and letting show_session wipe the whole arrangement,
	 * which is a destructive answer to a question nobody asked
	 * destructively.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session *first = window.registry()->create(named(QStringLiteral("first")));
	window.adopt_session(first);
	window.split_beside_current();
	QCoreApplication::processEvents();

	QCOMPARE(window.panes()->views().size(), 2);

	QTabBar *bar = window.findChild<QTabBar *>();
	QVERIFY(bar);
	QVERIFY2(bar->count() == 0,
	          "two panes and nothing else is a window with no tabs at all");

	window.drop_session(first, false);
	QCoreApplication::processEvents();

	QCOMPARE(window.panes()->views().size(), 1);
	QCOMPARE(bar->count(), 1);
	QCOMPARE(bar->tabText(0), first->title());
	QVERIFY2(window.shown_sessions().contains(first),
	          "un-tiling must not close the terminal, only stop showing it");

	/*
	 * And the LAST pane will not go. A window with no pane is a window
	 * showing nothing, and every route back to its terminals would be
	 * through a bar it grew by emptying itself -- the same answer
	 * tear_out gives one level out ("a window will not empty itself").
	 */
	bssh_session *last = window.panes()->focused_session();
	QVERIFY(last);

	window.drop_session(last, false);
	QCoreApplication::processEvents();

	QCOMPARE(window.panes()->views().size(), 1);
	QCOMPARE(window.panes()->focused_session(), last);

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::narrowing_a_window_turns_its_extra_panes_into_tabs() {
	/*
	 * sec 8.2 step 2: "panes convert to tabs, preserving order". Now that
	 * a pane and a tab are different states, that sentence has an
	 * observable consequence it did not have before -- the terminal that
	 * stops being a pane has to APPEAR in the bar, and the arrangement is
	 * remembered so widening brings it back.
	 *
	 * This is the case the tab bar's rebuild-on-panes-rebuilt exists for,
	 * and the reason it is not redundant with the one in the focus
	 * handler: a collapse changes what is on screen without changing the
	 * registry, without changing the window's session list, and often
	 * without changing which pane is focused. Nothing else would notice.
	 * The sabotage runner found that gap by reporting MISSED against a
	 * test that only ever split a window, which is a rebuild that DOES
	 * move focus.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session *first = window.registry()->create(named(QStringLiteral("first")));
	window.adopt_session(first);
	window.split_beside_current();
	QCoreApplication::processEvents();

	QTabBar *bar = window.findChild<QTabBar *>();
	QVERIFY(bar);
	QCOMPARE(window.panes()->views().size(), 2);
	QCOMPARE(bar->count(), 0);

	/* Phone width: one pane is all that fits (sec 8.2). */
	window.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	QCoreApplication::processEvents();

	QCOMPARE(window.panes()->views().size(), 1);
	QVERIFY2(bar->count() == 1,
	          "the terminal that stopped being a pane has to become a tab, "
	          "or it is on the window's list and reachable from nothing");

	/*
	 * And back. The intent is remembered rather than truncated (sec 8.2),
	 * so widening restores the split and the bar empties again -- which
	 * is the half that proves the collapse did not DELETE anything.
	 */
	window.resize(900, 600);
	QCoreApplication::processEvents();

	QCOMPARE(window.panes()->views().size(), 2);
	QCOMPARE(bar->count(), 0);

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::what_the_terminal_reports_is_what_it_paints() {
	/*
	 * sec 7.1's colour and metric answers, asserted as a RELATIONSHIP
	 * between what this terminal says and what it actually draws --
	 * never as either value on its own.
	 *
	 * The reason is a limit on the whole cross-checking arrangement,
	 * named from the other end by qtty and worth having in a test rather
	 * than only in prose: a client CANNOT detect a divergence here. It
	 * compares a reported colour against pixels that arrive through the
	 * same terminal, so a palette reported wrongly looks exactly like a
	 * theme. Having a real consumer on the other side of these answers
	 * does not cover this, and it never will -- some invariants are only
	 * checkable from inside this tree, and this is the test that does it.
	 *
	 * Each pair goes through the REAL wiring: the pane container is what
	 * tells the emulator its colours, palette and cell size, and the
	 * whole class of defect here is that wiring being absent or stale
	 * while both halves are individually correct.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	/*
	 * A transport, because the session DRAINS the emulator into it --
	 * take_output() is called on every output_ready and the bytes are
	 * dropped when there is nowhere to send them. Reading the emulator
	 * here would return empty and the test would pass against any answer
	 * whatsoever, including none. sec 12.4 records the same trap costing
	 * an XTGETTCAP test.
	 */
	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});

	{
		bssh_main_window window;
		window.resize(900, 600);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));

		bssh_session *session = window.registry()->create(named(QStringLiteral("asked")));
		window.adopt_session(session);
		session->connect_to_host();
		QCoreApplication::processEvents();
		QVERIFY(fake);

		bssh_terminal_view *view = window.panes()->focused_view();
		QVERIFY(view);

		const auto ask = [&](const QByteArray &query) {
			fake->clear_written();
			session->emulator()->feed(query);
			QCoreApplication::processEvents();
			return fake->written();
		};

		/*
		 * The background, against the pixel actually on screen rather
		 * than against the member it came from. A grab is the only
		 * reading a compositing client's own eye would agree with, and
		 * comparing the reply to terminal_background() would compare
		 * the wiring against itself.
		 */
		const QByteArray reported_bg = ask(QByteArrayLiteral("\x1b]11;?\x1b\\"));

		const QImage painted = view->grab().toImage();

		/*
		 * The MIDDLE of the view, not a corner. The first version of
		 * this sampled three pixels in from the bottom right and read
		 * f3f3f3 against a reported 000000 -- which was the SCROLLBAR,
		 * a child widget drawn in the application palette. The probe
		 * was measuring a place the fault could not appear in, and it
		 * would have failed identically against correct code.
		 *
		 * The centre is inside the grid whatever side the bar is on,
		 * and it is clear of the cursor at row 0 and of anything the
		 * session has printed.
		 */
		const QColor corner = painted.pixelColor(painted.width() / 2, painted.height() / 2);

		const QByteArray expected_bg = QByteArrayLiteral("\x1b]11;rgb:")
		        + QByteArray::number((corner.red() << 8) | corner.red(), 16)
		                  .rightJustified(4, '0')
		        + QByteArrayLiteral("/")
		        + QByteArray::number((corner.green() << 8) | corner.green(), 16)
		                  .rightJustified(4, '0')
		        + QByteArrayLiteral("/")
		        + QByteArray::number((corner.blue() << 8) | corner.blue(), 16)
		                  .rightJustified(4, '0')
		        + QByteArrayLiteral("\x1b\\");

		QCOMPARE(reported_bg, expected_bg);

		/*
		 * The cell, against the view's own measurement. This is the
		 * pair an image's aspect ratio depends on, and a stale value
		 * here is invisible from outside: a client scales to whatever
		 * it is told and the picture is simply slightly wrong.
		 */
		const QByteArray reported_cell = ask(QByteArrayLiteral("\x1b[16t"));
		QCOMPARE(reported_cell,
		          QByteArrayLiteral("\x1b[6;")
		          + QByteArray::number(view->cell_height())
		          + QByteArrayLiteral(";")
		          + QByteArray::number(view->cell_width())
		          + QByteArrayLiteral("t"));

		QVERIFY2(view->cell_width() > 0 && view->cell_height() > 0,
		          "a zero cell would satisfy the comparison above and mean nothing");

		/*
		 * And a palette index, against what the RENDERER resolves it
		 * to. Index 1 rather than 0: the low sixteen are the host's
		 * scheme and are the half a client cannot derive, and 1 is
		 * ordinary red in every scheme rather than a background that
		 * might coincide with something else.
		 */
		bssh_color_t red;
		red.kind = bssh_color_t::INDEXED;
		red.index = 1;

		const QColor drawn = view->colour_for_test(red);

		const QByteArray reported_red = ask(QByteArrayLiteral("\x1b]4;1;?\x1b\\"));
		QCOMPARE(reported_red,
		          QByteArrayLiteral("\x1b]4;1;rgb:")
		          + QByteArray::number((drawn.red() << 8) | drawn.red(), 16)
		                    .rightJustified(4, '0')
		          + QByteArrayLiteral("/")
		          + QByteArray::number((drawn.green() << 8) | drawn.green(), 16)
		                    .rightJustified(4, '0')
		          + QByteArrayLiteral("/")
		          + QByteArray::number((drawn.blue() << 8) | drawn.blue(), 16)
		                    .rightJustified(4, '0')
		          + QByteArrayLiteral("\x1b\\"));
	}

	bssh_ssh_session::set_factory(nullptr);
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_keystroke_in_a_pane_reaches_the_wire() {
	/*
	 * The most fundamental thing a terminal does, and nothing asserted
	 * it end to end.
	 *
	 * The input router has thirty-two tests -- keys in, bytes out, every
	 * modifier and both keyboard protocols -- and the transport has
	 * `session_records_what_the_client_wrote`, which calls send() on the
	 * fake and checks the fake recorded it. That test's comment calls
	 * itself "the seam M3's input router gets tested through", and no
	 * router, no view and no key event appear anywhere in it. So both
	 * halves were exhaustively covered and the chain between them --
	 * key event, view, router, emulator, session, transport -- was not
	 * covered at all.
	 *
	 * Found with a search key the sibling qtty derived from three of its
	 * own faults: the parse being right is what hides the delivery being
	 * wrong, because the parse is the half that is easy to assert. Find
	 * the exhaustively-tested translator, then ask what carries its
	 * output and whether anything asserts the carrying. Its own
	 * application of it found an image encoder with sixty-eight
	 * round-trip checks and no test that it ever reached the wire.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});

	{
		bssh_main_window window;
		window.resize(900, 600);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		window.activateWindow();

		bssh_session *session = window.registry()->create(named(QStringLiteral("typed")));
		window.adopt_session(session);
		session->connect_to_host();
		QCoreApplication::processEvents();
		QVERIFY(fake);

		bssh_terminal_view *view = window.panes()->focused_view();
		QVERIFY(view);
		view->setFocus();
		QTRY_VERIFY(view->hasFocus());

		fake->clear_written();

		/*
		 * A real key event at the real widget. QTest::keyClicks would
		 * do the same thing; this spells the event out so the
		 * modifiers are visible, since the interesting cases below are
		 * about them.
		 */
		QKeyEvent letter(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier, QStringLiteral("a"));
		QCoreApplication::sendEvent(view, &letter);
		QCoreApplication::processEvents();

		QCOMPARE(fake->written(), QByteArrayLiteral("a"));

		/*
		 * And a control chord, which is the half that would still work
		 * if the view were writing the text of the event straight out
		 * and the router were doing nothing. Ctrl+C is 0x03, and no
		 * amount of forwarding `text()` produces it -- QKeyEvent's text
		 * for this carries the control character on some platforms and
		 * not others, so this asserts the router ran rather than that
		 * the event happened to contain the answer.
		 */
		fake->clear_written();

		QKeyEvent interrupt(QEvent::KeyPress, Qt::Key_C, Qt::ControlModifier,
		                     QStringLiteral("c"));
		QCoreApplication::sendEvent(view, &interrupt);
		QCoreApplication::processEvents();

		QCOMPARE(fake->written(), QByteArrayLiteral("\x03"));

		/*
		 * An arrow, which no keystroke carries as text at all: it only
		 * exists as an escape sequence the router builds. If the wire
		 * ever showed a letter and not this, the router had been cut
		 * out of the path entirely.
		 */
		fake->clear_written();

		QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier, QString());
		QCoreApplication::sendEvent(view, &up);
		QCoreApplication::processEvents();

		QCOMPARE(fake->written(), QByteArrayLiteral("\x1b[A"));
	}

	bssh_ssh_session::set_factory(nullptr);
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_mouse_report_reaches_the_wire() {
	/*
	 * The other half of the gap the keystroke test exposed. Every
	 * mouse-report test in this tree reads the emulator's own output
	 * buffer, so all of them pass whether or not anything carries the
	 * bytes to the far end -- correct about the encoding, silent about
	 * the delivery.
	 *
	 * ADDED BESIDE THOSE RATHER THAN WIDENING THEM, and the reason is
	 * the sibling qtty's: they are a measurement of where this suite's
	 * attention was, and once they assert delivery too, nobody can tell
	 * they never did. The record is worth more than the coverage, and
	 * the coverage can be added without losing it.
	 *
	 * A click rather than a keystroke, because the two take different
	 * routes to the same wire: a key goes view, router, emulator, while
	 * a click goes view, emulator directly (sec 6.4). Only the tail is
	 * shared, so the keystroke test does not cover this.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});

	{
		bssh_main_window window;
		window.resize(900, 600);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		window.activateWindow();

		bssh_session *session = window.registry()->create(named(QStringLiteral("clicked")));
		window.adopt_session(session);
		session->connect_to_host();
		QCoreApplication::processEvents();
		QVERIFY(fake);

		bssh_terminal_view *view = window.panes()->focused_view();
		QVERIFY(view);

		/* The remote asks for the mouse, or a click is a local selection
		 * and there is nothing to send (sec 6.4). */
		session->emulator()->feed(QByteArrayLiteral("\x1b[?1000h\x1b[?1006h"));
		QCoreApplication::processEvents();
		QVERIFY(session->emulator()->mouse_reporting());

		fake->clear_written();

		const QPoint at(view->cell_width() * 3 + view->cell_width() / 2,
		                 view->cell_height() * 2 + view->cell_height() / 2);

		QMouseEvent press(QEvent::MouseButtonPress, at, view->mapToGlobal(at),
		                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
		QCoreApplication::sendEvent(view, &press);
		QCoreApplication::processEvents();

		/*
		 * SGR: CSI < button ; col ; row M, one-based, and the
		 * coordinates are the discriminating part. A report that
		 * arrived with the right shape and the wrong cell would satisfy
		 * any assertion about "something was sent" -- and the columns
		 * are where a scrollbar's width or a grid inset goes wrong,
		 * which is a fault this project has had once already.
		 */
		QCOMPARE(fake->written(), QByteArrayLiteral("\x1b[<0;4;3M"));

		fake->clear_written();

		QMouseEvent release(QEvent::MouseButtonRelease, at, view->mapToGlobal(at),
		                     Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
		QCoreApplication::sendEvent(view, &release);
		QCoreApplication::processEvents();

		QCOMPARE(fake->written(), QByteArrayLiteral("\x1b[<0;4;3m"));
	}

	bssh_ssh_session::set_factory(nullptr);
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_resized_pane_tells_the_remote_its_size() {
	/*
	 * A window-change is what stops a remote laying out for a terminal
	 * that is no longer that shape, and nothing in this suite asserted
	 * one is ever sent. The fake transport has counted resizes since it
	 * was written; the only caller that ever read the count is
	 * live_ssh_test, which needs a real server and does not run under
	 * `make check`.
	 *
	 * Found by finishing a lens rather than stopping where it last paid:
	 * the emitters that decide WHETHER to write, rather than what. The
	 * sibling qtty found the mirror of this in its own tree -- its
	 * backend delivering a resize was checked in one suite and its
	 * router resizing a window in another, with nothing running SIGWINCH
	 * through both.
	 *
	 * The two assertions are a pair and neither is sufficient. That a
	 * real change is sent is the feature. That an UNCHANGED size is not
	 * is the gate -- `if (cols == m_cols && rows == m_rows) return;` --
	 * and it is the half a test written to the feature's name would
	 * never ask, while being the half that fails silently: a gate
	 * comparing the wrong fields swallows real resizes and the remote
	 * lays out for the old shape for the rest of the session.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});

	{
		bssh_main_window window;
		window.resize(900, 600);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));

		bssh_session *session = window.registry()->create(named(QStringLiteral("sized")));
		window.adopt_session(session);
		session->connect_to_host();
		QCoreApplication::processEvents();
		QVERIFY(fake);

		bssh_terminal_view *view = window.panes()->focused_view();
		QVERIFY(view);

		const int before = fake->resize_count();

		/* A real resize of the WINDOW, which is how this happens: the
		 * view re-measures its grid and the session carries the new
		 * shape outward. */
		window.resize(640, 480);
		QCoreApplication::processEvents();

		/*
		 * The message carries what each end measured, because the bare
		 * condition cannot separate the three things that produce it:
		 * the window resize not changing the grid, the grid change not
		 * reaching the session, or the session's gate swallowing it.
		 * A QTRY takes five seconds to fail, so a message that
		 * guarantees a round of guessing costs more than the assertion
		 * saves.
		 *
		 * The rule is the sibling qtty's, and its tell is the sharp
		 * part: a temporary print added to diagnose a failure and then
		 * deleted is evidence the message was insufficient, and
		 * deleting it restores the insufficiency for the next reader.
		 */
		QTRY_VERIFY2(fake->resize_count() > before,
		              qPrintable(QStringLiteral("no window-change reached the transport; "
		                                         "the view measured %1x%2 and the transport "
		                                         "was last told %3x%4")
		                                 .arg(view->cols()).arg(view->rows())
		                                 .arg(fake->cols()).arg(fake->rows())));

		QVERIFY2(view->cols() > 0 && view->rows() > 0,
		          qPrintable(QStringLiteral("the grid measured %1x%2")
		                             .arg(view->cols()).arg(view->rows())));
		QCOMPARE(fake->cols(), view->cols());
		QCOMPARE(fake->rows(), view->rows());

		/*
		 * And the gate. Asking for the same size again must send
		 * nothing: a window-change per repaint is a message the remote
		 * answers by redrawing, so a gate that let them through would
		 * turn one drag of a window edge into a storm of full-screen
		 * repaints on the far end.
		 */
		const int settled = fake->resize_count();

		session->resize(view->cols(), view->rows());
		session->resize(view->cols(), view->rows());
		QCoreApplication::processEvents();

		QCOMPARE(fake->resize_count(), settled);

		/* One more real change, so the assertion above is shown to be
		 * about the repeat rather than about the counter having
		 * stopped. */
		session->resize(view->cols() + 1, view->rows());
		QCoreApplication::processEvents();

		QCOMPARE(fake->resize_count(), settled + 1);
		QCOMPARE(fake->cols(), view->cols() + 1);

		/*
		 * And a change in the ROWS ALONE, which is the case that
		 * separates a gate comparing both fields from one comparing
		 * the columns. Written because the sabotage runner reported
		 * MISSED without it: every change above moved the columns, so
		 * a columns-only gate satisfied the lot -- the assertions were
		 * a case where the right answer and the wrong one agree.
		 *
		 * A terminal made shorter without being made narrower is not
		 * exotic. It is what dragging the bottom edge of a window
		 * does, and what appearing or hiding a tab bar does to every
		 * pane under it.
		 */
		const int rows_only = fake->resize_count();

		session->resize(view->cols() + 1, view->rows() + 1);
		QCoreApplication::processEvents();

		QCOMPARE(fake->resize_count(), rows_only + 1);
		QCOMPARE(fake->rows(), view->rows() + 1);
	}

	bssh_ssh_session::set_factory(nullptr);
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::the_wheel_scrolls_the_settings_page_and_changes_nothing() {
	/*
	 * Reported from use: "suddenly the text became dark when looking
	 * through config menus". A QComboBox handles a wheel notch by
	 * changing its value whether or not it has focus, so scrolling the
	 * device settings with the pointer over the Appearance row moved it
	 * from "Follow the system" to "Light" -- applied immediately and
	 * written to app.ini, so it survived a restart. The page did not
	 * move, which is what makes it keep happening: the natural reaction
	 * to a page that will not scroll is to wheel again.
	 *
	 * Safe to write because the suite sandboxes HOME and XDG_CONFIG_HOME
	 * (test/main.cpp); restored afterwards regardless.
	 */
	const forced_touch_t pointer(0);

	const QString original = bssh_app_settings().theme();
	bssh_app_settings().set_theme(QStringLiteral("dark"));

	{
		bssh_app_settings_view view;

		/*
		 * Small enough that the form overflows. Without this the test
		 * is vacuous in the nastiest way: a page that cannot scroll
		 * satisfies "the scroll position is right" trivially, and the
		 * combo assertion below would be the only real one left.
		 */
		view.resize(420, 220);
		view.show();
		QVERIFY(QTest::qWaitForWindowExposed(&view));

		QScrollArea *scroll = view.findChild<QScrollArea *>();
		QVERIFY(scroll);
		QVERIFY2(scroll->verticalScrollBar()->maximum() > 0,
		          "the page has to overflow, or this test asks nothing");

		/*
		 * The Appearance combo found by what it CONTAINS rather than by
		 * a label or an object name: the item data is the thing the
		 * defect acted on, and it is what commit() writes.
		 */
		QComboBox *theme = nullptr;
		for (QComboBox *combo : view.findChildren<QComboBox *>()) {
			if (combo->findData(QStringLiteral("dark")) >= 0
			    && combo->findData(QStringLiteral("light")) >= 0) {
				theme = combo;
			}
		}
		QVERIFY(theme);

		const int before = theme->currentIndex();
		const int scrolled_before = scroll->verticalScrollBar()->value();

		const QPointF at(theme->width() / 2.0, theme->height() / 2.0);
		QWheelEvent wheel(at, theme->mapToGlobal(at.toPoint()), QPoint(0, -120),
		                   QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
		                   Qt::NoScrollPhase, false);
		QCoreApplication::sendEvent(theme, &wheel);
		QCoreApplication::processEvents();

		QCOMPARE(theme->currentIndex(), before);
		QCOMPARE(bssh_app_settings().theme(), QStringLiteral("dark"));

		/*
		 * The load-bearing third clause. A fix that merely SWALLOWS the
		 * wheel satisfies both assertions above while leaving the page
		 * unscrollable -- a different bug wearing this one's clothes,
		 * and the more annoying of the two to use.
		 */
		QVERIFY2(scroll->verticalScrollBar()->value() > scrolled_before,
		          "the notch has to reach the page it was aimed at");
	}

	bssh_app_settings().set_theme(original);
}

void layout_test::the_biometric_setting_is_absent_where_it_cannot_work() {
	/*
	 * sec 11's per-host biometric release, offered only where a prompt
	 * can actually be raised -- and the assertion is that the control is
	 * ABSENT rather than disabled, which is the same rule the
	 * hide-from-screenshots checkbox follows next door.
	 *
	 * Present-but-ineffective is the failure being avoided: a user who
	 * ticks a box promising a fingerprint dialog on a device with no
	 * reader, or with a reader and nothing enrolled, has made their own
	 * stored password unreachable and will find out at the worst moment.
	 *
	 * Both directions are driven through force_biometric, because a test
	 * that only checked the absent case would pass against a page that
	 * never offers it at all -- which is what the page did until today,
	 * and which no assertion about absence can tell from correctness.
	 */
	QTemporaryDir dir;
	bssh_profile_store store(dir.path());

	{
		bssh_platform::force_biometric(0);

		bssh_host_editor without(&store);
		bool found = false;
		for (const QCheckBox *box : without.findChildren<QCheckBox *>()) {
			if (box->text().contains(QStringLiteral("fingerprint"), Qt::CaseInsensitive)) {
				found = true;
			}
		}
		QVERIFY2(!found, "a device that cannot raise a prompt must not offer one");
	}

	{
		bssh_platform::force_biometric(1);

		bssh_host_editor with(&store);
		QCheckBox *offered = nullptr;
		for (QCheckBox *box : with.findChildren<QCheckBox *>()) {
			if (box->text().contains(QStringLiteral("fingerprint"), Qt::CaseInsensitive)) {
				offered = box;
			}
		}
		QVERIFY2(offered, "a device that can raise a prompt must offer the setting");

		/*
		 * And it carries the profile's value in both directions. A
		 * checkbox that displays the setting and does not write it back
		 * is sec 10.1's override badge again -- the control was there,
		 * and the thing it claimed to control was elsewhere.
		 */
		bssh_profile_t armed;
		armed.host = QStringLiteral("locked.example");
		armed.require_biometric = true;
		with.set_profile(armed);

		QVERIFY2(offered->isChecked(), "the stored setting must reach the control");

		offered->setChecked(false);
		QVERIFY2(!with.profile().require_biometric,
		          "and clearing it must reach the profile");

		offered->setChecked(true);
		QVERIFY(with.profile().require_biometric);
	}

	/*
	 * And a profile edited on a device that CANNOT prompt keeps its
	 * setting rather than being quietly disarmed -- a host synced from a
	 * phone with a reader, opened on a desktop without one, must not
	 * come back with the lock removed.
	 */
	{
		bssh_platform::force_biometric(0);

		bssh_host_editor elsewhere(&store);
		bssh_profile_t armed;
		armed.host = QStringLiteral("locked.example");
		armed.require_biometric = true;
		elsewhere.set_profile(armed);

		QVERIFY2(elsewhere.profile().require_biometric,
		          "editing a host where the control is absent must not clear it");
	}

	bssh_platform::force_biometric(-1);
}

void layout_test::every_surface_names_the_copyright_holder() {
	/*
	 * harmonization.md, 2026-08-29: the holder is named in --version, in
	 * the About window, and in the README. This asserts the two that are
	 * code; the README is prose and the gate that reads it is a person.
	 *
	 * ATTRIBUTION, NOT LICENSING, and the test says so because the next
	 * reader of this file will wonder: naming the holder is a statement
	 * of fact that grants nothing, creates no obligation to add a
	 * LICENSE, and does not touch this project's licensing position --
	 * which CLAUDE.md reserves to the copyright holder alone.
	 *
	 * Asserted against the CONSTANT rather than against a literal. A
	 * test carrying its own copy of the line would pass while the
	 * surfaces drifted from each other, which is the whole reason there
	 * is a constant -- and the address in it is exactly the kind of
	 * string a second copy gets subtly wrong.
	 */
	const QString expected = QStringLiteral(BSSH_COPYRIGHT_LINE);
	QVERIFY2(expected.contains(QStringLiteral("Nabeel Sowan")),
	          "the constant itself must name the holder, or every check below is vacuous");

	/*
	 * The touch surface, which had NO version and no attribution before
	 * this: a phone has no menu bar, so the desktop About box does not
	 * exist there and the licences page is the only place the app says
	 * what it is. That asymmetry is the one sec 10 keeps recording.
	 */
	{
		const forced_touch_t thumb(1);

		bssh_licences_view view;
		const QLabel *identity =
		        view.findChild<QLabel *>(QStringLiteral("licences_identity"));
		QVERIFY2(identity, "the touch front door must have somewhere the app names itself");

		/*
		 * IN THE LAYOUT, not merely constructed. The first version of
		 * this asked findChild() for the label and checked its text --
		 * and findChild finds a widget that was built with the view as
		 * its parent whether or not anything ever put it on the page.
		 * The sabotage runner said so: deleting the addWidget left the
		 * suite green, so the assertion was about construction and the
		 * user would have seen nothing.
		 *
		 * That is the same defect this session spent the day finding
		 * in other people's code and in this one's -- correctly built,
		 * delivered nowhere -- written into the test for the fix.
		 */
		QVERIFY(view.layout());
		QVERIFY2(view.layout()->indexOf(const_cast<QLabel *>(identity)) >= 0,
		          "the label exists but was never added to the page");

		const QString shown = identity->text();
		QVERIFY2(shown.contains(QStringLiteral("Nabeel Sowan")),
		          qPrintable(QStringLiteral("no attribution on the touch surface: %1")
		                             .arg(shown)));
		QVERIFY2(shown.contains(QStringLiteral("BeerSSH")), qPrintable(shown));
	}

	/*
	 * And the same page on a pointer, because it is reachable there too
	 * and a surface that names the holder only on one platform is the
	 * asymmetry this test exists to close.
	 */
	{
		const forced_touch_t pointer(0);

		bssh_licences_view view;
		const QLabel *identity =
		        view.findChild<QLabel *>(QStringLiteral("licences_identity"));
		QVERIFY(identity);
		QVERIFY(view.layout());
		QVERIFY(view.layout()->indexOf(const_cast<QLabel *>(identity)) >= 0);
		QVERIFY(identity->text().contains(QStringLiteral("Nabeel Sowan")));
	}
}

void layout_test::the_screenshot_setting_is_absent_where_it_cannot_work() {
	/*
	 * sec 11's "don't promise one in the UI", asserted on the UI rather
	 * than on the platform answer alone. The desktop cannot hide a window
	 * from a screenshot, so the checkbox must not be on the page at all
	 * -- present-but-ineffective is the failure being avoided, and it is
	 * one only a test of the assembled page can see.
	 */
	const forced_touch_t pointer(0);

	bssh_app_settings_view view;
	for (const QCheckBox *box : view.findChildren<QCheckBox *>()) {
		QVERIFY2(!box->text().contains(QStringLiteral("screenshot"), Qt::CaseInsensitive),
		          "the desktop must not offer a control it cannot honour");
	}

	/* The page still carries the settings this platform CAN honour. */
	bool found_caps = false;
	for (const QCheckBox *box : view.findChildren<QCheckBox *>()) {
		if (box->text().contains(QStringLiteral("Caps Lock"))) found_caps = true;
	}
	QVERIFY2(found_caps, "the rest of the page must still be there");
}

void layout_test::focus_reporting_follows_the_focused_pane() {
	/*
	 * sec 7.1's mode 1004 reaching the SESSION, which an emulator test
	 * cannot see. Removing this wiring leaves the emulator's own test
	 * passing and the feature dead -- the same shape as sec 6.2's key row
	 * supporting three modes with nothing setting one, and the reason the
	 * caps setting grew a third test.
	 *
	 * Per pane, because that is the question the remote asked: an editor
	 * in one pane wants to know when THAT pane is looked at.
	 */
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(1200, 400);
	layout.set_intent({a, b});
	QCoreApplication::processEvents();
	QCOMPARE(container.pane_count(), 2);

	/*
	 * The first pane already holds focus: the container focuses it as it
	 * builds, so asking for focus it already has raises no event and
	 * reports nothing. That is correct -- focus did not move -- and it is
	 * why this starts by moving focus AWAY.
	 */
	QCOMPARE(container.focused_session(), a);

	/* Only the first session's remote asks to be told. */
	a->emulator()->feed(QByteArrayLiteral("\x1b[?1004h"));
	a->emulator()->take_output();
	b->emulator()->take_output();

	/* Focus leaves the first pane: it is told so. */
	container.view_at(1)->setFocus(Qt::MouseFocusReason);
	QCoreApplication::processEvents();
	QCOMPARE(a->emulator()->take_output(), QByteArrayLiteral("\x1b[O"));

	/* And coming back reports the other direction. */
	container.view_at(0)->setFocus(Qt::MouseFocusReason);
	QCoreApplication::processEvents();
	QCOMPARE(a->emulator()->take_output(), QByteArrayLiteral("\x1b[I"));

	/*
	 * And the pane whose remote never enabled the mode hears nothing at
	 * all, however much its focus moves -- those bytes would land on its
	 * command line.
	 */
	QCOMPARE(b->emulator()->take_output(), QByteArray());
}

void layout_test::the_key_row_fits_a_phone() {
	/*
	 * The one touch surface that was not in every_touch_surface_fits_a_phone
	 * and is the only one on that list which exists ONLY for touch. Every
	 * other surface has a desktop form somebody looks at; this one is seen
	 * on a device or not at all, and it asked for 652px of a 360px screen
	 * for as long as sec 6.2's key list and sec 6.2's 48dp floor have both
	 * been true.
	 *
	 * Its own test rather than another entry in that loop, because the
	 * question differs: the row is allowed -- required -- to hold more keys
	 * than fit, and what must be true is that the WIDGET fits while its
	 * contents scroll.
	 */
	const forced_touch_t thumb(1);

	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	const bssh_key_row_catalog catalog;
	QVERIFY(!catalog.all().isEmpty());

	for (const bssh_key_row_layout_t &layout : catalog.all()) {
		bssh_key_row row(&router);
		row.set_layout(layout.id);
		/*
		 * Sideways is this one surface's whole design (sec 6.2): its
		 * keys run past the edge and scroll, because shrinking them
		 * under the touch floor would make a key nobody can hit -- and
		 * the assertions below check exactly that they DO run past.
		 */
		check_fits(QStringLiteral("key row: ") + layout.id, &row, true);
	}

	/*
	 * And the keys really are wider than the viewport -- otherwise this
	 * would pass just as well on a row that had quietly lost its keys,
	 * which is the failure a fits-the-screen check invites.
	 */
	bssh_key_row row(&router);
	row.set_layout(QString());
	row.resize(BSSH_PHONE_DP, 64);
	row.show();
	QVERIFY(QTest::qWaitForWindowExposed(&row));

	int widest = 0;
	for (const QAbstractButton *button : row.findChildren<QAbstractButton *>()) {
		widest = qMax(widest, button->mapTo(&row, QPoint(button->width(), 0)).x());
	}
	QVERIFY2(widest > BSSH_PHONE_DP,
	          qPrintable(QStringLiteral("the keys end at %1px; nothing is being scrolled, "
	                                     "so this row has lost keys rather than gained a "
	                                     "scroll area").arg(widest)));
}

void layout_test::every_key_that_sends_something_repeats() {
	/*
	 * Holding a key must go on sending it (sec 6.2). Reported from the
	 * phone: Backspace deleted one character however long it was held,
	 * which makes correcting a mistyped path a tap per character, and
	 * the arrows unusable for moving along a line.
	 *
	 * Asserted as a PARTITION rather than as a list of keys, because a
	 * list goes stale the first time somebody adds a key and the test
	 * cannot fail about a key it does not name. The rule is:
	 *
	 *   a keycap repeats if and only if it is not checkable
	 *   nothing outside the key grid repeats at all
	 *
	 * "Checkable" is the modifiers and the Fn toggle -- the keys that
	 * change a state rather than send something -- so the two halves of
	 * that sentence are exactly "sends something" and "does not". A kind
	 * added to bssh_key_t without a repeat call lands in the wrong half
	 * and fails here, which is the whole point of writing it this way.
	 */
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	int repeating = 0;
	int stateful = 0;

	const bssh_key_row_catalog catalog;
	QVERIFY(!catalog.all().isEmpty());

	for (const bssh_key_row_layout_t &layout : catalog.all()) {
		bssh_key_row row(&router);
		row.set_layout(layout.id);

		for (const QAbstractButton *button : row.findChildren<QAbstractButton *>()) {
			const QString where =
			        QStringLiteral("key row %1, key \"%2\"").arg(layout.id, button->text());
			QVERIFY2(button->autoRepeat() == !button->isCheckable(), qPrintable(where));
			if (button->isCheckable()) ++stateful;
			else ++repeating;
		}
	}

	/*
	 * The built-in keyboard, every group of both styles: the keys that
	 * needed this most -- Backspace, Delete, the arrows -- live on the
	 * terminal group rather than on the letters, so a check of whatever
	 * group the board opens on would have missed them.
	 */
	const bssh_soft_keyboard_catalog::style_t styles[] = {
		bssh_soft_keyboard_catalog::STYLE_COMPACT,
		bssh_soft_keyboard_catalog::STYLE_FULL
	};

	for (bssh_soft_keyboard_catalog::style_t style : styles) {
		bssh_soft_keyboard keyboard(&router);
		keyboard.set_style(style);

		const int groups = bssh_soft_keyboard_catalog::instance().all(style).size();
		QVERIFY(groups > 0);

		for (int group = 0; group < groups; ++group) {
			keyboard.set_group(group);

			for (const bssh_key_cap *cap : keyboard.findChildren<bssh_key_cap *>()) {
				const QString where = QStringLiteral("keyboard group %1, key \"%2\"")
				        .arg(keyboard.group_id(), cap->text());
				QVERIFY2(cap->autoRepeat() == !cap->isCheckable(), qPrintable(where));
				if (cap->isCheckable()) ++stateful;
				else ++repeating;
			}

			/*
			 * And the side column stays put. Settings, hide, the page
			 * button and the script toggle are about the KEYBOARD, and a
			 * thumb resting on the page button must not walk through
			 * every group -- which is what a repeat there would do.
			 */
			for (const QAbstractButton *button :
			      keyboard.findChildren<QAbstractButton *>()) {
				if (qobject_cast<const bssh_key_cap *>(button)) continue;
				QVERIFY2(!button->autoRepeat(),
				          qPrintable(QStringLiteral("side button \"%1\" repeats")
				                             .arg(button->objectName())));
			}
		}
	}

	/*
	 * Both halves have to be non-empty, or this passes over a board that
	 * has lost its keys exactly as loudly as over one that works.
	 */
	QVERIFY2(repeating > 100, qPrintable(QStringLiteral("only %1 repeating keys").arg(repeating)));
	QVERIFY2(stateful > 0, "no modifier anywhere, so the exclusion was never tested");
}

void layout_test::the_desktop_opens_a_shell_and_the_phone_opens_its_config() {
	/*
	 * sec 2's reversed non-goal. The desktop opens a local shell, because
	 * a terminal that opens showing a connection dialog cannot be used as
	 * a terminal; touch opens its configuration, because an Android app
	 * sandbox has no userland worth driving a shell from -- which is the
	 * gap Termux exists to fill, and not one this app is filling.
	 *
	 * A capability question, asked the same way as the front door's: a
	 * convertible in tablet mode gets the phone's answer.
	 */
	QVERIFY2(bssh_main_window::opens_a_local_shell(false),
	          "a pointer device opens a shell");
	QVERIFY2(!bssh_main_window::opens_a_local_shell(true),
	          "a phone opens its configuration, not a shell it cannot use");

	/* And the front door each device builds is unchanged by that. */
	QCOMPARE(bssh_main_window::front_door_for(true), bssh_main_window::FRONT_DOOR_HOST_LIST);
	QCOMPARE(bssh_main_window::front_door_for(false),
	          bssh_main_window::FRONT_DOOR_SESSION_DIALOG);
}

void layout_test::the_config_is_reachable_from_a_terminal() {
	/*
	 * "The config menu should always be reachable." On touch that is Back
	 * (sec 8.3); with the desktop now starting IN a terminal, a pointer
	 * needs its own route or the window has none at all -- the same
	 * one-way door that made the tab bar unreachable on Android, arriving
	 * on the other platform by the other road.
	 *
	 * Checked on the assembled menu rather than on the handler, because
	 * an entry that is not on the menu is not a route however well the
	 * handler works.
	 */
	for (int touch = 0; touch <= 1; ++touch) {
		const forced_touch_t arrangement(touch);

		bssh_main_window window;
		QMenu *menu = window.build_terminal_menu(&window);
		QVERIFY(menu);

		bool has_route = false;
		for (const QAction *action : menu->actions()) {
			if (action->objectName() == QStringLiteral("terminal_front_door")) {
				has_route = true;
				QVERIFY2(action->isEnabled(), "the way to the config must not be disabled");
			}
		}
		QVERIFY2(has_route,
		          touch ? "touch has no menu route to the configuration"
		                 : "a pointer has no route to the configuration at all");

		/* Separated from the edit verbs, which are a different kind of
		 * thing -- the same shape sec 6.4 already uses for Select All. */
		const QList<QAction *> actions = menu->actions();
		QVERIFY2(actions.size() >= 2 && actions.at(actions.size() - 2)->isSeparator(),
		          "the way out must not sit flush against Copy and Paste");

		delete menu;
	}
}

void layout_test::the_host_list_offers_the_local_shell() {
	/*
	 * sec 2's local shell, reachable from the list rather than only at
	 * startup. A phone does not OPEN with one -- an Android sandbox has
	 * little for a shell to do -- but "little" is not "nothing", and
	 * being unable to reach it at all is a different thing from it not
	 * being the default.
	 */
	const forced_touch_t thumb(1);

	QTemporaryDir dir;
	QVERIFY(dir.isValid());
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));

	bssh_host_list list(&store);
	QListWidget *rows = list.findChild<QListWidget *>();
	QVERIFY(rows);

	/*
	 * Present with no hosts saved at all, which is the first screen a new
	 * user sees: one thing that works before anything is configured.
	 */
	QVERIFY2(rows->count() >= 1, "the list must carry the local shell before any host");
	QVERIFY2(rows->item(0)->text().contains(QStringLiteral("Local shell")),
	          qPrintable(QStringLiteral("first row is \"%1\"").arg(rows->item(0)->text())));

	/* Opening it asks for a LOCAL profile, not a lookup that would fail. */
	QSignalSpy opened(&list, &bssh_host_list::open_requested);
	rows->setCurrentRow(0);
	emit rows->itemClicked(rows->item(0));

	QCOMPARE(opened.count(), 1);
	const bssh_profile_t asked = opened.first().at(0).value<bssh_profile_t>();
	QVERIFY2(asked.local, "the row must open a local session");
	QVERIFY2(asked.is_valid(), "and a local profile with no host is still valid");

	/*
	 * And a saved host still opens as itself -- the sentinel row must not
	 * have made every row local.
	 */
	bssh_profile_t saved;
	saved.name = QStringLiteral("build box");
	saved.host = QStringLiteral("build.example");
	store.save(saved);
	list.reload();

	QVERIFY(rows->count() >= 2);
	opened.clear();
	rows->setCurrentRow(1);
	emit rows->itemClicked(rows->item(1));

	QCOMPARE(opened.count(), 1);
	const bssh_profile_t host = opened.first().at(0).value<bssh_profile_t>();
	QVERIFY2(!host.local, "a saved host must not open as a local shell");
	QCOMPARE(host.host, QStringLiteral("build.example"));
}

void layout_test::two_windows_are_two_views_onto_one_registry() {
	/*
	 * sec 8.4, in as many words: "a window is just another view onto the
	 * registry -- the registry stays single and process-wide... Do not
	 * let a second window imply a second registry."
	 *
	 * The window used to OWN one, which made that impossible to honour: a
	 * second window brought a second set of sessions, and a session
	 * opened in one would have been invisible to the other. This is the
	 * property every future window, and the drag of a tab between two of
	 * them, rests on.
	 *
	 * It is also the seam a client/server split would cut along. Windows
	 * are meant to behave as though they were separate processes talking
	 * to a session server: they ask the registry and never reach into one
	 * another. Today that call is a pointer dereference; later it can be
	 * a message without the callers changing.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window first;
	bssh_main_window second;

	QVERIFY2(first.registry() == second.registry(),
	          "two windows must share one registry, or a session opened in one "
	          "is invisible to the other");
	QCOMPARE(first.registry(), shared);

	/* A session opened through one window is visible through the other. */
	bssh_session *session = first.registry()->create(named(QStringLiteral("shared")));
	QCOMPARE(second.registry()->count(), 1);
	QCOMPARE(second.registry()->at(0), session);

	/*
	 * And closing the FIRST window does not take the session with it --
	 * which is what "the window is a view" has to mean if a second window
	 * is to survive the first going away.
	 */
	first.close();
	QCOMPARE(shared->count(), 1);
	QCOMPARE(shared->at(0), session);

	shared->close(session);
	QCOMPARE(shared->count(), 0);
}

void layout_test::two_windows_write_to_one_host_list() {
	/*
	 * The registry rule's other half, never applied. sec 8.4 says a window
	 * is a view and must not imply a second registry; the profile store is
	 * the same shape -- one set of files, several views -- and the window
	 * constructs its own with `new bssh_profile_store(QString(), this)`.
	 *
	 * The store loads hosts into a QList once, at construction, and
	 * write_profiles() calls QSettings::clear() and writes that list back
	 * whole. Two stores over one hosts.ini are therefore two full-file
	 * writers with no reader between them: whichever saves second rewrites
	 * the file from a list that predates the other's save, and the host
	 * added in the first window is gone from disk.
	 *
	 * The id collision is worse than the loss and is why this is not
	 * merely untidy. A new id is highest+1 over the stale list, so both
	 * windows mint the SAME one. Credentials are keyed
	 * "profile:<id>:password" (sec 11), so the surviving host inherits the
	 * lost one's stored password -- a credential reachable from a host its
	 * owner never gave it to.
	 *
	 * project.md already describes the symptom, and calls it fixed: "with
	 * two desktop windows open, adding or deleting a host in one left the
	 * other's list showing something that was no longer there, and that
	 * window's next save wrote from a list the store had moved past."
	 * That fix wired each window's VIEWS to their own window's store, and
	 * could not reach this: there is no shared store for the other
	 * window's changed() to arrive on.
	 */
	bssh_main_window first;
	bssh_main_window second;

	QVERIFY2(first.profile_store() == second.profile_store(),
	          "two windows must share one profile store, or each rewrites "
	          "hosts.ini from a list that predates the other's save");

	/*
	 * And the behaviour, because pointer equality on its own would pass
	 * against a store that shared the object and still wrote stale lists.
	 */
	const int alpha = first.profile_store()->save(named(QStringLiteral("alpha")));
	QVERIFY(alpha != 0);
	const int beta = second.profile_store()->save(named(QStringLiteral("beta")));
	QVERIFY(beta != 0);

	QVERIFY2(beta != alpha,
	          "two windows minting the same host id makes one host inherit the "
	          "other's stored credential");

	/*
	 * Read back from the FILE, which is where the loss happens -- a store
	 * constructed now loads it rather than answering out of either
	 * window's memory.
	 */
	{
		bssh_profile_store on_disk;
		QStringList names;
		for (const bssh_profile_t &saved : on_disk.profiles()) names << saved.name;

		QVERIFY2(names.contains(QStringLiteral("alpha")),
		          qPrintable(QStringLiteral("the first window's host is gone; the "
		                                     "file holds: ") + names.join(QStringLiteral(", "))));
		QVERIFY2(names.contains(QStringLiteral("beta")),
		          qPrintable(QStringLiteral("the second window's host is gone; the "
		                                     "file holds: ") + names.join(QStringLiteral(", "))));
	}

	first.profile_store()->remove(alpha);
	first.profile_store()->remove(beta);
}

void layout_test::a_moved_session_asks_the_window_it_is_in() {
	/*
	 * A password prompt belongs to the window the session is IN, not the
	 * one it was opened in.
	 *
	 * prepare_profile() connects eight signals -- failed, the three
	 * host-key prompts, needs_key_passphrase, needs_password,
	 * shell_exited and state_changed -- with the creating window as their
	 * context object, and adopt_session() does not remake them. So a
	 * session torn out into a second window still asks the FIRST one:
	 * the dialog opens on a window the user is not looking at, behind the
	 * one they are, and the session sits in AUTHENTICATING until they
	 * find it.
	 *
	 * And if that window has since closed there is no prompt at all. Qt
	 * drops a connection when its context object dies, so the session
	 * emits needs_password into nothing and waits for a credential
	 * nobody will ever be asked for.
	 *
	 * Asserted by catching the modal rather than by reading a member,
	 * because which window OWNS the dialog is the whole question and only
	 * the widget knows. The timer fires from inside exec()'s own event
	 * loop; without it this test would hang on the dialog, which is also
	 * what the bug does to the user.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});

	{
		bssh_main_window opened_in(named(QStringLiteral("moved")));
		opened_in.resize(900, 600);
		opened_in.show();
		QVERIFY(QTest::qWaitForWindowExposed(&opened_in));
		QCoreApplication::processEvents();
		QVERIFY(fake);

		QCOMPARE(opened_in.shown_sessions().size(), 1);
		bssh_session *session = opened_in.shown_sessions().first();

		bssh_main_window moved_to;
		moved_to.resize(900, 600);
		moved_to.show();
		QVERIFY(QTest::qWaitForWindowExposed(&moved_to));
		moved_to.adopt_session(session);
		QCoreApplication::processEvents();

		QVERIFY2(!opened_in.shown_sessions().contains(session),
		          "the move has to have happened before the prompt is asked about");
		QVERIFY(moved_to.shown_sessions().contains(session));

		/*
		 * Dismisses whatever modal appears and records which window it
		 * belongs to. Re-arms rather than firing once, since the dialog
		 * is not up until exec() has started.
		 */
		QWidget *asked_on = nullptr;
		bool saw_prompt = false;
		QTimer closer;
		closer.setInterval(0);
		QObject::connect(&closer, &QTimer::timeout, [&] {
			QWidget *modal = QApplication::activeModalWidget();
			if (!modal) return;
			saw_prompt = true;
			asked_on = modal->parentWidget() ? modal->parentWidget()->window() : nullptr;
			if (QDialog *dialog = qobject_cast<QDialog *>(modal)) dialog->reject();
			else modal->close();
		});
		closer.start();

		fake->simulate_password_required();

		closer.stop();
		QCoreApplication::processEvents();

		/* The control: a prompt has to appear at all, on some window. */
		QVERIFY2(saw_prompt, "no password prompt was raised anywhere");

		QVERIFY2(asked_on == &moved_to,
		          asked_on == &opened_in
		                  ? "the prompt opened on the window the session LEFT"
		                  : "the prompt opened on no window this test knows about");
	}

	bssh_ssh_session::set_factory(nullptr);
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::focus_follows_the_pointer_only_when_asked() {
	/*
	 * sec 8.3's [OPEN] question, answered: offered, and off by default.
	 * Off because it surprises anyone expecting click-to-focus, and the
	 * cost of that surprise is typing into the wrong shell -- which is
	 * the same hazard that section calls non-negotiable about the focus
	 * border.
	 *
	 * Hover is implemented by asking for focus, so it goes down the same
	 * path a click does: the border, the status line, what copy and paste
	 * act on, and sec 7.1's focus reporting all follow from focusInEvent
	 * and none of them learns a second way of being focused. That is why
	 * asserting on focused_session() is enough here.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = shared->create(named(QStringLiteral("a")));
	bssh_session *b = shared->create(named(QStringLiteral("b")));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(1200, 400);
	layout.set_intent({a, b});
	QCoreApplication::processEvents();
	QCOMPARE(container.pane_count(), 2);
	QCOMPARE(container.focused_session(), a);

	bssh_terminal_view *second = container.view_at(1);
	QVERIFY(second);

	/* Off: the pointer entering the other pane changes nothing. */
	QEnterEvent enter(QPointF(5, 5), QPointF(5, 5), second->mapToGlobal(QPoint(5, 5)));
	QApplication::sendEvent(second, &enter);
	QCoreApplication::processEvents();
	QCOMPARE(container.focused_session(), a);

	/* On: it takes the keyboard without a click. */
	second->set_focus_follows_mouse(true);
	QApplication::sendEvent(second, &enter);
	QCoreApplication::processEvents();
	QCOMPARE(container.focused_session(), b);

	/* And the border went with it, because hover used the same path. */
	QCOMPARE(container.view_at(1)->property("bssh_focused").toBool(), true);
	QCOMPARE(container.view_at(0)->property("bssh_focused").toBool(), false);

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::every_config_category_shows_its_own_page() {
	/*
	 * The tree and the stack are joined by a list of magic indices, and
	 * removing the SSH page shifted the two below it -- which a list of
	 * numbers cannot notice. Clicking every category and asserting the
	 * page that arrives is what makes that list checkable at all.
	 *
	 * And every category must SET something. Three of them used to be
	 * signposts: a paragraph and a button reading "Open Presets", because
	 * what they name lives in a preset rather than a session. A tree that
	 * borrows PuTTY's shape and then leads nowhere reads as unfinished,
	 * which is how it was reported. So a page with no control on it is a
	 * failure here.
	 */
	bssh_platform::force_touch_primary(false);

	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));
	bssh_session_dialog dialog(&store);

	QTreeWidget *tree = dialog.findChild<QTreeWidget *>();
	QStackedWidget *pages = dialog.findChild<QStackedWidget *>();
	QVERIFY(tree && pages);

	QList<QTreeWidgetItem *> items;
	for (int i = 0; i < tree->topLevelItemCount(); ++i) {
		items.append(tree->topLevelItem(i));
		for (int child = 0; child < tree->topLevelItem(i)->childCount(); ++child) {
			items.append(tree->topLevelItem(i)->child(child));
		}
	}
	QVERIFY2(items.size() >= 6, "the category tree lost its categories");

	QSet<QWidget *> seen;
	for (QTreeWidgetItem *item : items) {
		tree->setCurrentItem(item);
		QCoreApplication::processEvents();

		QWidget *page = pages->currentWidget();
		const QString name = item->text(0);

		QVERIFY2(page, qPrintable(name + QStringLiteral(" selects no page")));
		QVERIFY2(!seen.contains(page),
		          qPrintable(name + QStringLiteral(" shows a page another category "
		                                            "already showed -- the indices have "
		                                            "shifted")));
		seen.insert(page);

		/*
		 * Something to operate. Licences is the one page that is meant
		 * to be read rather than set, and it has a list, so asking for
		 * "a control or a list" covers every category honestly.
		 */
		const bool has_control = !page->findChildren<QAbstractButton *>().isEmpty()
		        || !page->findChildren<QLineEdit *>().isEmpty()
		        || !page->findChildren<QComboBox *>().isEmpty()
		        || !page->findChildren<QAbstractItemView *>().isEmpty();

		QVERIFY2(has_control,
		          qPrintable(name + QStringLiteral(" sets nothing -- a category that "
		                                            "only points somewhere else reads as "
		                                            "unfinished")));
	}
}

void layout_test::closing_the_settings_window_means_cancel() {
	/*
	 * The settings are a window of their own on desktop (sec 8.5), which
	 * gave them a control the page-shaped version never had: the frame's
	 * close button. A window that hid itself without telling anybody
	 * would leave the flag saying the front door is up, and -- with no
	 * session behind it -- an app running with no terminal and no way
	 * back to one, which is the exact complaint that made this a window.
	 *
	 * So the X means what Cancel means. Asserted through close(), which
	 * is what a window manager sends, rather than by pressing the button.
	 */
	bssh_platform::force_touch_primary(false);

	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	bssh_session *live = window.registry()->create(named(QStringLiteral("live")));
	window.adopt_session(live);

	window.show_front_door();
	QVERIFY(window.showing_front_door());

	bssh_session_dialog *settings = window.findChild<bssh_session_dialog *>();
	QVERIFY2(settings, "the desktop front door must be a window of its own");

	settings->close();
	QCoreApplication::processEvents();

	QVERIFY2(!window.showing_front_door(),
	          "closing the settings window must count as leaving it");
	QVERIFY2(window.registry()->index_of(live) >= 0,
	          "and must not take the session with it");

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::cancelling_the_settings_keeps_the_session() {
	/*
	 * Reported from use: open the settings from a working shell, press
	 * Cancel, and the shell is gone. The front door is a PAGE in the same
	 * stack as the terminal, and Cancel was wired straight to close() --
	 * correct for the startup screen it was written for, fatal for the
	 * same screen reached from a live session, because an SSH session is
	 * not something a reconnect brings back (sec 5.3).
	 */
	bssh_platform::force_touch_primary(false);

	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	bssh_session *live = window.registry()->create(named(QStringLiteral("live")));
	window.adopt_session(live);
	QVERIFY2(!window.showing_front_door(), "a session must be in front to start with");

	/*
	 * Shown, because the assertion that matters is that the WINDOW
	 * survives -- and a window that was never on screen cannot be seen to
	 * close. The first version of this test skipped that and asked only
	 * whether the session still existed, which the sabotage runner
	 * reported as caught by nothing: sessions are process-wide (sec 8.4),
	 * so closing the window leaves the registry entry exactly where it
	 * was. The test passed while the window it was written about died.
	 */
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	window.show_front_door();
	QVERIFY(window.showing_front_door());

	QPushButton *cancel =
	        window.findChild<QPushButton *>(QStringLiteral("front_door_cancel"));
	QVERIFY2(cancel, "the front door must have a Cancel a user can press");
	cancel->click();
	QCoreApplication::processEvents();

	QVERIFY2(window.isVisible(),
	          "Cancel must leave the settings, not close the window behind them");
	QVERIFY2(!window.showing_front_door(),
	          "Cancel must go back to the terminal, not out of the app");
	QVERIFY2(window.registry()->index_of(live) >= 0,
	          "the session must still exist after cancelling the settings");

	/*
	 * And the startup case still quits, which is what the close() was
	 * there for: with nothing behind it, declining the front door means
	 * "do not start" rather than "show me an empty terminal".
	 */
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window empty;
	QVERIFY(empty.showing_front_door());
	empty.show();
	QVERIFY(QTest::qWaitForWindowExposed(&empty));

	QPushButton *decline =
	        empty.findChild<QPushButton *>(QStringLiteral("front_door_cancel"));
	QVERIFY(decline);
	decline->click();
	QCoreApplication::processEvents();

	QVERIFY2(!empty.isVisible(),
	          "with no session behind it, Cancel still means do not start");

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_window_shows_only_its_own_tabs() {
	/*
	 * sec 8.4: a session is shown in exactly one place. The tab bar was
	 * built from the whole registry, which was indistinguishable from
	 * correct while there could only be one window -- and wrong the
	 * moment there could be two, since both would have shown every tab
	 * and closing one anywhere would have closed it everywhere.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window first;
	bssh_main_window second;

	/*
	 * TWO sessions per window, because one of them is on screen. A pane
	 * and a tab are different states (sec 8.3), so a window holding a
	 * single terminal has no tab at all -- and a test that counted one
	 * would be asserting the old model rather than this one.
	 */
	bssh_session *mine = first.registry()->create(named(QStringLiteral("mine")));
	first.adopt_session(mine);
	bssh_session *mine_shown = first.registry()->create(named(QStringLiteral("mine-shown")));
	first.adopt_session(mine_shown);

	QTabBar *first_tabs = first.findChild<QTabBar *>();
	QTabBar *second_tabs = second.findChild<QTabBar *>();
	QVERIFY(first_tabs && second_tabs);

	QCOMPARE(first_tabs->count(), 1);
	QVERIFY2(second_tabs->count() == 0,
	          "the other window must not show a session it was never given");

	/* Both still see them in the registry -- one process, one set. */
	QCOMPARE(second.registry()->count(), 2);

	/*
	 * And each tab must name the session THIS window holds, which the
	 * counts above do not establish: the tab count comes from the shown
	 * list while the label comes from a second lookup, so a rebuild that
	 * indexed the whole registry for the text would keep both counts
	 * right and put the wrong name on the tab.
	 *
	 * Not hypothetical. tool/sabotage.py made exactly that edit and this
	 * test stayed green -- one window and one session cannot tell the two
	 * lists apart, because they hold the same thing in the same order.
	 * Two windows each holding a DIFFERENT session is the smallest case
	 * that can, and the registry's order is what decides which name a
	 * confused lookup would produce.
	 */
	bssh_session *theirs = second.registry()->create(named(QStringLiteral("theirs")));
	second.adopt_session(theirs);
	bssh_session *theirs_shown =
	        second.registry()->create(named(QStringLiteral("theirs-shown")));
	second.adopt_session(theirs_shown);
	QCoreApplication::processEvents();

	QCOMPARE(first_tabs->count(), 1);
	QCOMPARE(second_tabs->count(), 1);
	QCOMPARE(first_tabs->tabText(0), QStringLiteral("mine"));
	QVERIFY2(second_tabs->tabText(0) == QStringLiteral("theirs"),
	          "a window's tab must name its own session, not the registry's first");

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_torn_out_tab_becomes_a_window_without_moving_the_session() {
	/*
	 * The whole point of tearing out to a WINDOW rather than a process
	 * (sec 8.4): the session does not move. It stays in the registry,
	 * connected, with its scrollback and whatever is running -- only the
	 * view showing it changes. A new process could not do that, because a
	 * live SSH session cannot cross one.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	bssh_session *stay = window.registry()->create(named(QStringLiteral("stay")));
	bssh_session *go = window.registry()->create(named(QStringLiteral("go")));
	window.adopt_session(stay);
	window.adopt_session(go);

	/* Something in the scrollback, so "the session did not move" means
	 * something a user would notice. */
	go->emulator()->feed(QByteArrayLiteral("torn-out-marker\r\n"));

	QCOMPARE(window.shown_sessions().size(), 2);

	/*
	 * Named rather than indexed. `go` was adopted second, so it is the
	 * pane and `stay` is the one tab -- a tab INDEX would tear out the
	 * wrong terminal, and silently, since either one satisfies every
	 * assertion below except the scrollback.
	 */
	bssh_main_window *torn = window.tear_out(go);
	QVERIFY2(torn, "a session that is not the window's last must tear out");
	torn->setAttribute(Qt::WA_DeleteOnClose, false);

	/* The session is the SAME object, still registered, still carrying
	 * what was on its screen. */
	QCOMPARE(shared->count(), 2);
	QCOMPARE(shared->index_of(go) >= 0, true);
	QVERIFY(go->emulator()->screen().to_text().contains(QStringLiteral("torn-out-marker")));

	/* And it is shown in the new window and no longer in the old one. */
	QCOMPARE(window.shown_sessions(), QList<bssh_session *>{ stay });
	QCOMPARE(torn->shown_sessions(), QList<bssh_session *>{ go });

	/*
	 * A window will not empty itself: tearing out the last tab would
	 * leave a window with nothing in it, so the gesture declines.
	 */
	QVERIFY2(!torn->tear_out_tab(0), "the last tab must not tear out");

	delete torn;
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_dropped_session_leaves_the_window_it_came_from() {
	/*
	 * sec 8.4: a session is shown in exactly one place. Dropping it onto
	 * another window is a MOVE, and the window losing it has to be told.
	 *
	 * This is the defect the drop exposed. adopt_session() emitted
	 * changed(), which prunes sessions no longer in the REGISTRY -- and a
	 * moved session is still in it, so the old window went on showing it
	 * and the same terminal appeared in two windows at once. Tearing out
	 * hid the bug by removing the session itself before adopting; a drop
	 * has no such step.
	 *
	 * The announcement goes through the registry rather than between
	 * windows, because windows are meant to behave as though they were
	 * separate processes.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window from;
	bssh_main_window onto;

	bssh_session *moving = from.registry()->create(named(QStringLiteral("moving")));
	bssh_session *staying = from.registry()->create(named(QStringLiteral("staying")));
	from.adopt_session(moving);
	from.adopt_session(staying);

	QTabBar *from_tabs = from.findChild<QTabBar *>();
	QTabBar *onto_tabs = onto.findChild<QTabBar *>();
	QVERIFY(from_tabs && onto_tabs);
	QCOMPARE(from.shown_sessions().size(), 2);
	QCOMPARE(onto.shown_sessions().size(), 0);
	QCOMPARE(from_tabs->count(), 1);
	QCOMPARE(onto_tabs->count(), 0);

	onto.drop_session(moving, false);

	QCOMPARE(onto.shown_sessions(), QList<bssh_session *>{ moving });
	QVERIFY2(from.shown_sessions() == QList<bssh_session *>{ staying },
	          "the window it came from must let go, or the same terminal is "
	          "shown in two windows at once");

	/* The session itself is untouched: a move is not a close. */
	QCOMPARE(shared->count(), 2);
	QVERIFY(shared->index_of(moving) >= 0);

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_drop_on_the_panes_tiles_where_there_is_room() {
	/*
	 * Where the drop LANDS is what it means: the tab bar makes a tab, the
	 * panes make a tile. Expressed as a command rather than a synthesised
	 * drag, so the model can be driven without a window manager.
	 *
	 * And a tile is declined where there is no room for another pane
	 * (sec 8.2), falling back to a tab -- because a drop that appears to
	 * do nothing is worse than one that does the possible thing.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	window.resize(1400, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session *first = window.registry()->create(named(QStringLiteral("first")));
	window.adopt_session(first);

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	QCOMPARE(panes->pane_count(), 1);

	bssh_session *dropped = window.registry()->create(named(QStringLiteral("dropped")));
	window.drop_session(dropped, true);
	QCoreApplication::processEvents();

	QVERIFY2(panes->pane_count() == 2,
	          qPrintable(QStringLiteral("a drop on the panes of a wide window must tile; "
	                                     "got %1 pane(s)").arg(panes->pane_count())));

	/*
	 * Narrow: one pane is all that fits (sec 8.2), so the same drop is a
	 * tab instead of nothing at all.
	 */
	bssh_main_window narrow;
	narrow.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	narrow.show();
	QVERIFY(QTest::qWaitForWindowExposed(&narrow));

	bssh_session *only = narrow.registry()->create(named(QStringLiteral("only")));
	narrow.adopt_session(only);

	bssh_session *squeezed = narrow.registry()->create(named(QStringLiteral("squeezed")));
	narrow.drop_session(squeezed, true);
	QCoreApplication::processEvents();

	bssh_pane_container *narrow_panes = narrow.findChild<bssh_pane_container *>();
	QVERIFY(narrow_panes);
	QCOMPARE(narrow_panes->pane_count(), 1);

	/*
	 * It ARRIVES -- that is the assertion, and it is now made on the
	 * window's own list rather than on the tab bar. One pane fits, so
	 * the dropped session is the pane and the one it displaced is the
	 * tab; counting tabs would say 1 and mean nothing, because a pane
	 * and a tab are different states (sec 8.3).
	 */
	QCOMPARE(narrow.shown_sessions().size(), 2);
	QVERIFY(narrow.shown_sessions().contains(squeezed));

	QTabBar *narrow_tabs = narrow.findChild<QTabBar *>();
	QVERIFY(narrow_tabs);
	QVERIFY2(narrow_tabs->count() == 1,
	          "the terminal the drop displaced must still be reachable");

	while (shared->count() > 0) shared->close(shared->at(0));
}

/*
 * Builds the payload a dragged tab carries, for the two tests below.
 *
 * QDrag::exec() needs a real pointer grab and a nested loop the offscreen
 * platform cannot provide, so what is driven here is the half that decides
 * anything: the window's own dragEnter/drop handling. The gesture that
 * starts it is one line of eventFilter and is left to the device.
 */
static QMimeData *session_payload(int registry_index) {
	QMimeData *mime = new QMimeData;
	mime->setData(bssh_main_window::session_mime_type(),
	               QByteArray::number(registry_index));
	return mime;
}

void layout_test::a_dragged_tab_lands_where_it_is_dropped() {
	/*
	 * Where it lands is what it means (sec 8.3): on the tab bar it is a
	 * tab, on the panes it is a tile. Same payload, same window, two
	 * positions -- so the position is provably the only thing deciding.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	window.resize(1400, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	/*
	 * Two, because the bar only appears with a second session (sec 8.3)
	 * and a hidden bar is not something a drop can aim at. That is the
	 * real shape of the gesture: you drop onto a bar you can see.
	 */
	bssh_session *sitting = window.registry()->create(named(QStringLiteral("sitting")));
	bssh_session *beside = window.registry()->create(named(QStringLiteral("beside")));
	window.adopt_session(sitting);
	window.adopt_session(beside);

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QTabBar *tabs = window.findChild<QTabBar *>();
	QVERIFY(panes && tabs);
	QVERIFY2(tabs->isVisible(), "the bar must be showing or there is nothing to drop on");
	QCOMPARE(panes->pane_count(), 1);

	/* A drop offering a session this window can resolve is accepted. */
	bssh_session *carried = window.registry()->create(named(QStringLiteral("carried")));
	const int carried_index = shared->index_of(carried);
	QVERIFY(carried_index >= 0);

	QScopedPointer<QMimeData> onto_bar(session_payload(carried_index));
	QDragEnterEvent enter(tabs->mapTo(&window, tabs->rect().center()), Qt::MoveAction, onto_bar.data(),
	                       Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(&window, &enter);
	QVERIFY2(enter.isAccepted(),
	          "a window that will not accept the payload can never be dropped on");

	QDropEvent on_bar(QPointF(tabs->mapTo(&window, tabs->rect().center())), Qt::MoveAction, onto_bar.data(),
	                   Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(&window, &on_bar);
	QCoreApplication::processEvents();

	QVERIFY2(panes->pane_count() == 1,
	          "a drop on the tab bar makes a tab, so the panes must not split");
	QCOMPARE(window.shown_sessions().size(), 3);
	QCOMPARE(tabs->count(), 2);

	/*
	 * A different session, low in the window where the panes are.
	 *
	 * Different because a session cannot tile beside itself: the one just
	 * dropped on the bar is now the only pane, and split_with() declines a
	 * session the intent already holds. Dropping it on the panes is a
	 * no-op by design, not a defect -- the payload has to name a session
	 * that is somewhere else for "beside" to mean anything.
	 */
	bssh_session *tiled = window.registry()->create(named(QStringLiteral("tiled")));
	const int tiled_index = shared->index_of(tiled);
	QVERIFY(tiled_index >= 0);

	QScopedPointer<QMimeData> onto_panes(session_payload(tiled_index));
	/*
	 * Entered before it is dropped, as a real drag does. QApplication
	 * routes drag events through the current drop target, so a Drop
	 * sent on its own is discarded before it reaches dropEvent() --
	 * which reads exactly like a drop the window declined.
	 */
	const QPoint low(window.width() / 2, window.height() - 20);
	QDragEnterEvent enter_panes(low, Qt::MoveAction, onto_panes.data(),
	                             Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(&window, &enter_panes);
	QVERIFY(enter_panes.isAccepted());

	QDropEvent on_panes(QPointF(low), Qt::MoveAction, onto_panes.data(),
	                     Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(&window, &on_panes);
	QCoreApplication::processEvents();

	QVERIFY2(panes->pane_count() == 2,
	          qPrintable(QStringLiteral("a drop on the panes must tile; got %1 pane(s)")
	                             .arg(panes->pane_count())));

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_window_watches_for_a_screen_change() {
	/*
	 * sec 9.5 wants the grid re-laid-out when a window is dragged between
	 * monitors of different scaling, rather than the old monitor's cell
	 * stretched to fit. The re-layout itself needs two real screens and
	 * cannot be driven under the offscreen platform, which has one -- so
	 * what is held here is the half that was missing outright: nothing in
	 * src/ referred to QScreen at all, so the window was not listening
	 * and no amount of dragging could have reached the code.
	 *
	 * Asserted through disconnect()'s return value, the same way sec 8.3
	 * pins the tab bar's context-menu connection, because a connection is
	 * not something a widget can be asked about directly.
	 */
	bssh_main_window window;
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	QVERIFY2(window.windowHandle(),
	          "no window handle means screenChanged could never have been connected");

	const bool listening = QObject::disconnect(window.windowHandle(),
	                                            &QWindow::screenChanged, &window, nullptr);
	QVERIFY2(listening,
	          "a window that does not watch for a screen change cannot re-measure on one");

	/*
	 * And a re-measure on the same screen changes nothing. Worth pinning:
	 * this runs on an event the window does not control, so one that
	 * resized every session each time it fired would be sending the
	 * remote a window-change for no reason.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window sized;
	sized.resize(1000, 700);
	sized.show();
	QVERIFY(QTest::qWaitForWindowExposed(&sized));

	bssh_session *session = sized.registry()->create(named(QStringLiteral("steady")));
	sized.adopt_session(session);

	bssh_pane_container *panes = sized.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	QVERIFY(!panes->views().isEmpty());

	/*
	 * A turn of the event loop, because a view added to the splitter has
	 * no width until the layout runs -- and a view with no width no
	 * longer invents a grid to report (see recompute_grid). It used to
	 * answer "1 column" here and this read that as a real answer.
	 */
	QCoreApplication::processEvents();

	const int cols = panes->views().first()->cols();
	const int rows = panes->views().first()->rows();
	QVERIFY2(cols > 0 && rows > 0, "a shown pane must have measured itself");

	panes->remeasure_panes();

	QCOMPARE(panes->views().first()->cols(), cols);
	QCOMPARE(panes->views().first()->rows(), rows);

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_window_can_be_sized_in_cells() {
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	/*
	 * main()'s own order, which is the half that could go wrong on its
	 * own: the session is opened and the window sized BEFORE it is ever
	 * shown, so the measurement has to work against a widget tree that
	 * has never been laid out.
	 */
	bssh_main_window window;
	window.resize(900, 600);

	bssh_session *session = window.registry()->create(named(QStringLiteral("sized")));
	window.adopt_session(session);

	window.resize(window.size_for_grid(100, 30));
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	QVERIFY(!panes->views().isEmpty());

	/*
	 * Read back off the GRID rather than off the window. A window that is
	 * a hundred cells wide plus a border's worth of chrome is a window
	 * whose terminal is ninety-nine columns, and only the view can say
	 * which happened -- which is what this caught the first time.
	 */
	QCOMPARE(panes->views().first()->cols(), 100);
	QCOMPARE(panes->views().first()->rows(), 30);

	/* And again on a window that IS laid out, which takes the other path. */
	window.resize(window.size_for_grid(80, 24));
	QCoreApplication::processEvents();
	panes->remeasure_panes();

	QCOMPARE(panes->views().first()->cols(), 80);
	QCOMPARE(panes->views().first()->rows(), 24);

	/*
	 * And a SMALL one, which is the case that did not work. The pages
	 * share a QStackedWidget and a stack asks every page how small it may
	 * be, so the front door's category tree and form set a floor of
	 * 520x460 -- about 57 by 21 -- on a window showing a terminal.
	 * Anything under that came out bigger than it was asked for, and a
	 * desktop user could not drag a terminal window small either.
	 *
	 * 40x12 is well under the old floor in both directions, so a window
	 * that has gone back to obeying the hidden pages fails here rather
	 * than passing by a pixel.
	 */
	window.resize(window.size_for_grid(40, 12));
	QCoreApplication::processEvents();
	panes->remeasure_panes();

	QCOMPARE(panes->views().first()->cols(), 40);
	QCOMPARE(panes->views().first()->rows(), 12);

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_drop_naming_no_session_is_declined() {
	/*
	 * The payload is a registry index, not a pointer, so it can go stale
	 * -- a session closed while the pointer was still down. An index that
	 * names nothing must be refused rather than land on whichever session
	 * moved into that slot, which is the difference between a drop that
	 * does nothing and one that moves the wrong terminal.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	window.resize(1400, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session *only = window.registry()->create(named(QStringLiteral("only")));
	window.adopt_session(only);

	/*
	 * A second one so the bar exists to aim at: `only` is the pane and
	 * this is the tab. The drop below is aimed at the bar's rectangle,
	 * and a hidden bar has none.
	 */
	window.adopt_session(window.registry()->create(named(QStringLiteral("tabbed"))));

	QTabBar *tabs = window.findChild<QTabBar *>();
	QVERIFY(tabs);
	QCOMPARE(tabs->count(), 1);

	QScopedPointer<QMimeData> stale(session_payload(shared->count() + 7));
	QDragEnterEvent enter(tabs->mapTo(&window, tabs->rect().center()), Qt::MoveAction, stale.data(),
	                       Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(&window, &enter);
	QVERIFY2(!enter.isAccepted(),
	          "an index naming no session must not even be accepted for drop");

	QDropEvent drop(QPointF(tabs->mapTo(&window, tabs->rect().center())), Qt::MoveAction, stale.data(),
	                 Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(&window, &drop);
	QCoreApplication::processEvents();

	QCOMPARE(window.shown_sessions().size(), 2);
	QCOMPARE(tabs->count(), 1);

	/* And a payload of some other kind entirely is not ours to take. */
	QScopedPointer<QMimeData> foreign(new QMimeData);
	foreign->setText(QStringLiteral("/etc/passwd"));
	QDragEnterEvent text(tabs->mapTo(&window, tabs->rect().center()), Qt::MoveAction, foreign.data(),
	                      Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(&window, &text);
	QVERIFY2(!text.isAccepted(),
	          "dropping text on the terminal is a separate feature and is not built");

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_live_tab_is_not_offered_connect() {
	/*
	 * Connect exists for the tab a restore leaves disconnected (sec 8.4),
	 * and it must go away once the session is live. This is the half that
	 * would rot silently: the entry appearing when it should is visible
	 * the first time anybody looks, while an entry that lingers on a
	 * connected session is a verb offering to do something already done.
	 */
	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});

	bssh_main_window window;
	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);

	bssh_session *session = registry->create(named(QStringLiteral("live")));
	window.adopt_session(session);

	/* Adopted after it, so `session` is the tab and this is the pane. */
	window.adopt_session(registry->create(named(QStringLiteral("on-screen"))));
	QVERIFY(!session->is_live());

	QMenu *before = window.build_tab_menu(0, &window);
	QVERIFY(before);
	QCOMPARE(before->actions().first()->text(), QStringLiteral("Co&nnect"));
	delete before;

	session->connect_to_host();
	QVERIFY2(session->is_live(), "the fake transport should bring the session up");

	QMenu *after = window.build_tab_menu(0, &window);
	QVERIFY(after);
	for (const QAction *action : after->actions()) {
		QVERIFY2(action->text() != QStringLiteral("Co&nnect"),
		          "a live session must not be offered Connect");
	}
	delete after;

	bssh_ssh_session::set_factory(nullptr);
	while (registry->count() > 0) registry->close(registry->at(0));
}

void layout_test::split_beside_current_makes_a_second_pane() {
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session *only = window.registry()->create(named(QStringLiteral("only")));
	window.adopt_session(only);
	QCoreApplication::processEvents();
	QCOMPARE(window.panes()->views().size(), 1);

	/*
	 * Both Split menu items called split_with(m_current), and m_current is
	 * by construction always in the layout intent -- so split_with's
	 * "already showing" guard was unconditionally taken and the action was
	 * enabled, clickable and completely silent.
	 *
	 * This drives the same entry point the menus do rather than
	 * split_with() directly: the argument they pass IS the defect, so a
	 * test calling split_with() with something else would have passed
	 * throughout.
	 */
	window.split_beside_current();
	QCoreApplication::processEvents();

	QCOMPARE(window.panes()->views().size(), 2);
	QCOMPARE(window.registry()->count(), 2);

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_tab_dropped_on_a_pane_reaches_the_window() {
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window onto;
	onto.resize(900, 600);
	onto.show();
	QVERIFY(QTest::qWaitForWindowExposed(&onto));

	bssh_session *resident = onto.registry()->create(named(QStringLiteral("resident")));
	onto.adopt_session(resident);
	bssh_session *moving = onto.registry()->create(named(QStringLiteral("moving")));
	QCoreApplication::processEvents();

	QVERIFY(onto.panes());
	bssh_terminal_view *pane = onto.panes()->views().value(0);
	QVERIFY(pane);

	/*
	 * Delivered to the PANE, which is what Qt does: it picks the target
	 * before delivery, walking up to the first widget with acceptDrops(),
	 * and the terminal view has it. The view used to refuse a session
	 * payload believing the window would get it instead -- Qt does not
	 * re-target to an ancestor, so nobody got it, the drag showed the
	 * no-drop cursor, and the tab tore out into a new window every time.
	 *
	 * The existing drop tests send their events straight at the WINDOW,
	 * which is why they passed throughout: they exercise the model and
	 * never the route.
	 */
	QMimeData *mime = new QMimeData;
	mime->setData(bssh_session_mime_type(),
	               QByteArray::number(onto.registry()->index_of(moving)));

	const QPoint at(pane->width() / 2, pane->height() / 2);
	QDragEnterEvent enter(at, Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);

	/*
	 * Ignored FIRST, because a QDragMoveEvent arrives accepted by
	 * default -- so asserting isAccepted() on it straight out of the
	 * constructor passes whether the handler ran or not. The first
	 * version of this did exactly that and the sabotage runner reported
	 * the refusal as MISSED.
	 */
	enter.ignore();
	QCoreApplication::sendEvent(pane, &enter);
	QVERIFY2(enter.isAccepted(),
	          "the pane must accept a session drag, or Qt delivers the drop to nobody");

	QDropEvent drop(QPointF(at), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
	drop.ignore();
	QCoreApplication::sendEvent(pane, &drop);
	QVERIFY(drop.isAccepted());
	QCoreApplication::processEvents();

	QCOMPARE(onto.panes()->views().size(), 2);

	delete mime;
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_reorder_along_the_strip_is_not_a_tear_out() {
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	/*
	 * THREE sessions to get two tabs: the last one adopted is the pane,
	 * and a pane is not in the bar (sec 8.3). Two tabs is the smallest
	 * bar a reorder can happen in.
	 */
	window.adopt_session(window.registry()->create(named(QStringLiteral("one"))));
	window.adopt_session(window.registry()->create(named(QStringLiteral("two"))));
	window.adopt_session(window.registry()->create(named(QStringLiteral("shown"))));
	QCoreApplication::processEvents();

	QTabBar *bar = window.findChild<QTabBar *>();
	QVERIFY(bar);
	QCOMPARE(bar->count(), 2);

	/*
	 * Just past the last tab, horizontally, which is where a reorder to
	 * the final position puts the pointer.
	 *
	 * The tear-out test measured distance past `m_tabs->width()`, and the
	 * row lays the bar out at exactly its sizeHint -- so the bar IS the
	 * tab strip and that measured distance past the LAST TAB, the very
	 * thing its comment said it avoided. Forty pixels of slack, then the
	 * reorder silently became a window operation.
	 */
	const QPoint along(bar->width() + 60, bar->height() / 2);
	QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, bar->height() / 2),
	                   bar->mapToGlobal(QPoint(10, bar->height() / 2)),
	                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(bar, &press);

	QMouseEvent move(QEvent::MouseMove, QPointF(along), bar->mapToGlobal(along),
	                  Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(bar, &move);
	QCoreApplication::processEvents();

	/* Still one window's worth of terminals: dragging along the strip did
	 * not detach anything. */
	QCOMPARE(window.shown_sessions().size(), 3);
	QCOMPARE(bar->count(), 2);

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::closing_the_visible_tab_leaves_no_view_on_a_dead_session() {
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session *first = window.registry()->create(named(QStringLiteral("one")));
	window.adopt_session(first);
	bssh_session *second = window.registry()->create(named(QStringLiteral("two")));
	window.adopt_session(second);
	QCoreApplication::processEvents();

	/*
	 * The tab the user is looking at, which is where the close button
	 * lives and is therefore the ordinary case.
	 *
	 * detach_session() cleared m_current and THEN called
	 * m_layout->forget(m_current), so it always forgot nullptr and the
	 * dying session stayed in the layout intent. rebuild() then built a
	 * fresh view for it and attached the screen, scrollback, emulator and
	 * router of a session about to be deleted.
	 */
	window.registry()->close(second);

	/*
	 * The delete is deferred, so the crash is one turn of the event loop
	 * away -- a test that stopped at close() would have passed against
	 * the bug. This is what actually frees the emulator.
	 */
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCoreApplication::processEvents();

	/*
	 * And then paint, because the blink timer and paintEvent are what
	 * read the dangling screen. grab() forces a real paint now rather
	 * than waiting for a timer this test does not run long enough to see.
	 */
	window.grab();
	QCoreApplication::processEvents();

	QCOMPARE(window.registry()->count(), 1);
	QCOMPARE(window.registry()->at(0), first);

	/*
	 * The assertion that can actually fail: exactly one view, for the
	 * session that is left.
	 *
	 * "It did not crash" cannot express this and does not discriminate --
	 * reading freed memory usually returns stale bytes rather than
	 * faulting, so the first version of this test passed against the bug
	 * it was written for, and the sabotage runner said so. With the
	 * defect the layout intent still holds the dead session, so rebuild()
	 * builds a SECOND view and attaches a freed emulator to it. Counting
	 * views asks that directly.
	 */
	QVERIFY(window.panes());
	QCOMPARE(window.panes()->views().size(), 1);

	/*
	 * And closing the LAST one leaves none. This is where the ordering
	 * bug shows through: with forget() given a nulled m_current, the
	 * dying session stays in the intent and rebuild() builds a view for
	 * it, so the window ends up showing a pane for a session that is
	 * being deleted.
	 */
	window.registry()->close(first);
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCoreApplication::processEvents();

	QCOMPARE(window.panes()->views().size(), 0);
}

void layout_test::a_session_adopted_from_another_window_is_wired_to_this_one() {
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window first;
	first.resize(900, 600);
	first.show();
	QVERIFY(QTest::qWaitForWindowExposed(&first));

	bssh_session *session = first.registry()->create(named(QStringLiteral("moved")));
	first.adopt_session(session);
	QCoreApplication::processEvents();

	/*
	 * A second window takes it, which is what tearing a tab out does
	 * (sec 8.4). Only the window a session was CREATED in used to wire
	 * anything to it, because the wiring hung off session_added and that
	 * fires once -- so every per-session signal in a torn-out tab still
	 * pointed at the window it came from.
	 */
	bssh_main_window second;
	second.resize(900, 600);
	second.show();
	QVERIFY(QTest::qWaitForWindowExposed(&second));

	second.adopt_session(session);
	QCoreApplication::processEvents();

	QJsonObject settings;
	settings.insert(QStringLiteral("allow_clipboard_write"), true);
	session->set_settings(settings);

	QGuiApplication::clipboard()->setText(QStringLiteral("before"));

	/*
	 * OSC 52 is the readable half of the damage. The first window still
	 * holds its own connection but guards on showing the session, and it
	 * no longer shows it -- so if the second window was never wired, the
	 * remote's clipboard request reaches nobody at all and the write
	 * simply does not happen. Silently: no error, no status line.
	 */
	session->emulator()->feed(QByteArrayLiteral("\x1b]52;c;aGVsbG8=\x1b\\"));
	QCoreApplication::processEvents();

	QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("hello"));

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_shortcut_acts_only_on_the_window_it_was_typed_in() {
	/*
	 * A chord belongs to the pane it was typed into (sec 8.3, sec 8.4).
	 *
	 * wire_session() hangs off the registry's session_added, and the
	 * registry is process-wide -- so EVERY window wires EVERY session.
	 * That is deliberate and right for the handlers beside it: each one
	 * guards on showing the session, or on it being the current one, and a
	 * second listener that returns immediately costs nothing.
	 *
	 * shortcut_pressed was the one with no guard, and wire_session's own
	 * comment names the hazard without closing it -- handle_shortcut acts
	 * on m_terminal_view, m_current and m_panes, all of them THIS window's,
	 * and has no idea which window asked. With two windows open a paste
	 * typed in one was therefore also pasted into whatever the other
	 * happened to be showing. On a terminal that is not cosmetic: the text
	 * lands on a host the user was not aiming at, and a pasted line ending
	 * in a newline runs there.
	 *
	 * Two windows and no tear-out, because that is the smaller
	 * arrangement: the fault needs only a second window to exist, and a
	 * test that reached it by dragging a tab would have blamed the drag.
	 *
	 * Read off the TRANSPORT rather than the emulator, for the reason
	 * recorded further up this file: take_output() is drained on every
	 * output_ready, so asking the emulator returns empty and the test
	 * passes against any answer whatsoever, including none.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	QList<bssh_fake_ssh_session *> made;
	bssh_ssh_session::set_factory([&made](QObject *parent) -> bssh_ssh_session * {
		bssh_fake_ssh_session *fake = new bssh_fake_ssh_session(parent);
		made.append(fake);
		return fake;
	});

	{
		bssh_main_window elsewhere;
		elsewhere.resize(900, 600);
		elsewhere.show();
		QVERIFY(QTest::qWaitForWindowExposed(&elsewhere));

		bssh_session *bystander =
		    elsewhere.registry()->create(named(QStringLiteral("bystander")));
		elsewhere.adopt_session(bystander);
		bystander->connect_to_host();
		QCoreApplication::processEvents();

		bssh_main_window typed_in;
		typed_in.resize(900, 600);
		typed_in.show();
		QVERIFY(QTest::qWaitForWindowExposed(&typed_in));

		bssh_session *aimed_at =
		    typed_in.registry()->create(named(QStringLiteral("aimed")));
		typed_in.adopt_session(aimed_at);
		aimed_at->connect_to_host();
		QCoreApplication::processEvents();

		/*
		 * In creation order, and asserted rather than assumed: a window
		 * that opened a session of its own would shift the pairing and
		 * every assertion below would be about the wrong link.
		 */
		QCOMPARE(made.size(), 2);
		bssh_fake_ssh_session *bystander_link = made.at(0);
		bssh_fake_ssh_session *aimed_link = made.at(1);

		bystander_link->clear_written();
		aimed_link->clear_written();

		QGuiApplication::clipboard()->setText(QStringLiteral("whoami"));

		/*
		 * Through the router's own key handling rather than by emitting
		 * the signal: the chord is half of what is under test, and a test
		 * that raises the signal by hand says nothing about whether
		 * Ctrl+Shift+V still produces it. Kept by the app, so it must not
		 * also reach the remote as bytes.
		 */
		QKeyEvent paste(QEvent::KeyPress, Qt::Key_V,
		                 Qt::ControlModifier | Qt::ShiftModifier, QString());
		QVERIFY(!aimed_at->input_router()->handle_key(&paste));
		QCoreApplication::processEvents();

		/* The control. Without it a window that pasted NOWHERE would pass. */
		QCOMPARE(aimed_link->written(), QByteArrayLiteral("whoami"));

		QVERIFY2(bystander_link->written().isEmpty(),
		          "a paste typed in one window must not reach another window's session");
	}

	bssh_ssh_session::set_factory(nullptr);
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_failure_is_reported_against_the_session_it_came_from() {
	/*
	 * The status line composes ONE sentence out of two sources: the
	 * current session's target and state, and m_error -- which any session
	 * this window opened may set. So a failure belonging to one session is
	 * printed as though it described another:
	 *
	 *     prod.example connected -- connection reset
	 *
	 * a true clause and a false sentence, whose named host is the one that
	 * is fine. Worse than silence, because the reader has no way to tell
	 * it is being told about somewhere else.
	 *
	 * Both siblings in the same function guard: shell_exited returns
	 * unless the session is current, state_changed acts only when it is.
	 * on_failed guarded on nothing -- and being a slot that takes only the
	 * reason, it COULD not have guarded without being handed the session,
	 * which is how it came to be the one that did not.
	 *
	 * This does not silence a background tab. sec 8.3's per-tab dot is
	 * where a session nobody is looking at reports itself, and
	 * bssh_tab_status::state_for() already answers DISCONNECTED for a
	 * dropped one -- so the guard moves the message to the tab it belongs
	 * to rather than dropping it.
	 *
	 * Driven through the LAUNCH profile, because that is the only road a
	 * test can take to the connections that matter: they are made in
	 * prepare_profile(), which is private and reached from open_profile()
	 * -- protected -- and from the constructor. A session put in with
	 * registry()->create() and adopt_session() never gets them, which is
	 * why none of these eight signals had a test until now.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	QList<bssh_fake_ssh_session *> made;
	bssh_ssh_session::set_factory([&made](QObject *parent) -> bssh_ssh_session * {
		bssh_fake_ssh_session *fake = new bssh_fake_ssh_session(parent);
		made.append(fake);
		return fake;
	});

	/*
	 * The control, first and in its own window: a failure on the session
	 * the line is ABOUT must still be printed. Without it, a guard that
	 * threw every error away would pass the assertion below.
	 */
	{
		made.clear();
		bssh_main_window solo(named(QStringLiteral("solo")));
		solo.resize(900, 600);
		solo.show();
		QVERIFY(QTest::qWaitForWindowExposed(&solo));
		QCoreApplication::processEvents();

		QCOMPARE(made.size(), 1);
		made.at(0)->simulate_connection_lost();
		QCoreApplication::processEvents();

		/*
		 * Not asserted against the failure's own words. sec 5.3's
		 * reconnection machine schedules a retry in the same turn of the
		 * loop and REPLACES the text with "reconnecting in 2s (attempt
		 * 1)", which is the more useful thing to say and is correct. What
		 * the control has to establish is only that a failure on the
		 * current session reaches this line at all -- so it asks for the
		 * clause, not for its wording.
		 */
		QVERIFY2(solo.status_text().contains(QStringLiteral("solo.example")),
		          qPrintable(QStringLiteral("the line must be about solo, and says: ")
		                      + solo.status_text()));
		QVERIFY2(solo.status_text().contains(QStringLiteral(" -- ")),
		          qPrintable(QStringLiteral("a failure on the current session must reach "
		                                     "the line, which says: ")
		                      + solo.status_text()));
	}

	while (shared->count() > 0) shared->close(shared->at(0));

	{
		made.clear();
		bssh_main_window window(named(QStringLiteral("staging")));
		window.resize(900, 600);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		QCoreApplication::processEvents();

		QCOMPARE(made.size(), 1);
		bssh_fake_ssh_session *staging_link = made.at(0);

		/*
		 * A second host, which becomes the one on screen. staging is a
		 * background tab from here on -- still this window's, still
		 * connected to on_failed, and no longer what the line is about.
		 */
		bssh_session *prod = window.registry()->create(named(QStringLiteral("prod")));
		window.adopt_session(prod);
		QCoreApplication::processEvents();

		QVERIFY2(window.status_text().contains(QStringLiteral("prod.example")),
		          "the line has to be about prod before it can be wrong about it");

		staging_link->simulate_connection_lost();
		QCoreApplication::processEvents();

		QVERIFY2(!window.status_text().contains(QStringLiteral("connection reset")),
		          qPrintable(QStringLiteral("a background tab's failure must not be printed "
		                                     "against the session on screen, but the line "
		                                     "says: ")
		                      + window.status_text()));

		/* And the line still says what it is actually showing. */
		QVERIFY(window.status_text().contains(QStringLiteral("prod.example")));
	}

	bssh_ssh_session::set_factory(nullptr);
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::closing_a_window_leaves_the_other_windows_sessions_alone() {
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	/*
	 * A transport that connects, so that one of the two sessions is
	 * genuinely LIVE. The default fake in this file fails on connect,
	 * and against a dead session the defect is invisible: disconnecting
	 * something already disconnected does nothing anybody can see.
	 */
	bssh_ssh_session::set_factory([](QObject *parent) -> bssh_ssh_session * {
		return new bssh_fake_ssh_session(parent);
	});

	bssh_main_window keeper;
	keeper.resize(900, 600);
	keeper.show();
	QVERIFY(QTest::qWaitForWindowExposed(&keeper));

	bssh_session *stays = keeper.registry()->create(named(QStringLiteral("stays")));
	keeper.adopt_session(stays);
	stays->connect_to_host();
	QCoreApplication::processEvents();
	QVERIFY(stays->is_live());

	bssh_main_window going;
	going.resize(900, 600);
	going.show();
	QVERIFY(QTest::qWaitForWindowExposed(&going));

	bssh_session *leaves = going.registry()->create(named(QStringLiteral("leaves")));
	going.adopt_session(leaves);
	QCoreApplication::processEvents();

	/*
	 * Deliberately NOT connected. Closing a window with nothing live
	 * takes the quiet path -- no question asked -- which is what makes
	 * this runnable at all: a live session raises a modal box, and a
	 * test that has to click one is testing the box.
	 *
	 * It is also the sharpest form of the fault. The user closed the
	 * window that had nothing running, and the window that did lost its
	 * connection.
	 */
	QVERIFY(!leaves->is_live());

	going.close();
	QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
	QCoreApplication::processEvents();

	/*
	 * The whole report: "when I close one window it closes both". The
	 * close loop walked the REGISTRY, so every session in every window
	 * was disconnected by whichever window the user shut.
	 */
	QVERIFY(stays->is_live());
	QVERIFY(shared->index_of(stays) >= 0);

	/*
	 * And the closed window's own session is gone rather than merely
	 * disconnected. Nothing shows it now -- a session is on screen in
	 * exactly one place (sec 8.4) -- so leaving it in the registry
	 * leaves it counted by the tray and written into the saved layout
	 * while being reachable from nothing.
	 */
	QCOMPARE(shared->index_of(leaves), -1);

	/*
	 * Residency is the PROCESS's fact, and this is the reachable half of
	 * it: set_keep_alive() turns straight into quitOnLastWindowClosed on
	 * desktop. Said false unconditionally -- which is what the close did
	 * -- shutting an idle window would quit an app whose other window
	 * was hidden in the tray with a build running, because Qt counts
	 * VISIBLE windows and a backgrounded one is not one.
	 */
	QVERIFY(!QApplication::quitOnLastWindowClosed());
	QApplication::setQuitOnLastWindowClosed(true);

	bssh_ssh_session::set_factory(nullptr);
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::hovering_a_tiled_pane_moves_focus_to_it() {
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	/*
	 * sec 8.3's focus-follows-mouse, asserted between TILED PANES rather
	 * than on the setting. The stored value has a persistence test and
	 * the view has a setter, and neither says the two are joined: the
	 * setting is pushed into the views from apply_device_settings(), and
	 * a pane built later reads its own constructor default.
	 *
	 * Safe to write because the suite sandboxes HOME and XDG_CONFIG_HOME
	 * (test/main.cpp); restored afterwards regardless, since it is a
	 * process-wide file.
	 */
	const bool original = bssh_app_settings().focus_follows_mouse();
	bssh_app_settings().set_focus_follows_mouse(true);

	{
		bssh_main_window window;
		window.resize(900, 600);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		window.activateWindow();

		bssh_session *left = window.registry()->create(named(QStringLiteral("left")));
		window.adopt_session(left);

		/*
		 * Tiled, not tabbed -- drop_session's "beside" half, which is
		 * what a tab dropped onto the panes does (sec 8.4).
		 */
		bssh_session *right = window.registry()->create(named(QStringLiteral("right")));
		window.drop_session(right, true);
		QCoreApplication::processEvents();

		QCOMPARE(window.panes()->views().size(), 2);

		window.panes()->focus_session(left);
		QCoreApplication::processEvents();
		QCOMPARE(window.panes()->focused_session(), left);

		bssh_terminal_view *hovered = nullptr;
		for (bssh_terminal_view *view : window.panes()->views()) {
			if (view != window.panes()->focused_view()) hovered = view;
		}
		QVERIFY(hovered);

		/*
		 * The enter is synthesised at the view, which is as far down the
		 * route as a test reaches: whether the platform delivers Enter to
		 * the pane under the pointer is Qt's business and an offscreen
		 * cursor has no position to leave. What this pins is everything
		 * from the handler onwards -- that the pane asks for focus rather
		 * than telling the container, so the border, the status line and
		 * what copy acts on all follow from focusInEvent (sec 8.3).
		 */
		const QPointF at(5, 5);
		QEnterEvent enter(at, at, hovered->mapToGlobal(QPoint(5, 5)));
		QCoreApplication::sendEvent(hovered, &enter);
		QCoreApplication::processEvents();

		QCOMPARE(window.panes()->focused_session(), right);
	}

	bssh_app_settings().set_focus_follows_mouse(original);
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_trusted_host_can_actually_set_the_clipboard() {
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session *session = window.registry()->create(named(QStringLiteral("trusted")));
	window.adopt_session(session);
	QCoreApplication::processEvents();

	QGuiApplication::clipboard()->setText(QStringLiteral("before"));

	/*
	 * Off by default (sec 7.1), and that half must keep working: a host
	 * nobody trusted may not touch the clipboard.
	 */
	QJsonObject settings;
	settings.insert(QStringLiteral("allow_clipboard_write"), false);
	session->set_settings(settings);

	session->emulator()->feed(QByteArrayLiteral("\x1b]52;c;aGVsbG8=\x1b\\"));
	QCoreApplication::processEvents();
	QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("before"));

	/*
	 * And with the host trusted it really happens. This is the assertion
	 * the feature never passed: the window wired this up with
	 * Qt::UniqueConnection and a lambda, which Qt REFUSES -- in a release
	 * build it warns and returns an invalid connection, so the handler
	 * ran zero times rather than once. Measured with a ten-line probe
	 * against this Qt, because the failure is silent by construction:
	 * everything compiles, the signal is emitted, and nothing listens.
	 */
	settings.insert(QStringLiteral("allow_clipboard_write"), true);
	session->set_settings(settings);

	session->emulator()->feed(QByteArrayLiteral("\x1b]52;c;aGVsbG8=\x1b\\"));
	QCoreApplication::processEvents();

	QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("hello"));

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::a_long_title_cannot_push_the_other_tabs_off_the_bar() {
	/*
	 * A tab label is a string the REMOTE writes: it arrives by OSC
	 * (sec 8.3), and the emulator caps it at 1024 characters. With
	 * setExpanding(false) and no elide mode that is a tab about eleven
	 * thousand pixels wide, and every tab after it sits past the
	 * right-hand edge -- present, but reachable only through the scroll
	 * buttons, which is not where a user looks for the session they were
	 * just in.
	 *
	 * No hostility is required. A shell that puts the full command line
	 * or a deep path into its title gets there on an ordinary afternoon.
	 *
	 * Asserted structurally rather than in pixels: whatever the style and
	 * font, ONE tab must not be wider than the whole bar it lives in.
	 */
	bssh_main_window window;

	QTabBar *tabs = window.findChild<QTabBar *>();
	QVERIFY2(tabs, "the window must have a tab bar");

	tabs->addTab(QStringLiteral("web"));
	tabs->addTab(QString(1024, QLatin1Char('W')));
	tabs->addTab(QStringLiteral("db"));
	tabs->resize(600, tabs->sizeHint().height());

	QVERIFY2(tabs->tabRect(1).width() < tabs->width(),
	          qPrintable(QStringLiteral("one tab is %1px in a %2px bar")
	                             .arg(tabs->tabRect(1).width())
	                             .arg(tabs->width())));

	/* And a short label is still shown whole, so this is a check on the
	 * eliding rather than on tabs having been made uniformly small. */
	const int plain = tabs->tabRect(0).width();
	tabs->setTabText(0, QStringLiteral("web"));
	QCOMPARE(tabs->tabRect(0).width(), plain);
}

void layout_test::a_pane_drag_is_not_raised_from_inside_the_views_own_handler() {
	/*
	 * The view emits pane_drag_requested from its own mouseMoveEvent,
	 * and what that reaches runs QDrag::exec() -- a nested event loop
	 * that does not return until the pointer is released. Whatever
	 * happens to the panes meanwhile comes back through rebuild(), which
	 * qDeleteAll()s the views. Delivered directly, that destroys the view
	 * whose handler is still on the stack, inside its own emission.
	 *
	 * It does not misbehave today, and the reason is worth stating
	 * because it is not a design: nothing after the emit in
	 * mouseMoveEvent touches `this` -- the remaining lines are
	 * event->accept() and return. The next edit to that function has no
	 * way of knowing those two lines are load-bearing.
	 *
	 * So the delivery is QUEUED, and this asserts that rather than
	 * asserting the crash it prevents. A test for the crash would have to
	 * drive a real drag and then rely on undefined behaviour showing
	 * itself, which is not evidence of anything.
	 */
	bssh_session_registry registry;
	bssh_layout_manager layout;

	bssh_session *a = registry.create(named(QStringLiteral("a")));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(800, 400);

	layout.set_intent({a});
	QCOMPARE(container.pane_count(), 1);

	QWidget *view = container.view_at(0);
	QVERIFY(view);

	QSignalSpy raised(&container, &bssh_pane_container::pane_drag_requested);

	/*
	 * The signal a mouse drag would emit, without needing one. Signals
	 * are invokable, which is what lets this ask the delivery question
	 * directly.
	 */
	QVERIFY(QMetaObject::invokeMethod(view, "pane_drag_requested"));

	QVERIFY2(raised.count() == 0,
	          "the drag was raised synchronously, from inside the view's own "
	          "signal emission");

	QCoreApplication::processEvents();

	QVERIFY2(raised.count() == 1,
	          "the drag never arrived at all, so queueing lost it");
}

/*
 * The call site, which is the half a catalogue test cannot reach.
 *
 * sec 6.2's custom row is stored by the settings layer and pushed into
 * the keyboard catalogue by whoever owns the settings -- the catalogue
 * does not read them, because that dependency runs the wrong way. So
 * "the row is stored" and "the row is in use" are two facts, and M5's
 * sabotage recorded what happens when only the first is tested: the
 * three edits inside the reset were caught and removing the CALL SITE
 * was missed, because the test asked the object directly and a push
 * nothing invokes is indistinguishable from a working one.
 */
void layout_test::the_custom_row_reaches_the_catalogue_at_startup() {
	bssh_app_settings settings;
	const QString original = settings.custom_key_row();

	/*
	 * Deliberately not the default's keys: a spec that happened to match
	 * what the entry is born with would pass with the push removed, which
	 * is the fixture that does not reach the hazard.
	 */
	const QString spec = QStringLiteral("esc ctrl ~ pgup");
	settings.set_custom_key_row(spec);

	bssh_main_window window;

	const bssh_key_row_layout_t *custom =
	        bssh_key_row_catalog::instance().by_id(QStringLiteral("custom"));
	QVERIFY(custom != nullptr);
	QCOMPARE(bssh_key_row_spec(custom->keys), spec);

	/* Put the setting and the process-wide catalogue back, since both
	 * outlive this test. */
	settings.set_custom_key_row(original);
	if (!original.isEmpty()) {
		bssh_key_row_catalog::instance().set_custom(bssh_parse_key_row(original));
	}
}

/*
 * The colour scheme's half of the same push, which had no test at all.
 *
 * Found while building the key row's version of this mechanism: the row
 * was copied from the palette, so the palette's call site was read
 * closely, and nothing in the suite touched it. One line in
 * apply_device_settings hands the stored palette to a catalogue that
 * deliberately does not read settings itself -- delete it and every
 * custom scheme silently falls back to the black-and-white the entry is
 * born with, while the picker still says Custom and the id still
 * resolves. sec 10.1's "stored, read and applied to nothing", in the
 * feature this project cites as the pattern for the other three.
 *
 * It is the same shape as the key row's test above and deliberately not
 * shared with it: the two push different things into different
 * catalogues, and a helper covering both would pass if either worked.
 */
void layout_test::the_custom_palette_reaches_the_catalogue_at_startup() {
	bssh_app_settings settings;
	const bssh_colour_scheme_t original = settings.custom_scheme();

	/*
	 * A colour the entry is not born holding. The custom entry starts as
	 * black on white, so asserting on either would pass with the push
	 * removed -- the fixture that does not reach the hazard.
	 */
	bssh_colour_scheme_t wanted = original;
	wanted.background = qRgb(0x12, 0x34, 0x56);
	wanted.foreground = qRgb(0xab, 0xcd, 0xef);
	settings.set_custom_scheme(wanted);

	bssh_main_window window;

	const bssh_colour_scheme_t *custom =
	        bssh_colour_scheme_catalog::instance().by_id(QStringLiteral("custom"));
	QVERIFY(custom != nullptr);
	QCOMPARE(custom->background, wanted.background);
	QCOMPARE(custom->foreground, wanted.foreground);

	/* Both the setting and the process-wide catalogue outlive this test. */
	settings.set_custom_scheme(original);
	bssh_colour_scheme_catalog::instance().set_custom(original);
}

namespace {

/*
 * What a screen reader would actually be told about every control on one
 * surface, and how many controls that was.
 *
 * sec 14's M11 measured this by hand and found three states: one page
 * correct by accident (QFormLayout::addRow makes the label a buddy for
 * you), one with 4 of 13 anonymous because its rows are a label and a
 * field side by side in a box layout, and the phone's FIRST screen with
 * 2 of 2 read out as "vertical ellipsis" and "plus" -- what they look
 * like, not what they do. All were named, and nothing since then has
 * stopped them being renamed back.
 *
 * The existing sweep in terminal_view_test walks one dialog and looks at
 * QLineEdit and QSpinBox only. The two controls that were worst are
 * QToolButtons on a surface it does not visit, so widening the population
 * is the whole point of this one.
 *
 * Ask QAccessible rather than accessibleName(): a control with no
 * accessibleName can still be perfectly announced through a buddy label
 * or its own text, and asserting on the property would demand redundant
 * names for controls that already work. The interface answers the
 * question a user has.
 */
QString unnamed_controls(const QString &surface, QWidget *root, int *counted) {
	root->resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	root->show();

	QStringList anonymous;
	int seen = 0;

	for (QWidget *w : root->findChildren<QWidget *>()) {
		/*
		 * A scroll bar is deliberately not in this population, and it is
		 * the one the first run caught: QScrollBar is a QAbstractSlider,
		 * so the obvious filter picks it up, and it has no name to give.
		 * A reader announces it by ROLE -- "scroll bar" -- and naming it
		 * would be inventing a label for something already announced.
		 * Excluded by type rather than by an ignore list, because the
		 * rule is about what the class IS, not about this one instance.
		 */
		const bool interactive = qobject_cast<QAbstractButton *>(w)
		        || qobject_cast<QLineEdit *>(w) || qobject_cast<QComboBox *>(w)
		        || qobject_cast<QAbstractSpinBox *>(w)
		        || (qobject_cast<QAbstractSlider *>(w) && !qobject_cast<QScrollBar *>(w));
		if (!interactive || !w->isVisible()) continue;

		/*
		 * A spin box and a combo each own an internal line edit, and a
		 * reader lands on the container rather than its guts -- the same
		 * exclusion the terminal_view sweep already makes, for the same
		 * reason, and without it every combo on the page reports twice.
		 */
		bool internal = false;
		for (QWidget *up = w->parentWidget(); up; up = up->parentWidget()) {
			if (qobject_cast<QAbstractSpinBox *>(up) || qobject_cast<QComboBox *>(up)
			    || qobject_cast<QKeySequenceEdit *>(up)) {
				internal = true;
			}
		}
		if (internal) continue;

		++seen;

		QAccessibleInterface *a = QAccessible::queryAccessibleInterface(w);
		if (!a) continue;

		/*
		 * Emptiness is NOT the failure, and asserting on it was this
		 * sweep's own first mistake -- caught by sabotaging the two
		 * names M11 added and watching the suite stay green.
		 *
		 * `m_add' carries setText("+") beside its accessible name, so
		 * removing the name leaves QAccessible falling back to the text
		 * and answering "+". Non-empty, and exactly the state M11
		 * measured and fixed: a reader announcing "plus" and "vertical
		 * ellipsis", which is what the controls LOOK like rather than
		 * what they do.
		 *
		 * So the condition is that the name says something a person
		 * could act on. One character cannot: a glyph is an appearance,
		 * and a control announcing its own appearance is the defect
		 * whatever the character is.
		 */
		const QString name = a->text(QAccessible::Name).trimmed();

		bool describes = name.size() > 1;
		if (describes) {
			describes = false;
			for (const QChar ch : name) {
				if (ch.isLetterOrNumber()) { describes = true; break; }
			}
		}
		if (describes) continue;

		anonymous.append(QStringLiteral("%1 (%2) says \"%3\"")
		                         .arg(w->objectName().isEmpty()
		                                      ? QStringLiteral("unnamed")
		                                      : w->objectName())
		                         .arg(QString::fromLatin1(w->metaObject()->className()))
		                         .arg(name.isEmpty() ? QStringLiteral("empty") : name));
	}

	*counted += seen;

	if (anonymous.isEmpty()) return QString();
	return QStringLiteral("%1: %2 of %3 announce a glyph or nothing -- %4")
	        .arg(surface).arg(anonymous.size()).arg(seen)
	        .arg(anonymous.join(QStringLiteral(", ")));
}

} /* namespace */

void layout_test::every_touch_surface_names_its_controls() {
	const forced_touch_t arrangement(1);

	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));

	bssh_profile_t profile;
	profile.name = QStringLiteral("build server (europe)");
	profile.host = QStringLiteral("build.internal.example.com");
	profile.user = QStringLiteral("nabbe.testing");
	profile.port = 2299;
	QVERIFY(store.save(profile) != 0);

	int counted = 0;
	QString complaint;

	{
		bssh_host_list list(&store);
		complaint = unnamed_controls(QStringLiteral("host list"), &list, &counted);
		QVERIFY2(complaint.isEmpty(), qPrintable(complaint));
	}
	{
		bssh_host_editor editor(&store);
		editor.set_profile(profile);
		complaint = unnamed_controls(QStringLiteral("host editor"), &editor, &counted);
		QVERIFY2(complaint.isEmpty(), qPrintable(complaint));
	}
	{
		bssh_preset_editor editor(&store, bssh_preset_editor::TOUCH);
		complaint = unnamed_controls(QStringLiteral("preset editor"), &editor, &counted);
		QVERIFY2(complaint.isEmpty(), qPrintable(complaint));
	}
	{
		bssh_app_settings_view settings;
		complaint = unnamed_controls(QStringLiteral("settings"), &settings, &counted);
		QVERIFY2(complaint.isEmpty(), qPrintable(complaint));
	}
	{
		bssh_font_picker picker;
		complaint = unnamed_controls(QStringLiteral("font picker"), &picker, &counted);
		QVERIFY2(complaint.isEmpty(), qPrintable(complaint));
	}
	{
		bssh_password_prompt prompt(
		        QStringLiteral("nabbe.testing@build.internal.example.com:2299"), true);
		complaint = unnamed_controls(QStringLiteral("password prompt"), &prompt, &counted);
		QVERIFY2(complaint.isEmpty(), qPrintable(complaint));
	}

	/*
	 * And say how many were looked at, because a sweep that inspected
	 * nothing reports success exactly as loudly as one that inspected
	 * everything. If a filter above stops matching -- a control type
	 * renamed, a surface that no longer builds its children until shown --
	 * this is the assertion that notices, and the number is the only
	 * thing separating "all named" from "none found".
	 */
	QVERIFY2(counted >= 20,
	          qPrintable(QStringLiteral("only %1 controls inspected across six "
	                                     "surfaces -- the sweep has stopped seeing them")
	                             .arg(counted)));
}

/*
 * Every way of opening a link went through Ctrl+click, and a phone has
 * no Ctrl.
 *
 * That made the whole hyperlink feature -- OSC 8 and the inferred kind
 * alike -- desktop-only, on a project whose sec 14 rule is that a
 * milestone lands on both platforms before the next one starts. It is
 * working-practice's recorded shape: an override only one input device
 * can produce is not an override on the other, and the question is not
 * who owns the gesture but what the user can still reach.
 *
 * Driven through context_menu_requested because that is what BOTH routes
 * emit -- contextMenuEvent for a right-click, and the long-press handler
 * for a finger -- so this covers the receiver that was missing rather
 * than one platform's way of reaching it.
 */
void layout_test::a_link_is_reachable_without_a_control_key() {
	bssh_main_window window;
	window.resize(900, 400);
	window.show();

	bssh_session *session = window.registry()->create(named(QStringLiteral("a")));
	window.adopt_session(session);

	/*
	 * Let the layout happen. Every other window test here asks questions
	 * that do not need geometry -- a cursor style, a status dot -- and so
	 * never noticed that a shown window's view still reports a 0x0 grid
	 * until the pane container has had a resize. This one asks where a
	 * cell IS, which is the first question in the suite that cannot be
	 * answered before the pixels exist.
	 */
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	QApplication::processEvents();

	bssh_terminal_view *view = window.findChild<bssh_terminal_view *>();
	QVERIFY(view);
	QVERIFY(session->emulator());

	session->emulator()->feed(QByteArrayLiteral("see https://example.com/touched here\r\n"));

	qint64 line = 0;
	int col = 0;
	QVERIFY(view->cell_at(QPoint(2, 2), line, col));

	const QRect target = view->cell_rect(line, 10);
	QVERIFY2(target.isValid(), "the address is not on screen");

	/*
	 * The view answers for the position without any modifier at all,
	 * which is the fact the menu depends on.
	 */
	QCOMPARE(view->link_at(target.center()),
	          QStringLiteral("https://example.com/touched"));

	/*
	 * And the menu that a long press opens carries it. The popup is
	 * modal, so it is inspected and dismissed from a timer -- what is
	 * asserted is that show_terminal_menu ASKED, which is the wiring a
	 * test of build_terminal_menu alone would not have seen.
	 */
	bool offered = false;
	QString label;
	QTimer::singleShot(0, [&offered, &label] {
		QWidget *popup = QApplication::activePopupWidget();
		QMenu *menu = qobject_cast<QMenu *>(popup);
		if (!menu) return;

		if (QAction *open = menu->findChild<QAction *>(
		            QStringLiteral("terminal_open_link"))) {
			offered = true;
			label = open->text();
		}
		menu->close();
	});

	emit view->context_menu_requested(view->mapToGlobal(target.center()));

	QVERIFY2(offered, "a long press over a link offers no way to open it");

	/*
	 * The label names the address. "Open link" alone asks somebody to
	 * authorise something they cannot see, and the reason it asks at all
	 * is that the remote chose the URL.
	 */
	QVERIFY2(label.contains(QStringLiteral("example.com/touched")),
	          qPrintable(QStringLiteral("the entry does not say where it goes: %1")
	                             .arg(label)));

	/* And no entry where there is no link, rather than a disabled one:
	 * an item that is always there and usually dead is one people learn
	 * to skip. */
	bool offered_on_prose = false;
	const QRect prose = view->cell_rect(line, 1);
	QTimer::singleShot(0, [&offered_on_prose] {
		QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (!menu) return;
		offered_on_prose = menu->findChild<QAction *>(
		                           QStringLiteral("terminal_open_link")) != nullptr;
		menu->close();
	});
	emit view->context_menu_requested(view->mapToGlobal(prose.center()));

	QVERIFY2(!offered_on_prose, "offered to open a link over the word 'see'");
}

/*
 * sec 6.3's fourth answer: a user's own keyboard layout.
 *
 * The other three are values a user picks or composes; a layout had
 * nowhere to live, being a C table the key builders read through
 * g_layout. It is text now -- one entry per XKB slot, four levels
 * separated by a pipe -- and the interesting half is not the parse but
 * the REFUSAL: sec 6.3 asks that the catalogue's checks become the
 * contract every layout must satisfy, and the key row already showed
 * what it costs to leave such a rule in the suite. A rule that lives in
 * a test reports the breach after it has happened, and reports it
 * against the layouts that are correct.
 */
void layout_test::a_user_layout_is_transcribed_and_refused_if_it_cannot_type() {
	const layout_left_as_found_t restore;

	/* Start from a shipped table so the fixture is a real layout with one
	 * thing changed, rather than a hand-built toy that reaches nothing. */
	QMap<QString, QString> rows;
	rows.insert(QStringLiteral("AE01"), QStringLiteral("1|!|¹|¡"));
	rows.insert(QStringLiteral("AD01"), QStringLiteral("q|||"));
	rows.insert(QStringLiteral("AC01"), QStringLiteral("a|||"));

	const QList<bssh_custom_slot_t> parsed = bssh_parse_layout_slots(rows);
	QCOMPARE(parsed.size(), 3);

	/*
	 * A pipe is a real legend on BKSL, so the escape is the one thing
	 * the format cannot do without -- a split that ate it would drop the
	 * single key the escape exists for.
	 */
	QMap<QString, QString> bar;
	bar.insert(QStringLiteral("BKSL"), QStringLiteral("\\||\u00a6||"));
	const QList<bssh_custom_slot_t> bar_parsed = bssh_parse_layout_slots(bar);
	QCOMPARE(bar_parsed.size(), 1);
	QCOMPARE(bar_parsed.first().level[0], QStringLiteral("|"));

	/* A row with the wrong number of levels is dropped, and the rest of
	 * the layout stands: one mistyped line should cost that slot. */
	QMap<QString, QString> ragged;
	ragged.insert(QStringLiteral("AE01"), QStringLiteral("1|!"));
	ragged.insert(QStringLiteral("AE02"), QStringLiteral("2|@||"));
	QCOMPARE(bssh_parse_layout_slots(ragged).size(), 1);

	/*
	 * Three slots cannot type ASCII, so the contract refuses them -- and
	 * says which characters are missing rather than "invalid", because a
	 * person who has transcribed 47 rows needs to know which one is
	 * wrong.
	 */
	QString why;
	QVERIFY2(!bssh_set_custom_layout(QStringLiteral("Toy"), parsed, &why),
	          "a three-key layout was accepted as able to type");
	QVERIFY2(why.contains(QStringLiteral("cannot type")),
	          qPrintable(QStringLiteral("refused without saying what is missing: %1")
	                             .arg(why)));

	/*
	 * And the refusal left the keyboard alone. This is the half that
	 * matters on a phone: the layout IS the keyboard, so a refusal that
	 * cleared first would leave somebody unable to reach the settings
	 * that would fix it.
	 */
	QVERIFY2(!bssh_keyboard_layouts().isEmpty(), "the catalogue lost its layouts");
	bool custom_offered = false;
	for (const bssh_keyboard_layout_t &layout : bssh_keyboard_layouts()) {
		if (layout.id == QStringLiteral("custom")) custom_offered = true;
	}
	QVERIFY2(!custom_offered,
	          "a layout that was refused is offered in the chooser anyway");

	/*
	 * A whole layout, transcribed from a shipped one, IS accepted -- the
	 * positive control, without which every assertion above is satisfied
	 * by a function that refuses everything.
	 */
	QMap<QString, QString> whole;
	for (const bssh_custom_slot_t &slot : bssh_shipped_layout_slots(QStringLiteral("us-intl"))) {
		whole.insert(slot.slot, bssh_layout_slot_spec(slot));
	}
	QVERIFY2(whole.size() > 40, "the shipped layout came back too small to be one");

	why.clear();
	QVERIFY2(bssh_set_custom_layout(QStringLiteral("Mine"),
	                                 bssh_parse_layout_slots(whole), &why),
	          qPrintable(QStringLiteral("a transcription of US International was "
	                                     "refused: %1").arg(why)));

	/* Now it is offered, and selecting it works. */
	custom_offered = false;
	for (const bssh_keyboard_layout_t &layout : bssh_keyboard_layouts()) {
		if (layout.id == QStringLiteral("custom")) custom_offered = true;
	}
	QVERIFY2(custom_offered, "an accepted layout is not in the chooser");
	QVERIFY(bssh_set_keyboard_layout(QStringLiteral("custom")));
	QCOMPARE(bssh_keyboard_layout(), QStringLiteral("custom"));

	/*
	 * And the CONTROL a person actually looks at, which is one hop
	 * further than the catalogue and the hop this tree keeps losing
	 * things at: bssh_keyboard_layouts() can be perfectly right while
	 * the combo was built from something else, and the assertion above
	 * would not notice.
	 */
	bssh_app_settings_view settings;
	QComboBox *chooser =
	        settings.findChild<QComboBox *>(QStringLiteral("keyboard-layout"));
	QVERIFY2(chooser, "the layout chooser was never built");

	QVERIFY2(chooser->findData(QStringLiteral("custom")) >= 0,
	          "the chooser does not offer the layout the catalogue accepted");
	QVERIFY2(chooser->findData(QStringLiteral("us-intl")) >= 0,
	          "the chooser lost the shipped layouts");
}

/*
 * sec 6.3's answer is a reserved `custom' entry in a catalogue, and a
 * catalogue entry nobody can choose is the same nothing as no entry.
 *
 * Three features share that mechanism now -- the colour scheme, the key
 * row and the keyboard layout -- and until this only the layout's last
 * hop was asserted. The other two were reasoned about: both choosers
 * iterate all(), so both must list it. That reasoning is exactly what
 * was true of bssh_platform::notify(), the palette's push and the
 * selection handles, each of which was correct one layer down and
 * reached nobody.
 */
void layout_test::every_custom_entry_reaches_the_control_that_offers_it() {
	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));

	/*
	 * The key row's, which this session added. Its custom entry is born
	 * as a copy of the default, so it exists from the first run and
	 * needs nothing written before the chooser should list it.
	 */
	{
		bssh_preset_editor editor(&store, bssh_preset_editor::DESKTOP);
		QComboBox *rows =
		        editor.findChild<QComboBox *>(QStringLiteral("preset-key-row"));
		QVERIFY2(rows, "the preset editor has no key row chooser");
		QVERIFY2(rows->findData(QStringLiteral("custom")) >= 0,
		          "a user can compose a key row and not choose it");
		QVERIFY2(rows->findData(QStringLiteral("default")) >= 0,
		          "the chooser lost the shipped rows");
	}

	/*
	 * And the colour scheme's, which predates this session and is the
	 * feature the other two were copied from -- so if the pattern has a
	 * hole, it has it here first.
	 */
	{
		bssh_font_picker picker;
		QComboBox *schemes =
		        picker.findChild<QComboBox *>(QStringLiteral("font-picker-scheme"));
		QVERIFY2(schemes, "the font picker has no scheme chooser");
		QVERIFY2(schemes->findData(QStringLiteral("custom")) >= 0,
		          "a user can edit the custom palette and not choose it");
	}
}

void layout_test::a_rebuilt_pane_keeps_the_sessions_zoom() {
	/*
	 * The zoom lives on the session for exactly this reason: a pane is
	 * destroyed and rebuilt by every layout change, so a number kept on
	 * the widget is lost the first time a window splits or narrows --
	 * which is the moment somebody wants bigger text.
	 */
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("zoomed")));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(1200, 400);
	layout.set_intent({ a });
	container.rebuild();

	QPointer<bssh_terminal_view> first = container.view_at(0);
	QVERIFY(first);
	const int base = first->font_point_size();

	a->set_font_zoom(3);
	QCOMPARE(first->font_point_size(), base + 3);

	/* A second pane, which rebuilds every view in the container. */
	bssh_session *b = registry.create(named(QStringLiteral("other")));
	layout.set_intent({ a, b });
	container.rebuild();

	/*
	 * The fixture has to have REACHED the hazard: if the container had
	 * kept the old widget, this would pass without saying anything about
	 * where the zoom lives.
	 */
	QVERIFY2(first.isNull(), "the rebuild did not destroy the pane it was given");

	bssh_terminal_view *rebuilt = container.view_for(a);
	QVERIFY(rebuilt);
	QCOMPARE(rebuilt->font_point_size(), base + 3);

	/* And the session beside it was not dragged along with it. */
	bssh_terminal_view *neighbour = container.view_for(b);
	QVERIFY(neighbour);
	QCOMPARE(neighbour->font_point_size(), base);
}

void layout_test::ctrl_wheel_in_a_pane_zooms_that_panes_session() {
	/*
	 * The gesture's control: the view reports that it happened and the
	 * container names the session. Asserted on the SESSION, because the
	 * view emitting correctly into nothing is the shape this tree keeps
	 * finding.
	 */
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(1200, 400);
	layout.set_intent({ a, b });
	container.rebuild();

	bssh_terminal_view *second = container.view_at(1);
	QVERIFY(second);

	/* Which session that pane is showing, asked rather than assumed. */
	QCOMPARE(container.view_for(b), second);

	const QPointF at(10, 10);
	QWheelEvent event(at, second->mapToGlobal(at.toPoint()), QPoint(0, 0),
	                   QPoint(0, 120), Qt::NoButton, Qt::ControlModifier,
	                   Qt::NoScrollPhase, false);
	QApplication::sendEvent(second, &event);

	/* The pane that was turned in, and only that one. */
	QCOMPARE(b->font_zoom(), 1);
	QCOMPARE(a->font_zoom(), 0);
}

void layout_test::the_touch_terminal_menu_fits_a_phone() {
	/*
	 * The surface every_touch_surface_fits_a_phone does not cover, and
	 * the one that grows every time somebody adds a verb: sec 6.4's
	 * terminal menu is where this app has been putting the things a
	 * phone cannot otherwise reach -- split, un-split, move a pane, the
	 * way back to the configuration, and now zoom.
	 *
	 * Each of those was the right call on its own. Nothing was counting
	 * the total, which is how a menu ends up taller than the screen it
	 * pops up on: 48dp rows (sec 6.2) buy sixteen of them in 640dp and
	 * this menu passed that on the way past.
	 *
	 * A QMenu does not overlap its rows the way a form does -- Qt gives
	 * it scroll arrows instead -- so the failure is milder than the one
	 * check_fits describes, and it is still a menu whose bottom entries
	 * are behind a gesture nobody looks for.
	 */
	const forced_touch_t arrangement(1);

	bssh_main_window window;
	QMenu *menu = window.build_terminal_menu(&window);
	QVERIFY(menu);

	menu->ensurePolished();
	const QSize wanted = menu->sizeHint();

	/*
	 * Named in the failure, because the useful half of this assertion is
	 * the number: whoever trips it needs to know by how much and what
	 * the menu is carrying.
	 */
	QVERIFY2(wanted.height() <= BSSH_PHONE_TALL_DP,
	          qPrintable(QStringLiteral("the touch terminal menu wants %1px of a %2px "
	                                     "screen, with %3 entries")
	                             .arg(wanted.height())
	                             .arg(BSSH_PHONE_TALL_DP)
	                             .arg(menu->actions().size())));

	QVERIFY2(wanted.width() <= BSSH_PHONE_DP,
	          qPrintable(QStringLiteral("the touch terminal menu wants %1px of a %2px "
	                                     "screen in width")
	                             .arg(wanted.width()).arg(BSSH_PHONE_DP)));

	delete menu;
}
