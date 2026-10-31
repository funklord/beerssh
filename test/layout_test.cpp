#include <cstdio>

#include <QApplication>
#include <QAbstractButton>
#include <QGroupBox>
#include <QPushButton>
#include <QRadioButton>
#include <QAbstractItemView>
#include <QAbstractSpinBox>
#include <QLineEdit>
#include <QKeySequenceEdit>
#include <QSlider>
#include <QSpinBox>
#include <QAction>
#include <QComboBox>
#include <QFormLayout>
#include <QPair>
#include <QClipboard>
#include <QJsonObject>
#include <QAbstractScrollArea>
#include <QRandomGenerator>
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

#include <memory>
#include <QtTest>

#include <QAccessible>
#include <QAbstractSlider>
#include <QTimer>
#include "model/command_line.h"
#include "model/profile.h"
#include "platform/platform.h"
#include <QFile>
#include "term/font_catalog.h"
#include "model/custom_font.h"
#include <QTemporaryDir>
#include "model/profile_store.h"
#include "softkeys/key_cap.h"
#include "softkeys/keyboard.h"
#include "softkeys/keyboard_layout.h"
#include "softkeys/resize_grip.h"
#include "ui/input_selector.h"
#include "ui/pane_corner.h"
#include "input/input_router.h"
#include "term/emulator.h"
#include "ui/host_list.h"
#include "ui/host_editor.h"
#include "softkeys/keyboard_switch.h"
#include "ui/form_wrap.h"
#include "ui/preset_editor.h"
#include "ui/term_features_editor.h"
#include "ui/app_settings_view.h"
#include "ui/licences_view.h"
#include "ui/font_picker.h"
#include "ui/password_prompt.h"
#include "ui/session_dialog.h"
#include "ui/ssh_config_import.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QSplitter>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QSplitterHandle>
#include <QTabBar>
#include <QMetaMethod>
#include <QToolButton>
#include "ui/long_press.h"
#include "ui/main_window.h"
#include "fake_ssh_session.h"
#include "phone_metrics.h"
#include "layout_test.h"
#include "input/input_router.h"
#include "model/app_settings.h"
#include "ui/multiplexer_drawer.h"
#include "model/session.h"
#include "model/session_registry.h"
#include "term/emulator.h"
#include "softkeys/key_row.h"
#include "softkeys/key_row_layout.h"
#include "term/colour_scheme.h"
#include "ui/layout_manager.h"
#include "ui/touch_metrics.h"
#include "ui/input_selector.h"
#include "ui/pane_bar.h"
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

/* The device's screen, forced for a test and put back after it. */
struct forced_screen_t {
	explicit forced_screen_t(const QSize &size) { bssh_platform::force_screen_size(size); }
	~forced_screen_t() { bssh_platform::force_screen_size(QSize()); }
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

void layout_test::a_window_behind_another_program_is_away() {
	/*
	 * sec 8.3's away rule, settled by the holder on 2026-09-29: a pane in
	 * a window that can be seen behind a browser is not being watched, so
	 * a bell there raises the dot and the notification. Coming back
	 * counts as having seen it.
	 */
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(1200, 400);
	layout.set_intent({a});
	container.rebuild();

	QVERIFY(a->is_on_screen());

	bssh_platform::force_application_active(0);
	QVERIFY2(!a->is_on_screen(), "a pane behind another program still counts as watched");

	bssh_platform::force_application_active(1);
	QVERIFY(a->is_on_screen());
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

void layout_test::a_column_scroll_settles_on_a_pane_top() {
	/*
	 * sec 8.7: "The column's SCROLLING snaps too, so a terminal starts at
	 * the top of the viewport rather than half off it." The pure half,
	 * asserted without a widget for the same reason the divider's is.
	 */
	const int tops[] = { 0, 200, 400, 600 };
	const int count = int(sizeof(tops) / sizeof(tops[0]));
	const int max_offset = 500;

	/*
	 * A finger that stopped just past a top comes back to it, and one
	 * that stopped just short goes on to it. Both directions, because a
	 * settle that only ever rounded one way would pass the first of
	 * these and leave the terminal half off the top in the other.
	 */
	QCOMPARE(bssh_settle_column_scroll(215, tops, count, max_offset), 200);
	QCOMPARE(bssh_settle_column_scroll(385, tops, count, max_offset), 400);

	/*
	 * The end of the range is a candidate alongside the tops. The last
	 * pane starts at 600, which the scrollbar cannot reach, so a drag
	 * near the bottom settles showing that pane's bottom rather than
	 * being hauled back up to 400.
	 */
	QCOMPARE(bssh_settle_column_scroll(480, tops, count, max_offset), max_offset);

	/*
	 * And a tie goes to the smaller offset rather than to whichever way
	 * the arithmetic happened to round.
	 */
	QCOMPARE(bssh_settle_column_scroll(300, tops, count, max_offset), 200);

	/* Out of range in either direction lands inside it. */
	QCOMPARE(bssh_settle_column_scroll(-40, tops, count, max_offset), 0);
	QCOMPARE(bssh_settle_column_scroll(9000, tops, count, max_offset), max_offset);
}

void layout_test::letting_go_of_a_pan_settles_the_column() {
	/*
	 * And the wiring, which the case above cannot see: a correct function
	 * is not a working feature. The gesture is a Pan drag in a column
	 * tall enough to scroll, and what is asserted is that RELEASING it
	 * leaves the viewport on a pane top -- not that the helper agrees
	 * with itself.
	 */
	/* The body-drag path is the touch one, so the gesture needs a thumb. */
	const forced_touch_t touch(1);

	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(400);
	layout.set_arrangement(bssh_layout_manager::COLUMN);

	QList<bssh_session *> many;
	for (int i = 0; i < 6; ++i)
		many.append(registry.create(named(QStringLiteral("p%1").arg(i))));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(400, 400);
	layout.set_intent(many);
	QCoreApplication::processEvents();
	QVERIFY(QTest::qWaitForWindowExposed(&container));

	QScrollArea *area = container.findChild<QScrollArea *>();
	QVERIFY(area);
	QScrollBar *bar = area->verticalScrollBar();
	QVERIFY(bar);
	QVERIFY2(bar->maximum() > 0, "the column did not overflow, so there is "
	                              "nothing for a settle to land on");

	bssh_terminal_view *view = container.view_at(0);
	QVERIFY(view);
	view->set_input_mode(BSSH_INPUT_COLUMN_SCROLL);

	/*
	 * Park the viewport deliberately between two pane tops, which is the
	 * state this feature exists to remove. Asserted first, so a settle
	 * that did nothing cannot pass by the column happening to sit on a
	 * top already.
	 */
	QList<int> tops;
	for (int i = 0; i < container.pane_count(); ++i) {
		bssh_terminal_view *pane = container.view_at(i);
		QVERIFY(pane);
		tops.append(pane->y());
	}
	QVERIFY(tops.size() >= 2);

	const int between = (tops.at(0) + tops.at(1)) / 2 + 3;
	QVERIFY(between < bar->maximum());
	bar->setValue(between);
	QVERIFY2(!tops.contains(bar->value()),
	          "the column was parked on a pane top, so a settle that does "
	          "nothing would pass");

	/*
	 * A drag of a few pixels and then a release, sent as explicit events
	 * for the reason the Pan test beside this one is written that way:
	 * QTest::mouseMove posts to the window rather than handing the view
	 * a move with the button down, and the gesture never starts.
	 */
	const QPoint from(view->cell_width() + 1, view->cell_height() + 1);
	const QPoint to = from - QPoint(0, 6);
	QMouseEvent press(QEvent::MouseButtonPress, from, view->mapToGlobal(from),
	                   Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
	QMouseEvent moved(QEvent::MouseMove, to, view->mapToGlobal(to),
	                   Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
	QMouseEvent release(QEvent::MouseButtonRelease, to, view->mapToGlobal(to),
	                     Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QCoreApplication::sendEvent(view, &press);
	QCoreApplication::sendEvent(view, &moved);

	/* The drag itself moved it, which is the existing behaviour and the
	 * state the release has to settle. */
	QVERIFY2(bar->value() != between, "the Pan drag did not scroll at all, "
	                                   "so the release has nothing to settle");

	QCoreApplication::sendEvent(view, &release);
	QCoreApplication::processEvents();

	QVERIFY2(tops.contains(bar->value()) || bar->value() == bar->maximum(),
	          "letting go of a pan left the column between two terminals");
}

void layout_test::a_column_keeps_new_sessions_as_tabs() {
	const forced_touch_t column(1);

	bssh_main_window window;
	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);

	bssh_session *a = registry->create(named(QStringLiteral("a")));
	bssh_session *b = registry->create(named(QStringLiteral("b")));
	bssh_session *c = registry->create(named(QStringLiteral("c")));
	window.adopt_session(a);
	window.adopt_session(b);
	window.adopt_session(c);

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	QCOMPARE(panes->pane_count(), 1);
	QCOMPARE(window.layout_manager()->tabs(a), QList<bssh_session *>({ a, b, c }));

	bssh_pane_bar *bar = window.bar_for(window.layout_manager()->front_of(a));
	QVERIFY2(bar && !bar->isHidden(), "a pane of three tabs in a column shows its bar");
	QCOMPARE(bar->sessions(), QList<bssh_session *>({ a, b, c }));
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

void layout_test::the_input_selector_defaults_by_platform_and_reports_changes() {
	/*
	 * Scroll is touch's default (sec 8.7): a body drag is the only way a
	 * thumb reaches the history. Click is the desktop's (sec 8.7a), which
	 * is how the desktop always behaved -- the program gets the mouse
	 * when it asks.
	 */
	{
		const forced_touch_t touch(1);
		bssh_input_selector touch_selector;
		QCOMPARE(touch_selector.mode(), BSSH_INPUT_SCROLL);
	}
	const forced_touch_t desktop(0);
	bssh_input_selector selector;
	QCOMPARE(selector.mode(), BSSH_INPUT_CLICK);

	/*
	 * Three rows tall (the holder: "at least the height of 3 options").
	 * Three modes are all on it, in their order, whichever is current --
	 * "with only three options all on the screen, why would we scroll?"
	 */
	QCOMPARE(selector.sizeHint().height(), 3 * selector.row_height());
	QVERIFY(!selector.rotates());
	QCOMPARE(selector.shown(0), BSSH_INPUT_SCROLL);
	QCOMPARE(selector.shown(1), BSSH_INPUT_SELECT);
	QCOMPARE(selector.shown(2), BSSH_INPUT_CLICK);

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
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	bssh_session *a = window.registry()->create(named(QStringLiteral("a")));
	bssh_session *b = window.registry()->create(named(QStringLiteral("b")));
	window.adopt_session(a);
	window.adopt_session(b);
	window.split_with(a);
	QCoreApplication::processEvents();

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	QCOMPARE(panes->views().size(), 2);

	/*
	 * The default reached both panes without anyone touching a drum: a
	 * freshly built view starts from its own default, so the window has
	 * to tell it the window's mode after every rebuild (sec 8.7).
	 */
	for (bssh_terminal_view *view : panes->views()) QCOMPARE(view->input_mode(), BSSH_INPUT_SCROLL);

	/*
	 * Chosen on the focused pane's drum, it reaches the OTHER pane too:
	 * the mode is the window's (sec 8.7a), and the corner that chose it
	 * is only where it is shown.
	 */
	bssh_terminal_view *focused = panes->focused_view();
	QVERIFY(focused && focused->corner());
	bssh_terminal_view *other = panes->views().first() == focused ? panes->views().last()
	                                                                : panes->views().first();
	QAbstractButton *select = focused->corner()->selector()
	        ->findChild<QAbstractButton *>(QStringLiteral("input_select"));
	QVERIFY(select);
	select->click();

	QCOMPARE(focused->input_mode(), BSSH_INPUT_SELECT);
	QVERIFY2(other->input_mode() == BSSH_INPUT_SELECT,
	          "the mode chosen in one pane never reached the other");
	QVERIFY2(other->corner()->selector()->mode() == BSSH_INPUT_SELECT,
	          "the other pane's drum still shows the old mode");

	/* Shared registry: leave it as it was found (sec 8.4). */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::hidden_bars_are_reached_from_the_corner() {
	/*
	 * The holder: "if we hide the tab bar or the top menu bar they also
	 * get buttons there?" (sec 8.7a). Hiding a bar must not hide what it
	 * reached: the pane's tabs and the menus are in the focused pane's
	 * corner while their bars are hidden.
	 */
	const forced_touch_t desktop(0);
	struct restore_t {
		bssh_app_settings::tab_position_t tabs = bssh_app_settings().tab_position();
		bool menu = bssh_app_settings().menu_bar_shown();
		~restore_t() {
			bssh_app_settings().set_tab_position(tabs);
			bssh_app_settings().set_menu_bar_shown(menu);
		}
	} restore;
	bssh_app_settings().set_tab_position(bssh_app_settings::TABS_HIDDEN);
	bssh_app_settings().set_menu_bar_shown(false);

	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	window.activateWindow();
	QVERIFY(QTest::qWaitForWindowActive(&window));
	const registry_left_as_found_t tidy(window.registry());
	bssh_session *a = window.registry()->create(named(QStringLiteral("alpha")));
	bssh_session *b = window.registry()->create(named(QStringLiteral("beta")));
	window.adopt_session(a);
	window.adopt_session(b);
	QCoreApplication::processEvents();

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	QCOMPARE(panes->views().size(), 1);
	bssh_terminal_view *view = panes->focused_view();
	QVERIFY(view && view->corner());
	QTRY_VERIFY(view->corner()->isVisible());
	bssh_session *front = panes->focused_session();
	bssh_session *behind = front == a ? b : a;

	/* No bar on screen, and the corner counts the tabs instead. */
	QWidget *bar = panes->bar_for(front);
	QVERIFY2(!bar || !bar->isVisible(), "the tab bar shows while hidden");
	QToolButton *tabs = view->corner()->tabs_button();
	QVERIFY(tabs);
	QVERIFY2(tabs->isVisible(), "no tabs button with the tab bar hidden");
	QCOMPARE(tabs->text(), QStringLiteral("tabs 2"));

	/* Its list names both, the one in front marked, and a pick brings the
	 * other one there. */
	QStringList listed;
	bool front_marked = false;
	QTimer::singleShot(0, [&] {
		QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (!menu) return;
		QAction *pick = nullptr;
		for (QAction *action : menu->actions()) {
			if (action->isSeparator()) continue;
			listed << action->text();
			if (action->text() == front->title()) front_marked = action->isChecked();
			if (action->text() == behind->title()) pick = action;
		}
		if (pick) pick->trigger();
		menu->close();
	});
	tabs->click();
	QCoreApplication::processEvents();
	QVERIFY2(listed.contains(front->title()) && listed.contains(behind->title()),
	          qPrintable(listed.join(QStringLiteral(", "))));
	QVERIFY(listed.contains(QStringLiteral("New Tab")));
	QVERIFY2(front_marked, "the tab in front is not marked");
	QTRY_COMPARE(panes->focused_session(), behind);

	/* The menu bar hidden: its menus are behind the corner's button, and
	 * so is the way back to it. */
	QMenuBar *menu_bar = window.findChild<QMenuBar *>();
	QVERIFY(menu_bar);
	QVERIFY2(menu_bar->isHidden(), "the menu bar shows while hidden");
	/*
	 * The offscreen platform leaves no window active once a popup has
	 * closed, where a desktop's window manager keeps it so (seen under
	 * twin); the corner shows only on an active window, so it is made
	 * active again here.
	 */
	window.activateWindow();
	QVERIFY(QTest::qWaitForWindowActive(&window));
	view = panes->focused_view();
	QTRY_VERIFY(view->corner() && view->corner()->isVisible());
	QToolButton *menus = view->corner()->menu_button();
	QVERIFY2(menus->isVisible(), "no menu button with the menu bar hidden");
	/* At the top, just under the grip -- the holder's placement -- not
	 * under the drum and the tmux button. */
	QVERIFY2(menus->geometry().top() < view->corner()->selector()->geometry().top(),
	          "the menu button is under the drum, not under the grip");

	QStringList titles;
	QTimer::singleShot(0, [&] {
		QMenu *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
		if (!menu) return;
		QAction *back = nullptr;
		for (QAction *action : menu->actions()) {
			if (action->menu()) titles << action->text();
			if (action->objectName() == QStringLiteral("show_menu_bar")) back = action;
		}
		if (back) back->trigger();
		menu->close();
	});
	menus->click();
	QCoreApplication::processEvents();
	QVERIFY2(titles.contains(QStringLiteral("&File")) && titles.contains(QStringLiteral("&View")),
	          qPrintable(titles.join(QStringLiteral(", "))));
	QVERIFY2(!menu_bar->isHidden(), "Show Menu Bar did not bring it back");
	QVERIFY(bssh_app_settings().menu_bar_shown());
	window.activateWindow();
	QVERIFY(QTest::qWaitForWindowActive(&window));
	QTRY_VERIFY2(!view->corner()->menu_button()->isVisible(),
	              "the menu button stayed with the menu bar shown");
}

void layout_test::the_bar_side_and_the_tab_bands_are_where_the_pointer_is() {
	/* The handle: whichever side is nearest, as a share of the area. */
	const QSize area(800, 400);
	QCOMPARE(bssh_bar_side_at(QPoint(10, 200), area), Qt::LeftEdge);
	QCOMPARE(bssh_bar_side_at(QPoint(790, 200), area), Qt::RightEdge);
	QCOMPARE(bssh_bar_side_at(QPoint(400, 10), area), Qt::TopEdge);
	QCOMPARE(bssh_bar_side_at(QPoint(400, 390), area), Qt::BottomEdge);
	QCOMPARE(bssh_bar_side_at(QPoint(300, 370), area), Qt::BottomEdge);

	/*
	 * The drop: a band along the edge is a tab zone, the rest of the edge
	 * zone splits as before, and the middle is the middle.
	 */
	typedef bssh_layout_manager layout;
	const int band = 20;
	QCOMPARE(bssh_drop_zone_at(QPoint(5, 200), area, band), BSSH_DROP_TAB_BASE + layout::EDGE_LEFT);
	QCOMPARE(bssh_drop_zone_at(QPoint(60, 200), area, band), int(layout::EDGE_LEFT));
	QCOMPARE(bssh_drop_zone_at(QPoint(795, 200), area, band), BSSH_DROP_TAB_BASE + layout::EDGE_RIGHT);
	QCOMPARE(bssh_drop_zone_at(QPoint(400, 3), area, band), BSSH_DROP_TAB_BASE + layout::EDGE_TOP);
	QCOMPARE(bssh_drop_zone_at(QPoint(400, 398), area, band), BSSH_DROP_TAB_BASE + layout::EDGE_BOTTOM);
	QCOMPARE(bssh_drop_zone_at(QPoint(400, 200), area, band), int(layout::EDGE_MIDDLE));
}

void layout_test::the_tab_bar_is_dragged_to_another_side() {
	/*
	 * The holder: "i want the tab bar to have a handle so it can be drag n
	 * dropped to move to one of the 3 other sides" (sec 8.7a).
	 */
	const forced_touch_t desktop(0);
	struct restore_t {
		bssh_app_settings::tab_position_t tabs = bssh_app_settings().tab_position();
		~restore_t() { bssh_app_settings().set_tab_position(tabs); }
	} restore;
	bssh_app_settings().set_tab_position(bssh_app_settings::TABS_TOP);

	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	const registry_left_as_found_t tidy(window.registry());
	bssh_session *alpha = window.registry()->create(named(QStringLiteral("alpha")));
	window.adopt_session(alpha);
	window.adopt_session(window.registry()->create(named(QStringLiteral("beta"))));
	QCoreApplication::processEvents();

	/* And a terminal of its own beside the pane with the bar. */
	bssh_session *gamma = window.registry()->create(named(QStringLiteral("gamma")));
	window.adopt_session(gamma);
	QCoreApplication::processEvents();
	window.drop_session_on(gamma, window.layout_manager()->front_of(gamma), bssh_layout_manager::EDGE_RIGHT);
	QCoreApplication::processEvents();

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	QCOMPARE(panes->bar_edge(), Qt::TopEdge);
	const auto handle_now = [&window, alpha]() -> QWidget * {
		bssh_pane_bar *row = window.bar_for(window.layout_manager()->front_of(alpha));
		return row ? row->findChild<QWidget *>(QStringLiteral("pane_bar_handle")) : nullptr;
	};
	QWidget *handle = handle_now();
	QVERIFY(handle);
	QTRY_VERIFY(handle->isVisible());
	bssh_terminal_view *own = panes->view_for(window.layout_manager()->front_of(alpha));
	bssh_terminal_view *other = panes->view_for(gamma);
	QVERIFY(own && other && own != other);

	/*
	 * Over the other terminal: the bar is its own pane's, and nothing is
	 * lit there or anywhere -- the holder: "a tab bar shouldn't be movable
	 * to other terminals". Let go there, nothing moves.
	 */
	{
		const QPoint start = handle->rect().center();
		const QPoint over_other = handle->mapFromGlobal(
		        other->mapToGlobal(QPoint(8, other->height() / 2)));
		QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
		QTest::mouseMove(handle, start + QPoint(QApplication::startDragDistance() * 2, 0));
		QTest::mouseMove(handle, over_other);
		for (bssh_terminal_view *view : panes->views())
			QVERIFY2(view->drop_zone() == -1, "a pane is lit by another pane's bar");
		QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, over_other);
		QCoreApplication::processEvents();
		QCoreApplication::processEvents();
		QCOMPARE(panes->bar_edge(), Qt::TopEdge);
		handle = handle_now();
		QVERIFY(handle);
		own = panes->view_for(window.layout_manager()->front_of(alpha));
		other = panes->view_for(gamma);
		QVERIFY(own && other);
	}

	/* Pressed, and moved toward the middle of its own pane's left edge. */
	const QPoint start = handle->rect().center();
	const QPoint left_middle = handle->mapFromGlobal(
	        own->mapToGlobal(QPoint(8, own->height() / 2)));
	QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, start);
	QTest::mouseMove(handle, start + QPoint(QApplication::startDragDistance() * 2, 0));
	QTest::mouseMove(handle, left_middle);

	/* The side it would go to is lit, along the left of its own pane. */
	QCOMPARE(own->drop_zone(), BSSH_DROP_TAB_BASE + int(bssh_layout_manager::EDGE_LEFT));
	QCOMPARE(other->drop_zone(), -1);

	QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, left_middle);
	QTRY_COMPARE(panes->bar_edge(), Qt::LeftEdge);
	QCOMPARE(bssh_app_settings().tab_position(), bssh_app_settings::TABS_LEFT);
	for (bssh_terminal_view *after : panes->views())
		QVERIFY2(after->drop_zone() == -1, "a pane stayed lit after the drop");
}

void layout_test::a_terminal_dropped_on_a_side_band_makes_a_tab_bar_there() {
	/*
	 * The holder: "if there is no tab bar then a terminal should be
	 * droppable on any one of the four sides to create a tab bar there in
	 * addition to the other drop zones we already have" (sec 8.7a).
	 */
	const forced_touch_t desktop(0);
	struct restore_t {
		bssh_app_settings::tab_position_t tabs = bssh_app_settings().tab_position();
		~restore_t() { bssh_app_settings().set_tab_position(tabs); }
	} restore;
	bssh_app_settings().set_tab_position(bssh_app_settings::TABS_TOP);

	bssh_main_window window;
	window.resize(1000, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	const registry_left_as_found_t tidy(window.registry());
	bssh_session *a = window.registry()->create(named(QStringLiteral("alpha")));
	bssh_session *b = window.registry()->create(named(QStringLiteral("beta")));
	window.adopt_session(a);
	window.adopt_session(b);
	window.split_with(a);
	QCoreApplication::processEvents();

	bssh_layout_manager *layout = window.findChild<bssh_layout_manager *>();
	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(layout && panes);
	QCOMPARE(layout->intent().size(), 2);
	QCOMPARE(layout->tabs(a).size(), 1);

	/* a's pane shows no bar, so its right band is a tab zone. */
	const int right_band = BSSH_DROP_TAB_BASE + bssh_layout_manager::EDGE_RIGHT;
	QCOMPARE(window.drop_zone_for(b, a, right_band), right_band);

	window.drop_session_on(b, a, right_band);
	QCoreApplication::processEvents();
	QCOMPARE(layout->intent().size(), 1);
	QCOMPARE(layout->tabs(layout->intent().first()).size(), 2);
	QTRY_COMPARE(panes->bar_edge(), Qt::RightEdge);
	QCOMPARE(bssh_app_settings().tab_position(), bssh_app_settings::TABS_RIGHT);

	/*
	 * Now the pane HAS a bar: a band there is just the edge it lies on, a
	 * split, as it was before bands existed.
	 */
	bssh_session *c = window.registry()->create(named(QStringLiteral("gamma")));
	window.adopt_session(c);
	QCoreApplication::processEvents();

	/* c arrives as a third tab of that pane; an edge drop of it on its own
	 * pane splits it off into a pane of its own, beside the barred one. */
	window.drop_session_on(c, c, bssh_layout_manager::EDGE_RIGHT);
	QCoreApplication::processEvents();
	QCOMPARE(layout->intent().size(), 2);
	bssh_session *front = layout->front_of(a);
	QVERIFY(front);
	QVERIFY2(!layout->tabs(front).contains(c), "c is still one of the barred pane's tabs");
	QCOMPARE(layout->bar_tabs(front).size(), 2);
	const int top_band = BSSH_DROP_TAB_BASE + bssh_layout_manager::EDGE_TOP;
	QCOMPARE(window.drop_zone_for(c, front, top_band), int(bssh_layout_manager::EDGE_TOP));
}

void layout_test::a_sessions_trouble_is_written_over_its_pane() {
	/*
	 * The holder: "the status bar showing connected makes no sense on a
	 * local terminal, and even with ssh, maybe we should only indicate on
	 * the main terminal display that it is disconnected/retrying etc."
	 * (sec 8.7a). Nothing while all is well; the trouble over the pane.
	 */
	QList<bssh_fake_ssh_session *> fakes;
	const bssh_factory_reset_t factory_reset;
	bssh_ssh_session::set_factory([&fakes](QObject *parent) -> bssh_ssh_session * {
		fakes.append(new bssh_fake_ssh_session(parent));
		return fakes.last();
	});

	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	const registry_left_as_found_t tidy(window.registry());

	bssh_profile_t host = named(QStringLiteral("remote"));
	host.host = QStringLiteral("remote.example");
	bssh_session *session = window.registry()->create(host);
	window.adopt_session(session);
	QCoreApplication::processEvents();
	bssh_terminal_view *view = window.panes()->view_for(session);
	QVERIFY(view);

	session->connect_to_host();
	QTRY_VERIFY2(!view->status_note().isEmpty(), "nothing says the session is connecting");

	fakes.last()->simulate_key_auth_success();
	QTRY_VERIFY2(view->status_note().isEmpty(),
	              qPrintable(QStringLiteral("a connected session still says \"%1\"").arg(view->status_note())));

	fakes.last()->simulate_connection_lost();
	QTRY_VERIFY2(!view->status_note().isEmpty(), "a lost connection says nothing over its pane");
	const QString lost = view->status_note();
	QVERIFY2(lost.contains(QStringLiteral("Reconnect")) || lost.contains(QStringLiteral("Disconnect"))
	                 || lost.contains(QStringLiteral("failed")),
	          qPrintable(lost));

	/* The status line is hidden unless chosen. */
	QLabel *status = window.findChild<QLabel *>(QStringLiteral("status_line"));
	QVERIFY(status);
	QVERIFY2(!status->isVisible(), "the status line shows by default");

	/* And it follows the setting in the window it was changed in, not
	 * only after a restart -- the holder found it did not. */
	{
		struct restore_t {
			bssh_app_settings::status_line_t status = bssh_app_settings().status_line();
			~restore_t() { bssh_app_settings().set_status_line(status); }
		} restore;
		bssh_app_settings().set_status_line(bssh_app_settings::STATUS_LINE_SESSION);
		window.apply_device_settings();
		QVERIFY2(status->isVisible(), "turning the status line on shows nothing until a restart");
		bssh_app_settings().set_status_line(bssh_app_settings::STATUS_LINE_OFF);
		window.apply_device_settings();
		QVERIFY2(!status->isVisible(), "turning the status line off leaves it until a restart");
	}

	/* A local shell: nothing over its pane, whatever its state. */
	bssh_session *local = window.registry()->create(bssh_profile_t::local_shell());
	window.adopt_session(local);
	window.split_with(session);
	QCoreApplication::processEvents();
	local->connect_to_host();
	QTest::qWait(100);
	bssh_terminal_view *local_view = window.panes()->view_for(local);
	if (local_view)
		QVERIFY2(local_view->status_note().isEmpty(),
		          qPrintable(QStringLiteral("a local shell says \"%1\"").arg(local_view->status_note())));
	local->disconnect_from_host();
}

void layout_test::another_processs_drag_is_previewed_here() {
	/*
	 * Every window is its own process (sec 8.4), so a terminal dragged from
	 * one is over another's windows, which could not see the drag and drew
	 * nothing -- the holder: "there are some issues with the visual
	 * indicators for the drag and drop of new tab bars". The dragging
	 * process says where it is; this one draws the zone, as for a drag of
	 * its own, and takes it down when told (sec 8.7a).
	 */
	const forced_touch_t desktop(0);
	struct restore_t {
		bssh_app_settings::tab_position_t tabs = bssh_app_settings().tab_position();
		~restore_t() { bssh_app_settings().set_tab_position(tabs); }
	} restore;
	bssh_app_settings().set_tab_position(bssh_app_settings::TABS_TOP);

	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	const registry_left_as_found_t tidy(window.registry());
	bssh_session *only = window.registry()->create(named(QStringLiteral("only")));
	window.adopt_session(only);
	QCoreApplication::processEvents();
	bssh_terminal_view *view = window.panes()->view_for(only);
	QVERIFY(view);

	/* Over the left band of a pane with no bar: the tab zone, lit. */
	const QPoint band = view->mapToGlobal(QPoint(3, view->height() / 2));
	bssh_main_window::preview_arrival(band, true);
	QCOMPARE(view->drop_zone(), BSSH_DROP_TAB_BASE + int(bssh_layout_manager::EDGE_LEFT));

	/* Over the middle: the middle -- a swap -- lit instead. */
	bssh_main_window::preview_arrival(view->mapToGlobal(view->rect().center()), true);
	QCOMPARE(view->drop_zone(), int(bssh_layout_manager::EDGE_MIDDLE));

	/* Left, or ended without a drop: nothing lit. */
	bssh_main_window::preview_arrival(QPoint(), false);
	QCOMPARE(view->drop_zone(), -1);
}

void layout_test::only_the_focused_pane_shows_its_corner() {
	/*
	 * The holder: "we keep all of the annoying things that can cover the
	 * terminal in one place, and hidden while that window is inactive"
	 * (sec 8.7a). Every other pane shows its text whole.
	 */
	bssh_main_window window;
	window.resize(900, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	const registry_left_as_found_t tidy(window.registry());
	bssh_session *a = window.registry()->create(named(QStringLiteral("a")));
	bssh_session *b = window.registry()->create(named(QStringLiteral("b")));
	window.adopt_session(a);
	window.adopt_session(b);
	window.split_with(a);
	QCoreApplication::processEvents();

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	QCOMPARE(panes->views().size(), 2);

	for (bssh_session *focus : { a, b, a }) {
		panes->focus_session(focus);
		QCoreApplication::processEvents();
		int shown = 0;
		for (bssh_terminal_view *view : panes->views()) {
			QVERIFY(view->corner());
			const bool expected = view == panes->focused_view();
			QVERIFY2(view->corner()->isVisible() == expected,
			          expected ? "the focused pane has no corner"
			                   : "an inactive pane shows its corner");
			shown += view->corner()->isVisible() ? 1 : 0;
		}
		QCOMPARE(shown, 1);
	}
}

void layout_test::the_drum_steps_with_the_wheel_and_a_drag_and_wraps() {
	/*
	 * "We can scroll between the modes easily as that is a very common
	 * need" (sec 8.7a). Three modes stay put in their order and the wheel
	 * moves the mark along them, stopping at the ends; the column's five
	 * turn the drum, wrapping, so every mode is two steps away.
	 */
	const forced_touch_t desktop(0);
	bssh_input_selector drum;
	drum.resize(drum.sizeHint());
	drum.show();
	QVERIFY(QTest::qWaitForWindowExposed(&drum));
	QCOMPARE(drum.mode(), BSSH_INPUT_CLICK);
	QSignalSpy spy(&drum, &bssh_input_selector::mode_changed);

	const QPoint middle(drum.width() / 2, drum.height() / 2);
	const auto wheel = [&](int delta) {
		QWheelEvent event(middle, drum.mapToGlobal(middle), QPoint(), QPoint(0, delta),
		                  Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
		QCoreApplication::sendEvent(&drum, &event);
	};
	const int row = drum.row_height();

	/* Fixed: toward you goes up the list, and the top is the end. */
	wheel(120);
	QCOMPARE(drum.mode(), BSSH_INPUT_SELECT);
	wheel(120);
	QCOMPARE(drum.mode(), BSSH_INPUT_SCROLL);
	wheel(120);
	QCOMPARE(drum.mode(), BSSH_INPUT_SCROLL);
	QCOMPARE(drum.shown(0), BSSH_INPUT_SCROLL);
	QCOMPARE(spy.count(), 2);

	/*
	 * Half a notch twice is one step, not none and not two -- both ways,
	 * since a threshold is kept per direction.
	 */
	wheel(-60);
	QCOMPARE(drum.mode(), BSSH_INPUT_SCROLL);
	wheel(-60);
	QCOMPARE(drum.mode(), BSSH_INPUT_SELECT);
	wheel(60);
	QCOMPARE(drum.mode(), BSSH_INPUT_SELECT);
	wheel(60);
	QCOMPARE(drum.mode(), BSSH_INPUT_SCROLL);
	wheel(-120);
	QCOMPARE(drum.mode(), BSSH_INPUT_SELECT);

	/* A tap on any row chooses it, the middle one included. */
	QTest::mouseClick(&drum, Qt::LeftButton, Qt::NoModifier, QPoint(middle.x(), row * 5 / 2));
	QCOMPARE(drum.mode(), BSSH_INPUT_CLICK);
	QTest::mouseClick(&drum, Qt::LeftButton, Qt::NoModifier, QPoint(middle.x(), row * 3 / 2));
	QCOMPARE(drum.mode(), BSSH_INPUT_SELECT);

	/* Five: the drum turns, the current one in the middle, wrapping. */
	drum.set_column_available(true);
	QVERIFY(drum.rotates());
	QCOMPARE(drum.shown(1), BSSH_INPUT_SELECT);
	drum.set_mode(BSSH_INPUT_SCROLL);
	QCOMPARE(drum.shown(0), BSSH_INPUT_COLUMN_SCROLL);
	QCOMPARE(drum.shown(1), BSSH_INPUT_SCROLL);
	QCOMPARE(drum.shown(2), BSSH_INPUT_SELECT);
	wheel(120);
	QCOMPARE(drum.mode(), BSSH_INPUT_COLUMN_SCROLL);

	/* A drag down by one row brings the mode above into the middle. */
	const bssh_input_mode_t above = drum.shown(0);
	QTest::mousePress(&drum, Qt::LeftButton, Qt::NoModifier, middle);
	QTest::mouseMove(&drum, middle + QPoint(0, row + 1));
	QTest::mouseRelease(&drum, Qt::LeftButton, Qt::NoModifier, middle + QPoint(0, row + 1));
	QCOMPARE(drum.mode(), above);
}

void layout_test::the_move_state_is_offered_only_in_a_column() {
	bssh_input_selector selector;

	QAbstractButton *move =
	        selector.findChild<QAbstractButton *>(QStringLiteral("input_move"));
	QVERIFY(move);

	/* No column to reorder yet, so the state is not offered. */
	QVERIFY2(!selector.available().contains(BSSH_INPUT_MOVE),
	          "Move is offered before there is a column to reorder");

	selector.set_column_available(true);
	QVERIFY2(selector.available().contains(BSSH_INPUT_MOVE),
	          "Move is not offered once a column exists");

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
	QVERIFY2(!selector.available().contains(BSSH_INPUT_MOVE),
	          "Move stayed on offer after its column went");
	QVERIFY2(move->isHidden(), "the Move row is still on the drum");
	QCOMPARE(selector.mode(), bssh_default_input_mode());
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
	/* Three panes, made as panes: a session opened in a column is a tab (sec 8.3). */
	window.adopt_session(a);
	window.drop_session_on(b, a, bssh_layout_manager::EDGE_BOTTOM);
	window.drop_session_on(c, b, bssh_layout_manager::EDGE_BOTTOM);

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
	/*
	 * A new session joins the focused pane -- c's, the one just moved --
	 * at its front, and the order the user made stays as it was.
	 */
	bssh_session *d = reg->create(named(QStringLiteral("d")));
	window.adopt_session(d);
	QCOMPARE(layout->intent(), (QList<bssh_session *>{ a, d, b }));
	QCOMPARE(layout->tabs(d), (QList<bssh_session *>{ c, d }));

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
	window.drop_session_on(b, a, bssh_layout_manager::EDGE_BOTTOM);

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
	QVERIFY2(!selector.available().contains(BSSH_INPUT_COLUMN_SCROLL),
	          "Pan is offered before there is a column");
	selector.set_column_available(true);
	QVERIFY2(selector.available().contains(BSSH_INPUT_COLUMN_SCROLL),
	          "Pan is not offered once a column exists");

	pan->click();
	QCOMPARE(selector.mode(), BSSH_INPUT_COLUMN_SCROLL);

	QSignalSpy spy(&selector, &bssh_input_selector::mode_changed);
	selector.set_column_available(false);
	QVERIFY2(!selector.available().contains(BSSH_INPUT_COLUMN_SCROLL),
	          "Pan stayed on offer after its column went");
	QVERIFY2(pan->isHidden(), "the Pan row is still on the drum");
	QCOMPARE(selector.mode(), bssh_default_input_mode());
	QCOMPARE(spy.count(), 1);
}

void layout_test::the_focused_pane_stays_on_screen_in_a_column() {
	/*
	 * Sec 8.7a: the mode drum is in the focused pane's corner, so in a
	 * scrolling column the focused pane must not be off screen. Seen on an
	 * emulator turned sideways: the column showed the top pane, focus was
	 * on the one below, and no mode was on screen anywhere.
	 */
	const forced_touch_t touch(1);

	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(400);
	layout.set_arrangement(bssh_layout_manager::COLUMN);
	QList<bssh_session *> many;
	for (int i = 0; i < 6; ++i)
		many.append(registry.create(named(QStringLiteral("s%1").arg(i))));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(400, 400);
	container.set_column_visible(2);
	layout.set_intent(many);
	QCoreApplication::processEvents();
	QVERIFY(QTest::qWaitForWindowExposed(&container));

	QScrollArea *scroll = container.findChild<QScrollArea *>();
	QVERIFY(scroll);
	QScrollBar *bar = scroll->verticalScrollBar();
	QVERIFY2(bar->maximum() > 0, "the column does not overflow");

	/* Focus the first pane, then pan to the bottom and let it settle:
	 * focus follows to a pane that is on screen. */
	container.focus_session(many.first());
	bar->setValue(0);
	QCoreApplication::processEvents();
	bar->setValue(bar->maximum());
	container.settle_column_scroll();
	QCoreApplication::processEvents();
	QVERIFY2(container.focused_session() != many.first(),
	          "focus stayed on a pane panned out of view");
	bssh_terminal_view *focused = container.focused_view();
	QVERIFY(focused);
	const QRect seen(QPoint(0, bar->value()), scroll->viewport()->size());
	QVERIFY2(seen.intersects(QRect(focused->mapTo(scroll->widget(), QPoint(0, 0)), focused->size())),
	          "the pane focus went to is not on screen either");

	/* Fewer on screen -- turned, or shrunk -- with the focused pane out of
	 * view: brought back once the layout settles. */
	container.focus_session(many.first());
	QCoreApplication::processEvents();
	bar->setValue(bar->maximum());
	container.set_column_visible(1);
	QTRY_VERIFY2(container.focused_in_view(), "the focused pane was left off screen");
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
		/*
		 * The dialog behind the preset editor's Features button (sec
		 * 7.1). Fourteen rows at a touch height is taller than a phone
		 * by construction, so it carries a scroll area -- and this is
		 * the assertion that says the scroll area is doing the job
		 * rather than the dialog merely being long.
		 */
		const QString nothing_off;
		bssh_term_features_editor features(nothing_off);
		check_fits(QStringLiteral("terminal features"), &features);
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
		/*
		 * The prompt sizes itself to the screen's width when shown, so the
		 * screen is a phone's here; offscreen it would be the test
		 * screen's, and the measurement would be of an 800px prompt.
		 */
		const forced_screen_t phone(QSize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP));
		bssh_password_prompt prompt(QStringLiteral("nabbe.testing@build.internal.example.com:2299"),
		                             true);
		check_fits(QStringLiteral("password prompt"), &prompt);
	}
	{
		/*
		 * sec 8.7's selector, which is on screen the whole time a phone
		 * is showing a terminal -- so it is the one surface here a user
		 * cannot navigate away from. It was not in this sweep and has
		 * no desktop form to be noticed on, the same two properties
		 * that left the key row unmeasured until it asked for 652px of
		 * a 360px screen.
		 *
		 * Not sideways: a row of five states that scrolls is a row
		 * whose last state is hidden, and sec 8.7 wants all of them
		 * visible at once.
		 */
		bssh_input_selector selector;

		/*
		 * The population first, because a bar that built no buttons
		 * would fit a phone beautifully and mean nothing -- the gate
		 * over an empty list, wearing this sweep's clothes. Five
		 * states, and two of them (Move and Pan) exist only where a
		 * column does, so this asks for the three that are always
		 * there plus the two that must at least have been built.
		 */
		const QStringList states = { QStringLiteral("input_scroll"),
		                              QStringLiteral("input_select"),
		                              QStringLiteral("input_click"),
		                              QStringLiteral("input_move"),
		                              QStringLiteral("input_pan") };
		for (const QString &state : states) {
			QVERIFY2(selector.findChild<QAbstractButton *>(state),
			          qPrintable(QStringLiteral("the selector has no %1 button, so "
			                                     "measuring it proves nothing")
			                             .arg(state)));
		}

		check_fits(QStringLiteral("input selector"), &selector);
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
void check_keyboard_shape(sk_catalog::style_t style) {
	const QList<sk_group_t> &groups =
	        sk_catalog::instance().all(style);

	QVERIFY2(groups.size() >= 2,
	          "a side button that walks one group is a button with nothing to do");

	const int rows = groups.first().rows.size();
	QVERIFY2(rows > 0, "a group with no rows is a keyboard with no keys");

	for (const sk_group_t &group : groups) {
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
			for (const sk_key_t &key : group.rows.at(r)) {
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
		QVERIFY2(group.columns <= SK_KEYBOARD_MAX_COLUMNS,
		          "a group wider than SK_KEYBOARD_MAX_COLUMNS is drawn clipped");
	}
}

} /* namespace */

/*
 * The full keyboard answers about ITSELF, which for an hour it did not.
 *
 * sk_keyboard's catalogue lookups took a style that defaulted to
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
	sk_keyboard keyboard(&router);
	keyboard.set_style(sk_catalog::STYLE_FULL);

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
	/* The touch rows' style at all, for the desktop arm below: a literal
	 * "min-height: 48px" stopped matching when the floor was split into
	 * content and padding, and absence of it would then prove nothing. */
	const QString floor = QStringLiteral("QMenu::item");

	{
		const forced_touch_t thumb(1);
		bssh_host_list list(&store);
		QMenu *overflow = list.findChild<QMenu *>();
		QVERIFY2(overflow, "the front door must carry an overflow menu");
		/*
		 * The ROW's height, measured, rather than a min-height read out of
		 * the stylesheet: the floor is the content plus its padding, and
		 * since 2026-10-22 the two are split so a row is 48dp rather than
		 * 48 plus padding. Laid out by sizeHint(), which computes the rows.
		 */
		overflow->sizeHint();
		const int row = overflow->actionGeometry(overflow->actions().first()).height();
		QVERIFY2(row >= BSSH_TOUCH_TARGET_DP,
		          qPrintable(QStringLiteral("touch rows must reach the %1dp floor, "
		                                     "a row is %2 (stylesheet \"%3\")")
		                             .arg(BSSH_TOUCH_TARGET_DP).arg(row)
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
	sk_catalog &catalog = sk_catalog::instance();

	QHash<int, sk_key_t> compact;
	for (const sk_group_t &group :
	     catalog.all(sk_catalog::STYLE_COMPACT)) {
		for (const QList<sk_key_t> &row : group.rows) {
			for (const sk_key_t &key : row) {
				if (key.kind == sk_key_t::TEXT && !compact.contains(key.value)) {
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
	for (const sk_group_t &group :
	     catalog.all(sk_catalog::STYLE_FULL)) {
		if (!group.id.contains(QStringLiteral("main"))) continue;

		for (const QList<sk_key_t> &row : group.rows) {
			for (const sk_key_t &key : row) {
				if (key.kind != sk_key_t::TEXT) continue;
				if (!compact.contains(key.value)) continue;

				const sk_key_t &twin = compact.value(key.value);
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
	QVERIFY(sk_key_cap::needs_own_paint(QStringLiteral("&"), QString()));
	QVERIFY(sk_key_cap::needs_own_paint(QStringLiteral("q"),
	                                       QString(QChar(0x00e4))));

	/*
	 * And the ordinary caps are still the style's, which is the half
	 * that keeps a keyboard looking like its own buttons -- and keeps
	 * the eliding the full board's narrow keys rely on.
	 */
	QVERIFY(!sk_key_cap::needs_own_paint(QStringLiteral("Enter"), QString()));
	QVERIFY(!sk_key_cap::needs_own_paint(QStringLiteral("g"), QString()));

	/*
	 * And the keyboard really does carry one, or the two lines above are
	 * a rule about nothing.
	 */
	bool found = false;
	for (const sk_group_t &group :
	     sk_catalog::instance().all(
	             sk_catalog::STYLE_COMPACT)) {
		for (const QList<sk_key_t> &row : group.rows) {
			for (const sk_key_t &key : row) {
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

	layout_left_as_found_t() : id(sk_keyboard_layout()) {}
	~layout_left_as_found_t() { sk_set_keyboard_layout(id); }
};

/*
 * Every character a style can send, over every modifier combination.
 *
 * Through sk_keyboard_char rather than by reading the stored levels,
 * which is the difference between what a key SENDS and what it holds: a
 * capital comes from toUpper at send time, so a probe reading levels
 * reports every capital as unreachable. One did, during this work, and
 * the false gap looked exactly like a real one.
 */
QSet<uint> every_character(sk_catalog::style_t style) {
	QSet<uint> sent;
	for (const sk_group_t &group :
	     sk_catalog::instance().all(style)) {
		for (const QList<sk_key_t> &row : group.rows) {
			for (const sk_key_t &key : row) {
				if (key.kind != sk_key_t::TEXT) continue;

				for (int modifiers = 0; modifiers < 4; ++modifiers) {
					const QChar character = sk_keyboard_char(
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
	for (const sk_group_t &group :
	     sk_catalog::instance().all(
	             sk_catalog::STYLE_FULL)) {
		for (const QList<sk_key_t> &row : group.rows) {
			for (const sk_key_t &key : row) {
				if (key.kind == sk_key_t::TEXT
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
 * Capitals are excluded because they are not a level: sk_shifted_char
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

	for (const sk_keyboard_layout_t &layout : sk_keyboard_layouts()) {
		QVERIFY2(sk_set_keyboard_layout(layout.id),
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
	QVERIFY(sk_set_keyboard_layout(bssh_app_settings().keyboard_layout()));
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

	for (const sk_keyboard_layout_t &layout : sk_keyboard_layouts()) {
		QVERIFY(sk_set_keyboard_layout(layout.id));

		const QSet<uint> full =
		        every_character(sk_catalog::STYLE_FULL);
		const QSet<uint> compact =
		        every_character(sk_catalog::STYLE_COMPACT);

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

	QVERIFY(sk_set_keyboard_layout(QStringLiteral("us-intl")));
	QCOMPARE(int(sk_catalog::instance()
	                     .all(sk_catalog::STYLE_COMPACT).size()), 3);

	/*
	 * Swedish is the sharper half of the control: it has letters the
	 * ASCII palette does not carry, and they are on AD11 and AC11 where
	 * the letters page already reaches them.
	 */
	QVERIFY(sk_set_keyboard_layout(QStringLiteral("se")));
	QCOMPARE(int(sk_catalog::instance()
	                     .all(sk_catalog::STYLE_COMPACT).size()), 3);

	QVERIFY(sk_set_keyboard_layout(QStringLiteral("de")));
	const QList<sk_group_t> german =
	        sk_catalog::instance().all(
	                sk_catalog::STYLE_COMPACT);
	QCOMPARE(int(german.size()), 4);

	/*
	 * And it is the page that carries the letter rather than merely a
	 * fourth page: the button says what it is for, and the keys send it.
	 */
	QCOMPARE(german.last().button_label, QString(QChar(0x00df)));

	bool eszett = false;
	for (const QList<sk_key_t> &row : german.last().rows) {
		for (const sk_key_t &key : row) {
			if (key.kind != sk_key_t::TEXT) continue;
			for (int modifiers = 0; modifiers < 4; ++modifiers) {
				if (sk_keyboard_char(key, modifiers & 1, modifiers & 2)
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
	QVERIFY(sk_set_keyboard_layout(QStringLiteral("fr")));
	const QList<sk_group_t> french =
	        sk_catalog::instance().all(
	                sk_catalog::STYLE_COMPACT);
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

	QVERIFY(sk_set_keyboard_layout(QStringLiteral("ru")));
	QVERIFY2(!sk_layout_has_latin(), "Russian is being read as a Latin layout");
	QVERIFY2(sk_latin_mode(), "a non-Latin layout did not open in Latin");

	const QSet<uint> opened =
	        every_character(sk_catalog::STYLE_COMPACT);
	for (char letter = 'a'; letter <= 'z'; ++letter) {
		QVERIFY2(opened.contains(uint(letter)),
		          qPrintable(QStringLiteral("Russian opens without %1")
		                             .arg(QChar(letter))));
	}

	/* And the script it was chosen for is one tap away. */
	sk_set_latin_mode(false);
	const QSet<uint> script =
	        every_character(sk_catalog::STYLE_COMPACT);
	QVERIFY2(script.contains(0x0439),
	          "Russian cannot type short-i with Latin mode off");
	QVERIFY2(!script.contains(uint('q')),
	          "Latin mode off still draws a Latin letter block");

	sk_set_latin_mode(true);
	QVERIFY(every_character(sk_catalog::STYLE_COMPACT)
	                .contains(uint('q')));

	/*
	 * A layout change resets it. Carrying the mode across would mean
	 * choosing Greek and getting whichever script the last layout was
	 * left showing.
	 */
	sk_set_latin_mode(false);
	QVERIFY(sk_set_keyboard_layout(QStringLiteral("gr")));
	QVERIFY2(sk_latin_mode(), "the toggle survived a layout change");
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

	QVERIFY(sk_set_keyboard_layout(QStringLiteral("se")));
	{
		sk_keyboard keyboard(&router);
		keyboard.set_style(sk_catalog::STYLE_COMPACT);
		QToolButton *toggle =
		        keyboard.findChild<QToolButton *>(QStringLiteral("keyboard-script"));
		QVERIFY2(toggle, "the toggle was never built");
		QVERIFY2(toggle->isHidden(), "Swedish is offered a script toggle");
	}

	QVERIFY(sk_set_keyboard_layout(QStringLiteral("ru")));
	{
		sk_keyboard keyboard(&router);
		keyboard.set_style(sk_catalog::STYLE_COMPACT);
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
		QVERIFY2(!sk_latin_mode(), "pressing the toggle changed nothing");
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

	bssh_session *first = registry->create(named(QStringLiteral("first")));
	bssh_session *second = registry->create(named(QStringLiteral("second")));
	bssh_session *third = registry->create(named(QStringLiteral("third")));
	window.adopt_session(first);
	window.adopt_session(second);
	window.adopt_session(third);

	/* One pane, its three tabs in the order they arrived (sec 8.3's bar per pane). */
	QCOMPARE(window.layout_manager()->tabs(first), QList<bssh_session *>({first, second, third}));

	QMenu *at_first = window.build_tab_menu(first, &window);
	QVERIFY2(at_first, "the first tab has no menu");
	QAction *first_left = at_first->findChild<QAction *>(QStringLiteral("tab_move_left"));
	QAction *first_right = at_first->findChild<QAction *>(QStringLiteral("tab_move_right"));
	QVERIFY2(first_left && first_right, "the tab menu offers no way to reorder");
	QVERIFY2(!first_left->isEnabled(), "the first tab was offered a move left");
	QVERIFY2(first_right->isEnabled(), "a tab with a tab beside it was refused");
	delete at_first;

	QMenu *at_last = window.build_tab_menu(third, &window);
	QVERIFY2(at_last, "the last tab has no menu, so there is no last tab");
	QVERIFY(at_last->findChild<QAction *>(QStringLiteral("tab_move_left"))->isEnabled());
	QVERIFY2(!at_last->findChild<QAction *>(QStringLiteral("tab_move_right"))->isEnabled(),
	          "the last tab was offered a move right");
	delete at_last;

	/*
	 * And the move itself lands, both ways, in the pane's own order and
	 * without touching the registry's -- which is what keeps another
	 * window's bar out of it.
	 */
	const QList<bssh_session *> registry_before = registry->sessions();

	window.move_tab(first, 1);
	QCOMPARE(window.layout_manager()->tabs(first), QList<bssh_session *>({second, first, third}));
	QMenu *moved = window.build_tab_menu(second, &window);
	QVERIFY(moved);
	QVERIFY2(!moved->findChild<QAction *>(QStringLiteral("tab_move_left"))->isEnabled(),
	          "the first tab must still refuse a move left after a swap");
	delete moved;
	QCOMPARE(registry->sessions(), registry_before);

	window.move_tab(first, -1);
	QCOMPARE(window.layout_manager()->tabs(first), QList<bssh_session *>({first, second, third}));
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

	const int tabs = window.panes()->focused_session()
	        ? int(window.layout_manager()->tabs(window.panes()->focused_session()).size()) : 0;
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

	sk_keyboard keyboard(&router);
	keyboard.set_style(sk_catalog::STYLE_COMPACT);

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
	const forced_touch_t desktop(0);

	bssh_terminal_view view;
	view.set_pane_handle(true);
	view.set_corner_active(true);
	view.resize(600, 400);
	view.show();
	QVERIFY(QTest::qWaitForWindowExposed(&view));

	/*
	 * The grip is the top slot of the pane's corner (sec 8.7a), a widget
	 * of its own, so it is pressed where it is rather than on the view
	 * under it.
	 */
	QVERIFY2(!view.pane_grip_rect().isNull(), "a 600x400 pane should have room for a handle");
	QVERIFY(view.corner());
	QWidget *grip = view.corner()->findChild<QWidget *>(QStringLiteral("pane_grip"));
	QVERIFY(grip);
	QVERIFY(grip->isVisible());
	const QPoint at = grip->rect().center();

	QSignalSpy asked(&view, &bssh_terminal_view::pane_drag_requested);

	QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier, at);
	QCOMPARE(asked.count(), 0);

	/* Short of the threshold: still not a drag. */
	QTest::mouseMove(grip, at + QPoint(2, 0));
	QCOMPARE(asked.count(), 0);

	QTest::mouseMove(grip, at + QPoint(QApplication::startDragDistance() * 3, 40));
	QCOMPARE(asked.count(), 1);

	QTest::mouseRelease(grip, Qt::LeftButton, Qt::NoModifier, at + QPoint(60, 40));
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
void layout_test::a_window_whose_last_session_goes_to_another_process_closes() {
	/*
	 * The holder: "when moving the last terminal out of a process it
	 * stays". Every window is its own process (sec 8.4), so the window a
	 * terminal is dragged out of is the last of its process, which the
	 * empty-window rule spares. Given to another BeerSSH, the terminal
	 * lives on there, and this window goes.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	{
		bssh_main_window window;
		window.resize(800, 500);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		bssh_session *only = window.registry()->create(named(QStringLiteral("only")));
		window.adopt_session(only);
		QCoreApplication::processEvents();
		QVERIFY(window.isVisible());

		window.registry()->give_away(only);
		QTRY_VERIFY2(!window.isVisible(), "a window emptied by a hand-off to another process stayed");
	}

	/* Released any other way, the last window stays, as it always has. */
	bssh_main_window kept;
	kept.resize(800, 500);
	kept.show();
	QVERIFY(QTest::qWaitForWindowExposed(&kept));
	bssh_session *other = kept.registry()->create(named(QStringLiteral("other")));
	kept.adopt_session(other);
	QCoreApplication::processEvents();
	kept.registry()->release(other);
	QTest::qWait(50);
	QVERIFY2(kept.isVisible(), "a plain release closed the last window");
	while (shared->count() > 0) shared->close(shared->at(0));
}

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

	bssh_session *first = registry->create(named(QStringLiteral("first")));
	window.adopt_session(first);
	window.adopt_session(registry->create(named(QStringLiteral("second"))));
	QTest::qWait(50);

	/* The pane's own bar now (sec 8.3's bar per pane). */
	bssh_pane_bar *row = window.bar_for(window.layout_manager()->front_of(first));
	QTabBar *bar = row ? row->tab_bar() : nullptr;
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

	QVERIFY2(window.tab_row_at(on_a_tab), "a tab is not on the tab row");
	QVERIFY2(window.tab_row_at(same_row),
	          "the row beyond the last tab is not treated as the tab bar");
	QVERIFY2(!window.tab_row_at(below),
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
/*
 * A tree as text -- "V(a,H(b,c))" -- so a shape is compared in one line
 * rather than walked node by node in every test.
 */
static QString shape_of(const bssh_pane_node &node,
                        const QHash<bssh_session *, QString> &names) {
	if (node.is_leaf()) return names.value(node.session, QStringLiteral("?"));
	if (node.is_empty()) return QString();

	QStringList parts;
	for (const bssh_pane_node &child : node.children) parts.append(shape_of(child, names));
	return (node.orientation == Qt::Horizontal ? QStringLiteral("H(") : QStringLiteral("V("))
	       + parts.join(QLatin1Char(',')) + QLatin1Char(')');
}

void layout_test::a_pane_dropped_on_an_edge_splits_the_target_that_way() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));
	bssh_session *c = registry.create(named(QStringLiteral("c")));
	const QHash<bssh_session *, QString> names{
		{a, QStringLiteral("a")}, {b, QStringLiteral("b")}, {c, QStringLiteral("c")}};

	layout.set_intent({a, b});
	QCOMPARE(shape_of(layout.tree(), names), QStringLiteral("H(a,b)"));

	/* b dropped on a's bottom edge: the row becomes a column. */
	QVERIFY(layout.place(b, a, bssh_layout_manager::EDGE_BOTTOM));
	QCOMPARE(shape_of(layout.tree(), names), QStringLiteral("V(a,b)"));

	/* c arrives on b's right: across b's column, so b itself is split. */
	QVERIFY(layout.place(c, b, bssh_layout_manager::EDGE_RIGHT));
	QCOMPARE(shape_of(layout.tree(), names), QStringLiteral("V(a,H(b,c))"));
	QCOMPARE(layout.intent(), (QList<bssh_session *>{a, b, c}));

	/* And a on c's left, along c's own row: one more beside, no new level. */
	QVERIFY(layout.place(a, c, bssh_layout_manager::EDGE_LEFT));
	QCOMPARE(shape_of(layout.tree(), names), QStringLiteral("H(b,a,c)"));

	/* A pane dropped on itself changes nothing. */
	QVERIFY(!layout.place(a, a, bssh_layout_manager::EDGE_TOP));
}

void layout_test::a_pane_dropped_in_the_middle_swaps() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));
	bssh_session *c = registry.create(named(QStringLiteral("c")));
	bssh_session *d = registry.create(named(QStringLiteral("d")));
	const QHash<bssh_session *, QString> names{{a, QStringLiteral("a")},
		{b, QStringLiteral("b")}, {c, QStringLiteral("c")}, {d, QStringLiteral("d")}};

	layout.set_intent({a, b});
	QVERIFY(layout.place(c, b, bssh_layout_manager::EDGE_BOTTOM));
	QCOMPARE(shape_of(layout.tree(), names), QStringLiteral("H(a,V(b,c))"));

	/* Two panes trade places; the shape stays. */
	QVERIFY(layout.place(a, c, bssh_layout_manager::EDGE_MIDDLE));
	QCOMPARE(shape_of(layout.tree(), names), QStringLiteral("H(c,V(b,a))"));

	/*
	 * A session that was not a pane takes the target's place, and the
	 * target leaves the panes -- a tab, never closed (sec 8.2).
	 */
	QVERIFY(layout.place(d, b, bssh_layout_manager::EDGE_MIDDLE));
	QCOMPARE(shape_of(layout.tree(), names), QStringLiteral("H(c,V(d,a))"));
	QVERIFY(!layout.intent().contains(b));
	QCOMPARE(registry.count(), 4);
}


namespace {

/* A point well inside the zone `edge` of a pane this size. */
/*
 * A point in an edge's split zone, which reaches a quarter of the pane in:
 * a fifth in, clear of the band along the very edge that is a tab zone on
 * a pane with no bar (sec 8.7a). A tenth in, as it was, fell inside that
 * band on a narrow pane.
 */
QPoint zone_point(const QSize &size, int edge) {
	switch (edge) {
	case bssh_layout_manager::EDGE_LEFT:   return QPoint(size.width() / 5, size.height() / 2);
	case bssh_layout_manager::EDGE_RIGHT:  return QPoint(size.width() * 4 / 5, size.height() / 2);
	case bssh_layout_manager::EDGE_TOP:    return QPoint(size.width() / 2, size.height() / 5);
	case bssh_layout_manager::EDGE_BOTTOM: return QPoint(size.width() / 2, size.height() * 4 / 5);
	default:                               return QPoint(size.width() / 2, size.height() / 2);
	}
}

/* Where a session's pane is, in global coordinates; null if not a pane. */
QRect pane_rect(bssh_main_window &window, bssh_session *session) {
	bssh_terminal_view *view = window.panes() ? window.panes()->view_for(session) : nullptr;
	if (!view || !view->isVisible()) return QRect();
	return QRect(view->mapToGlobal(QPoint(0, 0)), view->size());
}

/*
 * Drags `session` over the pane showing `target` at `edge`, as Qt
 * delivers it, and lets go: returns the zone the pane DREW just before.
 */
int drag_over_and_drop(bssh_main_window &from, bssh_main_window &onto,
                       bssh_session *session, bssh_session *target, int edge) {
	bssh_terminal_view *view = onto.panes()->view_for(target);
	if (!view) return -100;

	std::unique_ptr<QMimeData> mime(from.session_drag_mime(session));
	if (!mime) return -101;

	const QPoint at = zone_point(view->size(), edge);
	QDragEnterEvent enter(at, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(view, &enter);
	QDragMoveEvent move(at, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(view, &move);
	const int drawn = view->drop_zone();

	QDropEvent drop(QPointF(at), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(view, &drop);
	QCoreApplication::processEvents();
	QCoreApplication::processEvents();
	return drawn;
}

/* Whether `a` lies on side `edge` of `b`, overlapping it across. */
bool lies_beside(const QRect &a, const QRect &b, int edge) {
	switch (edge) {
	case bssh_layout_manager::EDGE_LEFT:
		return a.right() < b.left() && a.top() < b.bottom() && b.top() < a.bottom();
	case bssh_layout_manager::EDGE_RIGHT:
		return a.left() > b.right() && a.top() < b.bottom() && b.top() < a.bottom();
	case bssh_layout_manager::EDGE_TOP:
		return a.bottom() < b.top() && a.left() < b.right() && b.left() < a.right();
	case bssh_layout_manager::EDGE_BOTTOM:
		return a.top() > b.bottom() && a.left() < b.right() && b.left() < a.right();
	}
	return false;
}

bool near(const QRect &a, const QRect &b) {
	return qAbs(a.left() - b.left()) <= 2 && qAbs(a.top() - b.top()) <= 2
	        && qAbs(a.width() - b.width()) <= 2 && qAbs(a.height() - b.height()) <= 2;
}

} /* namespace */

void layout_test::an_unfocused_pane_resizing_leaves_the_focused_session_its_size() {
	/*
	 * Every view that has had focus stays connected to the window's
	 * grid_resized handler, which resized the FOCUSED session -- so a
	 * pane that once had focus, resizing later, gave the focused
	 * session its own size, the wrong window size for that remote. The
	 * same path read a focused view the container had already deleted
	 * while it rebuilt, a use-after-free found under ASan in
	 * the_zone_drawn_is_what_the_drop_does.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
	const registry_left_as_found_t tidy(shared);

	bssh_main_window window;
	window.resize(1400, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	bssh_session *a = window.registry()->create(named(QStringLiteral("a")));
	window.adopt_session(a);
	bssh_session *b = window.registry()->create(named(QStringLiteral("b")));
	window.drop_session_on(b, a, bssh_layout_manager::EDGE_RIGHT);
	QCoreApplication::processEvents();
	QCOMPARE(window.panes()->views().size(), 2);

	/* Both views wired: a had focus, then b has it. */
	window.panes()->focus_session(a);
	QCoreApplication::processEvents();
	window.panes()->focus_session(b);
	QCoreApplication::processEvents();
	bssh_terminal_view *view_a = window.panes()->view_for(a);
	QVERIFY(view_a);

	const int cols = b->emulator()->screen().cols();
	const int rows = b->emulator()->screen().rows();
	QVERIFY(cols != 7 || rows != 5);
	emit view_a->grid_resized(7, 5);
	QCoreApplication::processEvents();
	QCOMPARE(b->emulator()->screen().cols(), cols);
	QCOMPARE(b->emulator()->screen().rows(), rows);
}

void layout_test::the_zone_drawn_is_what_the_drop_does() {
	/*
	 * The relationship, not a value: whatever a pane draws while a drag
	 * is over it, the release does -- a side puts the dropped terminal on
	 * that side, the middle trades the two, a cancel changes nothing.
	 * Three rows of sec 8.4's table broke it: a side into a full window
	 * landing as a tab, left and right in the column landing above and
	 * below, and zones lit on the pane being carried.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
	const registry_left_as_found_t tidy(shared);

	const auto check = [](int drawn, int edge, const QRect &s_before, const QRect &t_before,
	                      const QRect &s_after, const QRect &t_after, const char *what) {
		const QByteArray context = QByteArray(what) + " zone " + QByteArray::number(edge)
		        + " drawn " + QByteArray::number(drawn);
		if (drawn == BSSH_DROP_CANCEL) {
			QVERIFY2(near(s_after, s_before) && near(t_after, t_before), context.constData());
		} else if (drawn == bssh_layout_manager::EDGE_MIDDLE) {
			/* Each in the other's slot: a rebuild may redistribute a few
			 * pixels between panes, so the test is the slot, not the size. */
			QVERIFY2(t_before.contains(s_after.center()) && s_before.contains(t_after.center()),
			         context.constData());
		} else {
			QVERIFY2(drawn >= 0, context.constData());
			QVERIFY2(lies_beside(s_after, t_after, drawn), context.constData());
		}
	};

	/* One window, two panes, every zone of the second. */
	for (int edge = bssh_layout_manager::EDGE_LEFT; edge <= bssh_layout_manager::EDGE_MIDDLE; ++edge) {
		bssh_main_window window;
		window.resize(1400, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		bssh_session *a = window.registry()->create(named(QStringLiteral("a")));
		window.adopt_session(a);
		bssh_session *b = window.registry()->create(named(QStringLiteral("b")));
		window.drop_session_on(b, a, bssh_layout_manager::EDGE_RIGHT);
		QCoreApplication::processEvents();
		QCOMPARE(window.panes()->views().size(), 2);

		const QRect a_before = pane_rect(window, a), b_before = pane_rect(window, b);
		const int drawn = drag_over_and_drop(window, window, a, b, edge);
		QCOMPARE(drawn, edge);
		check(drawn, edge, a_before, b_before, pane_rect(window, a), pane_rect(window, b),
		      "same window");
		while (window.registry()->count() > 0) window.registry()->close(window.registry()->at(0));
	}

	/* The pane being carried: a cancel, wherever on it. */
	for (const int edge : { int(bssh_layout_manager::EDGE_MIDDLE), int(bssh_layout_manager::EDGE_LEFT) }) {
		bssh_main_window window;
		window.resize(1400, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		bssh_session *a = window.registry()->create(named(QStringLiteral("a")));
		window.adopt_session(a);
		bssh_session *b = window.registry()->create(named(QStringLiteral("b")));
		window.drop_session_on(b, a, bssh_layout_manager::EDGE_RIGHT);
		QCoreApplication::processEvents();

		const QRect a_before = pane_rect(window, a), b_before = pane_rect(window, b);
		const int drawn = drag_over_and_drop(window, window, a, a, edge);
		QCOMPARE(drawn, BSSH_DROP_CANCEL);
		check(drawn, edge, a_before, b_before, pane_rect(window, a), pane_rect(window, b),
		      "own pane");
		while (window.registry()->count() > 0) window.registry()->close(window.registry()->at(0));
	}

	/*
	 * The portrait column stacks its panes whatever the tree says, so
	 * left and right have no meaning there: drawn and done as the swap.
	 */
	struct arrangement_restored_t {
		const bssh_app_settings::arrangement_t was = bssh_app_settings().arrangement();
		~arrangement_restored_t() { bssh_app_settings().set_arrangement(was); }
	} arrangement_restored;
	bssh_app_settings().set_arrangement(bssh_app_settings::ARRANGEMENT_COLUMN);
	for (const int edge : { int(bssh_layout_manager::EDGE_LEFT), int(bssh_layout_manager::EDGE_RIGHT),
	                        int(bssh_layout_manager::EDGE_BOTTOM) }) {
		bssh_main_window window;
		window.resize(900, 900);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		bssh_session *a = window.registry()->create(named(QStringLiteral("a")));
		window.adopt_session(a);

		/* A pane of its own below: in a column a session opened is a tab (sec 8.3). */
		bssh_session *b = window.registry()->create(named(QStringLiteral("b")));
		window.drop_session_on(b, a, bssh_layout_manager::EDGE_BOTTOM);
		QCoreApplication::processEvents();
		bssh_layout_manager *layout = window.findChild<bssh_layout_manager *>();
		QVERIFY(layout);
		QCOMPARE(layout->arrangement(), bssh_layout_manager::COLUMN);
		QCOMPARE(window.panes()->views().size(), 2);

		const QRect a_before = pane_rect(window, a), b_before = pane_rect(window, b);
		const int drawn = drag_over_and_drop(window, window, a, b, edge);
		QCOMPARE(drawn, edge == bssh_layout_manager::EDGE_BOTTOM ? edge
		                                                          : int(bssh_layout_manager::EDGE_MIDDLE));
		check(drawn, edge, a_before, b_before, pane_rect(window, a), pane_rect(window, b),
		      "column");
		while (window.registry()->count() > 0) window.registry()->close(window.registry()->at(0));
	}
	bssh_app_settings().set_arrangement(arrangement_restored.was);

	/*
	 * Into a FULL window from another: 700 wide holds two, so a side
	 * would need a third pane -- it is drawn as the middle and swaps, the
	 * resident going back to the slot the arrival left.
	 */
	{
		const QRect screen = QGuiApplication::primaryScreen()->geometry();
		bssh_main_window from;
		from.resize(700, 500);
		from.move(screen.topLeft() + QPoint(0, 0));
		from.show();
		bssh_main_window onto;
		onto.resize(700, 500);
		onto.move(screen.topLeft() + QPoint(720, 0));
		onto.show();
		QVERIFY(QTest::qWaitForWindowExposed(&from));
		QVERIFY(QTest::qWaitForWindowExposed(&onto));

		bssh_session *x = from.registry()->create(named(QStringLiteral("x")));
		from.adopt_session(x);
		bssh_session *y = from.registry()->create(named(QStringLiteral("y")));
		from.drop_session_on(y, x, bssh_layout_manager::EDGE_RIGHT);
		bssh_session *a = onto.registry()->create(named(QStringLiteral("a")));
		onto.adopt_session(a);
		bssh_session *b = onto.registry()->create(named(QStringLiteral("b")));
		onto.drop_session_on(b, a, bssh_layout_manager::EDGE_RIGHT);
		QCoreApplication::processEvents();
		QCOMPARE(onto.panes()->views().size(), 2);
		QCOMPARE(from.panes()->views().size(), 2);

		const QRect x_before = pane_rect(from, x), b_before = pane_rect(onto, b);
		const int drawn = drag_over_and_drop(from, onto, x, b, bssh_layout_manager::EDGE_LEFT);
		QCOMPARE(drawn, int(bssh_layout_manager::EDGE_MIDDLE));
		check(drawn, bssh_layout_manager::EDGE_LEFT, x_before, b_before,
		      pane_rect(onto, x), pane_rect(from, b), "full window");
		QVERIFY2(onto.shown_sessions().contains(x) && from.shown_sessions().contains(b),
		         "a swap across windows trades the two between them");
	}
}


namespace {

/* The tabs of the pane holding `member`, in order (sec 8.3's bar per pane). */
QList<bssh_session *> tab_sessions(bssh_main_window &window, bssh_session *member) {
	return window.layout_manager()->tabs(member);
}

/* Drags `session` over the drop overlay of the pane showing `front`, at `at`, and lets go. */
void drop_on_tab_row(bssh_main_window &window, bssh_session *front, bssh_session *session,
                     const QPoint &at, bool *accepted) {
	QWidget *overlay = window.tab_drop_target(front);
	QVERIFY(overlay);
	std::unique_ptr<QMimeData> mime(window.session_drag_mime(session));
	QDragEnterEvent enter(at, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(overlay, &enter);
	QDragMoveEvent move(at, Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(overlay, &move);
	QDropEvent drop(QPointF(at), Qt::MoveAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(overlay, &drop);
	if (accepted) *accepted = drop.isAccepted();
	QCoreApplication::processEvents();
	QCoreApplication::processEvents();
}

} /* namespace */

void layout_test::the_tab_row_takes_a_drop_at_the_slot_it_marks() {
	/*
	 * The holder's rule (sec 8.4, 2026-10-09): only the place where the
	 * tab bar is, or would be, makes a tab; with a bar the drop position
	 * picks the slot, marked before the release; without one, a strip
	 * where the bar would appear takes the drop. Asserted as the relation
	 * between the slot marked and where the tab lands.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
	const registry_left_as_found_t tidy(shared);

	bssh_main_window window;
	window.resize(1400, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session *a = window.registry()->create(named(QStringLiteral("a")));
	window.adopt_session(a);
	QCoreApplication::processEvents();
	QVERIFY2(!window.tab_drop_target(a), "a pane's drop target exists only while a drag is up");

	/* A pane of one has no bar (sec 8.3's bar per pane). */
	QVERIFY(window.bar_for(a) && window.bar_for(a)->isHidden());

	window.set_session_drag_active(true);
	QPointer<QWidget> overlay = window.tab_drop_target(a);
	QVERIFY2(overlay && overlay->isVisible(), "with no bar, the strip is where a tab can be made");

	/* The pane's only tab over its own strip: nothing marked, nothing done. */
	int mark = -1;
	QCOMPARE(window.tab_drop_slot(a, a, overlay->rect().center(), &mark), -1);
	bool accepted = false;
	drop_on_tab_row(window, a, a, overlay->rect().center(), &accepted);
	QVERIFY2(accepted, "a drop that does nothing is accepted, or the source tears out a window");
	QCOMPARE(window.panes()->pane_count(), 1);
	QCOMPARE(tab_sessions(window, a), QList<bssh_session *>({ a }));

	/*
	 * Another session over the strip: one slot, after the pane's own tab,
	 * and it comes to the front -- the pane stays one pane, now with a bar.
	 */
	bssh_session *c = window.registry()->create(named(QStringLiteral("c")));
	QCOMPARE(window.tab_drop_slot(c, a, overlay->rect().center(), &mark), 1);
	drop_on_tab_row(window, a, c, overlay->rect().center(), nullptr);
	window.set_session_drag_active(false);
	QCoreApplication::processEvents();
	QVERIFY2(!overlay || !overlay->isVisible(), "the drop targets outlived the drag");
	QCOMPARE(window.panes()->pane_count(), 1);
	QVERIFY(window.panes()->view_for(c));
	QCOMPARE(tab_sessions(window, a), QList<bssh_session *>({ a, c }));

	/*
	 * With a bar: before each tab and after the last, a fresh session each
	 * time, and it lands at the slot that was marked.
	 */
	for (int slot = 0; slot <= 2; ++slot) {
		bssh_session *front = window.layout_manager()->front_of(a);
		window.set_session_drag_active(true);
		bssh_pane_bar *row = window.bar_for(front);
		QVERIFY(row && row->isVisible());
		QTabBar *bar = row->tab_bar();
		QWidget *target = window.tab_drop_target(front);
		QVERIFY(target);
		const QPoint in_bar = slot < bar->count()
		        ? bar->tabRect(slot).center() - QPoint(4, 0)
		        : bar->tabRect(bar->count() - 1).center() + QPoint(4, 0);
		const QPoint at = target->mapFromGlobal(bar->mapToGlobal(in_bar));

		bssh_session *arriving = window.registry()->create(named(QStringLiteral("n%1").arg(slot)));
		const int wanted = qMin(slot, bar->count());
		mark = -1;
		QCOMPARE(window.tab_drop_slot(arriving, front, at, &mark), wanted);
		QVERIFY2(mark >= 0, "a slot with no mark drawn is a slot nobody can aim at");

		drop_on_tab_row(window, front, arriving, at, nullptr);
		window.set_session_drag_active(false);
		QCoreApplication::processEvents();
		QCOMPARE(tab_sessions(window, a).indexOf(arriving), wanted);
		QCOMPARE(window.panes()->pane_count(), 1);
	}
}


void layout_test::a_drop_from_another_process_is_refused() {
	/*
	 * sec 8.4's defect 1: the payload was a bare registry index, resolved
	 * by whichever process received it -- so a drop onto another BeerSSH
	 * moved one of THAT process's sessions, the one at that index, and the
	 * source kept its own. A live session cannot cross processes, and the
	 * holder chose refusal: accepted as a link, which moves nothing, so the
	 * source neither tears out nor believes it moved.
	 *
	 * Index 0 is a real session here, which is the point: the foreign
	 * payload names a slot that exists, as it would in the real fault.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
	const registry_left_as_found_t tidy(shared);

	bssh_main_window window;
	window.resize(1400, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	bssh_session *a = window.registry()->create(named(QStringLiteral("a")));
	window.adopt_session(a);
	bssh_session *b = window.registry()->create(named(QStringLiteral("b")));
	window.drop_session_on(b, a, bssh_layout_manager::EDGE_RIGHT);
	bssh_session *t = window.registry()->create(named(QStringLiteral("t")));
	window.drop_session_as_tab(t, a, -1);
	window.layout_manager()->bring_to_front(a);
	QCoreApplication::processEvents();
	QCOMPARE(shared->index_of(a), 0);

	const QList<bssh_session *> shown = window.shown_sessions();
	const int panes = window.panes()->pane_count();

	const QByteArray other = QByteArray::number(QCoreApplication::applicationPid() + 1) + ":0";
	for (const QByteArray &payload : { other, QByteArray("0") }) {
		QMimeData mime;
		mime.setData(bssh_session_mime_type(), payload);

		const auto offer = [&mime](QWidget *to, const QPoint &at, const char *what) {
			QDragEnterEvent enter(at, Qt::MoveAction | Qt::LinkAction, &mime, Qt::LeftButton, Qt::NoModifier);
			QCoreApplication::sendEvent(to, &enter);
			QDragMoveEvent move(at, Qt::MoveAction | Qt::LinkAction, &mime, Qt::LeftButton, Qt::NoModifier);
			QCoreApplication::sendEvent(to, &move);
			QDropEvent drop(QPointF(at), Qt::MoveAction | Qt::LinkAction, &mime, Qt::LeftButton,
			                Qt::NoModifier);
			QCoreApplication::sendEvent(to, &drop);
			QVERIFY2(drop.isAccepted() && drop.dropAction() == Qt::LinkAction, what);
			QCoreApplication::processEvents();
			QCoreApplication::processEvents();
		};

		bssh_terminal_view *pane = window.panes()->view_for(b);
		QVERIFY(pane);
		offer(pane, QPoint(pane->width() / 10, pane->height() / 2), "a terminal answers with a link");
		QCOMPARE(pane->drop_zone(), -1);

		window.set_session_drag_active(true);
		QWidget *row = window.tab_drop_target(b);
		QVERIFY(row);
		offer(row, row->rect().center(), "the tab row answers with a link");
		window.set_session_drag_active(false);

		QCOMPARE(window.shown_sessions(), shown);
		QCOMPARE(window.panes()->pane_count(), panes);
		QVERIFY(window.panes()->view_for(a));
	}
}

void layout_test::reordering_the_panes_keeps_the_shape_they_are_in() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));
	bssh_session *c = registry.create(named(QStringLiteral("c")));
	const QHash<bssh_session *, QString> names{
		{a, QStringLiteral("a")}, {b, QStringLiteral("b")}, {c, QStringLiteral("c")}};

	layout.set_intent({a, b});
	QVERIFY(layout.place(c, b, bssh_layout_manager::EDGE_BOTTOM));
	QCOMPARE(shape_of(layout.tree(), names), QStringLiteral("H(a,V(b,c))"));

	/*
	 * Everything that moves panes by the LIST -- the menu's Move Pane, a
	 * column's reorder -- still says "these, in this order". The places
	 * the user built stay, and the sessions move through them.
	 */
	layout.set_intent({c, a, b});
	QCOMPARE(shape_of(layout.tree(), names), QStringLiteral("H(c,V(a,b))"));

	/* A pane that goes leaves no hole: its split closes up. */
	layout.forget(a);
	QCOMPARE(shape_of(layout.tree(), names), QStringLiteral("H(c,b)"));

	/* And a newcomer by the list arrives beside the rest, as a split always did. */
	layout.set_intent({c, b, a});
	QCOMPARE(shape_of(layout.tree(), names), QStringLiteral("H(c,b,a)"));
}

void layout_test::a_narrow_window_shows_the_first_panes_of_the_tree() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));
	bssh_session *c = registry.create(named(QStringLiteral("c")));
	const QHash<bssh_session *, QString> names{
		{a, QStringLiteral("a")}, {b, QStringLiteral("b")}, {c, QStringLiteral("c")}};

	layout.set_intent({a, b});
	QVERIFY(layout.place(c, b, bssh_layout_manager::EDGE_RIGHT));
	QVERIFY(layout.place(b, a, bssh_layout_manager::EDGE_BOTTOM));
	QCOMPARE(shape_of(layout.tree(), names), QStringLiteral("H(V(a,b),c)"));

	/*
	 * MEDIUM holds two. The first two keep their places and the third
	 * becomes a tab; the whole shape is remembered for when there is room
	 * again (sec 8.2), which the list alone could not do for a tree.
	 */
	layout.set_width(700);
	QCOMPARE(shape_of(layout.visible_tree(), names), QStringLiteral("V(a,b)"));
	QCOMPARE(shape_of(layout.tree(), names), QStringLiteral("H(V(a,b),c)"));

	layout.set_width(1200);
	QCOMPARE(shape_of(layout.visible_tree(), names), QStringLiteral("H(V(a,b),c)"));
}

void layout_test::the_drop_zones_are_the_edges_and_the_middle() {
	const QSize pane(400, 200);

	QCOMPARE(bssh_drop_edge_at(QPoint(10, 100), pane), int(bssh_layout_manager::EDGE_LEFT));
	QCOMPARE(bssh_drop_edge_at(QPoint(390, 100), pane), int(bssh_layout_manager::EDGE_RIGHT));
	QCOMPARE(bssh_drop_edge_at(QPoint(200, 10), pane), int(bssh_layout_manager::EDGE_TOP));
	QCOMPARE(bssh_drop_edge_at(QPoint(200, 190), pane), int(bssh_layout_manager::EDGE_BOTTOM));
	QCOMPARE(bssh_drop_edge_at(QPoint(200, 100), pane), int(bssh_layout_manager::EDGE_MIDDLE));

	/*
	 * Bands are a share of the pane, not pixels: 60 of 400 is inside the
	 * left quarter, and 60 of 200 is outside the top one.
	 */
	QCOMPARE(bssh_drop_edge_at(QPoint(90, 60), pane), int(bssh_layout_manager::EDGE_LEFT));
	QCOMPARE(bssh_drop_edge_at(QPoint(110, 60), pane), int(bssh_layout_manager::EDGE_MIDDLE));

	/* In a corner the nearer edge wins. */
	QCOMPARE(bssh_drop_edge_at(QPoint(5, 30), pane), int(bssh_layout_manager::EDGE_LEFT));
	QCOMPARE(bssh_drop_edge_at(QPoint(60, 4), pane), int(bssh_layout_manager::EDGE_TOP));
}

void layout_test::a_drop_on_a_pane_that_is_not_focused_places_the_pane() {
	bssh_main_window window;
	window.resize(1000, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);

	bssh_session *first = registry->create(named(QStringLiteral("first")));
	window.adopt_session(first);
	window.split_beside_current();
	QCoreApplication::processEvents();

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	QCOMPARE(panes->views().size(), 2);

	/*
	 * The drop goes to the pane that is NOT focused, carrying the one
	 * that is -- the ordinary gesture, and the one that reached nothing
	 * while only the focused view was connected. Which sessions those are
	 * is asked of the panes: the split opens a session of its own.
	 */
	bssh_session *moving = panes->focused_session();
	bssh_session *target = nullptr;
	for (int i = 0; i < registry->count(); ++i) {
		bssh_session *each = registry->at(i);
		if (each != moving && panes->view_for(each)) target = each;
	}
	QVERIFY2(moving && target, "the split did not leave two sessions on screen");
	bssh_terminal_view *onto = panes->view_for(target);

	QMimeData mime;
	mime.setData(bssh_session_mime_type(), bssh_session_payload(registry->index_of(moving)));

	/* The bottom edge of the target: the moving pane goes below it. */
	/*
	 * Entered and moved over first, as a real drag is: Qt delivers a drop
	 * only to the widget the drag is over, and the move is what lights
	 * the zone.
	 */
	/* In the bottom zone, clear of the band along the very edge, which on
	 * a pane with no bar is a tab zone now (sec 8.7a). */
	const QPoint at(onto->width() / 2, onto->height() * 4 / 5);
	QDragEnterEvent enter(at, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(onto, &enter);
	QDragMoveEvent move(at, Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(onto, &move);

	QDropEvent drop(QPointF(at), Qt::MoveAction, &mime, Qt::LeftButton, Qt::NoModifier);
	QApplication::sendEvent(onto, &drop);
	QCoreApplication::processEvents();

	QSplitter *root = panes->findChild<QSplitter *>(QString(), Qt::FindDirectChildrenOnly);
	QVERIFY(root);
	QCOMPARE(root->orientation(), Qt::Vertical);
	QCOMPARE(panes->views().size(), 2);
	QCOMPARE(panes->view_at(0), panes->view_for(target));
	QCOMPARE(panes->view_at(1), panes->view_for(moving));
	QCOMPARE(panes->focused_session(), moving);
}

void layout_test::a_release_on_a_strip_makes_a_tab() {
	/*
	 * BeerSSH's own drag (X11) places the release through
	 * finish_session_drag(), and a pane of one tab has no bar -- only the
	 * strip drawn while the drag is up. Released there, it is a tab, the
	 * same as under QDrag. Found on Xvfb: the strips were taken down
	 * before the release was placed, so it split the pane's top edge
	 * instead. That ordering lives in the interactive drag loop, which
	 * this cannot run; what it pins is that the placement reads the
	 * strip while it is up.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
	const registry_left_as_found_t tidy(shared);

	bssh_main_window window;
	window.resize(1000, 700);
	window.move(0, 0);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session *a = window.registry()->create(named(QStringLiteral("a")));
	window.adopt_session(a);
	bssh_session *c = window.registry()->create(named(QStringLiteral("c")));
	QCoreApplication::processEvents();
	QVERIFY(window.bar_for(a) && window.bar_for(a)->isHidden());

	window.set_session_drag_active(true);
	QWidget *strip = window.tab_drop_target(a);
	QVERIFY(strip && strip->isVisible());
	window.finish_session_drag(c, strip->mapToGlobal(strip->rect().center()));
	window.set_session_drag_active(false);
	QCoreApplication::processEvents();

	QCOMPARE(window.panes()->pane_count(), 1);
	QCOMPARE(tab_sessions(window, a), QList<bssh_session *>({ a, c }));
}

namespace {

/* Answers the close prompt that opens next, by the button's text. */
void answer_next_box(const QString &button, QString *offered = nullptr) {
	QTimer::singleShot(0, [button, offered] {
		QMessageBox *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
		if (!box) return;
		if (offered) {
			QStringList texts;
			for (QAbstractButton *each : box->buttons()) texts << each->text();
			*offered = texts.join(QLatin1Char('|'));
		}
		for (QAbstractButton *each : box->buttons()) {
			if (each->text().remove(QLatin1Char('&')) == button) {
				each->click();
				return;
			}
		}
		box->reject();
	});
}

} /* namespace */

namespace {

/*
 * Keeping sessions past their window is opt-in on a desktop since
 * 2026-10-25 (sec 5.5), so a test of the keep-running prompt turns it on,
 * and puts back what it found: settings outlive a test in this run.
 */
struct keep_sessions_on_close_t {
	bool was = bssh_app_settings().keep_sessions_on_close();
	explicit keep_sessions_on_close_t(bool keep = true) {
		bssh_app_settings().set_keep_sessions_on_close(keep);
	}
	~keep_sessions_on_close_t() { bssh_app_settings().set_keep_sessions_on_close(was); }
};

}

void layout_test::a_window_kept_running_comes_back_from_the_tray() {
	const bssh_factory_reset_t factory_reset;
	/*
	 * sec 5.5: the tray icon is the way back to a window kept running.
	 * It was not -- its menu held Quit and nothing else, and a click on
	 * it did nothing -- so "Keep running" hid a live shell nothing could
	 * show again. And the icon went with the last live session, so one
	 * that dropped while hidden left a process with no way back at all.
	 */
	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});
	bssh_platform::force_way_back(1);
	bssh_platform *platform = bssh_platform::instance();
	const keep_sessions_on_close_t keep;

	{
		bssh_main_window window;
		window.resize(1000, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		bssh_session_registry *registry = window.registry();
		const registry_left_as_found_t tidy(registry);

		bssh_profile_t host = named(QStringLiteral("kept"));
		host.host = QStringLiteral("kept.example");
		bssh_session *session = registry->create(host);
		window.adopt_session(session);
		session->connect_to_host();
		QVERIFY(fake);
		fake->simulate_key_auth_success();
		QCoreApplication::processEvents();
		QVERIFY(session->is_live());

		answer_next_box(QStringLiteral("Keep running"));
		window.close();
		QCoreApplication::processEvents();
		QVERIFY2(window.isHidden() && window.is_in_background(), "Keep running did not put it away");
		QVERIFY(!QApplication::quitOnLastWindowClosed());

		/*
		 * The session stops being live while nobody is looking -- here
		 * disconnected, as a failed reconnect leaves it: the window stays,
		 * and so must the icon that brings it back.
		 */
		session->disconnect_from_host();
		QCoreApplication::processEvents();
		QCoreApplication::processEvents();
		QVERIFY(!session->is_live());
		QVERIFY(window.is_in_background());
		QVERIFY2(!QApplication::quitOnLastWindowClosed(),
		          "the way back went with the last live session, the window still hidden");

		emit platform->show_requested();
		QCoreApplication::processEvents();
		QVERIFY2(!window.is_in_background(), "the tray asked for the window and it stayed away");
		QVERIFY2(window.isVisible(), "the tray asked for the window and it stayed hidden");
		QVERIFY2(QApplication::quitOnLastWindowClosed(),
		          "resident with nothing live and nothing hidden");

	}

	{
		/*
		 * Put away, and this time the shell ENDS: an ending closes the
		 * window, hidden or not, and nothing is left to stay for.
		 */
		bssh_main_window window;
		window.resize(1000, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		bssh_session_registry *registry = window.registry();
		const registry_left_as_found_t tidy(registry);

		bssh_profile_t host = named(QStringLiteral("ending"));
		host.host = QStringLiteral("ending.example");
		bssh_session *session = registry->create(host);
		window.adopt_session(session);
		session->connect_to_host();
		fake->simulate_key_auth_success();
		QCoreApplication::processEvents();
		QVERIFY(session->is_live());

		answer_next_box(QStringLiteral("Keep running"));
		window.close();
		QCoreApplication::processEvents();
		QVERIFY(window.is_in_background());

		fake->simulate_remote_exit(0);
		QTRY_VERIFY2_WITH_TIMEOUT(!window.is_in_background(),
		                          "a window closed by its shell ending still holds the tray", 2000);
		QVERIFY2(QApplication::quitOnLastWindowClosed(),
		          "resident for a window that has closed");
	}

	bssh_platform::force_way_back(-1);
	bssh_ssh_session::set_factory(nullptr);
}

void layout_test::closing_a_window_closes_its_sessions_without_asking() {
	/*
	 * The desktop default since 2026-10-25 (sec 5.5): a window closes
	 * what it shows, as TDE Konsole's does. It used to ask whether to
	 * keep a live session running hidden, which only a tray icon could
	 * ever bring back -- a question the holder asked why it was being
	 * asked at all. Tested with a tray, where the old prompt would have
	 * offered exactly that.
	 */
	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});
	const bssh_factory_reset_t factory_reset;
	bssh_platform::force_way_back(1);
	const keep_sessions_on_close_t keep(false);

	QPointer<bssh_session> shown;
	{
		bssh_main_window window;
		window.resize(1000, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		bssh_session_registry *registry = window.registry();
		const registry_left_as_found_t tidy(registry);

		bssh_profile_t host = named(QStringLiteral("closing"));
		host.host = QStringLiteral("closing.example");
		bssh_session *session = registry->create(host);
		shown = session;
		window.adopt_session(session);
		session->connect_to_host();
		QVERIFY(fake);
		fake->simulate_key_auth_success();
		QCoreApplication::processEvents();
		QVERIFY(session->is_live());

		/* A box, had one appeared, is recorded and dismissed rather than
		 * left to hold the test. */
		QString offered;
		answer_next_box(QStringLiteral("Disconnect and quit"), &offered);
		window.close();
		QCoreApplication::processEvents();

		QVERIFY2(offered.isEmpty(), qPrintable(QStringLiteral("it asked: ") + offered));
		QVERIFY2(!window.isVisible() && !window.is_in_background(), "the window did not close");
		QVERIFY2(!shown || !registry->sessions().contains(shown),
		         "the session outlived its window");
	}

	bssh_platform::force_way_back(-1);
	bssh_ssh_session::set_factory(nullptr);
}

namespace {

/* The drawer on whichever view in the window is offering one. */
bssh_multiplexer_drawer *drawer_in(const QWidget &window) {
	for (bssh_terminal_view *view : window.findChildren<bssh_terminal_view *>())
		if (view->multiplexer_drawer()) return view->multiplexer_drawer();
	return nullptr;
}

}

void layout_test::the_tmux_drawer_switches_on_a_click() {
	/*
	 * sec 10.2a: sessions found running when a shell connects are offered
	 * from a drawer on the terminal's edge -- a handle closed, a list open.
	 * A click switches the tab to one, and the drawer stays to switch to
	 * another, until it is dismissed.
	 */
	QList<bssh_fake_ssh_session *> fakes;
	const bssh_factory_reset_t factory_reset;
	bssh_ssh_session::set_factory([&fakes](QObject *parent) -> bssh_ssh_session * {
		fakes.append(new bssh_fake_ssh_session(parent));
		return fakes.last();
	});
	/* On for this test, and off again however it ends (test/main.cpp). */
	struct offer_t {
		offer_t() { bssh_app_settings().set_offer_multiplexer_sessions(true); }
		~offer_t() { bssh_app_settings().set_offer_multiplexer_sessions(false); }
	} const offer;

	{
		bssh_main_window window;
		window.resize(1000, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		bssh_session_registry *registry = window.registry();
		const registry_left_as_found_t tidy(registry);

		bssh_profile_t host = named(QStringLiteral("muxed"));
		host.host = QStringLiteral("muxed.example");
		bssh_session *session = registry->create(host);
		window.adopt_session(session);
		session->connect_to_host();
		fakes.last()->simulate_key_auth_success();
		QVERIFY2(!drawer_in(window), "a drawer before anything was found");
		bssh_fake_ssh_session *muxed_transport = fakes.last();

		muxed_transport->simulate_side_output("work\t3\t1\nbuild\t1\t0\n@@bssh-screen@@\n");
		QVERIFY2(drawer_in(window), "no drawer for sessions that are running");

		/*
		 * A split rebuilds every pane's view, and the offer outlives the
		 * view it first appeared on: the session's new view has it too.
		 */
		bssh_profile_t other = named(QStringLiteral("beside"));
		other.host = QStringLiteral("beside.example");
		bssh_session *beside = registry->create(other);
		window.adopt_session(beside);
		window.split_with(session);
		QCoreApplication::processEvents();
		const int before = registry->count();

		bssh_multiplexer_drawer *drawer = drawer_in(window);
		QVERIFY2(drawer, "the drawer was lost when the panes were rebuilt");
		/*
		 * Closed, the drawer is a button in the focused pane's corner
		 * (sec 8.7a) and shows nothing of its own over the text.
		 */
		bssh_terminal_view *pane = qobject_cast<bssh_terminal_view *>(drawer->parentWidget());
		QVERIFY(pane);
		bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
		QVERIFY(panes);
		panes->focus_session(session);
		QTRY_VERIFY2(pane->corner() && pane->corner()->isVisible(),
		              "the focused pane shows no corner");
		QToolButton *handle = pane->corner()->multiplexer_button();
		QVERIFY(handle);
		QVERIFY2(handle->isVisible(), "the corner has no tmux button");
		QVERIFY2(!drawer->isVisible(), "the closed drawer covers the terminal");
		QVERIFY2(!drawer->is_open(), "the drawer opened over the terminal by itself");
		QCOMPARE(drawer->handle_text(), QString::fromUtf8("tmux · 2"));
		QCOMPARE(handle->text(), QStringLiteral("tmux 2"));
		QCOMPARE(handle->toolTip(), drawer->handle_text());

		/* The button opens it over the terminal, beside the corner. */
		handle->click();
		QVERIFY(drawer->is_open());
		QVERIFY(drawer->isVisible());
		QVERIFY2(drawer->geometry().right() < pane->corner()->x(),
		          "the open drawer covers the corner that closes it");

		/* One click switches: the shell replaced by an attach to it. */
		QListWidget *list = drawer->findChild<QListWidget *>(QStringLiteral("multiplexer_sessions"));
		QVERIFY(list);
		QCOMPARE(list->count(), 2);
		QVERIFY2(list->item(0)->flags() & Qt::ItemIsEnabled, "an entry cannot be clicked");
		QVERIFY2(!(list->item(0)->flags() & Qt::ItemIsUserCheckable), "entries still carry ticks");
		emit list->itemClicked(list->item(1));
		QCoreApplication::processEvents();

		const QStringList build = bssh_mux_switch_command(session->pending_multiplexers().at(1), false);
		QCOMPARE(muxed_transport->restarts(), QStringList{ build.join(QLatin1Char(' ')) });
		QVERIFY2(!muxed_transport->written().contains("tmux attach"), "the switch was typed");
		QCOMPARE(registry->count(), before);

		/* And the drawer stays, closed to its handle, saying which it shows. */
		drawer = drawer_in(window);
		QVERIFY2(drawer, "the drawer went once a session was chosen");
		QVERIFY(!drawer->is_open());
		list = drawer->findChild<QListWidget *>(QStringLiteral("multiplexer_sessions"));
		QVERIFY(list->item(1)->text().contains(QStringLiteral("build")));
		QVERIFY2(list->item(1)->font().bold(), "the one shown is not marked");
		QVERIFY(!list->item(0)->font().bold());

		/* So another can be switched to after it. */
		drawer->set_open(true);
		emit list->itemClicked(list->item(0));
		QCoreApplication::processEvents();
		QCOMPARE(muxed_transport->restarts().size(), 2);
		QVERIFY(muxed_transport->restarts().last().contains(QStringLiteral("=work")));
	}

	{
		/*
		 * Closed with its x: the panel goes, the shell is left as it is,
		 * and the offer stays -- the corner's button with it (sec 8.7a;
		 * the holder found the button going "now unexpected").
		 */
		bssh_main_window window;
		window.resize(1000, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		bssh_session_registry *registry = window.registry();
		const registry_left_as_found_t tidy(registry);

		bssh_profile_t host = named(QStringLiteral("declined"));
		host.host = QStringLiteral("declined.example");
		bssh_session *session = registry->create(host);
		window.adopt_session(session);
		session->connect_to_host();
		fakes.last()->simulate_key_auth_success();
		fakes.last()->clear_written();
		fakes.last()->simulate_side_output("work\t3\t1\n@@bssh-screen@@\n");
		bssh_multiplexer_drawer *drawer = drawer_in(window);
		QVERIFY(drawer);
		drawer->set_open(true);
		QToolButton *close = drawer->findChild<QToolButton *>(QStringLiteral("multiplexer_close"));
		QVERIFY(close);
		close->click();
		QVERIFY2(!drawer->is_open(), "the x did not close the drawer");
		QVERIFY2(drawer_in(window), "the x took the offer away");
		QVERIFY(fakes.last()->written().isEmpty());
		QVERIFY(!session->pending_multiplexers().isEmpty());

		/* The terminal menu lists them again, refreshing what is offered. */
		QMenu *menu = window.build_terminal_menu(&window);
		QAction *again = menu->findChild<QAction *>(QStringLiteral("terminal_multiplexers"));
		QVERIFY2(again, "no way back to the drawer once it is closed");
		QVERIFY(again->isEnabled());
		const int listed = fakes.last()->side_commands().size();
		window.act_on_terminal_menu(menu, again);
		QCOMPARE(fakes.last()->side_commands().size(), listed + 1);
		fakes.last()->simulate_side_output("work\t3\t1\n@@bssh-screen@@\n");
		QVERIFY2(drawer_in(window), "the menu did not bring the drawer back");
		delete menu;
	}

}

void layout_test::with_no_tray_a_window_is_not_kept_running() {
	const bssh_factory_reset_t factory_reset;
	/*
	 * No notification area, so no icon to come back from: a window hidden
	 * there is a live shell in a process nobody can reach. Not offered;
	 * Cancel stands in for it, and a remembered "keep running" is asked
	 * again rather than obeyed.
	 */
	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});
	bssh_platform::force_way_back(0);
	const keep_sessions_on_close_t keep;

	{
		bssh_main_window window;
		window.resize(1000, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		bssh_session_registry *registry = window.registry();
		const registry_left_as_found_t tidy(registry);

		bssh_profile_t host = named(QStringLiteral("trayless"));
		host.host = QStringLiteral("trayless.example");
		bssh_session *session = registry->create(host);
		window.adopt_session(session);
		session->connect_to_host();
		QVERIFY(fake);
		fake->simulate_key_auth_success();
		QCoreApplication::processEvents();

		QString offered;
		answer_next_box(QStringLiteral("Cancel"), &offered);
		window.close();
		QCoreApplication::processEvents();
		QVERIFY2(!offered.contains(QStringLiteral("Keep running")),
		          qPrintable(QStringLiteral("offered: ") + offered));
		QVERIFY2(window.isVisible() && !window.is_in_background(), "Cancel closed it anyway");
		QVERIFY(session->is_live());

		registry->close(session);
		QCoreApplication::processEvents();
	}

	QCOMPARE(bssh_main_window::close_action(true, true, false), bssh_main_window::ASK_FIRST);
	bssh_platform::force_way_back(-1);
	bssh_ssh_session::set_factory(nullptr);
}

void layout_test::a_restored_host_the_shape_leaves_out_is_still_a_tab() {
	/*
	 * The hosts are saved from the whole registry -- every window's -- and
	 * the shape from the closing window's tree. A restore opened every
	 * host and then replaced the tree with the shape, so a host the shape
	 * did not name was open in this window and in no pane and no bar.
	 */
	bssh_profile_store *store = bssh_profile_store::shared();
	QVERIFY(store && store->is_open());

	const int left_id = store->save(named(QStringLiteral("unnamed-left")));
	const int right_id = store->save(named(QStringLiteral("unnamed-right")));
	const int other_id = store->save(named(QStringLiteral("unnamed-other")));
	QVERIFY(left_id && right_id && other_id);
	const bssh_profile_store::layout_state_t was = store->layout();

	bssh_profile_store::layout_state_t saved;
	saved.profile_ids = {left_id, right_id, other_id};
	saved.pane_count = 2;
	saved.shape = QStringLiteral("H(%1,%2)").arg(left_id).arg(right_id);
	QVERIFY(store->save_layout(saved));

	{
		bssh_main_window window;
		window.resize(1200, 800);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		const registry_left_as_found_t tidy(window.registry());

		window.restore_layout();
		QCoreApplication::processEvents();

		const QList<bssh_session *> placed = window.layout_manager()->sessions();
		int restored = 0;
		for (bssh_session *session : window.registry()->sessions()) {
			const int id = session->profile().id;
			if (id != left_id && id != right_id && id != other_id) continue;
			++restored;
			QVERIFY2(placed.contains(session),
			          qPrintable(session->profile().name + QStringLiteral(" is open in no pane")));
		}
		QCOMPARE(restored, 3);
		QCOMPARE(window.panes()->pane_count(), 2);
	}

	store->save_layout(was);
	store->remove(left_id);
	store->remove(right_id);
	store->remove(other_id);
}

void layout_test::a_background_tab_swapped_across_windows_stays_in_a_bar() {
	/*
	 * A tab dragged from behind its pane's front onto the middle of a
	 * pane in another window: the two trade places. Only fronts were
	 * exchanged, so the arrival was listed in the first window and in no
	 * bar there, and the tab that left stayed in that window's stack
	 * while the second showed it too.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
	const registry_left_as_found_t tidy(shared);

	bssh_main_window first;
	first.resize(900, 600);
	first.show();
	bssh_main_window second;
	second.resize(900, 600);
	second.move(1000, 0);
	second.show();
	QVERIFY(QTest::qWaitForWindowExposed(&first));
	QVERIFY(QTest::qWaitForWindowExposed(&second));

	bssh_session *a = shared->create(named(QStringLiteral("a")));
	bssh_session *b = shared->create(named(QStringLiteral("b")));
	bssh_session *c = shared->create(named(QStringLiteral("c")));
	first.adopt_session(a);
	first.adopt_session(b);
	second.adopt_session(c);
	QCoreApplication::processEvents();
	first.layout_manager()->bring_to_front(a);
	QCoreApplication::processEvents();
	QCOMPARE(tab_sessions(first, a), QList<bssh_session *>({ a, b }));
	QCOMPARE(first.layout_manager()->front_of(b), a);

	second.drop_session_on(b, c, bssh_layout_manager::EDGE_MIDDLE);
	QCoreApplication::processEvents();

	QVERIFY2(first.layout_manager()->sessions().contains(c),
	          "the session that came back is in no pane of the first window");
	QVERIFY2(!first.layout_manager()->sessions().contains(b),
	          "the tab that left is still in the first window's stack");
	QCOMPARE(tab_sessions(first, a), QList<bssh_session *>({ a, c }));
	QVERIFY(second.layout_manager()->sessions().contains(b));
	QVERIFY(!second.layout_manager()->sessions().contains(c));
}

void layout_test::a_drag_released_outside_every_window_tears_out() {
	bssh_main_window window;
	window.resize(1000, 700);
	window.move(0, 0);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);

	bssh_session *staying = registry->create(named(QStringLiteral("staying")));
	bssh_session *leaving = registry->create(named(QStringLiteral("leaving")));
	window.adopt_session(staying);
	window.adopt_session(leaving);
	QCoreApplication::processEvents();

	/* Nowhere any window of ours is. */
	const QPoint outside(window.geometry().right() + 3000, window.geometry().bottom() + 3000);
	QVERIFY(!QApplication::topLevelAt(outside));

	window.finish_session_drag(leaving, outside);
	QCoreApplication::processEvents();

	bssh_main_window *torn = nullptr;
	for (QWidget *top : QApplication::topLevelWidgets()) {
		bssh_main_window *each = qobject_cast<bssh_main_window *>(top);
		if (each && each != &window && each->isVisible()) torn = each;
	}
	QVERIFY2(torn, "released outside every window, and no window of its own opened");
	QVERIFY(!window.findChild<bssh_pane_container *>()->view_for(leaving));

	/* Put where the pointer let go, not wherever a new window lands. */
	QVERIFY2((torn->pos() - outside).manhattanLength() < 100,
	          qPrintable(QStringLiteral("opened at %1,%2 for a release at %3,%4")
	                     .arg(torn->x()).arg(torn->y()).arg(outside.x()).arg(outside.y())));

	torn->close();
	delete torn;
}

void layout_test::a_drag_released_on_another_windows_pane_places_it() {
	/*
	 * The target inside the screen, and the source clear of it: topLevelAt
	 * answers only for points on a screen, and the offscreen platform's is
	 * 800x600, so a target placed beside a full-size window is on no
	 * screen at all.
	 */
	const QRect screen = QGuiApplication::primaryScreen()->geometry();

	bssh_main_window from;
	from.resize(600, 400);
	from.move(screen.right() + 100, 0);
	from.show();
	QVERIFY(QTest::qWaitForWindowExposed(&from));

	bssh_main_window onto;
	onto.resize(qMin(700, screen.width()), qMin(500, screen.height()));
	onto.move(screen.topLeft());
	onto.show();
	QVERIFY(QTest::qWaitForWindowExposed(&onto));

	bssh_session_registry *registry = from.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);

	bssh_session *resident = registry->create(named(QStringLiteral("resident")));
	onto.adopt_session(resident);
	bssh_session *staying = registry->create(named(QStringLiteral("staying")));
	bssh_session *moving = registry->create(named(QStringLiteral("moving")));
	from.adopt_session(staying);
	from.adopt_session(moving);
	QCoreApplication::processEvents();

	bssh_terminal_view *pane = onto.panes()->view_for(resident);
	QVERIFY(pane);

	/*
	 * The LEFT edge of the other window's pane, because the right one
	 * gives the same order as a drop that found no pane and fell back to
	 * "beside" -- and a test that the fallback also passes asks nothing.
	 */
	/* The left zone, clear of the tab band along the very edge (sec 8.7a). */
	const QPoint at = pane->mapToGlobal(QPoint(pane->width() / 5, pane->height() / 2));
	QCOMPARE(QApplication::topLevelAt(at), &onto);

	from.finish_session_drag(moving, at);
	QCoreApplication::processEvents();

	QVERIFY2(onto.panes()->views().size() == 2,
	          "700px is MEDIUM, which holds two: the drop must have had room");
	QCOMPARE(onto.panes()->view_at(0), onto.panes()->view_for(moving));
	QCOMPARE(onto.panes()->view_at(1), onto.panes()->view_for(resident));
	QVERIFY(!from.panes()->view_for(moving));
}

void layout_test::the_screen_decides_the_arrangement_and_the_count() {
	using lm = bssh_layout_manager;
	const auto is = [](lm::arrangement_choice_t choice, lm::arrangement_t arrangement,
	                   int visible) {
		return choice.arrangement == arrangement && choice.visible == visible;
	};

	/* No screen to follow: the older answer -- touch stacks, a pointer tiles. */
	QVERIFY(is(lm::choose_arrangement(lm::PREFER_AUTO, false, QSize()), lm::TILED, 0));
	QVERIFY(is(lm::choose_arrangement(lm::PREFER_AUTO, true, QSize()), lm::COLUMN, 0));

	/* A phone: two held upright, one held sideways. */
	QVERIFY(is(lm::choose_arrangement(lm::PREFER_AUTO, true, QSize(360, 740)), lm::COLUMN, 2));
	QVERIFY(is(lm::choose_arrangement(lm::PREFER_AUTO, true, QSize(740, 360)), lm::COLUMN, 1));

	/* The Fold's cover screen sideways is landscape, whatever its width. */
	QVERIFY(is(lm::choose_arrangement(lm::PREFER_AUTO, true, QSize(700, 390)), lm::COLUMN, 1));

	/* Unfolded, either way up: side by side. */
	QVERIFY(is(lm::choose_arrangement(lm::PREFER_AUTO, true, QSize(673, 841)), lm::TILED, 0));
	QVERIFY(is(lm::choose_arrangement(lm::PREFER_AUTO, true, QSize(841, 673)), lm::TILED, 0));

	/*
	 * A choice made by hand stands, and a hand-chosen column on a device
	 * still counts by how it is held.
	 */
	QVERIFY(is(lm::choose_arrangement(lm::PREFER_TILED, true, QSize(360, 740)), lm::TILED, 0));
	QVERIFY(is(lm::choose_arrangement(lm::PREFER_COLUMN, true, QSize(841, 673)), lm::COLUMN, 1));
	QVERIFY(is(lm::choose_arrangement(lm::PREFER_COLUMN, false, QSize()), lm::COLUMN, 0));
}

void layout_test::turning_the_device_rearranges_the_window() {
	const forced_touch_t touch(1);
	forced_screen_t held(QSize(360, 740));

	bssh_main_window window;
	window.resize(360, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);
	window.adopt_session(registry->create(named(QStringLiteral("one"))));
	QCoreApplication::processEvents();

	bssh_pane_container *panes = window.panes();
	QVERIFY(panes);
	QCOMPARE(panes->column_visible(), 2);

	/*
	 * The on-screen keyboard shrinks the window and not the screen, and
	 * must not rebuild the terminal it is typing into.
	 */
	QPointer<bssh_terminal_view> before = panes->views().value(0);
	window.resize(360, 400);
	QCoreApplication::processEvents();
	QVERIFY2(before && panes->views().value(0) == before,
	          "opening the keyboard rebuilt the pane");

	/* Turned sideways: one on screen. */
	bssh_platform::force_screen_size(QSize(740, 360));
	window.resize(740, 340);
	QCoreApplication::processEvents();
	QCOMPARE(panes->column_visible(), 1);

	/* Unfolded: side by side. */
	bssh_platform::force_screen_size(QSize(673, 841));
	window.resize(673, 800);
	QCoreApplication::processEvents();
	QSplitter *root = panes->findChild<QSplitter *>();
	QVERIFY(root);
	QCOMPARE(panes->column_visible(), 0);
	QVERIFY2(!panes->findChild<QScrollArea *>()->isVisible(),
	          "an unfolded screen is still stacking its terminals in a column");
}

void layout_test::the_forms_start_on_the_chosen_keyboard() {
	/*
	 * The holder, 2026-10-22: "for the password it always brings up the
	 * android keyboard, we should be consistent with the chosen keyboard".
	 * The forms -- which the password prompt follows -- started on the
	 * system keyboard whatever the settings said. Both settings, so a
	 * forms keyboard that never followed the setting fails one of them.
	 */
	const forced_touch_t touch(1);
	forced_screen_t held(QSize(360, 740));
	bssh_app_settings settings;
	const bssh_app_settings::keyboard_t before = settings.keyboard();
	struct restore_t {
		bssh_app_settings::keyboard_t was;
		~restore_t() { bssh_app_settings().set_keyboard(was); }
	} restore{before};

	for (const bool built_in : { true, false }) {
		settings.set_keyboard(built_in ? bssh_app_settings::KEYBOARD_BUILT_IN
		                               : bssh_app_settings::KEYBOARD_SYSTEM);

		bssh_main_window window;
		window.resize(360, 700);
		window.show();
		window.activateWindow();
		QVERIFY(QTest::qWaitForWindowActive(&window));

		sk_keyboard *ours = window.findChild<sk_keyboard *>(QStringLiteral("form-keyboard"));
		sk_keyboard_switch *toggle = window.findChild<sk_keyboard_switch *>();
		QVERIFY(ours && toggle);
		toggle->set_panel_probe([] { return true; });

		QStackedWidget *pages = window.findChild<QStackedWidget *>();
		bssh_host_editor *editor = window.findChild<bssh_host_editor *>();
		QVERIFY(pages && editor);
		QWidget *page = editor;
		while (page && page->parentWidget() != pages) page = page->parentWidget();
		QVERIFY(page);
		pages->setCurrentWidget(page);
		QCoreApplication::processEvents();

		QLineEdit *field = nullptr;
		for (QLineEdit *each : editor->findChildren<QLineEdit *>())
			if (!field && each->isVisible() && !each->isReadOnly()) field = each;
		QVERIFY(field);
		field->setFocus();

		if (built_in) {
			QTRY_VERIFY2(ours->isVisible(), "BeerSSH's own keyboard was chosen and a form did not get it");
			QVERIFY2(!qApp->autoSipEnabled(), "the system keyboard is still asked for");
		} else {
			QTRY_VERIFY2(toggle->isVisible(), "the system keyboard was chosen and a form did not get it");
			QVERIFY(!ours->isVisible());
		}
		pages->setCurrentWidget(pages->widget(pages->count() - 1));
		QCoreApplication::processEvents();
	}
	QVERIFY(qApp->autoSipEnabled());
}

void layout_test::a_form_switches_to_softkeys_and_back() {
	const forced_touch_t touch(1);
	forced_screen_t held(QSize(360, 740));

	/*
	 * Starting from the system keyboard, set rather than assumed: the forms
	 * begin on the keyboard the settings chose (2026-10-22), and on touch
	 * the default is BeerSSH's own, so the switch would not be showing.
	 */
	bssh_app_settings settings;
	const bssh_app_settings::keyboard_t before = settings.keyboard();
	settings.set_keyboard(bssh_app_settings::KEYBOARD_SYSTEM);
	struct restore_t {
		bssh_app_settings::keyboard_t was;
		~restore_t() { bssh_app_settings().set_keyboard(was); }
	} restore{before};

	bssh_main_window window;
	window.resize(360, 700);
	window.show();
	window.activateWindow();
	QVERIFY(QTest::qWaitForWindowActive(&window));

	sk_keyboard *ours = window.findChild<sk_keyboard *>(QStringLiteral("form-keyboard"));
	sk_keyboard_switch *toggle = window.findChild<sk_keyboard_switch *>();
	QVERIFY(ours);
	QVERIFY(toggle);

	/* No system keyboard offscreen, so this says when there is one. */
	bool panel = true;
	toggle->set_panel_probe([&panel] { return panel; });

	/* The host editor's first line edit, on its page. */
	QStackedWidget *pages = window.findChild<QStackedWidget *>();
	bssh_host_editor *editor = window.findChild<bssh_host_editor *>();
	QVERIFY(pages);
	QVERIFY(editor);
	QWidget *page = editor;
	while (page && page->parentWidget() != pages) page = page->parentWidget();
	QVERIFY(page);
	pages->setCurrentWidget(page);
	QCoreApplication::processEvents();

	QLineEdit *field = nullptr;
	for (QLineEdit *each : editor->findChildren<QLineEdit *>()) {
		if (each->isVisible() && !each->isReadOnly()) {
			field = each;
			break;
		}
	}
	QVERIFY(field);
	field->setFocus();
	field->clear();
	QTRY_VERIFY(toggle->isVisible());
	QVERIFY(!ours->isVisible());
	QVERIFY(qApp->autoSipEnabled());

	toggle->click();
	QTRY_VERIFY(ours->isVisible());
	QVERIFY(!toggle->isVisible());
	QVERIFY2(!qApp->autoSipEnabled(), "the system keyboard is still asked for by every field");
	QCOMPARE(QApplication::focusWidget(), static_cast<QWidget *>(field));

	/* The editing page is one press of the side button away. */
	QCOMPARE(ours->pages().value(1), QStringLiteral("editing"));

	QAbstractButton *a = nullptr;
	for (QAbstractButton *button : ours->findChildren<QAbstractButton *>()) {
		if (button->text() == QStringLiteral("a") && button->isVisible()) a = button;
	}
	QVERIFY(a);
	a->click();
	QCOMPARE(field->text(), QStringLiteral("a"));

	/* Hide leaves the field, and the field brings it back. */
	ours->findChild<QAbstractButton *>(QStringLiteral("keyboard-hide"))->click();
	QTRY_VERIFY(!ours->isVisible());
	field->setFocus();
	QTRY_VERIFY(ours->isVisible());

	/*
	 * The terminal's page -- the last one built -- has its own keyboard
	 * and asks for the panel itself.
	 */
	pages->setCurrentWidget(pages->widget(pages->count() - 1));
	QCoreApplication::processEvents();
	QVERIFY(!ours->isVisible());
	QVERIFY(qApp->autoSipEnabled());

	/* Back on the form, and the system key undoes the switch. */
	pages->setCurrentWidget(page);
	field->setFocus();
	QTRY_VERIFY(ours->isVisible());
	ours->system_button()->click();
	QTRY_VERIFY(!ours->isVisible());
	QVERIFY(qApp->autoSipEnabled());
	QTRY_VERIFY(toggle->isVisible());
}

void layout_test::the_password_prompt_types_with_softkeys() {
	const forced_touch_t touch(1);
	forced_screen_t held(QSize(360, 740));

	bssh_password_prompt prompt(QStringLiteral("nabbe@example.com"), true);
	QLabel *note = prompt.findChild<QLabel *>(QStringLiteral("password-note"));
	sk_keyboard *keys = prompt.findChild<sk_keyboard *>(QStringLiteral("prompt-keyboard"));
	sk_keyboard_switch *toggle = prompt.findChild<sk_keyboard_switch *>();
	QLineEdit *field = prompt.findChild<QLineEdit *>();
	QVERIFY(note);
	QVERIFY(keys);
	QVERIFY(toggle);
	QVERIFY(field);
	toggle->set_panel_probe([] { return true; });


	QVERIFY(note->isHidden());
	prompt.set_note(QStringLiteral("The server did not accept that password."));
	QVERIFY(!note->isHidden());

	prompt.use_softkeys(true);
	prompt.show();
	prompt.activateWindow();
	QVERIFY(QTest::qWaitForWindowActive(&prompt));
	field->setFocus();
	QTRY_VERIFY(keys->isVisible());
	QVERIFY(!toggle->isVisible());
	QVERIFY2(!qApp->autoSipEnabled(), "the system keyboard is still asked for");

	/*
	 * The screen's width, so its keyboard is the terminal's width, and
	 * not the whole screen: a field, a box and two buttons needed no more
	 * (the holder, 2026-10-22). It was maximized.
	 */
	const QRect room = prompt.screen()->availableGeometry();
	QVERIFY2(!(prompt.windowState() & Qt::WindowMaximized), "the prompt fills the screen again");
	QCOMPARE(prompt.width(), qMin(room.width(), 360));
	QVERIFY2(prompt.height() < room.height(),
	          qPrintable(QStringLiteral("the prompt is %1 tall on a %2 screen")
	                             .arg(prompt.height()).arg(room.height())));

	/*
	 * On the screen's bottom edge while its keys are up, so they come up
	 * where the terminal's were; it sat at the top and the keys jumped up
	 * with it (the holder, 2026-10-23).
	 */
	QCOMPARE(prompt.frameGeometry().bottom(), room.bottom());

	/*
	 * The checkbox on the buttons' row, at its left: on a row of its own
	 * its 48dp touch height read as a gap above the buttons (the holder,
	 * 2026-10-23).
	 */
	QCheckBox *box = prompt.findChild<QCheckBox *>();
	QDialogButtonBox *button_box = prompt.findChild<QDialogButtonBox *>();
	QVERIFY(box);
	QVERIFY(button_box);
	{
		const QRect at(box->mapTo(&prompt, QPoint(0, 0)), box->size());
		const QRect row(button_box->mapTo(&prompt, QPoint(0, 0)), button_box->size());
		QVERIFY2(at.center().y() >= row.top() && at.center().y() <= row.bottom(),
		          qPrintable(QStringLiteral("the checkbox at %1-%2, the buttons at %3-%4")
		                             .arg(at.top()).arg(at.bottom()).arg(row.top()).arg(row.bottom())));
		QVERIFY(at.right() < row.left());
		QVERIFY(row.right() < prompt.width());

		/*
		 * And only as tall as it is at that width. The destination line
		 * was measured at a narrower default, and the height it asked for
		 * and did not use stood empty above the buttons (seen on the
		 * Fold, 2026-10-23). The two spacings either side of the stretch
		 * are all that may separate the field from them.
		 */
		const int gap = row.top() - field->mapTo(&prompt, QPoint(0, field->height())).y();
		QVERIFY2(gap <= 2 * prompt.layout()->spacing(),
		          qPrintable(QStringLiteral("%1px between the field and the buttons").arg(gap)));
	}

	/*
	 * And still whole when a margin arrives after it was sized. On the
	 * phone OK and Cancel were cut off at the bottom (2026-10-22), taken to
	 * be Android's safe area arriving late. NOTE what this does not prove:
	 * offscreen the layout grows the window to its new minimum by itself,
	 * so this passes with or without the prompt's own re-fit -- a sabotage
	 * removing it stayed green. The phone is the check for that.
	 */
	QPushButton *cancel = nullptr;
	for (QPushButton *button : prompt.findChildren<QPushButton *>())
		if (button->text().remove(QLatin1Char('&')) == QStringLiteral("Cancel")) cancel = button;
	QVERIFY(cancel);
	prompt.setContentsMargins(0, 80, 0, 0);
	QCoreApplication::processEvents();
	QVERIFY2(cancel->geometry().bottom() < prompt.height(),
	          qPrintable(QStringLiteral("Cancel ends at %1 in a prompt %2 tall")
	                             .arg(cancel->geometry().bottom()).arg(prompt.height())));
	prompt.setContentsMargins(0, 0, 0, 0);
	QCoreApplication::processEvents();

	/*
	 * And OK works the first time. Pressing it took the focus from the
	 * field, which hid the keyboard on the press, so the dialog shrank
	 * under the finger and the release landed off the button. Asserted on
	 * the mechanism: after the press, the field still has the focus, the
	 * keyboard is still up, and OK has not moved.
	 */
	QPushButton *ok = nullptr;
	for (QPushButton *button : prompt.findChildren<QPushButton *>())
		if (button->text().remove(QLatin1Char('&')) == QStringLiteral("OK")) ok = button;
	QVERIFY(ok);
	const QPoint ok_at = ok->mapToGlobal(QPoint(0, 0));
	QTest::mousePress(ok, Qt::LeftButton);
	QCoreApplication::processEvents();
	QVERIFY2(field->hasFocus(), "pressing OK took the focus from the field");
	QVERIFY2(keys->isVisible(), "pressing OK put the keyboard away under the finger");
	QCOMPARE(ok->mapToGlobal(QPoint(0, 0)), ok_at);
	QTest::mouseRelease(ok, Qt::LeftButton);
	QCOMPARE(prompt.result(), int(QDialog::Accepted));
	prompt.show();
	QVERIFY(QTest::qWaitForWindowActive(&prompt));
	field->setFocus();
	QTRY_VERIFY(keys->isVisible());

	QAbstractButton *a = nullptr;
	for (QAbstractButton *button : keys->findChildren<QAbstractButton *>()) {
		if (button->text() == QStringLiteral("a") && button->isVisible()) a = button;
	}
	QVERIFY(a);
	a->click();
	QCOMPARE(prompt.password(), QStringLiteral("a"));

	keys->system_button()->click();
	QTRY_VERIFY(!keys->isVisible());
	QVERIFY(!prompt.uses_softkeys());
	QVERIFY(qApp->autoSipEnabled());

	/* And at the top with Android's, which comes up below it. */
	QTRY_COMPARE(prompt.frameGeometry().top(), room.top());
	QTRY_VERIFY(toggle->isVisible());

	toggle->click();
	QTRY_VERIFY(keys->isVisible());
	QVERIFY(!qApp->autoSipEnabled());

	/*
	 * Android's Back is Cancel. The prompt ignored it, so Android took it
	 * and sent the app to the background with the prompt still open
	 * (seen on the Fold, 2026-10-23). Sent to the field, where it lands.
	 */
	QKeyEvent back(QEvent::KeyPress, Qt::Key_Back, Qt::NoModifier);
	QApplication::sendEvent(field, &back);
	QCoreApplication::processEvents();
	QCOMPARE(prompt.result(), int(QDialog::Rejected));
	QVERIFY2(!prompt.isVisible(), "Back left the prompt open");

	/* Closing gives the system keyboard back to everything else. */
	QVERIFY(qApp->autoSipEnabled());

	/*
	 * Too narrow for the checkbox beside both buttons, it takes a row of
	 * its own above them: it does not wrap, and a row that did not fit
	 * pushed the prompt past the screen's edge once before (sec 11).
	 */
	{
		forced_screen_t narrow(QSize(200, 640));
		bssh_password_prompt small(QStringLiteral("nabbe@example.com"), true);
		small.use_softkeys(true);
		small.show();
		QVERIFY(QTest::qWaitForWindowExposed(&small));
		QCheckBox *alone = small.findChild<QCheckBox *>();
		QDialogButtonBox *under = small.findChild<QDialogButtonBox *>();
		QVERIFY(alone);
		QVERIFY(under);
		QTRY_VERIFY(alone->mapTo(&small, QPoint(0, alone->height())).y()
		            <= under->mapTo(&small, QPoint(0, 0)).y());
		QCOMPARE(small.width(), 200);
	}
}

void layout_test::a_column_shows_the_count_and_scrolls_to_the_rest() {
	const forced_touch_t touch(1);
	const forced_screen_t held(QSize(740, 360));

	bssh_main_window window;
	window.resize(740, 340);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);
	/* Two panes, made as panes: a session opened in a column is a tab (sec 8.3). */
	bssh_session *one = registry->create(named(QStringLiteral("one")));
	window.adopt_session(one);
	window.drop_session_on(registry->create(named(QStringLiteral("two"))), one,
	                       bssh_layout_manager::EDGE_BOTTOM);
	QCoreApplication::processEvents();

	bssh_pane_container *panes = window.panes();
	QScrollArea *scroll = panes->findChild<QScrollArea *>();
	QVERIFY(scroll && scroll->isVisible());
	QCOMPARE(panes->views().size(), 2);
	QCOMPARE(panes->column_visible(), 1);

	/*
	 * Held sideways, one terminal fills the screen and the other is
	 * scrolled to: two viewports' worth of column.
	 */
	const int viewport = scroll->viewport()->height();
	QVERIFY(viewport > 0);
	QCOMPARE(scroll->widget()->minimumHeight(), viewport * 2);

	/* Upright, two share it and nothing scrolls. */
	panes->set_column_visible(2);
	QCOMPARE(scroll->widget()->minimumHeight(), 0);
}

void layout_test::the_tab_bar_goes_where_it_is_set() {
	const bssh_app_settings::tab_position_t was = bssh_app_settings().tab_position();
	bssh_app_settings().set_tab_position(bssh_app_settings::TABS_TOP);

	{
		bssh_main_window window;
		window.resize(1000, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));

		bssh_session_registry *registry = window.registry();
		QVERIFY(registry);
		const registry_left_as_found_t tidy(registry);
		bssh_session *one = registry->create(named(QStringLiteral("one")));
		window.adopt_session(one);
		window.adopt_session(registry->create(named(QStringLiteral("two"))));
		QCoreApplication::processEvents();

		/*
		 * Each pane's bar on the set side of that pane's terminal (sec 8.3's
		 * bar per pane). Rebuilt with the panes when the side changes, so it
		 * is asked for afresh each time.
		 */
		const auto bar_now = [&window, one]() -> QTabBar * {
			bssh_pane_bar *row = window.bar_for(window.layout_manager()->front_of(one));
			return row && row->isVisible() ? row->tab_bar() : nullptr;
		};
		const auto view_now = [&window, one]() -> QWidget * {
			return window.panes()->view_for(window.layout_manager()->front_of(one));
		};
		const auto global = [](QWidget *w) { return QRect(w->mapToGlobal(QPoint(0, 0)), w->size()); };

		QTabBar *bar = bar_now();
		QVERIFY(bar && view_now());

		/* The control: where it has always been. */
		QVERIFY(global(bar).bottom() <= global(view_now()).top());

		bssh_app_settings().set_tab_position(bssh_app_settings::TABS_LEFT);
		window.apply_device_settings();
		QCoreApplication::processEvents();

		bar = bar_now();
		QVERIFY(bar);
		QCOMPARE(bar->shape(), QTabBar::RoundedWest);
		QVERIFY2(global(bar).right() <= global(view_now()).left(),
		          "set to the left and not beside the terminal on the left");

		/*
		 * A drop anywhere down that side is on the bar, and one at the top
		 * of the terminal -- where the bar used to be -- is not.
		 */
		const QPoint low_on_the_side = window.mapFromGlobal(
		        QPoint(global(bar).center().x(), global(view_now()).bottom() - 10));
		const QPoint above_the_terminal = window.mapFromGlobal(
		        QPoint(global(view_now()).center().x(), global(view_now()).top() + 5));
		QVERIFY(window.tab_row_at(low_on_the_side));
		QVERIFY(!window.tab_row_at(above_the_terminal));

		bssh_app_settings().set_tab_position(bssh_app_settings::TABS_BOTTOM);
		window.apply_device_settings();
		QCoreApplication::processEvents();

		bar = bar_now();
		QVERIFY(bar);
		QCOMPARE(bar->shape(), QTabBar::RoundedSouth);
		QVERIFY2(global(bar).top() >= global(view_now()).bottom(),
		          "set to the bottom and not below the terminal");
	}

	bssh_app_settings().set_tab_position(was);
}

void layout_test::no_terminal_is_lost_however_the_panes_are_moved() {
	/*
	 * The holder: "There are many bugs when creating and moving around
	 * tiled terminals. terminals disappear etc." (2026-10-30). A walk of
	 * what a user does to panes -- open, split, drop on an edge or the
	 * middle, tab, pick, move, close, narrow and widen -- asking after
	 * every step whether each session can still be reached: shown in a
	 * pane with a view, or a tab in a shown pane's bar. Seeded, and each
	 * failure prints the steps that led to it.
	 */
	const forced_touch_t desktop(0);
	struct restore_t {
		bssh_app_settings::tab_position_t tabs = bssh_app_settings().tab_position();
		~restore_t() { bssh_app_settings().set_tab_position(tabs); }
	} restore;

	const int seeds = qEnvironmentVariableIntValue("BSSH_WALK_SEEDS") > 0
	        ? qEnvironmentVariableIntValue("BSSH_WALK_SEEDS") : 12;
	for (int seed = 1; seed <= seeds; ++seed) {
		bssh_app_settings().set_tab_position(bssh_app_settings::TABS_TOP);
		bssh_main_window window;
		window.resize(1400, 800);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		const registry_left_as_found_t tidy(window.registry());
		bssh_session_registry *registry = window.registry();
		bssh_layout_manager *layout = window.layout_manager();

		QRandomGenerator random { quint32(seed) };
		int asked_width = 1400;
		QStringList steps;
		QHash<bssh_session *, QString> names;
		int made = 0;
		const auto make = [&]() {
			bssh_session *session = registry->create(named(QStringLiteral("s%1").arg(made)));
			names.insert(session, QStringLiteral("s%1").arg(made++));
			return session;
		};
		/* Sessions the walk did not make itself -- "split beside" opens
		 * one -- are named as they are first met, so a trace can tell
		 * them apart. */
		int met = 0;
		const auto name = [&names, &met](bssh_session *session) {
			if (!session) return QStringLiteral("null");
			if (!names.contains(session)) names.insert(session, QStringLiteral("n%1").arg(met++));
			return names.value(session);
		};
		const auto any = [&random](const QList<bssh_session *> &from) -> bssh_session * {
			return from.isEmpty() ? nullptr : from.at(int(random.bounded(quint32(from.size()))));
		};
		const auto settle = [] {
			for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
		};

		/* Every session reachable, and what is drawn is what the model says. */
		const auto lost = [&]() -> QString {
			const QList<bssh_session *> visible = layout->visible_panes();
			for (bssh_session *session : registry->sessions()) {
				if (!layout->sessions().contains(session))
					return QStringLiteral("%1 is in no pane and no tab").arg(name(session));
				bool reached = visible.contains(session);
				for (bssh_session *front : visible)
					reached = reached || layout->bar_tabs(front).contains(session);
				if (!reached)
					return QStringLiteral("%1 is neither shown nor a shown pane's tab").arg(name(session));
			}
			/* Nothing in it holds the window wider than it was asked to be:
			 * tabs down a side and panes' minimums both once did. */
			if (window.width() > asked_width)
				return QStringLiteral("held at %1 wide when asked for %2").arg(window.width())
				        .arg(asked_width);
			if (window.panes()->views().size() != visible.size())
				return QStringLiteral("%1 views for %2 panes").arg(window.panes()->views().size())
				        .arg(visible.size());
			for (bssh_session *front : visible) {
				bssh_terminal_view *view = window.panes()->view_for(front);
				if (!view || !view->isVisible())
					return QStringLiteral("%1 has no view on screen").arg(name(front));
				const QList<bssh_session *> tabs = layout->bar_tabs(front);
				bssh_pane_bar *bar = window.bar_for(front);
				if (tabs.size() > 1 && (!bar || !bar->isVisible() || bar->sessions() != tabs))
					return QStringLiteral("%1's bar does not list its %2 tabs").arg(name(front))
					        .arg(tabs.size());
			}
			const QList<bssh_session *> held = window.shown_sessions();
			const QList<bssh_session *> placed = layout->sessions();
			if (QSet<bssh_session *>(held.begin(), held.end())
			    != QSet<bssh_session *>(placed.begin(), placed.end()))
				return QStringLiteral("the window holds %1 terminals and its panes %2")
				        .arg(held.size()).arg(placed.size());
			return QString();
		};

		window.adopt_session(make());
		settle();
		for (int step = 0; step < 60; ++step) {
			const QList<bssh_session *> all = registry->sessions();
			const QList<bssh_session *> visible = layout->visible_panes();
			bssh_session *session = any(all);
			bssh_session *target = any(visible);
			QString did;
			const bool trace = qEnvironmentVariableIsSet("BSSH_WALK_TRACE");
			if (trace) fprintf(stderr, "walk: seed %d step %d\n", seed, step);
			switch (random.bounded(14)) {
			case 0: {
				bssh_session *fresh = make();
				window.adopt_session(fresh);
				did = QStringLiteral("open %1").arg(name(fresh));
				break;
			}
			case 1: {
				bssh_session *fresh = make();
				window.adopt_session(fresh);
				window.split_with(fresh);
				did = QStringLiteral("open %1 and split").arg(name(fresh));
				break;
			}
			case 2:
			case 3: {
				const int edge = int(random.bounded(5));
				did = QStringLiteral("drop %1 on %2 at %3").arg(name(session), name(target)).arg(edge);
				window.drop_session_on(session, target, edge);
				break;
			}
			case 4: {
				const int slot = int(random.bounded(4));
				did = QStringLiteral("tab %1 into %2 at %3").arg(name(session), name(target)).arg(slot);
				window.drop_session_as_tab(session, target, slot);
				break;
			}
			case 5: {
				/* As a click on its tab does, through the bar. */
				did = QStringLiteral("pick %1").arg(name(session));
				for (bssh_session *front : visible) {
					bssh_pane_bar *bar = window.bar_for(front);
					const int at = bar && bar->isVisible() ? bar->sessions().indexOf(session) : -1;
					if (at >= 0) bar->tab_bar()->setCurrentIndex(at);
				}
				break;
			}
			case 6: {
				const int direction = random.bounded(2) ? 1 : -1;
				did = QStringLiteral("move pane %1 by %2").arg(name(target)).arg(direction);
				window.move_pane(target, direction);
				break;
			}
			case 7:
				if (all.size() < 3) continue;
				did = QStringLiteral("close %1").arg(name(session));
				registry->close(session);
				break;
			case 11: {
				/* The bars to another side, as the handle or the setting does. */
				static const bssh_app_settings::tab_position_t sides[] = {
					bssh_app_settings::TABS_TOP, bssh_app_settings::TABS_BOTTOM,
					bssh_app_settings::TABS_LEFT, bssh_app_settings::TABS_RIGHT };
				const bssh_app_settings::tab_position_t side = sides[random.bounded(4)];
				did = QStringLiteral("tabs to side %1").arg(int(side));
				bssh_app_settings().set_tab_position(side);
				window.apply_device_settings();
				break;
			}
			case 12:
			case 13: {
				/*
				 * A real drag's release, anywhere in the window: what is
				 * under the point decides -- a pane's edge or middle, a
				 * tab row, or nothing -- through the same resolution a
				 * QDrag ends in. The tab rows exist only while a drag is
				 * on, so it is turned on as a drag does.
				 */
				/*
				 * Only where the window is on its screen: the offscreen
				 * platform's is 800x600, and a point past it is outside
				 * every window to topLevelAt() -- a tear-out, which takes
				 * the terminal into a window this walk does not watch.
				 */
				const QRect seen = QRect(window.mapToGlobal(QPoint(0, 0)), window.size())
				                   & window.screen()->geometry();
				if (seen.isEmpty()) continue;
				const QPoint at = window.mapFromGlobal(
				        seen.topLeft() + QPoint(int(random.bounded(quint32(seen.width()))),
				                                int(random.bounded(quint32(seen.height())))));
				did = QStringLiteral("release %1 at %2,%3").arg(name(session)).arg(at.x()).arg(at.y());
				window.set_session_drag_active(true);
				QCoreApplication::processEvents();
				window.finish_session_drag(session, window.mapToGlobal(at));
				window.set_session_drag_active(false);
				break;
			}
			case 10:
				/* As the drag to another BeerSSH ends here (sec 8.4). */
				if (all.size() < 3) continue;
				did = QStringLiteral("give %1 to another process").arg(name(session));
				registry->give_away(session);
				break;
			case 8: {
				static const int widths[] = { 500, 1000, 1400, 2000 };
				const int width = widths[random.bounded(4)];
				did = QStringLiteral("width %1").arg(width);
				asked_width = width;
				window.resize(width, 800);
				break;
			}
			default:
				did = QStringLiteral("split beside the current one");
				window.split_beside_current();
				break;
			}
			if (trace) fprintf(stderr, "walk:   %s\n", qPrintable(did));
			if (trace) {
				settle();
				fprintf(stderr, "walk:   tree %s\n",
				        qPrintable(bssh_layout_manager::shape_text(layout->tree(), name)));
			}
			settle();
			steps << did;
			/* Only what lasts: a pane is allowed a moment to come up. */
			QString why = lost();
			if (!why.isEmpty()) {
				QTest::qWait(300);
				why = lost();
			}
			if (!why.isEmpty()) {
				QStringList visible_names, intent_names, view_names;
				for (bssh_session *front : layout->visible_panes()) visible_names << name(front);
				for (bssh_session *pane : layout->intent()) intent_names << name(pane);
				for (bssh_terminal_view *view : window.panes()->views())
					view_names << (view && view->isVisible() ? QStringLiteral("shown") : QStringLiteral("hidden"));
				why += QStringLiteral("\n  tree %1\n  visible %2 (max %3), intent %4, views %5")
				        .arg(bssh_layout_manager::shape_text(layout->tree(), name),
				             visible_names.join(QLatin1Char(','))).arg(layout->max_panes())
				        .arg(intent_names.join(QLatin1Char(',')), view_names.join(QLatin1Char(',')));
			}
			QVERIFY2(why.isEmpty(), qPrintable(QStringLiteral("seed %1: %2\n  after: %3")
			                                           .arg(seed).arg(why, steps.join(QStringLiteral("\n         ")))));
		}
	}
}

void layout_test::a_wide_window_tiles_as_many_as_fit() {
	/*
	 * The holder: "there seems to be a limit of 4 terminals" (2026-10-30).
	 * A desktop's wide window shows as many panes as fit at a terminal's
	 * least, the rest tabs until there is room (sec 8.2) -- and it can
	 * still be made narrow, which a sum of the panes' minimums forbade.
	 */
	const forced_touch_t desktop(0);
	struct restore_t {
		bssh_app_settings::tab_position_t tabs = bssh_app_settings().tab_position();
		~restore_t() { bssh_app_settings().set_tab_position(tabs); }
	} restore;
	bssh_app_settings().set_tab_position(bssh_app_settings::TABS_TOP);

	bssh_main_window window;
	window.resize(1600, 1000);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	const registry_left_as_found_t tidy(window.registry());
	bssh_layout_manager *layout = window.layout_manager();
	QCOMPARE(layout->arrangement(), bssh_layout_manager::TILED);

	bssh_session *first = window.registry()->create(named(QStringLiteral("p0")));
	window.adopt_session(first);
	QTest::qWait(50);

	/*
	 * Each new terminal opens as a tab of the focused pane and is split
	 * off its right edge until a split is refused -- by the width running
	 * out, well short of the ceiling, so the refusal is the fit's.
	 */
	for (int i = 1; i < 30; ++i) {
		const int edge = bssh_layout_manager::EDGE_RIGHT;
		bssh_session *next = window.registry()->create(named(QStringLiteral("p%1").arg(i)));
		window.adopt_session(next);
		QTest::qWait(30);
		bssh_session *pane = layout->front_of(next);
		if (!layout->room_to_split(pane, edge)) break;
		window.drop_session_on(next, pane, edge);
		QTest::qWait(30);
	}
	const int wide = layout->visible_panes().size();
	QVERIFY2(wide > 4 && wide < bssh_layout_manager::BSSH_TILED_PANE_LIMIT,
	          qPrintable(QStringLiteral("%1 panes in a 1600x1000 window").arg(wide)));
	/* Every split that was offered made a pane: none was offered that
	 * could only become a tab. */
	QCOMPARE(layout->intent().size(), wide);
	QCOMPARE(window.panes()->views().size(), wide);
	for (bssh_terminal_view *view : window.panes()->views()) {
		QVERIFY(view->isVisible());
		QVERIFY2(view->cols() >= 20 && view->rows() >= 4,
		          qPrintable(QStringLiteral("a pane of %1x%2").arg(view->cols()).arg(view->rows())));
	}

	/* Narrowed: the window does narrow, fewer panes, nothing lost. */
	window.resize(1000, 1000);
	QTest::qWait(100);
	QVERIFY2(window.width() <= 1000, qPrintable(QStringLiteral("held at %1 wide").arg(window.width())));
	const int narrow = layout->visible_panes().size();
	QVERIFY2(narrow < wide, qPrintable(QStringLiteral("%1 then %2").arg(wide).arg(narrow)));
	for (bssh_terminal_view *view : window.panes()->views())
		QVERIFY2(view->cols() >= 20, qPrintable(QStringLiteral("squeezed to %1 columns").arg(view->cols())));
	QList<bssh_session *> reached = layout->visible_panes();
	for (bssh_session *front : layout->visible_panes()) reached += layout->bar_tabs(front);
	for (bssh_session *session : window.registry()->sessions())
		QVERIFY2(reached.contains(session), "a terminal went with the room");

	/* And back. */
	window.resize(1600, 1000);
	QTest::qWait(100);
	QCOMPARE(layout->visible_panes().size(), wide);
}

void layout_test::moving_a_pane_where_it_does_not_fit_folds_no_other() {
	/*
	 * A row as full as it fits, and a pane stacked under its last one.
	 * Moved beside the first, the stacked pane would need a column the
	 * window has not got -- and the fold that keeps a tree within its
	 * room would then turn the LAST pane into a tab: a drop on one
	 * terminal making another disappear. The edge is drawn and done as
	 * the swap instead, as it is for a new pane with no room.
	 */
	const forced_touch_t desktop(0);
	struct restore_t {
		bssh_app_settings::tab_position_t tabs = bssh_app_settings().tab_position();
		~restore_t() { bssh_app_settings().set_tab_position(tabs); }
	} restore;
	bssh_app_settings().set_tab_position(bssh_app_settings::TABS_TOP);

	bssh_main_window window;
	window.resize(1600, 1000);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	const registry_left_as_found_t tidy(window.registry());
	bssh_layout_manager *layout = window.layout_manager();

	bssh_session *first = window.registry()->create(named(QStringLiteral("p0")));
	window.adopt_session(first);
	QTest::qWait(50);
	bssh_session *last = first;
	for (int i = 1; i < 30; ++i) {
		bssh_session *next = window.registry()->create(named(QStringLiteral("p%1").arg(i)));
		window.adopt_session(next);
		QTest::qWait(30);
		bssh_session *pane = layout->front_of(next);
		if (!layout->room_to_split(pane, bssh_layout_manager::EDGE_RIGHT)) {
			window.registry()->close(next);
			break;
		}
		window.drop_session_on(next, pane, bssh_layout_manager::EDGE_RIGHT);
		QTest::qWait(30);
		last = next;
	}

	bssh_session *under = window.registry()->create(named(QStringLiteral("under")));
	window.adopt_session(under);
	QTest::qWait(30);
	QVERIFY(layout->room_to_split(layout->front_of(under), bssh_layout_manager::EDGE_BOTTOM));
	window.drop_session_on(under, layout->front_of(under), bssh_layout_manager::EDGE_BOTTOM);
	QTest::qWait(50);
	const QList<bssh_session *> before = layout->visible_panes();
	QVERIFY(before.contains(under) && before.contains(first) && before.contains(last));
	QVERIFY2(before.size() > 4, qPrintable(QString::number(before.size())));

	QCOMPARE(window.drop_zone_for(under, first, bssh_layout_manager::EDGE_LEFT),
	         int(bssh_layout_manager::EDGE_MIDDLE));
	window.drop_session_on(under, first, bssh_layout_manager::EDGE_LEFT);
	QTest::qWait(50);
	const QList<bssh_session *> after = layout->visible_panes();
	QCOMPARE(after.size(), before.size());
	for (bssh_session *pane : before)
		QVERIFY2(after.contains(pane), "a pane that was not moved became a tab");
}

void layout_test::tabs_down_the_side_scroll_and_read_across() {
	/*
	 * The holder: "When tabs are on left or right they force the terminal
	 * longer, and they should be like browser tabs horizontal and scroll
	 * up and down, not stretch the window" (sec 8.7a).
	 */
	const forced_touch_t desktop(0);
	struct restore_t {
		bssh_app_settings::tab_position_t tabs = bssh_app_settings().tab_position();
		~restore_t() { bssh_app_settings().set_tab_position(tabs); }
	} restore;
	bssh_app_settings().set_tab_position(bssh_app_settings::TABS_LEFT);

	bssh_main_window window;
	window.resize(900, 500);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	const registry_left_as_found_t tidy(window.registry());

	bssh_session *first = window.registry()->create(named(QStringLiteral(
	        "a title far longer than any tab down the side should be as wide as")));
	window.adopt_session(first);
	for (int i = 0; i < 30; ++i)
		window.adopt_session(window.registry()->create(named(QStringLiteral("tab %1").arg(i))));
	QCoreApplication::processEvents();

	bssh_pane_bar *row = window.bar_for(window.layout_manager()->front_of(first));
	QVERIFY(row && row->isVisible());
	QTabBar *bar = row->tab_bar();
	QCOMPARE(bar->shape(), QTabBar::RoundedWest);
	QVERIFY2(bar->count() > 20, qPrintable(QString::number(bar->count())));

	/* Thirty tabs are taller than the window: it is not made taller. */
	QVERIFY2(bar->sizeHint().height() > 500, qPrintable(QString::number(bar->sizeHint().height())));
	QVERIFY2(window.height() <= 500 && window.minimumSizeHint().height() <= 500,
	          qPrintable(QStringLiteral("%1, wanting %2").arg(window.height())
	                             .arg(window.minimumSizeHint().height())));

	/* Across, not on their side: a short title is wider than it is tall. */
	const QRect tab = bar->tabRect(bar->count() - 1);
	QVERIFY2(tab.width() > tab.height(),
	          qPrintable(QStringLiteral("%1x%2").arg(tab.width()).arg(tab.height())));

	/* And a long one does not make the bar as wide as itself. */
	const int widest = bar->fontMetrics().averageCharWidth() * 30;
	QVERIFY2(row->width() < widest,
	          qPrintable(QStringLiteral("%1 wide").arg(row->width())));

	/* The wheel scrolls the tabs rather than switching them. */
	QScrollArea *scroll = row->findChild<QScrollArea *>(QStringLiteral("pane-bar-scroll"));
	QVERIFY(scroll);
	QScrollBar *slider = scroll->verticalScrollBar();
	QVERIFY2(slider->maximum() > 0, "thirty tabs and nothing to scroll");
	const int current = bar->currentIndex();
	const QPoint over = bar->tabRect(0).center();
	QWheelEvent wheel(over, bar->mapToGlobal(over), QPoint(), QPoint(0, -120), Qt::NoButton,
	                  Qt::NoModifier, Qt::NoScrollPhase, false);
	QCoreApplication::sendEvent(bar, &wheel);
	QVERIFY2(slider->value() > 0, "the wheel did not scroll the tabs");
	QCOMPARE(bar->currentIndex(), current);

	/*
	 * Picking a tab far down rebuilds the panes and the bar with them; the
	 * new list shows the tab picked, rather than starting at the top with
	 * the current one scrolled out of sight.
	 */
	bssh_session *far = row->session_at(bar->count() - 2);
	QVERIFY(far);
	bar->setCurrentIndex(bar->count() - 2);
	QTRY_COMPARE(window.layout_manager()->front_of(first), far);
	QTest::qWait(50);
	row = window.bar_for(far);
	QVERIFY(row);
	scroll = row->findChild<QScrollArea *>(QStringLiteral("pane-bar-scroll"));
	QVERIFY(scroll);
	const QRect picked = row->tab_bar()->tabRect(row->tab_bar()->currentIndex());
	/* mapTo, from the tab bar up to its ancestor: mapFrom takes the
	 * ancestor as its argument and reads nonsense the other way round. */
	const QRect seen(row->tab_bar()->mapTo(scroll->viewport(), picked.topLeft()), picked.size());
	QVERIFY2(seen.top() >= 0 && seen.bottom() <= scroll->viewport()->height(),
	          qPrintable(QStringLiteral("the picked tab at %1,%2 is outside the %3-tall list")
	                             .arg(seen.x()).arg(seen.y()).arg(scroll->viewport()->height())));

	/* And when the list grows and shrinks again -- a keyboard opening --
	 * the tab in front stays in sight. */
	window.resize(900, 900);
	QTest::qWait(50);
	window.resize(900, 300);
	QTest::qWait(50);
	{
		const QRect front = row->tab_bar()->tabRect(row->tab_bar()->currentIndex());
		const QRect at(row->tab_bar()->mapTo(scroll->viewport(), front.topLeft()), front.size());
		QVERIFY2(at.top() >= 0 && at.bottom() <= scroll->viewport()->height(),
		          qPrintable(QStringLiteral("after a shrink the front tab is at %1 in a %2-tall list")
		                             .arg(at.y()).arg(scroll->viewport()->height())));
	}
}

void layout_test::side_tabs_take_a_tap_and_a_swipe_on_touch() {
	/*
	 * Down the side the tabs sit in a scroll area a thumb drags (sec
	 * 8.7a), over a tab bar with its own long press. A scroller that
	 * swallowed the press would leave the tabs untappable on a tablet or
	 * an unfolded Fold -- the places a touch window tiles.
	 */
	const forced_touch_t touch(1);
	const forced_screen_t screen(QSize(1800, 1200));
	struct restore_t {
		bssh_app_settings::tab_position_t tabs = bssh_app_settings().tab_position();
		bssh_app_settings::arrangement_t arrangement = bssh_app_settings().arrangement();
		~restore_t() {
			bssh_app_settings().set_tab_position(tabs);
			bssh_app_settings().set_arrangement(arrangement);
		}
	} restore;
	bssh_app_settings().set_tab_position(bssh_app_settings::TABS_LEFT);
	bssh_app_settings().set_arrangement(bssh_app_settings::ARRANGEMENT_TILED);

	bssh_main_window window;
	window.resize(780, 500);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	const registry_left_as_found_t tidy(window.registry());
	QCOMPARE(window.layout_manager()->arrangement(), bssh_layout_manager::TILED);

	bssh_session *first = window.registry()->create(named(QStringLiteral("t0")));
	window.adopt_session(first);
	for (int i = 1; i < 30; ++i)
		window.adopt_session(window.registry()->create(named(QStringLiteral("t%1").arg(i))));
	QTest::qWait(50);

	const auto bar_now = [&window, first]() -> bssh_pane_bar * {
		return window.bar_for(window.layout_manager()->front_of(first));
	};
	bssh_pane_bar *row = bar_now();
	QVERIFY(row && row->isVisible());
	QTabBar *bar = row->tab_bar();
	QCOMPARE(bar->shape(), QTabBar::RoundedWest);

	/* Each tab at least sec 6.2's 48dp tall: the target a thumb picks. */
	for (int i = 0; i < bar->count(); ++i)
		QVERIFY2(bar->tabRect(i).height() >= BSSH_TOUCH_TARGET_DP,
		          qPrintable(QStringLiteral("a side tab %1 tall").arg(bar->tabRect(i).height())));

	/*
	 * A tap on a tab that is not in front brings it forward -- one on
	 * screen, since the list opens on the tab in front, the last.
	 */
	QScrollArea *list = row->findChild<QScrollArea *>(QStringLiteral("pane-bar-scroll"));
	QVERIFY(list);
	int shown = -1;
	for (int i = bar->count() - 2; i >= 0; --i) {
		const QPoint at = bar->mapTo(list->viewport(), bar->tabRect(i).center());
		if (list->viewport()->rect().contains(at)) {
			shown = i;
			break;
		}
	}
	QVERIFY2(shown >= 0, "no tab but the front one is on screen");
	bssh_session *wanted = row->session_at(shown);
	QVERIFY(wanted && wanted != window.layout_manager()->front_of(first));
	const QPoint on_tab = bar->tabRect(shown).center();
	QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, on_tab);
	QTest::qWait(20);
	QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, on_tab);
	QTRY_COMPARE(window.layout_manager()->front_of(first), wanted);

	/* A thumb dragged up the list scrolls it. */
	row = bar_now();
	QVERIFY(row);
	bar = row->tab_bar();
	QScrollArea *scroll = row->findChild<QScrollArea *>(QStringLiteral("pane-bar-scroll"));
	QVERIFY(scroll && scroll->verticalScrollBar()->maximum() > 0);
	QWidget *viewport = scroll->viewport();
	const QPoint low(viewport->width() / 2, viewport->height() - 20);
	QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, low);
	for (int step = 1; step <= 12; ++step) {
		QTest::qWait(16);
		QMouseEvent move(QEvent::MouseMove, low - QPoint(0, step * 20), viewport->mapToGlobal(low - QPoint(0, step * 20)),
		                 Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
		QCoreApplication::sendEvent(viewport, &move);
	}
	QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, low - QPoint(0, 240));
	QTRY_VERIFY2(scroll->verticalScrollBar()->value() > 0, "a swipe up the side tabs did not scroll them");

	/* A swipe that starts ON a tab scrolls and picks nothing. */
	{
		bssh_session *front = window.layout_manager()->front_of(first);
		scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
		QCoreApplication::processEvents();
		int on = -1;
		for (int i = bar->count() - 1; i >= 0; --i) {
			const QPoint at = bar->mapTo(scroll->viewport(), bar->tabRect(i).center());
			if (scroll->viewport()->rect().contains(at) && row->session_at(i) != front) {
				on = i;
				break;
			}
		}
		QVERIFY(on >= 0);
		const int before = scroll->verticalScrollBar()->value();
		const QPoint from = bar->tabRect(on).center();
		QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, from);
		for (int step = 1; step <= 10; ++step) {
			const QPoint to = from + QPoint(0, step * 20);
			QMouseEvent move(QEvent::MouseMove, to, bar->mapToGlobal(to), Qt::NoButton, Qt::LeftButton,
			                 Qt::NoModifier);
			QCoreApplication::sendEvent(bar, &move);
		}
		QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, from + QPoint(0, 200));
		QTest::qWait(50);
		QVERIFY2(scroll->verticalScrollBar()->value() < before, "a swipe down from a tab did not scroll");
		QCOMPARE(window.layout_manager()->front_of(first), front);
		/* And the bar still marks the tab in front, not the one the
		 * swipe began on. */
		QCOMPARE(row->session_at(bar->currentIndex()), front);
	}

	/* Along the top, the bar is as thick as a thumb needs too. */
	bssh_app_settings().set_tab_position(bssh_app_settings::TABS_TOP);
	window.apply_device_settings();
	QCoreApplication::processEvents();
	row = bar_now();
	QVERIFY(row && row->isVisible());
	bar = row->tab_bar();
	QCOMPARE(bar->shape(), QTabBar::RoundedNorth);
	QVERIFY2(bar->tabRect(0).height() >= BSSH_TOUCH_TARGET_DP,
	          qPrintable(QStringLiteral("a top tab %1 tall").arg(bar->tabRect(0).height())));
}

void layout_test::a_touch_windows_pane_controls_reach_the_floor() {
	/*
	 * Sec 6.2's sweep holds every touch surface on its list to 48dp, and
	 * the pane's own controls were not on it: the tab bars and the corner
	 * are built inside a window, at run time, rather than being a dialog
	 * the sweep can open. Their tabs measured about 27dp and their buttons
	 * less. Asked here of a real touch window: every button in a pane bar
	 * or a pane corner that shows.
	 */
	const forced_touch_t touch(1);
	const forced_screen_t screen(QSize(1800, 1200));
	struct restore_t {
		bssh_app_settings::tab_position_t tabs = bssh_app_settings().tab_position();
		bssh_app_settings::arrangement_t arrangement = bssh_app_settings().arrangement();
		~restore_t() {
			bssh_app_settings().set_tab_position(tabs);
			bssh_app_settings().set_arrangement(arrangement);
		}
	} restore;
	bssh_app_settings().set_arrangement(bssh_app_settings::ARRANGEMENT_TILED);

	for (const bssh_app_settings::tab_position_t side : { bssh_app_settings::TABS_TOP,
	                                                       bssh_app_settings::TABS_LEFT }) {
		bssh_app_settings().set_tab_position(side);
		bssh_main_window window;
		window.resize(1200, 800);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		const registry_left_as_found_t tidy(window.registry());
		bssh_session *first = window.registry()->create(named(QStringLiteral("a")));
		window.adopt_session(first);
		window.adopt_session(window.registry()->create(named(QStringLiteral("b"))));
		QTest::qWait(50);

		/* The corner's buttons, all of them showing. */
		bssh_terminal_view *view = window.panes()->view_for(window.layout_manager()->front_of(first));
		QVERIFY(view && view->corner());
		view->corner()->set_multiplexer_text(QStringLiteral("tmux 2"));
		view->corner()->set_tabs_text(QStringLiteral("tabs 2"));
		view->corner()->set_menu_shown(true);
		QCoreApplication::processEvents();

		/* And the two panels a pane opens: the tmux drawer and find. */
		bssh_mux_session_t one;
		one.kind = bssh_mux_session_t::TMUX;
		one.name = QStringLiteral("main");
		bssh_mux_session_t two = one;
		two.name = QStringLiteral("build");
		view->offer_multiplexers({ one, two });
		bssh_multiplexer_drawer *drawer = view->findChild<bssh_multiplexer_drawer *>();
		QVERIFY(drawer);
		drawer->set_open(true);
		/* Find by its chord: a touch window has no menu bar to find it in. */
		view->setFocus();
		QTest::keyClick(view, Qt::Key_F, Qt::ControlModifier | Qt::ShiftModifier);
		QCoreApplication::processEvents();
		bssh_find_bar *find = window.findChild<bssh_find_bar *>();
		QVERIFY(find && find->isVisible() && drawer->isVisible());

		QList<QWidget *> surfaces;
		for (bssh_pane_bar *bar : window.pane_bars())
			if (bar && bar->isVisible()) surfaces << bar;
		surfaces << view->corner() << drawer << find;
		QVERIFY2(surfaces.size() >= 2, "no bar showing, so measuring it proves nothing");

		QStringList short_ones;
		for (QWidget *surface : surfaces) {
			for (QAbstractItemView *list : surface->findChildren<QAbstractItemView *>()) {
				if (!list->isVisibleTo(surface) || !list->model() || list->model()->rowCount() == 0)
					continue;
				if (list->sizeHintForRow(0) >= BSSH_TOUCH_TARGET_DP) continue;
				short_ones << QStringLiteral("%1 %2 rows %3dp").arg(
				        QString::fromLatin1(surface->metaObject()->className()),
				        QString::fromLatin1(list->metaObject()->className()))
				                .arg(list->sizeHintForRow(0));
			}
			for (QAbstractButton *button : surface->findChildren<QAbstractButton *>()) {
				if (!button->isVisibleTo(surface)) continue;
				if (button->height() >= BSSH_TOUCH_TARGET_DP) continue;
				short_ones << QStringLiteral("%1 %2 '%3' %4dp").arg(
				        QString::fromLatin1(surface->metaObject()->className()),
				        QString::fromLatin1(button->metaObject()->className()),
				        button->objectName().isEmpty() ? button->text() : button->objectName())
				                .arg(button->height());
			}
		}
		/* The bar's handle is grabbed before it is dragged. */
		for (bssh_pane_bar *bar : window.pane_bars()) {
			if (!bar || !bar->isVisible()) continue;
			QWidget *handle = bar->findChild<QWidget *>(QStringLiteral("pane_bar_handle"));
			if (handle && (handle->width() < BSSH_TOUCH_TARGET_DP || handle->height() < BSSH_TOUCH_TARGET_DP))
				short_ones << QStringLiteral("bar handle %1x%2dp").arg(handle->width()).arg(handle->height());
		}
		QVERIFY2(short_ones.isEmpty(),
		          qPrintable(QStringLiteral("side %1: ").arg(int(side)) + short_ones.join(QStringLiteral("; "))));
	}
}

void layout_test::a_dragged_divider_stays_where_it_was_put() {
	/*
	 * The holder: "can we stop the dividers from resizing and jumbling
	 * everything so much after a change in layout? It seems to have a
	 * preference or a springiness back to certain shape." Every layout
	 * change rebuilds the splitters, and a new splitter starts at its
	 * default split -- so a divider dragged to a third went back to the
	 * middle when a tab was opened, picked or moved (sec 8.3).
	 */
	const forced_touch_t desktop(0);
	struct restore_t {
		bssh_app_settings::tab_position_t tabs = bssh_app_settings().tab_position();
		~restore_t() { bssh_app_settings().set_tab_position(tabs); }
	} restore;
	bssh_app_settings().set_tab_position(bssh_app_settings::TABS_TOP);

	bssh_main_window window;
	window.resize(1500, 800);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	const registry_left_as_found_t tidy(window.registry());
	bssh_layout_manager *layout = window.layout_manager();

	bssh_session *left = window.registry()->create(named(QStringLiteral("left")));
	bssh_session *right = window.registry()->create(named(QStringLiteral("right")));
	window.adopt_session(left);
	window.adopt_session(right);
	QTest::qWait(30);
	window.drop_session_on(right, layout->front_of(right), bssh_layout_manager::EDGE_RIGHT);
	QTest::qWait(50);
	QCOMPARE(layout->visible_panes().size(), 2);

	/* The split put at a quarter, as a drag of the divider would. */
	const auto root = [&window]() {
		return window.panes()->findChild<QSplitter *>(QString(), Qt::FindDirectChildrenOnly);
	};
	const auto share = [&window, layout](bssh_session *pane) {
		bssh_terminal_view *view = window.panes()->view_for(layout->front_of(pane));
		return view ? double(view->parentWidget()->width()) / window.panes()->width() : -1.0;
	};
	QSplitter *splitter = root();
	QVERIFY(splitter && splitter->count() == 2);
	const int total = splitter->sizes().at(0) + splitter->sizes().at(1);
	splitter->setSizes({ total / 4, total - total / 4 });
	QTest::qWait(50);
	QVERIFY2(qAbs(share(left) - 0.25) < 0.05, qPrintable(QString::number(share(left))));

	const auto still_a_quarter = [&](const char *after) {
		const double now = share(left);
		QVERIFY2(qAbs(now - 0.25) < 0.05,
		          qPrintable(QStringLiteral("after %1 the left pane is %2 of the width, not a quarter")
		                             .arg(QString::fromLatin1(after)).arg(now)));
	};

	/* A tab opened in the right pane: the same shape, rebuilt. */
	bssh_session *third = window.registry()->create(named(QStringLiteral("third")));
	window.panes()->focus_session(layout->front_of(right));
	window.adopt_session(third);
	QTest::qWait(50);
	still_a_quarter("a tab opened");

	/* The bars moved to the bottom: rebuilt again. */
	bssh_app_settings().set_tab_position(bssh_app_settings::TABS_BOTTOM);
	window.apply_device_settings();
	QTest::qWait(50);
	still_a_quarter("the bars moved");

	/* A new pane split off the right one: the left keeps its width. */
	window.drop_session_on(third, layout->front_of(third), bssh_layout_manager::EDGE_BOTTOM);
	QTest::qWait(50);
	QCOMPARE(layout->visible_panes().size(), 3);
	still_a_quarter("the right pane was split");
}

namespace {

/*
 * Each visible note on `surface` measured against the controls around it:
 * the gap to the nearest one above, and to the nearest one below, both
 * over the same columns. A note equally far from both reads as belonging
 * to either -- the holder's complaint -- so the one above must be clearly
 * nearer. Returns what fails, empty when nothing does.
 */
QStringList bssh_notes_far_from_their_controls(QWidget *surface, const QString &name, int *measured) {
	QStringList far;
	const auto rect_of = [surface](QWidget *widget) {
		return QRect(widget->mapTo(surface, QPoint(0, 0)), widget->size());
	};
	const auto is_control = [](QWidget *widget) {
		return qobject_cast<QAbstractButton *>(widget) || qobject_cast<QComboBox *>(widget)
		       || qobject_cast<QAbstractSlider *>(widget) || qobject_cast<QLineEdit *>(widget)
		       || qobject_cast<QAbstractSpinBox *>(widget);
	};
	for (QLabel *note : surface->findChildren<QLabel *>()) {
		if (!note->property("bssh_note").toBool() || !note->isVisibleTo(surface)) continue;
		if (note->text().isEmpty()) continue;
		const QRect at = rect_of(note);
		int above = INT_MAX;
		int below = INT_MAX;
		for (QWidget *other : surface->findChildren<QWidget *>()) {
			if (!is_control(other) || !other->isVisibleTo(surface)) continue;
			if (qobject_cast<QAbstractButton *>(other->parentWidget())) continue;
			const QRect it = rect_of(other);
			if (it.right() < at.left() || it.left() > at.right()) continue;
			if (it.bottom() <= at.top()) above = qMin(above, at.top() - it.bottom());
			else if (it.top() >= at.bottom()) below = qMin(below, it.top() - at.bottom());
		}
		/* A note with no control above is an introduction; with none below
		 * it is the last thing, and nothing can be confused with it. */
		if (above == INT_MAX || below == INT_MAX) continue;
		++*measured;
		if (above * 2 > below)
			far << QStringLiteral("%1: \"%2...\" is %3px under its control and %4px over the next")
			               .arg(name, note->text().left(40)).arg(above).arg(below);
	}
	return far;
}

} /* namespace */

void layout_test::a_note_sits_with_the_control_it_explains() {
	/*
	 * The holder, 2026-10-31: "in the config alignment of widget and its
	 * help text makes it often unclear what it pertains to as it is
	 * equidistant from the top and the bottom widget, this should be taken
	 * as general advice for widgets and their accompanying help text."
	 */
	QStringList far;
	int measured = 0;
	for (const int touch : { 0, 1 }) {
		const forced_touch_t forced(touch);
		for (const bssh_app_settings_view::section_t section :
		     { bssh_app_settings_view::SECTION_KEYBOARD, bssh_app_settings_view::SECTION_APPEARANCE,
		       bssh_app_settings_view::SECTION_TERMINAL, bssh_app_settings_view::SECTION_PASSWORDS }) {
			bssh_app_settings_view page(section, nullptr);
			page.resize(touch ? 360 : 700, 2400);
			page.show();
			QVERIFY(QTest::qWaitForWindowExposed(&page));
			QTest::qWait(20);
			QScrollArea *area = page.findChild<QScrollArea *>();
			QWidget *content = area && area->widget() ? area->widget() : &page;
			far += bssh_notes_far_from_their_controls(
			        content, QStringLiteral("%1 page %2").arg(touch ? "touch" : "desktop").arg(int(section)),
			        &measured);
		}
	}
	/* A check over no notes passes too, and says nothing. */
	QVERIFY2(measured >= 12, qPrintable(QStringLiteral("only %1 notes measured").arg(measured)));
	QVERIFY2(far.isEmpty(), qPrintable(far.join(QLatin1Char('\n'))));
}

void layout_test::a_shape_survives_being_written_down() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));
	bssh_session *c = registry.create(named(QStringLiteral("c")));
	const QHash<bssh_session *, QString> names{
		{a, QStringLiteral("a")}, {b, QStringLiteral("b")}, {c, QStringLiteral("c")}};

	layout.set_intent({a, b});
	QVERIFY(layout.place(b, a, bssh_layout_manager::EDGE_BOTTOM));
	QVERIFY(layout.place(c, b, bssh_layout_manager::EDGE_RIGHT));

	const auto name = [&names](bssh_session *session) { return names.value(session); };
	const auto find = [&names](const QString &text) { return names.key(text, nullptr); };

	const QString text = bssh_layout_manager::shape_text(layout.tree(), name);
	QCOMPARE(text, QStringLiteral("V(a,H(b,c))"));
	QCOMPARE(shape_of(bssh_layout_manager::parse_shape(text, find), names), text);

	/* A host that did not come back is dropped, and its split closes up. */
	QCOMPARE(shape_of(bssh_layout_manager::parse_shape(QStringLiteral("V(a,H(b,gone))"), find),
	                  names),
	         QStringLiteral("V(a,b)"));

	/* A pane with no name to write is left out the same way. */
	const auto without_c = [&names](bssh_session *session) {
		return session == nullptr || names.value(session) == QStringLiteral("c")
		               ? QString() : names.value(session);
	};
	QCOMPARE(bssh_layout_manager::shape_text(layout.tree(), without_c), QStringLiteral("V(a,b)"));

	/* Text that does not parse is nothing saved, not half a shape. */
	QVERIFY(bssh_layout_manager::parse_shape(QStringLiteral("V(a,H(b"), find).is_empty());
	QVERIFY(bssh_layout_manager::parse_shape(QStringLiteral("V(a,b))"), find).is_empty());
	QVERIFY(bssh_layout_manager::parse_shape(QString(), find).is_empty());
}

namespace {

/* A layout's shape with every pane's tabs, "H(b|*a,c)", for one-line compares. */
QString stacked_shape(const bssh_layout_manager &layout,
                      const QHash<bssh_session *, QString> &names) {
	return bssh_layout_manager::shape_text(layout.tree(), [&names](bssh_session *each) {
		return names.value(each);
	});
}

} /* namespace */

void layout_test::a_pane_holds_tabs_and_closes_with_its_last() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));
	bssh_session *c = registry.create(named(QStringLiteral("c")));
	const QHash<bssh_session *, QString> names{
		{a, QStringLiteral("a")}, {b, QStringLiteral("b")}, {c, QStringLiteral("c")}};

	layout.set_intent({a, b});
	QVERIFY(layout.add_tab(c, a, 1));

	/* In front of a's pane, behind it in the bar; the intent is the fronts. */
	QCOMPARE(layout.tabs(a), QList<bssh_session *>({a, c}));
	QCOMPARE(layout.intent(), QList<bssh_session *>({c, b}));
	QCOMPARE(stacked_shape(layout, names), QStringLiteral("H(a|*c,b)"));
	QCOMPARE(layout.sessions(), QList<bssh_session *>({a, c, b}));

	/* Picking a tab changes the front and nothing else. */
	QVERIFY(layout.bring_to_front(a));
	QCOMPARE(layout.intent(), QList<bssh_session *>({a, b}));
	QCOMPARE(layout.tabs(c), QList<bssh_session *>({a, c}));

	/* The front closing hands the pane to the next tab... */
	layout.forget(a);
	QCOMPARE(layout.intent(), QList<bssh_session *>({c, b}));
	QCOMPARE(layout.tabs(c), QList<bssh_session *>({c}));

	/* ...and the last one going closes the pane. */
	layout.forget(c);
	QCOMPARE(layout.intent(), QList<bssh_session *>({b}));
	QCOMPARE(stacked_shape(layout, names), QStringLiteral("b"));
	QVERIFY(!layout.add_tab(c, a, 0));
}

void layout_test::a_tab_moves_between_panes_and_along_a_bar() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));
	bssh_session *c = registry.create(named(QStringLiteral("c")));
	const QHash<bssh_session *, QString> names{
		{a, QStringLiteral("a")}, {b, QStringLiteral("b")}, {c, QStringLiteral("c")}};

	layout.set_intent({a, b, c});

	/* b's pane had only b: moving it into a's closes it. */
	QVERIFY(layout.add_tab(b, a, 0));
	QCOMPARE(stacked_shape(layout, names), QStringLiteral("H(*b|a,c)"));

	/* Along its own bar: past a, so the slot counts the bar as it is. */
	QVERIFY(layout.add_tab(b, a, 2));
	QCOMPARE(layout.tabs(a), QList<bssh_session *>({a, b}));

	/* A tab out of a bar of two leaves a pane of one behind. */
	QVERIFY(layout.add_tab(b, c, 1));
	QCOMPARE(stacked_shape(layout, names), QStringLiteral("H(a,c|*b)"));

	/* An edge gives a tab a pane of its own, out of its bar. */
	QVERIFY(layout.place(b, a, bssh_layout_manager::EDGE_LEFT));
	QCOMPARE(stacked_shape(layout, names), QStringLiteral("H(b,a,c)"));

	/* A list reorder moves panes with their tabs. */
	QVERIFY(layout.add_tab(b, c, 0));
	QCOMPARE(stacked_shape(layout, names), QStringLiteral("H(a,*b|c)"));
	layout.set_intent({b, a});
	QCOMPARE(stacked_shape(layout, names), QStringLiteral("H(*b|c,a)"));

	/* A pane split by an edge drop keeps its tabs: the split is around it. */
	QVERIFY(layout.place(a, b, bssh_layout_manager::EDGE_BOTTOM));
	QCOMPARE(stacked_shape(layout, names), QStringLiteral("V(*b|c,a)"));
}

void layout_test::the_middle_swaps_panes_whole_and_trades_a_tab() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));
	bssh_session *c = registry.create(named(QStringLiteral("c")));
	bssh_session *d = registry.create(named(QStringLiteral("d")));
	const QHash<bssh_session *, QString> names{
		{a, QStringLiteral("a")}, {b, QStringLiteral("b")},
		{c, QStringLiteral("c")}, {d, QStringLiteral("d")}};

	layout.set_intent({a, b});
	QVERIFY(layout.add_tab(c, a, 1));
	QVERIFY(layout.bring_to_front(a));
	QCOMPARE(stacked_shape(layout, names), QStringLiteral("H(*a|c,b)"));

	/* Pane on pane: they swap whole, a's tab going with it. */
	QVERIFY(layout.place(a, b, bssh_layout_manager::EDGE_MIDDLE));
	QCOMPARE(stacked_shape(layout, names), QStringLiteral("H(b,*a|c)"));

	/* A tab on another pane: it fronts that pane, the front taking its slot. */
	QVERIFY(layout.place(c, b, bssh_layout_manager::EDGE_MIDDLE));
	QCOMPARE(stacked_shape(layout, names), QStringLiteral("H(c,*a|b)"));

	/* A tab on its own pane comes to the front. */
	QVERIFY(layout.place(b, a, bssh_layout_manager::EDGE_MIDDLE));
	QCOMPARE(stacked_shape(layout, names), QStringLiteral("H(c,a|*b)"));

	/* An arrival takes the target's place; the target leaves for the caller. */
	QVERIFY(layout.place(d, b, bssh_layout_manager::EDGE_MIDDLE));
	QCOMPARE(stacked_shape(layout, names), QStringLiteral("H(c,a|*d)"));

	/* And a target that is not a pane's front is not one to drop on. */
	QVERIFY(!layout.place(c, a, bssh_layout_manager::EDGE_MIDDLE));
}

void layout_test::a_collapsed_pane_lives_on_in_the_last_bar() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));
	bssh_session *c = registry.create(named(QStringLiteral("c")));
	bssh_session *d = registry.create(named(QStringLiteral("d")));

	layout.set_intent({a, b});
	QVERIFY(layout.add_tab(c, b, 1));
	QVERIFY(layout.add_tab(d, a, 1));
	QCOMPARE(layout.bar_tabs(d), QList<bssh_session *>({a, d}));

	/* Too narrow for two: the second pane's tabs join the first's bar. */
	layout.set_width(400);
	QCOMPARE(layout.visible_panes(), QList<bssh_session *>({d}));
	QCOMPARE(layout.bar_tabs(d), QList<bssh_session *>({a, d, b, c}));

	/* Picked from there, it is shown -- the panes trade places, not tabs. */
	QVERIFY(layout.bring_to_front(b));
	QCOMPARE(layout.visible_panes(), QList<bssh_session *>({b}));
	QCOMPARE(layout.tabs(b), QList<bssh_session *>({b, c}));
	QCOMPARE(layout.tabs(d), QList<bssh_session *>({a, d}));

	/* Wide again: both panes, every tab where it was. */
	layout.set_width(1200);
	QCOMPARE(layout.visible_panes().size(), 2);
	QCOMPARE(layout.sessions().size(), 4);
}

void layout_test::a_pane_of_tabs_survives_being_written_down() {
	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(1200);

	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));
	bssh_session *c = registry.create(named(QStringLiteral("c")));
	const QHash<bssh_session *, QString> names{
		{a, QStringLiteral("a")}, {b, QStringLiteral("b")}, {c, QStringLiteral("c")}};
	const auto name = [&names](bssh_session *session) { return names.value(session); };
	const auto find = [&names](const QString &text) { return names.key(text, nullptr); };

	layout.set_intent({a, c});
	QVERIFY(layout.add_tab(b, a, 0));
	QVERIFY(layout.bring_to_front(a));

	const QString text = bssh_layout_manager::shape_text(layout.tree(), name);
	QCOMPARE(text, QStringLiteral("H(b|*a,c)"));

	bssh_layout_manager restored;
	restored.set_width(1200);
	restored.set_tree(bssh_layout_manager::parse_shape(text, find));
	QCOMPARE(bssh_layout_manager::shape_text(restored.tree(), name), text);
	QCOMPARE(restored.intent(), QList<bssh_session *>({a, c}));

	/* A tab whose host is gone is dropped, the rest of its pane kept. */
	QCOMPARE(bssh_layout_manager::shape_text(
	                 bssh_layout_manager::parse_shape(QStringLiteral("H(gone|*a|b,c)"), find), name),
	         QStringLiteral("H(*a|b,c)"));

	/* The front gone: the first tab left takes its place. */
	QCOMPARE(bssh_layout_manager::shape_text(
	                 bssh_layout_manager::parse_shape(QStringLiteral("H(b|*gone,c)"), find), name),
	         QStringLiteral("H(b,c)"));
}

void layout_test::each_pane_keeps_its_own_tabs() {
	const forced_touch_t pointer(0);
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));
	const registry_left_as_found_t tidy(shared);

	bssh_main_window window;
	window.resize(1400, 700);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	bssh_layout_manager *layout = window.layout_manager();

	bssh_session *left = shared->create(named(QStringLiteral("left")));
	bssh_session *right = shared->create(named(QStringLiteral("right")));
	window.adopt_session(left);
	window.drop_session_on(right, left, bssh_layout_manager::EDGE_RIGHT);
	QCoreApplication::processEvents();
	QCOMPARE(window.panes()->pane_count(), 2);

	/* A session opened lands in the FOCUSED pane, at its front. */
	window.panes()->focus_session(right);
	bssh_session *second = shared->create(named(QStringLiteral("second")));
	window.adopt_session(second);
	QCoreApplication::processEvents();
	QCOMPARE(layout->tabs(right), QList<bssh_session *>({ right, second }));
	QCOMPARE(layout->tabs(left), QList<bssh_session *>({ left }));

	/* Each pane's bar lists its own: the right one shows, the left one has none. */
	QVERIFY(window.bar_for(left)->isHidden());
	bssh_pane_bar *right_bar = window.bar_for(second);
	QVERIFY(right_bar && !right_bar->isHidden());
	QCOMPARE(right_bar->sessions(), QList<bssh_session *>({ right, second }));

	/* With the left pane focused, a session opened goes there instead. */
	window.panes()->focus_session(left);
	bssh_session *third = shared->create(named(QStringLiteral("third")));
	window.adopt_session(third);
	QCoreApplication::processEvents();
	QCOMPARE(layout->tabs(left), QList<bssh_session *>({ left, third }));
	QCOMPARE(layout->tabs(second), QList<bssh_session *>({ right, second }));

	/* Picking a tab in a bar brings it to the front of that pane only. */
	bssh_pane_bar *left_bar = window.bar_for(third);
	QVERIFY(left_bar);
	emit left_bar->tab_picked(left);
	QCoreApplication::processEvents();
	QCOMPARE(layout->intent(), QList<bssh_session *>({ left, second }));

	/* Show as Tab: the left pane's tabs go into the right pane's bar, behind. */
	window.show_as_tab(left);
	QCoreApplication::processEvents();
	QCOMPARE(window.panes()->pane_count(), 1);
	QCOMPARE(layout->intent(), QList<bssh_session *>({ second }));
	QCOMPARE(layout->tabs(second), QList<bssh_session *>({ right, second, left, third }));
	QCOMPARE(window.shown_sessions().size(), 4);

	/* The last pane has no neighbour to join, and stays a pane. */
	window.show_as_tab(second);
	QCoreApplication::processEvents();
	QCOMPARE(layout->intent(), QList<bssh_session *>({ second }));

	/*
	 * A pane's front dragged onto its own pane: an edge splits it off, the
	 * tab beside it taking the front; the middle does nothing -- and a
	 * pane of one has nothing to split from.
	 */
	QCOMPARE(window.drop_zone_for(second, second, bssh_layout_manager::EDGE_MIDDLE),
	         int(BSSH_DROP_CANCEL));
	QCOMPARE(window.drop_zone_for(second, second, bssh_layout_manager::EDGE_RIGHT),
	         int(bssh_layout_manager::EDGE_RIGHT));
	window.drop_session_on(second, second, bssh_layout_manager::EDGE_RIGHT);
	QCoreApplication::processEvents();
	QCOMPARE(layout->intent(), QList<bssh_session *>({ left, second }));
	QCOMPARE(layout->tabs(left), QList<bssh_session *>({ right, left, third }));
	QCOMPARE(window.drop_zone_for(second, second, bssh_layout_manager::EDGE_LEFT),
	         int(BSSH_DROP_CANCEL));
}

void layout_test::a_background_tab_can_be_dragged_out() {
	bssh_session_registry registry;
	bssh_session *a = registry.create(named(QStringLiteral("a")));
	bssh_session *b = registry.create(named(QStringLiteral("b")));
	bssh_session *c = registry.create(named(QStringLiteral("c")));

	QWidget host;
	QVBoxLayout *column = new QVBoxLayout(&host);
	bssh_pane_bar *row = new bssh_pane_bar(c, { a, b, c }, Qt::TopEdge, true, &host);
	column->addWidget(row);
	column->addStretch(1);
	host.resize(600, 300);
	host.show();
	QVERIFY(QTest::qWaitForWindowExposed(&host));
	QTabBar *bar = row->tab_bar();
	QCOMPARE(bar->currentIndex(), 2);

	QSignalSpy picked(row, &bssh_pane_bar::tab_picked);
	QSignalSpy dragged(row, &bssh_pane_bar::drag_requested);

	const auto send = [bar](QEvent::Type type, const QPoint &at, Qt::MouseButton button,
	                        Qt::MouseButtons held) {
		QMouseEvent event(type, at, bar->mapToGlobal(at), button, held, Qt::NoModifier);
		QApplication::sendEvent(bar, &event);
	};

	/* Pressed: selected in the bar, but not yet picked. */
	const QPoint on_a = bar->tabRect(0).center();
	send(QEvent::MouseButtonPress, on_a, Qt::LeftButton, Qt::LeftButton);
	QCOMPARE(picked.count(), 0);

	/* Dragged well clear: the tab under the press is carried, and never picked. */
	for (int step = 1; step <= 4; ++step)
		send(QEvent::MouseMove, on_a + QPoint(0, step * 40), Qt::NoButton, Qt::LeftButton);
	QCOMPARE(dragged.count(), 1);
	QCOMPARE(dragged.at(0).at(0).value<bssh_session *>(), a);
	QCOMPARE(picked.count(), 0);

	/* A click on another: picked when the button comes up. */
	const QPoint on_b = bar->tabRect(1).center();
	send(QEvent::MouseButtonPress, on_b, Qt::LeftButton, Qt::LeftButton);
	QCOMPARE(picked.count(), 0);
	send(QEvent::MouseButtonRelease, on_b, Qt::LeftButton, Qt::NoButton);
	QCOMPARE(picked.count(), 1);
	QCOMPARE(picked.at(0).at(0).value<bssh_session *>(), b);

	/*
	 * Sideways first, past a neighbour, then out: the bar's own reorder
	 * is held while the button is down. Applied at once it rebuilt the
	 * panes, and this bar, under the pointer -- which is why a tab moved
	 * along its row on X11 could never go on to tear out. Measured on
	 * Xvfb before this was written.
	 */
	QSignalSpy moved(row, &bssh_pane_bar::tab_moved);
	const QPoint on_c = bar->tabRect(2).center();
	const int stride = bar->tabRect(1).width();
	send(QEvent::MouseButtonPress, on_c, Qt::LeftButton, Qt::LeftButton);
	for (int step = 1; step <= 6; ++step)
		send(QEvent::MouseMove, on_c - QPoint(step * stride / 4, 0), Qt::NoButton, Qt::LeftButton);
	QCOMPARE(moved.count(), 0);
	for (int step = 1; step <= 4; ++step)
		send(QEvent::MouseMove, on_c - QPoint(6 * stride / 4, -step * 40), Qt::NoButton, Qt::LeftButton);
	QCOMPARE(dragged.count(), 2);
	QCOMPARE(dragged.at(1).at(0).value<bssh_session *>(), c);
	QCOMPARE(moved.count(), 0);

	/* And a reorder that stays on the row lands when the button comes up. */
	const QPoint on_first = bar->tabRect(0).center();
	send(QEvent::MouseButtonPress, on_first, Qt::LeftButton, Qt::LeftButton);
	for (int step = 1; step <= 6; ++step)
		send(QEvent::MouseMove, on_first + QPoint(step * stride / 4, 0), Qt::NoButton, Qt::LeftButton);
	QCOMPARE(moved.count(), 0);
	send(QEvent::MouseButtonRelease, on_first + QPoint(6 * stride / 4, 0), Qt::LeftButton, Qt::NoButton);
	QCOMPARE(moved.count(), 1);
}

void layout_test::a_restored_window_comes_back_in_its_shape() {
	bssh_profile_store *store = bssh_profile_store::shared();
	QVERIFY(store && store->is_open());

	bssh_profile_t top = named(QStringLiteral("shape-top"));
	top.host = QStringLiteral("top.example");
	bssh_profile_t left = named(QStringLiteral("shape-left"));
	left.host = QStringLiteral("left.example");
	bssh_profile_t right = named(QStringLiteral("shape-right"));
	right.host = QStringLiteral("right.example");
	const int top_id = store->save(top);
	const int left_id = store->save(left);
	const int right_id = store->save(right);
	QVERIFY(top_id && left_id && right_id);

	const bssh_profile_store::layout_state_t was = store->layout();

	bssh_profile_store::layout_state_t saved;
	saved.profile_ids = {top_id, left_id, right_id};
	saved.pane_count = 3;
	saved.shape = QStringLiteral("V(%1,H(%2,%3))").arg(top_id).arg(left_id).arg(right_id);
	QVERIFY(store->save_layout(saved));

	{
		bssh_main_window window;
		window.resize(1200, 800);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		const registry_left_as_found_t tidy(window.registry());

		window.restore_layout();
		QCoreApplication::processEvents();

		bssh_pane_container *panes = window.panes();
		QCOMPARE(panes->views().size(), 3);

		/*
		 * One above, two beside each other below it: the root splitter
		 * runs vertically and holds a horizontal one.
		 */
		QSplitter *root = panes->findChild<QSplitter *>(QString(), Qt::FindDirectChildrenOnly);
		QVERIFY(root);
		QCOMPARE(root->orientation(), Qt::Vertical);
		QSplitter *nested = root->findChild<QSplitter *>(QString(), Qt::FindDirectChildrenOnly);
		QVERIFY2(nested && nested->orientation() == Qt::Horizontal,
		          "the two panes that were side by side came back stacked");
	}

	store->save_layout(was);
	store->remove(top_id);
	store->remove(left_id);
	store->remove(right_id);
}

void layout_test::a_closed_window_writes_down_its_shape() {
	bssh_profile_store *store = bssh_profile_store::shared();
	QVERIFY(store && store->is_open());

	bssh_profile_t upper = named(QStringLiteral("close-upper"));
	upper.host = QStringLiteral("upper.example");
	bssh_profile_t lower = named(QStringLiteral("close-lower"));
	lower.host = QStringLiteral("lower.example");
	upper.id = store->save(upper);
	lower.id = store->save(lower);
	QVERIFY(upper.id && lower.id);

	const bssh_profile_store::layout_state_t was = store->layout();

	{
		bssh_main_window window;
		window.resize(1200, 800);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		bssh_session_registry *registry = window.registry();
		const registry_left_as_found_t tidy(registry);

		/*
		 * Two unconnected profile sessions and nothing else: a split
		 * opens a local shell, which is live, and closing a window with a
		 * live session asks first.
		 */
		bssh_session *a = registry->create(upper);
		bssh_session *b = registry->create(lower);
		window.adopt_session(a);
		window.adopt_session(b);
		QCoreApplication::processEvents();

		/* a above b: a shape the old pane count could not have saved. */
		window.drop_session_on(a, b, bssh_layout_manager::EDGE_TOP);
		QCoreApplication::processEvents();
		QCOMPARE(window.panes()->views().size(), 2);

		/* The first close leaves the terminals for the host list; the second closes. */
		window.close();
		window.close();
		QCoreApplication::processEvents();
	}

	const QString shape = store->layout().shape;
	QVERIFY2(shape.startsWith(QStringLiteral("V(%1,%2").arg(upper.id).arg(lower.id)),
	          qPrintable(QStringLiteral("saved shape: [%1]").arg(shape)));

	store->save_layout(was);
	store->remove(upper.id);
	store->remove(lower.id);
}

void layout_test::the_picture_history_setting_reaches_every_terminal() {
	const int was = bssh_app_settings().image_history_mb();
	bssh_app_settings().set_image_history_mb(8);

	{
		bssh_main_window window;
		window.resize(1000, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		bssh_session_registry *registry = window.registry();
		const registry_left_as_found_t tidy(registry);

		const qint64 mb = 1024 * 1024;
		bssh_session *open = registry->create(named(QStringLiteral("open")));
		window.adopt_session(open);
		QCOMPARE(open->emulator()->images().memory_budget(), 8 * mb);

		/*
		 * Through the page's own control, then applied to a terminal that
		 * is already open -- the setting is per terminal, so it has to
		 * reach the ones that exist, not only the next.
		 */
		bssh_app_settings_view *page = window.findChild<bssh_app_settings_view *>();
		QVERIFY(page);
		QSpinBox *control = page->findChild<QSpinBox *>(QStringLiteral("image_history"));
		QVERIFY2(control && !control->isHidden(), "the settings page offers no picture history");
		control->setValue(64);
		QCOMPARE(bssh_app_settings().image_history_mb(), 64);

		window.apply_device_settings();
		QCOMPARE(open->emulator()->images().memory_budget(), 64 * mb);

		/*
		 * A terminal no window has shown -- a background tab's case --
		 * has the setting too: pictures land in it whether or not anything
		 * draws it, and a window re-applies settings only when it builds
		 * a view.
		 */
		bssh_session *later = registry->create(named(QStringLiteral("later")));
		QCOMPARE(later->emulator()->images().memory_budget(), 64 * mb);
	}

	bssh_app_settings().set_image_history_mb(was);
}

void layout_test::a_host_remembers_its_zoom() {
	bssh_profile_store *store = bssh_profile_store::shared();
	QVERIFY(store && store->is_open());

	bssh_profile_t host = named(QStringLiteral("zoomed"));
	host.host = QStringLiteral("zoomed.example");
	host.font_zoom = 3;
	host.id = store->save(host);
	QVERIFY(host.id);

	/* Written down and read back, as a restart would. */
	QCOMPARE(store->profile(host.id).font_zoom, 3);

	{
		bssh_main_window window;
		window.resize(1000, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		bssh_session_registry *registry = window.registry();
		const registry_left_as_found_t tidy(registry);

		/* A new session of the host opens where it was left. */
		bssh_session *session = registry->create(store->profile(host.id));
		window.adopt_session(session);
		QCOMPARE(session->font_zoom(), 3);

		/*
		 * Zoomed again, and the host remembers -- after the pause, not on
		 * every step of the gesture.
		 */
		session->set_font_zoom(-2);
		QCOMPARE(store->profile(host.id).font_zoom, 3);
		QTRY_COMPARE_WITH_TIMEOUT(store->profile(host.id).font_zoom, -2, 3000);
	}

	store->remove(host.id);
}

void layout_test::a_connected_keep_awake_host_holds_off_sleep() {
	const bssh_factory_reset_t factory_reset;
	bssh_fake_ssh_session *fake = nullptr;
	bssh_ssh_session::set_factory([&fake](QObject *parent) -> bssh_ssh_session * {
		fake = new bssh_fake_ssh_session(parent);
		return fake;
	});

	bssh_platform *platform = bssh_platform::instance();
	QVERIFY(!platform->sleep_inhibited());

	{
		bssh_main_window window;
		window.resize(1000, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		bssh_session_registry *registry = window.registry();
		const registry_left_as_found_t tidy(registry);

		bssh_profile_t host = named(QStringLiteral("builder"));
		host.host = QStringLiteral("builder.example");
		host.keep_awake = true;
		bssh_session *session = registry->create(host);
		window.adopt_session(session);

		/* Asked for and never connected: nothing is running to lose. */
		QCoreApplication::processEvents();
		QVERIFY(!platform->sleep_inhibited());

		/*
		 * Connecting counts, as reconnecting will: a build waiting on the
		 * link to come back after a drop is still a build a sleep would
		 * lose.
		 */
		session->connect_to_host();
		QVERIFY(fake);
		fake->simulate_key_auth_success();
		QCoreApplication::processEvents();
		QVERIFY2(platform->sleep_inhibited(),
		          "a connected keep-awake host did not hold off sleep");

		registry->close(session);
		QCoreApplication::processEvents();
		QVERIFY2(!platform->sleep_inhibited(), "sleep stayed held after the host closed");
	}

	platform->set_sleep_inhibited(false);
	bssh_ssh_session::set_factory(nullptr);
}

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
	bssh_session *back = panes->views().size() >= 2 ? second : first;
	window.drop_session_on(back, back, bssh_layout_manager::EDGE_MIDDLE);

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

	QVERIFY(sk_set_keyboard_layout(QStringLiteral("us-intl")));
	QVERIFY(!sk_set_keyboard_layout(QStringLiteral("klingon")));
	QCOMPARE(sk_keyboard_layout(), QStringLiteral("us-intl"));

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

	QVERIFY(sk_set_keyboard_layout(QStringLiteral("us-intl")));
	QCOMPARE(slot_character(U'['), QChar(U'['));

	QVERIFY(sk_set_keyboard_layout(QStringLiteral("se")));
	QCOMPARE(slot_character(0x00e5), QChar(0x00e5));   /* a-ring, AD11 */
	QCOMPARE(slot_character(0x00f6), QChar(0x00f6));   /* o-diaeresis, AC10 */
	QCOMPARE(slot_character(0x00e4), QChar(0x00e4));   /* a-diaeresis, AC11 */

	/*
	 * And the geometry did not move. Same groups, same rows, same
	 * columns -- which is the whole claim the slot split makes.
	 */
	const QList<sk_group_t> &swedish =
	        sk_catalog::instance().all(
	                sk_catalog::STYLE_FULL);
	QVERIFY(sk_set_keyboard_layout(QStringLiteral("us-intl")));
	const QList<sk_group_t> &american =
	        sk_catalog::instance().all(
	                sk_catalog::STYLE_FULL);
	QCOMPARE(swedish.size(), american.size());
}

namespace {

void check_every_printable_character(const QString &layout_id) {
	for (sk_catalog::style_t style :
	     { sk_catalog::STYLE_COMPACT,
	       sk_catalog::STYLE_FULL }) {
		QSet<uint> reachable;
		for (const sk_group_t &group :
		     sk_catalog::instance().all(style)) {
			for (const QList<sk_key_t> &row : group.rows) {
				for (const sk_key_t &key : row) {
					if (key.kind != sk_key_t::TEXT) continue;

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
	for (sk_catalog::style_t style :
	     { sk_catalog::STYLE_COMPACT,
	       sk_catalog::STYLE_FULL }) {
		const QList<sk_group_t> &groups =
		        sk_catalog::instance().all(style);

		quint8 offered = 0;
		quint8 advertised = 0;
		for (const sk_group_t &group : groups) {
			for (const QList<sk_key_t> &row : group.rows) {
				for (const sk_key_t &key : row) {
					if (key.kind == sk_key_t::MODIFIER) offered |= key.modifier;
					if (key.kind != sk_key_t::TEXT) continue;

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

	for (const sk_group_t &group :
	     sk_catalog::instance().all(
	             sk_catalog::STYLE_FULL)) {
		if (!group.id.contains(QStringLiteral("keypad"))) continue;

		for (const QList<sk_key_t> &row : group.rows) {
			for (const sk_key_t &key : row) {
				if (key.kind != sk_key_t::TEXT) continue;

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
	sk_catalog &catalog = sk_catalog::instance();

	for (sk_catalog::style_t style :
	     { sk_catalog::STYLE_COMPACT,
	       sk_catalog::STYLE_FULL }) {
		for (const sk_group_t &group : catalog.all(style)) {
			for (const QList<sk_key_t> &row : group.rows) {
				for (const sk_key_t &key : row) {
					if (key.kind != sk_key_t::TEXT) continue;
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

	for (sk_catalog::style_t style :
	     { sk_catalog::STYLE_COMPACT,
	       sk_catalog::STYLE_FULL }) {
		for (const sk_group_t &group :
		     sk_catalog::instance().all(style)) {
			for (const QList<sk_key_t> &row : group.rows) {
				for (const sk_key_t &key : row) {
					if (key.kind != sk_key_t::TEXT) continue;
					plain_only.insert(sk_keyboard_char(key, false, false).unicode());
					with_altgr.insert(sk_keyboard_char(key, false, true).unicode());
					with_shift_altgr.insert(sk_keyboard_char(key, true, true).unicode());
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
	const QList<sk_group_t> &full =
	        sk_catalog::instance().all(
	                sk_catalog::STYLE_FULL);
	for (const sk_group_t &group : full) {
		for (const QList<sk_key_t> &row : group.rows) {
			for (const sk_key_t &key : row) {
				if (key.kind != sk_key_t::TEXT) continue;
				producible.insert(sk_shifted_char(char32_t(key.value),
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
	QCOMPARE(sk_shifted_char(U'a', 0, true), QChar('A'));
	QCOMPARE(sk_shifted_char(U'a', 0, false), QChar('a'));

	/*
	 * The keycap and the character come from one function, so a key
	 * cannot show one thing and send another. Asserted by asking the same
	 * question the widget asks for its label.
	 */
	QCOMPARE(sk_shifted_char(U'1', U'!', true), QChar('!'));
	QCOMPARE(sk_shifted_char(U'1', U'!', false), QChar('1'));
}

/*
 * The keyboard's own two keys, and the way back (sec 6.2).
 *
 * Driven through the window rather than the widget, because every part of
 * this that can be wrong is a connection: the keyboard does not hide
 * itself, the window remembers that the user asked, and the way back is a
 * gesture in a DIFFERENT widget that has to reach the window through the
 * pane container. A test of sk_keyboard alone would pass with none
 * of that wired.
 */
void layout_test::the_hide_key_is_remembered_and_a_tap_undoes_it() {
	const bssh_factory_reset_t factory_reset;
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

	sk_keyboard *keys = window.findChild<sk_keyboard *>(QStringLiteral("terminal-keyboard"));
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
	window.resize(SK_FULL_KEYBOARD_MIN_DP + 100, BSSH_PHONE_TALL_DP);
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
	const bssh_factory_reset_t factory_reset;
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
 * The built-in keyboard declines the system one in EVERY pane, not only the
 * focused one (sec 6.2).
 *
 * Found on the Fold, 2026-10-07: with the built-in keyboard chosen,
 * Samsung's keyboard came up under it on opening a second and a third
 * session, and stayed up over the terminal. The choice was applied only to
 * the focused view, and every view is built able to raise the system
 * keyboard -- so the panes beside it kept that, and focus passing through
 * one while the panes were rebuilt was enough.
 */
void layout_test::the_built_in_keyboard_declines_the_system_one_in_every_pane() {
	const forced_touch_t arrangement(1);

	bssh_app_settings settings;
	const bssh_app_settings::keyboard_t before = settings.keyboard();
	settings.set_keyboard(bssh_app_settings::KEYBOARD_BUILT_IN_FULL);

	bssh_main_window window;
	window.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	const registry_left_as_found_t tidy(registry);

	window.adopt_session(registry->create(named(QStringLiteral("first"))));
	window.adopt_session(registry->create(named(QStringLiteral("second"))));
	window.split_beside_current();
	QCoreApplication::processEvents();

	bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
	QVERIFY(panes);
	QVERIFY2(panes->views().size() >= 2, "two sessions must be on screen for this to ask anything");

	for (bssh_terminal_view *view : panes->views()) {
		QVERIFY2(!view->testAttribute(Qt::WA_InputMethodEnabled),
		         view == panes->focused_view() ? "the focused pane may raise the system keyboard"
		                                       : "a pane beside the focused one may raise the system keyboard");
	}

	settings.set_keyboard(before);
}

/*
 * The way back from the host list to the open sessions (sec 8.5, decided
 * 2026-10-07). Back at a terminal goes up to the list; without a way back
 * down, the Fold showed what that costs -- the only route to a running
 * session was to open another one.
 *
 * Since 2026-10-22 (the holder) the list NAMES them: one row per open
 * session above the hosts, and a tap goes back to that session. The single
 * "N sessions open" button it replaced named none, so a phone with two
 * went Back, the button, then the tab bar.
 */
void layout_test::the_host_list_leads_back_to_open_sessions() {
	const forced_touch_t arrangement(1);

	bssh_main_window window;
	window.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	bssh_session_registry *registry = window.registry();
	const registry_left_as_found_t tidy(registry);

	QListWidget *list = window.findChild<QListWidget *>(QStringLiteral("host_list"));
	QVERIFY(list);
	const auto row_named = [list](const QString &text) -> QListWidgetItem * {
		for (int i = 0; i < list->count(); ++i)
			if (list->item(i)->text().startsWith(text)) return list->item(i);
		return nullptr;
	};
	QVERIFY2(!row_named(QStringLiteral("Sessions")), "with nothing open there is nothing to list");

	bssh_session *first = registry->create(named(QStringLiteral("first")));
	bssh_session *second = registry->create(named(QStringLiteral("second")));
	window.adopt_session(first);
	window.adopt_session(second);
	QCoreApplication::processEvents();
	QVERIFY(!window.showing_front_door());

	/* Up, by the key Android sends. */
	QKeyEvent back(QEvent::KeyPress, Qt::Key_Back, Qt::NoModifier);
	QApplication::sendEvent(&window, &back);
	QVERIFY(window.showing_front_door());

	QVERIFY2(row_named(QStringLiteral("Sessions")), "the open sessions are not listed");
	QListWidgetItem *first_row = row_named(first->title());
	QVERIFY2(first_row, "an open session has no row of its own");
	QVERIFY2(list->row(first_row) < list->row(row_named(QStringLiteral("Local shell"))),
	          "the sessions must come before the hosts");

	/*
	 * One heading, not two: the page is titled Hosts, and a "Hosts" row
	 * under it was the same word twice (the holder, 2026-10-22).
	 */
	for (int i = 0; i < list->count(); ++i)
		QVERIFY2(list->item(i)->text() != QStringLiteral("Hosts"),
		          "a Hosts heading under a page titled Hosts");

	/* And nothing drawn as chosen that nobody chose. */
	QVERIFY2(list->selectedItems().isEmpty() && list->currentRow() == -1,
	          "a row comes back highlighted though nothing tapped it");

	/* Down again, to THAT session rather than a new one or the last one. */
	const int before = registry->count();
	list->setCurrentItem(first_row);
	emit list->itemClicked(first_row);
	QCoreApplication::processEvents();
	QVERIFY(!window.showing_front_door());
	QCOMPARE(registry->count(), before);
	QCOMPARE(window.layout_manager()->front_of(first), first);
	QVERIFY(window.layout_manager()->visible_panes().contains(first));

	/*
	 * And the terminal's own menu leads up there too, not only Back --
	 * by its one front-door entry, named for the page it opens here. A
	 * second entry (Session > Sessions) was the same door twice.
	 */
	QMenu *menu = window.build_terminal_menu(&window, QString());
	QVERIFY(menu);
	QAction *door = menu->findChild<QAction *>(QStringLiteral("terminal_front_door"));
	QVERIFY2(door, "the touch terminal menu has no way to the sessions");
	QCOMPARE(door->text(), QStringLiteral("&Sessions and Hosts"));
	QVERIFY2(!menu->findChild<QAction *>(QStringLiteral("terminal_sessions")),
	          "the sessions are offered twice in one menu");
	QVERIFY2(!menu->findChild<QAction *>(QStringLiteral("terminal_new_tab")),
	          "New Tab opens the same page on touch, a third door to it");
	delete menu;
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
	const bssh_factory_reset_t factory_reset;
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
	 * One level further, into a section, and back out of it. On the Fold
	 * the page remembered was a single slot: the section replaced "the
	 * session" with "the index", so the second Back went to the host
	 * list, and the only way to a terminal from there was a new session.
	 */
	QListWidget *sections = device->findChild<QListWidget *>();
	QVERIFY2(sections && sections->count() > 1, "the settings index must list its sections");
	emit sections->itemClicked(sections->item(1));
	QVERIFY2(pages->currentWidget() != device, "a section did not open from the index");

	QKeyEvent out_of_section(QEvent::KeyPress, Qt::Key_Back, Qt::NoModifier);
	QApplication::sendEvent(&window, &out_of_section);
	QVERIFY2(out_of_section.isAccepted(), "Back on a settings section was not handled");
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

	/*
	 * From the terminal Back goes up to the host list (decided by the
	 * holder 2026-10-07: the terminal is not the root on touch, sec 8.5),
	 * and only there, at the bottom of the stack, is it left alone.
	 */
	QKeyEvent at_terminal(QEvent::KeyPress, Qt::Key_Back, Qt::NoModifier);
	QApplication::sendEvent(&window, &at_terminal);
	QVERIFY2(at_terminal.isAccepted(), "Back at the terminal did not go up to the host list");
	QVERIFY(window.showing_front_door());

	QKeyEvent at_root(QEvent::KeyPress, Qt::Key_Back, Qt::NoModifier);
	QApplication::sendEvent(&window, &at_root);
	QVERIFY2(!at_root.isAccepted(),
	          "Back was swallowed with no page to leave, so nothing can background the app");
	QVERIFY(window.showing_front_door());

	settings.set_keyboard(before);
	bssh_ssh_session::set_factory(nullptr);
}

void layout_test::a_short_wide_screen_gets_the_split_keyboard() {
	typedef sk_catalog catalog;

	/*
	 * Short and wide: whichever keyboard was chosen, a keyboard under
	 * the terminal takes most of the height, so it splits (sec 6.2). The
	 * Fold's cover screen in landscape is about 860 by 320.
	 */
	QCOMPARE(sk_keyboard_style_for(catalog::STYLE_COMPACT, 860, 320), catalog::STYLE_SPLIT);
	QCOMPARE(sk_keyboard_style_for(catalog::STYLE_FULL, 860, 320), catalog::STYLE_SPLIT);

	/* At the boundary and either side of it. */
	QCOMPARE(sk_keyboard_style_for(catalog::STYLE_COMPACT, 1200,
	                                  SK_SPLIT_KEYBOARD_MAX_HEIGHT_DP - 1),
	          catalog::STYLE_SPLIT);
	QCOMPARE(sk_keyboard_style_for(catalog::STYLE_COMPACT, 1200,
	                                  SK_SPLIT_KEYBOARD_MAX_HEIGHT_DP),
	          catalog::STYLE_COMPACT);

	/* Wide is not enough: a tall tablet keeps its keyboard underneath. */
	QCOMPARE(sk_keyboard_style_for(catalog::STYLE_FULL, 1200, 900), catalog::STYLE_FULL);

	/* Portrait never splits by itself, however short. */
	QCOMPARE(sk_keyboard_style_for(catalog::STYLE_COMPACT, 320, 500), catalog::STYLE_COMPACT);

	/* And split chosen is split everywhere. */
	QCOMPARE(sk_keyboard_style_for(catalog::STYLE_SPLIT, 320, 2000), catalog::STYLE_SPLIT);
}

void layout_test::the_split_keyboard_holds_every_key_once() {
	typedef sk_catalog catalog;
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);
	sk_keyboard keyboard(&router);

	const auto labels = [](const QWidget *within) {
		QStringList out;
		for (const sk_key_cap *cap : within->findChildren<sk_key_cap *>()) {
			out.append(cap->text());
		}
		out.sort();
		return out;
	};

	/*
	 * Every group, compared with the compact keyboard it is cut from: the
	 * same keys, none lost in the cut -- plus exactly the two a split needs
	 * because each thumb reaches only its half (sec 6.2): a second space
	 * bar, and a second Shift wherever the page has one. A relationship
	 * rather than a list, so a key added to compact is checked here too.
	 */
	const int groups = catalog::instance().all(catalog::STYLE_COMPACT).size();
	QVERIFY(groups > 0);
	for (int g = 0; g < groups; ++g) {
		keyboard.set_style(catalog::STYLE_COMPACT);
		keyboard.set_group(g);
		const QStringList compact = labels(&keyboard);
		QVERIFY(!compact.isEmpty());

		keyboard.set_style(catalog::STYLE_SPLIT);
		keyboard.set_group(g);
		const QStringList left = labels(keyboard.half(0));
		const QStringList right = labels(keyboard.half(1));
		QStringList split = left + right;
		split.sort();

		/*
		 * Less the digit row, which the split leaves out: asked for in
		 * portrait, and five rows did not fit a landscape phone.
		 */
		QStringList expected = compact;
		const sk_group_t &page = catalog::instance().all(catalog::STYLE_COMPACT).at(g);
		if (page.digit_row_first) {
			for (const sk_key_t &key : page.rows.at(0)) expected.removeOne(key.label);
		}
		if (compact.contains(QStringLiteral("space"))) expected.append(QStringLiteral("space"));
		if (compact.contains(QStringLiteral("Shift"))) expected.append(QStringLiteral("Shift"));
		expected.sort();
		QCOMPARE(split, expected);

		/* And each half has its own space bar, and its own Shift where the page has one. */
		QVERIFY2(left.contains(QStringLiteral("space")) && right.contains(QStringLiteral("space")),
		          "each thumb reaches a space bar");
		if (compact.contains(QStringLiteral("Shift"))) {
			QVERIFY2(left.contains(QStringLiteral("Shift")) && right.contains(QStringLiteral("Shift")),
			          "each thumb reaches a Shift");
		}
	}

	/* Cut where a keyboard is cut: q on the left, p on the right. */
	keyboard.set_style(catalog::STYLE_SPLIT);
	keyboard.set_group(0);
	QVERIFY(labels(keyboard.half(0)).contains(QStringLiteral("q")));
	QVERIFY(labels(keyboard.half(1)).contains(QStringLiteral("p")));

	/* The side buttons go with the right half, where they always were. */
	QAbstractButton *settings = keyboard.findChild<QAbstractButton *>(
	        QStringLiteral("keyboard-settings"));
	QVERIFY(settings);
	QVERIFY(keyboard.half(1)->isAncestorOf(settings));

	/*
	 * One key, one width, on either side. Laid over their own column
	 * counts the left half's keys came out a third wider than the right's,
	 * measured on the Fold.
	 */
	QWidget host;
	QHBoxLayout *row = new QHBoxLayout(&host);
	row->addWidget(keyboard.half(0));
	row->addWidget(new QWidget(&host), 1);
	row->addWidget(keyboard.half(1));

	/*
	 * The keyboard itself goes in too, because the halves show only while
	 * it does: left out, the halves stayed hidden, every key kept Qt's
	 * unlaid 100-pixel default, and the comparison below passed whatever
	 * the widths were -- a sabotage that made them differ was missed.
	 */
	row->addWidget(&keyboard);

	/*
	 * Handed back before the host goes: the keyboard is this function's,
	 * and a host that deleted it as a child would leave its destructor to
	 * run a second time. Declared after the host so it runs first, on an
	 * early return as well.
	 */
	struct hand_back_t {
		QWidget *widget;
		~hand_back_t() { widget->setParent(nullptr); }
	} hand_back { &keyboard };
	keyboard.set_half_width(200);
	host.resize(900, 400);
	host.show();
	QVERIFY(QTest::qWaitForWindowExposed(&host));
	QCoreApplication::processEvents();
	QVERIFY2(keyboard.half(0)->isVisible() && keyboard.half(1)->isVisible(),
	          "the halves must be laid out for their keys' widths to mean anything");
	sk_key_cap *q = nullptr;
	sk_key_cap *p = nullptr;
	for (sk_key_cap *cap : host.findChildren<sk_key_cap *>()) {
		if (cap->text() == QStringLiteral("q")) q = cap;
		if (cap->text() == QStringLiteral("p")) p = cap;
	}
	QVERIFY(q && p);
	QVERIFY2(qAbs(q->width() - p->width()) <= 2,
	          qPrintable(QStringLiteral("q is %1 wide and p is %2").arg(q->width()).arg(p->width())));
}

void layout_test::every_compact_page_starts_with_the_digits() {
	typedef sk_catalog catalog;

	/*
	 * Asked for by the holder 2026-10-08: digits without a page switch,
	 * for ports, PIDs and line numbers. On EVERY page, so the pages keep
	 * one height (check_keyboard_shape asserts that part), and the same
	 * palette row everywhere, so Shift on a digit means one thing.
	 */
	const QStringList digits = { QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"),
	                             QStringLiteral("4"), QStringLiteral("5"), QStringLiteral("6"),
	                             QStringLiteral("7"), QStringLiteral("8"), QStringLiteral("9"),
	                             QStringLiteral("0"), QStringLiteral("-") };
	for (const sk_group_t &group : catalog::instance().all(catalog::STYLE_COMPACT)) {
		QStringList top;
		for (const sk_key_t &key : group.rows.value(0)) top.append(key.label);
		QVERIFY2(top == digits,
		          qPrintable(QStringLiteral("the %1 page starts with %2")
		                             .arg(group.display_name, top.join(QLatin1Char(' ')))));
	}

	/*
	 * The row the digits left on the numbers page holds the shell
	 * characters no page offered without Shift.
	 */
	const QList<sk_group_t> &groups = catalog::instance().all(catalog::STYLE_COMPACT);
	const int numbers = catalog::instance().index_of(QStringLiteral("numbers"), catalog::STYLE_COMPACT);
	QStringList plain;
	for (const QList<sk_key_t> &row : groups.at(numbers).rows) {
		for (const sk_key_t &key : row) {
			if (key.kind == sk_key_t::TEXT) plain.append(QString(QChar(uint(key.value))));
		}
	}
	for (const char *ch : { "+", "<", ">", "{", "}", ":", "|", "%", "^", "!", "#" }) {
		QVERIFY2(plain.contains(QString::fromLatin1(ch)),
		          qPrintable(QStringLiteral("%1 is not on the numbers page without Shift").arg(QLatin1String(ch))));
	}
}

void layout_test::the_split_keyboard_sits_beside_the_terminal() {
	const forced_touch_t touch(1);
	const forced_screen_t held(QSize(860, 320));

	bssh_app_settings settings;
	const bssh_app_settings::keyboard_t before = settings.keyboard();
	settings.set_keyboard(bssh_app_settings::KEYBOARD_BUILT_IN);

	{
		bssh_main_window window;
		window.resize(860, 320);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));

		bssh_session_registry *registry = window.registry();
		QVERIFY(registry);
		const registry_left_as_found_t tidy(registry);
		window.adopt_session(registry->create(named(QStringLiteral("one"))));
		QCoreApplication::processEvents();

		sk_keyboard *keyboard = window.findChild<sk_keyboard *>(QStringLiteral("terminal-keyboard"));
		bssh_pane_container *panes = window.panes();
		QVERIFY(keyboard && panes);
		QCOMPARE(keyboard->style(), sk_catalog::STYLE_SPLIT);

		QWidget *left = keyboard->half(0);
		QWidget *right = keyboard->half(1);
		QVERIFY(left->isVisible());
		QVERIFY(right->isVisible());

		const auto global = [](QWidget *w) { return QRect(w->mapToGlobal(QPoint(0, 0)), w->size()); };

		/* Either side of the terminal, not under it. */
		QVERIFY2(global(left).right() < global(panes).left(), "the left half is left of the terminal");
		QVERIFY2(global(right).left() > global(panes).right(), "the right half is right of the terminal");

		/*
		 * Which is the point: the terminal keeps its height. Under the
		 * terminal, the keyboard left it one row on this screen.
		 */
		QVERIFY2(panes->height() > window.height() * 3 / 4,
		          qPrintable(QStringLiteral("the terminal is %1 of %2 tall")
		                             .arg(panes->height()).arg(window.height())));

		/* Hide puts both halves away, as it puts the keyboard away. */
		QAbstractButton *hide = window.findChild<QAbstractButton *>(QStringLiteral("keyboard-hide"));
		QVERIFY(hide);
		hide->click();
		QCoreApplication::processEvents();
		QVERIFY(!left->isVisible());
		QVERIFY(!right->isVisible());
	}

	{
		/* Upright, the same choice is the compact keyboard underneath. */
		const forced_screen_t upright(QSize(320, 860));
		bssh_main_window window;
		window.resize(320, 860);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));
		sk_keyboard *keyboard = window.findChild<sk_keyboard *>(QStringLiteral("terminal-keyboard"));
		QVERIFY(keyboard);
		QCOMPARE(keyboard->style(), sk_catalog::STYLE_COMPACT);
		QVERIFY(!keyboard->half(0)->isVisible());
		QVERIFY(!keyboard->half(1)->isVisible());
	}

	settings.set_keyboard(before);
}

namespace {

/* Drags a resize bar by (dx, dy) as a finger would, in one move. */
void drag_grip(QWidget *grip, int dx, int dy) {
	const QPoint from = grip->rect().center();
	QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier, from);
	QMouseEvent move(QEvent::MouseMove, QPointF(from + QPoint(dx, dy)),
	                 grip->mapToGlobal(QPointF(from + QPoint(dx, dy))),
	                 Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
	QCoreApplication::sendEvent(grip, &move);
	QTest::mouseRelease(grip, Qt::LeftButton, Qt::NoModifier, from + QPoint(dx, dy));
}

/* The bar that is a descendant of `within`, or null. */
QWidget *grip_in(QWidget *within) {
	for (QWidget *grip : within->findChildren<QWidget *>(QStringLiteral("keyboard-grip"))) {
		if (grip->isVisible()) return grip;
	}
	return nullptr;
}

} /* namespace */

void layout_test::dragging_a_bar_resizes_the_keyboard() {
	typedef sk_catalog catalog;
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	/* In a window tall enough to grow into, as the keyboard is in the app. */
	QWidget host;
	QVBoxLayout *column = new QVBoxLayout(&host);
	column->addStretch(1);
	sk_keyboard *keyboard = new sk_keyboard(&router);
	column->addWidget(keyboard);
	host.resize(400, 800);
	host.show();
	QVERIFY(QTest::qWaitForWindowExposed(&host));
	QCoreApplication::processEvents();

	QSignalSpy chosen(keyboard, &sk_keyboard::size_chosen);

	/* The bar on top: up is taller, down is shorter. */
	QWidget *top = grip_in(keyboard);
	QVERIFY2(top, "the keyboard under the terminal has a bar on its top edge");
	const int natural = keyboard->height();
	drag_grip(top, 0, -60);

	/* Laid out on the way back up through posted events, as in the app. */
	QTRY_VERIFY2(keyboard->height() > natural,
	          qPrintable(QStringLiteral("dragged up, %1 -> %2, %3 size(s) chosen, last %4")
	                             .arg(natural).arg(keyboard->height()).arg(chosen.count())
	                             .arg(chosen.isEmpty() ? -1 : chosen.last().at(1).toInt())));
	QCOMPARE(chosen.count(), 1);
	QCOMPARE(chosen.last().at(0).value<catalog::style_t>(), catalog::STYLE_COMPACT);

	/*
	 * Far down is as small as keys go, and no smaller: SMALLEST_KEY_DP,
	 * below sec 6.2's 48dp only because somebody dragged it there.
	 */
	drag_grip(top, 0, 2000);
	const auto shortest = [keyboard] {
		int out = 1000;
		for (sk_key_cap *cap : keyboard->findChildren<sk_key_cap *>()) {
			if (cap->isVisible()) out = qMin(out, cap->height());
		}
		return out;
	};
	QTRY_VERIFY2(shortest() < BSSH_TOUCH_TARGET_DP,
	              qPrintable(QStringLiteral("the shortest key is %1").arg(shortest())));
	QVERIFY2(shortest() >= sk_keyboard::SMALLEST_KEY_DP,
	          qPrintable(QStringLiteral("the shortest key is %1").arg(shortest())));

	/*
	 * Split: each half's bar is on its INNER edge, so toward the middle
	 * widens both -- right on the left half, left on the right one.
	 */
	keyboard->set_style(catalog::STYLE_SPLIT);
	keyboard->set_half_width(150);
	host.resize(900, 400);
	QCoreApplication::processEvents();
	QWidget *left = grip_in(keyboard->half(0));
	QWidget *right = grip_in(keyboard->half(1));
	QVERIFY(left && right);

	/*
	 * One drag resizes both halves, so holding either bar lights both:
	 * a bar that stays dim while its half moves reads as one that does
	 * nothing.
	 */
	sk_resize_grip *left_bar = qobject_cast<sk_resize_grip *>(left);
	sk_resize_grip *right_bar = qobject_cast<sk_resize_grip *>(right);
	QVERIFY(left_bar && right_bar);
	QTest::mousePress(left, Qt::LeftButton, Qt::NoModifier, left->rect().center());
	QVERIFY2(right_bar->lit(), "holding the left bar lights the right one");
	QTest::mouseRelease(left, Qt::LeftButton, Qt::NoModifier, left->rect().center());
	QVERIFY(!right_bar->lit());
	QVERIFY(!left_bar->lit());

	drag_grip(left, 30, 0);
	QCOMPARE(chosen.last().at(0).value<catalog::style_t>(), catalog::STYLE_SPLIT);
	QCOMPARE(chosen.last().at(1).toInt(), 180);
	drag_grip(right, -20, 0);
	QCOMPARE(chosen.last().at(1).toInt(), 200);

	/* And the bars go away when the setting says so. */
	keyboard->set_grips_shown(false);
	QVERIFY(!grip_in(keyboard->half(0)));
	QVERIFY(!grip_in(keyboard->half(1)));
}

void layout_test::a_dragged_keyboard_size_is_kept() {
	const forced_touch_t touch(1);
	const forced_screen_t held(QSize(860, 320));

	bssh_app_settings settings;
	const bssh_app_settings::keyboard_t before = settings.keyboard();
	settings.set_keyboard(bssh_app_settings::KEYBOARD_BUILT_IN);
	settings.reset_keyboard_size();

	const auto open = [](bssh_main_window &window) {
		window.resize(860, 320);
		window.show();
		QCoreApplication::processEvents();
	};

	{
		bssh_main_window window;
		open(window);
		sk_keyboard *keyboard = window.findChild<sk_keyboard *>(QStringLiteral("terminal-keyboard"));
		QVERIFY(keyboard);
		QCOMPARE(keyboard->style(), sk_catalog::STYLE_SPLIT);

		/*
		 * 22% of the window per half by default: 30% left the Fold's
		 * cover screen about 30 terminal columns, reported as way too big.
		 */
		bool found = false;
		for (QWidget *child : keyboard->half(0)->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
			if (child->objectName() != QStringLiteral("keyboard-grip")) {
				QCOMPARE(child->maximumWidth(), 860 * 22 / 100);
				found = true;
			}
		}
		QVERIFY(found);

		emit keyboard->size_chosen(sk_catalog::STYLE_SPLIT, 210);
	}

	QCOMPARE(bssh_app_settings().keyboard_split_width(), 210);

	{
		/* A new window -- a new run, as far as the keyboard knows. */
		bssh_main_window window;
		open(window);
		sk_keyboard *keyboard = window.findChild<sk_keyboard *>(QStringLiteral("terminal-keyboard"));
		QVERIFY(keyboard);
		bool found = false;
		for (QWidget *child : keyboard->half(0)->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
			if (child->objectName() != QStringLiteral("keyboard-grip")) {
				QCOMPARE(child->maximumWidth(), 210);
				found = true;
			}
		}
		QVERIFY(found);
	}

	/* Reset forgets every chosen size. */
	{
		bssh_app_settings_view page(bssh_app_settings_view::SECTION_KEYBOARD, nullptr);
		QPushButton *reset = page.findChild<QPushButton *>(QStringLiteral("keyboard_reset"));
		QVERIFY(reset);
		reset->click();
	}
	QCOMPARE(bssh_app_settings().keyboard_split_width(), 0);
	QCOMPARE(bssh_app_settings().keyboard_height(false), 0);

	settings.set_keyboard(before);
}

void layout_test::the_status_line_keeps_its_place_and_the_drum_its_corner() {
	const forced_touch_t touch(1);

	/* The built-in keyboard, so a split is what shares the landscape height. */
	bssh_app_settings settings;
	const bssh_app_settings::keyboard_t before = settings.keyboard();
	settings.set_keyboard(bssh_app_settings::KEYBOARD_BUILT_IN);
	struct restore_t {
		bssh_app_settings::keyboard_t keyboard;
		bssh_app_settings::status_line_t status;
		~restore_t() {
			bssh_app_settings().set_keyboard(keyboard);
			bssh_app_settings().set_status_line(status);
		}
	} restore { before, settings.status_line() };

	/* Shown, which is no longer the default (sec 8.7a): where it goes is
	 * still the question for anyone who turns it on. */
	settings.set_status_line(bssh_app_settings::STATUS_LINE_SESSION);

	/* Where the status line belongs: under the terminal, or over the
	 * split keyboard's left half. */
	enum where_t { UNDER, OVER_HALVES };
	const auto check = [](int width, int height, where_t where, bool turned) {
		const forced_screen_t held(QSize(width, height));
		bssh_main_window window;

		/*
		 * Turned: opened the other way up and then rotated, which is how
		 * a phone arrives in landscape. Opening already wide passed while
		 * the Fold cut the status line off: the upright layout's minimum
		 * height was still on the window when the new height arrived, and
		 * clamped it before the keyboard had become the split one.
		 */
		window.resize(turned ? QSize(height, width) : QSize(width, height));
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));

		bssh_session_registry *registry = window.registry();
		QVERIFY(registry);
		const registry_left_as_found_t tidy(registry);
		window.adopt_session(registry->create(named(QStringLiteral("one"))));
		QCoreApplication::processEvents();
		if (turned) {
			window.resize(width, height);
			QCoreApplication::processEvents();
		}
		QCOMPARE(window.size(), QSize(width, height));

		/*
		 * The mode drum is in the focused pane's corner (sec 8.7a), not a
		 * row of the window: it gave a narrow phone's 48dp row back to the
		 * terminal. One selector, and it is the corner's.
		 */
		bssh_pane_container *panes = window.findChild<bssh_pane_container *>();
		QVERIFY(panes);
		bssh_terminal_view *view = panes->focused_view();
		QVERIFY(view);
		QVERIFY(view->corner());
		QCOMPARE(window.findChildren<bssh_input_selector *>().size(), 1);
		bssh_input_selector *selector = window.findChild<bssh_input_selector *>();
		QVERIFY2(view->corner()->isAncestorOf(selector), "the drum is not in the pane's corner");
		QTRY_VERIFY(selector->isVisible());
		const QRect at(selector->mapTo(&window, QPoint(0, 0)), selector->size());
		QVERIFY2(at.bottom() < height,
		          qPrintable(QStringLiteral("the drum ends at %1 on a screen %2 tall")
		                             .arg(at.bottom()).arg(height)));
		QVERIFY(at.height() >= 3 * BSSH_TOUCH_TARGET_DP);

		QLabel *status = window.findChild<QLabel *>(QStringLiteral("status_line"));
		QVERIFY(status);
		const QRect said(status->mapTo(&window, QPoint(0, 0)), status->size());
		QVERIFY2(said.bottom() < height,
		          qPrintable(QStringLiteral("the status line ends at %1 on a screen %2 tall")
		                             .arg(said.bottom()).arg(height)));

		sk_keyboard *keyboard = window.findChild<sk_keyboard *>(QStringLiteral("terminal-keyboard"));
		QVERIFY(keyboard);
		if (where == OVER_HALVES) {
			/*
			 * Over the split halves, where the keys leave a band empty:
			 * the status text over the left half. The point is the height:
			 * under the terminal, its row cost 48dp of a landscape phone's
			 * 296, and the holder asked for it back (2026-10-09).
			 */
			QTRY_VERIFY(keyboard->half_header(0)->isAncestorOf(status));
			/*
			 * The status line's emptied row gives its height back when the
			 * outer layout next runs -- a posted request, so this waits for
			 * it rather than reading the frame the label left in.
			 */
			QTRY_VERIFY(panes->mapTo(&window, QPoint(0, panes->height())).y() == window.height());
			const int panes_bottom = panes->mapTo(&window, QPoint(0, panes->height())).y();
			QVERIFY2(panes_bottom == window.height(),
			          qPrintable(QStringLiteral("the terminal ends at %1 of %2")
			                             .arg(panes_bottom).arg(window.height())));

			/*
			 * And back under the terminal when the keyboard is put away,
			 * or hiding it would take the status line with it. Hiding
			 * resizes nothing, so this is what the window's watch on the
			 * halves' own show and hide is for.
			 */
			QToolButton *hide = window.findChild<QToolButton *>(QStringLiteral("keyboard-hide"));
			QVERIFY(hide);
			hide->click();
			QTRY_VERIFY(!keyboard->half(1)->isVisible());
			QTRY_VERIFY(!keyboard->half_header(0)->isAncestorOf(status));
			QTRY_VERIFY(status->isVisible());
		} else {
			QVERIFY2(!keyboard->half_header(0)->isAncestorOf(status),
			          "the status line is over a keyboard half that is not there");

			/*
			 * Under the terminal, the keys are the bottom edge and the
			 * status line above them, as nothing of an app's sits under
			 * Android's keyboard (the holder, 2026-10-23).
			 */
			QTRY_VERIFY(keyboard->isVisible());
			const QRect keys(keyboard->mapTo(&window, QPoint(0, 0)), keyboard->size());
			QVERIFY2(said.bottom() < keys.top(),
			          qPrintable(QStringLiteral("the status line ends at %1, the keys start at %2")
			                             .arg(said.bottom()).arg(keys.top())));
			QVERIFY2(keys.bottom() + 1 == window.height(),
			          qPrintable(QStringLiteral("the keys end at %1 of %2")
			                             .arg(keys.bottom() + 1).arg(window.height())));
		}
	};

	/*
	 * The Fold's cover screen in landscape, less the system bars -- the
	 * size the overflow was seen at. Measured against the height GIVEN
	 * rather than the window's own: off-screen, a window whose contents
	 * need more simply grows, where a phone's cannot and cuts them off.
	 */
	for (const bool turned : { false, true }) {
		check(822, 290, OVER_HALVES, turned);
		check(860, 320, OVER_HALVES, turned);
		check(320, 860, UNDER, turned);

		/* Wide but tall enough to keep the keyboard under the terminal. */
		check(1000, 700, UNDER, turned);
	}
}

void layout_test::a_narrow_screen_falls_back_to_the_compact_keyboard() {
	typedef sk_catalog catalog;

	/* The rule, at the boundary and either side of it. */
	QCOMPARE(sk_keyboard_style_for(catalog::STYLE_FULL, BSSH_PHONE_DP, 2000),
	          catalog::STYLE_COMPACT);
	QCOMPARE(sk_keyboard_style_for(catalog::STYLE_FULL,
	                                  SK_FULL_KEYBOARD_MIN_DP - 1, 2000),
	          catalog::STYLE_COMPACT);
	QCOMPARE(sk_keyboard_style_for(catalog::STYLE_FULL,
	                                  SK_FULL_KEYBOARD_MIN_DP, 2000),
	          catalog::STYLE_FULL);

	/*
	 * And the compact choice is never widened INTO the full one. The
	 * fallback narrows what was asked for; it does not overrule someone
	 * who asked for the thumb keyboard on a tablet.
	 */
	QCOMPARE(sk_keyboard_style_for(catalog::STYLE_COMPACT, 2000, 2000),
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

	sk_keyboard *keys = window.findChild<sk_keyboard *>(QStringLiteral("terminal-keyboard"));
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
	window.resize(SK_FULL_KEYBOARD_MIN_DP + 100, BSSH_PHONE_TALL_DP);
	QVERIFY(QTest::qWaitForWindowExposed(&window));

	QVERIFY2(keys->group_id().startsWith(QStringLiteral("full-")),
	          qPrintable(QStringLiteral("a wide window drew '%1'").arg(keys->group_id())));
	QCOMPARE(bssh_app_settings().keyboard(), bssh_app_settings::KEYBOARD_BUILT_IN_FULL);

	settings.set_keyboard(before);
}

void layout_test::the_full_keyboard_answers_about_itself() {
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	sk_keyboard keyboard(&router);
	keyboard.set_style(sk_catalog::STYLE_FULL);

	const QList<sk_group_t> &full =
	        sk_catalog::instance().all(
	                sk_catalog::STYLE_FULL);
	QVERIFY2(full.size() >= 2, "the full keyboard needs a second page to walk to");

	QCOMPARE(keyboard.group_id(), full.first().id);

	/*
	 * Every group, and a wrap past the end. The wrap is the case that was
	 * broken: an index the compact list accepts and the full one does not.
	 */
	for (int i = 0; i <= full.size(); ++i) {
		keyboard.set_group(i);

		const sk_group_t &expected = full.at(i % full.size());
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

	sk_keyboard keyboard(&router);
	keyboard.set_style(sk_catalog::STYLE_FULL);

	sk_key_cap *q = nullptr;
	for (sk_key_cap *cap : keyboard.findChildren<sk_key_cap *>()) {
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

	for (const sk_keyboard_layout_t &layout : sk_keyboard_layouts()) {
		QVERIFY(sk_set_keyboard_layout(layout.id));
		check_keyboard_shape(sk_catalog::STYLE_COMPACT);
		check_keyboard_shape(sk_catalog::STYLE_FULL);
	}
}

void layout_test::the_built_in_keyboard_fits_a_phone() {
	const forced_touch_t arrangement(1);

	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	sk_keyboard keyboard(&router);
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
	const int groups = sk_catalog::instance()
	        .all(sk_catalog::STYLE_COMPACT).size();
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

		sk_key_row *row = window.findChild<sk_key_row *>();
		sk_keyboard *keyboard = window.findChild<sk_keyboard *>(QStringLiteral("terminal-keyboard"));
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

		sk_key_row *row = window.findChild<sk_key_row *>();
		sk_keyboard *keyboard = window.findChild<sk_keyboard *>(QStringLiteral("terminal-keyboard"));
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
	const QString exit_button = QStringLiteral("Back");

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
	bssh_session_registry *registry = window.registry();
	QVERIFY(registry);
	const registry_left_as_found_t tidy(registry);
	bssh_session *one = registry->create(named(QStringLiteral("one")));
	window.adopt_session(one);

	/* A pane's own bar (sec 8.3's bar per pane), built with the pane. */
	bssh_pane_bar *row = window.bar_for(window.layout_manager()->front_of(one));
	QVERIFY2(row, "the pane must have a tab bar");
	QTabBar *tabs = row->tab_bar();

	/*
	 * The right-click route: a custom context menu policy, the bar turning
	 * it into a request, and the WINDOW listening for that request.
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
	QVERIFY2(QObject::disconnect(row, &bssh_pane_bar::menu_requested, nullptr, nullptr),
	          "nothing listens for a right-click on the tab bar");

	/*
	 * And the press-and-hold route, which Qt does not provide: an event
	 * filter of ours must be watching the bar itself.
	 */
	bool watches_tabs = false;
	for (const bssh_long_press *hold : row->findChildren<bssh_long_press *>()) {
		if (hold->parent() == row) watches_tabs = true;
	}
	QVERIFY2(watches_tabs, "no long-press gesture belongs to the bar");
}

void layout_test::backing_out_of_a_session_reaches_the_host_list() {
	const bssh_factory_reset_t factory_reset;
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

	/*
	 * And by the KEY, sent where Android sends it: to whatever has focus,
	 * which at a terminal is the terminal. The close event above is what
	 * the comment said Back was; on the Fold, Qt 6 delivered Back as
	 * Qt::Key_Back, and unaccepted at the terminal it backgrounded the app
	 * and never closed anything -- the host list was unreachable from a
	 * live session.
	 */
	QStackedWidget *pages = window.findChild<QStackedWidget *>();
	QVERIFY(pages);
	QWidget *terminal_page = nullptr;
	for (int i = 0; i < pages->count(); ++i) {
		if (pages->widget(i)->findChild<bssh_terminal_view *>()) terminal_page = pages->widget(i);
	}
	QVERIFY2(terminal_page, "the window must have a page holding the terminals");
	pages->setCurrentWidget(terminal_page);
	QVERIFY(!window.showing_front_door());

	bssh_terminal_view *view = terminal_page->findChild<bssh_terminal_view *>();
	QVERIFY(view);
	QKeyEvent key_back(QEvent::KeyPress, Qt::Key_Back, Qt::NoModifier);
	QApplication::sendEvent(view, &key_back);
	QVERIFY2(window.showing_front_door(), "Back at a terminal must go up to the host list");
	QVERIFY2(key_back.isAccepted(), "Back at a terminal was left for Android, which backgrounds the app");

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
	const bssh_factory_reset_t factory_reset;
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
	 * sec 8.3's drag-to-reorder is DESKTOP ONLY: the bar scrolls on a
	 * phone, and a bar that both scrolls and rearranges under one drag
	 * picks one by accident. Asked of a pane's own bar (sec 8.3's bar per
	 * pane) under each input.
	 */
	const auto pane_bar = [](bssh_main_window &window, bssh_session *one) -> QTabBar * {
		window.adopt_session(one);
		bssh_pane_bar *row = window.bar_for(window.layout_manager()->front_of(one));
		return row ? row->tab_bar() : nullptr;
	};

	bssh_session_registry *registry = bssh_session_registry::shared();
	const registry_left_as_found_t tidy(registry);
	{
		const forced_touch_t pointer(0);
		bssh_main_window window;
		QTabBar *tabs = pane_bar(window, registry->create(named(QStringLiteral("p"))));
		QVERIFY(tabs);
		QVERIFY2(tabs->isMovable(), "a pointer must be able to drag tabs");
	}

	const forced_touch_t thumb(1);
	bssh_main_window window;
	QTabBar *tabs = pane_bar(window, registry->create(named(QStringLiteral("t"))));
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

	/* Three in one pane, so its bar holds all three (sec 8.3's bar per pane). */
	bssh_session *a = registry->create(named(QStringLiteral("a")));
	bssh_session *b = registry->create(named(QStringLiteral("b")));
	bssh_session *shown = registry->create(named(QStringLiteral("shown")));
	window.adopt_session(a);
	window.adopt_session(b);
	window.adopt_session(shown);
	QCOMPARE(registry->count(), 3);

	const auto bar_now = [&window, a]() -> QTabBar * {
		bssh_pane_bar *row = window.bar_for(window.layout_manager()->front_of(a));
		return row ? row->tab_bar() : nullptr;
	};
	QTabBar *tabs = bar_now();
	QVERIFY(tabs);
	QCOMPARE(tabs->count(), 3);

	/*
	 * The LEFT button over a tab must not close anything -- it selects,
	 * which is what a tab bar is for. Asserted first, because a filter
	 * that closes on any button would pass the middle-click half and
	 * destroy a session on every click.
	 */
	const QPoint on_second = tabs->tabRect(1).center();
	QMouseEvent left(QEvent::MouseButtonRelease, on_second, tabs->mapToGlobal(on_second),
	                  Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(tabs, &left);
	QCoreApplication::processEvents();
	QCOMPARE(registry->count(), 3);

	/* Re-read: the bar is rebuilt with its pane, and the click is aimed at a rectangle. */
	tabs = bar_now();
	QVERIFY(tabs);
	const QPoint on_first = tabs->tabRect(0).center();
	QMouseEvent middle(QEvent::MouseButtonRelease, on_first, tabs->mapToGlobal(on_first),
	                    Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(tabs, &middle);

	/* Queued: closing rebuilds the pane, and with it the bar being clicked. */
	QCoreApplication::processEvents();

	/* The one under the pointer went, and the others did not. */
	QCOMPARE(registry->count(), 2);
	QVERIFY(registry->index_of(a) < 0);
	QVERIFY(registry->index_of(b) >= 0);
	QVERIFY(registry->index_of(shown) >= 0);

	/*
	 * The registry is process-wide now (sec 8.4), so a session left here
	 * is one the next test inherits.
	 */
	registry->close(b);
	registry->close(shown);
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
	 * comes to the front of the same pane, which leaves the first as a
	 * tab behind it in that pane's bar (sec 8.3's bar per pane).
	 */
	bssh_session *a = registry->create(named(QStringLiteral("a")));
	window.adopt_session(a);
	window.adopt_session(registry->create(named(QStringLiteral("on-screen"))));

	QMenu *menu = window.build_tab_menu(a, &window);
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

	/* A session this window does not hold gets no menu rather than an empty one. */
	bssh_session *stray = registry->create(named(QStringLiteral("stray")));
	QVERIFY2(!window.build_tab_menu(stray, &window), "a menu was built for a tab that is not there");
	registry->close(stray);

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

void layout_test::a_drop_with_no_room_keeps_an_arrangement_it_cannot_join() {
	/*
	 * A drop the window has no room for used to replace the intent with
	 * the dropped session ALONE: two panes somebody had arranged became
	 * tabs and the newcomer took the whole window. Nothing was closed --
	 * sec 8.2's rule held -- but its other half did not, because "the
	 * layout intent is remembered per tab-group" cannot survive the
	 * intent being overwritten. An arrangement made by hand went in one
	 * drag, silently.
	 *
	 * Both halves are asserted, because the fix is a rule rather than a
	 * removal: a window showing SEVERAL panes keeps them and the drop
	 * arrives as a tab, and a window showing ONE still hands the screen
	 * over -- there is no arrangement to lose there, and a drop that
	 * appears to do nothing is what the fallback was written against.
	 */
	bssh_session_registry *shared = bssh_session_registry::shared();
	while (shared->count() > 0) shared->close(shared->at(0));

	bssh_main_window window;
	window.resize(800, 600);
	window.show();
	QVERIFY(QTest::qWaitForWindowExposed(&window));
	QCoreApplication::processEvents();

	bssh_session *first = window.registry()->create(named(QStringLiteral("first")));
	window.adopt_session(first);
	window.split_beside_current();
	QCoreApplication::processEvents();

	/*
	 * Two panes and no room for a third: 800px is MEDIUM, which sec 8.1
	 * caps at two. Asserted rather than assumed, because a window that
	 * still had room would take the split branch and this test would
	 * pass while asking nothing.
	 */
	QCOMPARE(window.panes()->views().size(), 2);

	const int placed_before = int(window.layout_manager()->sessions().size());

	bssh_session *third = window.registry()->create(named(QStringLiteral("third")));
	window.drop_session_on(third, window.panes()->focused_session(), bssh_layout_manager::EDGE_RIGHT);
	QCoreApplication::processEvents();

	QVERIFY2(window.panes()->views().size() == 2,
	          "a drop with no room collapsed a split the user had arranged");
	QCOMPARE(int(window.layout_manager()->sessions().size()), placed_before + 1);

	/*
	 * And the one-pane case, where handing the screen over is right:
	 * nothing is lost, and a drop that changed nothing would read as a
	 * drop that failed.
	 */
	bssh_main_window narrow;
	narrow.resize(400, 600);
	narrow.show();
	QVERIFY(QTest::qWaitForWindowExposed(&narrow));
	QCoreApplication::processEvents();

	bssh_session *alone = narrow.registry()->create(named(QStringLiteral("alone")));
	narrow.adopt_session(alone);
	QCoreApplication::processEvents();
	QCOMPARE(narrow.panes()->views().size(), 1);

	bssh_session *dropped = narrow.registry()->create(named(QStringLiteral("dropped")));
	narrow.drop_session_on(dropped, alone, bssh_layout_manager::EDGE_RIGHT);
	QCoreApplication::processEvents();

	QCOMPARE(narrow.panes()->views().size(), 1);

	/*
	 * Asserted on WHICH session is the pane, through the tab the other
	 * one was pushed into. "One pane, and the dropped session is in the
	 * window" was the first form and it is true either way -- of a drop
	 * that handed the screen over and of one that quietly made the
	 * newcomer a tab. The sabotage that returns early said so; the count
	 * could not tell them apart.
	 */
	/*
	 * The dropped session is the pane, and the one it displaced is a tab
	 * behind it in that pane's bar (sec 8.3's bar per pane) -- not
	 * covering the drop, and not closed.
	 */
	QVERIFY(narrow.panes()->view_for(dropped));
	bssh_pane_bar *narrow_bar = narrow.bar_for(dropped);
	QVERIFY(narrow_bar && !narrow_bar->isHidden());
	QCOMPARE(narrow.layout_manager()->front_of(alone), dropped);
	QVERIFY2(narrow_bar->sessions().contains(alone),
	          "the displaced session should be a tab in the pane's bar");
	QVERIFY2(window.panes()->views().size() == 2,
	          "the second window disturbed the first");
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

	/*
	 * Three terminals in one pane: its bar lists all three (sec 8.3's bar
	 * per pane), and one of them is on screen.
	 */
	QCOMPARE(window.shown_sessions().size(), 3);
	QCOMPARE(window.panes()->views().size(), 1);
	QCOMPARE(window.bar_for(shown)->sessions(), QList<bssh_session *>({ tiled, waiting, shown }));

	/* Tiled beside what is already there -- the drop-on-the-panes half. */
	window.drop_session_on(tiled, window.panes()->focused_session(), bssh_layout_manager::EDGE_RIGHT);
	QCoreApplication::processEvents();

	QCOMPARE(window.panes()->views().size(), 2);
	QCOMPARE(window.shown_sessions().size(), 3);

	/*
	 * And it is gone from the bar. This is the assertion the whole change
	 * is about: same three terminals, one more of them on screen, one
	 * fewer in the bar.
	 */
	QCOMPARE(window.bar_for(shown)->sessions(), QList<bssh_session *>({ waiting, shown }));
	QVERIFY2(window.bar_for(tiled) && window.bar_for(tiled)->isHidden(),
	          "a pane of one shows no bar");

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

	/* Two panes of one: no bar on either (sec 8.3's bar per pane). */
	bssh_session *second = window.layout_manager()->visible_panes().value(1);
	QVERIFY(second && second != first);
	QVERIFY(window.bar_for(first)->isHidden() && window.bar_for(second)->isHidden());

	/* The first dropped on the second's bar: one pane, two tabs. */
	window.drop_session_as_tab(first, second, -1);
	QCoreApplication::processEvents();

	QCOMPARE(window.panes()->views().size(), 1);
	bssh_pane_bar *bar = window.bar_for(window.layout_manager()->front_of(first));
	QVERIFY(bar && !bar->isHidden());
	QCOMPARE(bar->sessions(), QList<bssh_session *>({ second, first }));
	QCOMPARE(bar->tab_bar()->tabText(1), first->title());
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

	window.drop_session_as_tab(last, last, -1);
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

	/* The last shown pane's bar, read afresh: it is rebuilt with the panes. */
	const auto last_bar = [&window]() -> bssh_pane_bar * {
		const QList<bssh_session *> shown = window.layout_manager()->visible_panes();
		return shown.isEmpty() ? nullptr : window.bar_for(shown.last());
	};
	QCOMPARE(window.panes()->views().size(), 2);
	QVERIFY2(last_bar() && last_bar()->isHidden(), "two panes of one show no bar");

	/* Phone width: one pane is all that fits (sec 8.2). */
	window.resize(BSSH_PHONE_DP, BSSH_PHONE_TALL_DP);
	QCoreApplication::processEvents();

	QCOMPARE(window.panes()->views().size(), 1);
	QVERIFY2(last_bar() && !last_bar()->isHidden() && last_bar()->sessions().size() == 2,
	          "the terminal that stopped being a pane has to be a tab in the "
	          "pane still shown, or it is on the window's list and reachable "
	          "from nothing (sec 8.2)");

	/*
	 * And back. The intent is remembered rather than truncated (sec 8.2),
	 * so widening restores the split and the bar empties again -- which
	 * is the half that proves the collapse did not DELETE anything.
	 */
	window.resize(900, 600);
	QCoreApplication::processEvents();

	QCOMPARE(window.panes()->views().size(), 2);
	QVERIFY(last_bar() && last_bar()->isHidden());

	while (shared->count() > 0) shared->close(shared->at(0));
}

void layout_test::what_the_terminal_reports_is_what_it_paints() {
	const bssh_factory_reset_t factory_reset;
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
	const bssh_factory_reset_t factory_reset;
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
	const bssh_factory_reset_t factory_reset;
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
	const bssh_factory_reset_t factory_reset;
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
		 * Asked for again, because the view asked for above is GONE: 900
		 * to 640 crosses from EXPANDED into MEDIUM (sec 8.1), which
		 * rebuilds the panes and deletes their views. Reading the old
		 * pointer read freed memory -- valgrind named the free in
		 * bssh_pane_container::rebuild() -- and it passed on stale
		 * values until a CI run in the Qt 6.4 image read the grid as
		 * 0x0. The product is right to rebuild; the test kept a
		 * pointer across a rebuild it caused itself.
		 */
		view = window.panes()->focused_view();
		QVERIFY(view);

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

void layout_test::a_gap_in_a_custom_key_row_holds_its_space() {
	/*
	 * `gap` is one of the words a custom row may be written with (sec
	 * 6.2's "the row is written as words"), and it did nothing at all.
	 * The row's build switch had no case for SPACER, so the token
	 * parsed, the editor's live preview drew a row without it, and the
	 * keys after it closed up -- a word in the vocabulary that is
	 * accepted and ignored, which is worse than one refused.
	 *
	 * The COMPILER had been saying so for as long as the token existed:
	 * `enumeration value 'SPACER' not handled in switch`, riding along
	 * in every build. This is that warning turned into an assertion.
	 *
	 * Asserted on the row's WIDTH rather than on a widget count, because
	 * what a spacer is for is the geometry: the enum's own comment says
	 * "the geometry has to hold the space or every key after it shifts".
	 * A count would pass against a zero-width placeholder.
	 */
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	const QList<sk_key_t> plain = sk_parse_key_row(QStringLiteral("esc tab ctrl"));
	const QList<sk_key_t> spaced =
	        sk_parse_key_row(QStringLiteral("esc tab gap ctrl"));

	/*
	 * The parser keeps the gap, which is the precondition: it skips a
	 * token it does not know, so a `gap` it dropped would make the
	 * comparison below trivially equal and the test would pass while
	 * asserting nothing.
	 */
	QCOMPARE(plain.size(), 3);
	QCOMPARE(spaced.size(), 4);

	sk_key_row_catalog &rows = sk_key_row_catalog::instance();

	QVERIFY2(rows.set_custom(plain), "the plain row was refused");
	sk_key_row without(&router);
	without.set_layout(QStringLiteral("custom"));
	/*
	 * The KEYS' extent, not the row's size hint. The row is a scroll area,
	 * whose hint is capped at 36 font heights and cached the first time it
	 * is asked: a row shown during construction cached its DEFAULT keys at
	 * the cap, and both rows then reported 612px whatever they held -- in
	 * the full suite and not alone, where nothing had shown it.
	 */
	const auto keys_width = [](sk_key_row &row) {
		QScrollArea *area = row.findChild<QScrollArea *>();
		return area && area->widget() ? area->widget()->sizeHint().width() : -1;
	};
	without.adjustSize();
	const int narrow = keys_width(without);

	QVERIFY2(rows.set_custom(spaced), "the spaced row was refused");
	sk_key_row with(&router);
	with.set_layout(QStringLiteral("custom"));
	with.adjustSize();
	const int wide = keys_width(with);

	/*
	 * A KEY's width, not merely wider. `wide > narrow` was the first
	 * form and it passed against a zero-width placeholder, because a box
	 * layout charges its own spacing for the extra item -- so the row
	 * grew by a few pixels of spacing and the assertion read that as a
	 * gap. The sabotage that sets the width to zero is what said so; the
	 * suite was green and the reason was the layout rather than the fix.
	 */
	QVERIFY2(wide - narrow >= BSSH_TOUCH_TARGET_DP,
	          qPrintable(QStringLiteral("a row with a gap wants %1px and the same "
	                                     "row without one wants %2px -- %3px more, "
	                                     "where a key is %4px, so the gap holds "
	                                     "nothing a finger could see")
	                             .arg(wide).arg(narrow)
	                             .arg(wide - narrow).arg(BSSH_TOUCH_TARGET_DP)));
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

	const sk_key_row_catalog catalog;
	QVERIFY(!catalog.all().isEmpty());

	for (const sk_key_row_layout_t &layout : catalog.all()) {
		sk_key_row row(&router);
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
	sk_key_row row(&router);
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
	 * added to sk_key_t without a repeat call lands in the wrong half
	 * and fails here, which is the whole point of writing it this way.
	 */
	bssh_emulator emulator(80, 24);
	bssh_input_router router(&emulator);

	int repeating = 0;
	int stateful = 0;

	const sk_key_row_catalog catalog;
	QVERIFY(!catalog.all().isEmpty());

	for (const sk_key_row_layout_t &layout : catalog.all()) {
		sk_key_row row(&router);
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
	const sk_catalog::style_t styles[] = {
		sk_catalog::STYLE_COMPACT,
		sk_catalog::STYLE_FULL
	};

	for (sk_catalog::style_t style : styles) {
		sk_keyboard keyboard(&router);
		keyboard.set_style(style);

		const int groups = sk_catalog::instance().all(style).size();
		QVERIFY(groups > 0);

		for (int group = 0; group < groups; ++group) {
			keyboard.set_group(group);

			for (const sk_key_cap *cap : keyboard.findChildren<sk_key_cap *>()) {
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
				if (qobject_cast<const sk_key_cap *>(button)) continue;
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
	const bssh_factory_reset_t factory_reset;
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

/*
 * Brightness on the Appearance page, where it is looked for, and only
 * there: it lived under "This device" and was reported as nowhere to be
 * found (2026-10-10). Each setting has one widget, so the device page
 * must not show it a second time.
 */
void layout_test::brightness_is_on_the_appearance_page() {
	const forced_touch_t touch(0);

	QTemporaryDir dir;
	bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));
	bssh_session_dialog dialog(&store);
	QTreeWidget *tree = dialog.findChild<QTreeWidget *>();
	QStackedWidget *pages = dialog.findChild<QStackedWidget *>();
	QVERIFY(tree && pages);

	const auto sliders_on = [&](const QString &category) {
		const QList<QTreeWidgetItem *> found = tree->findItems(category, Qt::MatchExactly);
		if (found.isEmpty()) return -1;
		tree->setCurrentItem(found.first());
		int count = 0;
		for (const QSlider *slider : pages->currentWidget()->findChildren<QSlider *>()) {
			if (slider->accessibleName() == QStringLiteral("Text brightness")
			    && slider->isVisibleTo(pages->currentWidget())) ++count;
		}
		return count;
	};
	QCOMPARE(sliders_on(QStringLiteral("Appearance")), 1);
	QCOMPARE(sliders_on(QStringLiteral("This device")), 0);

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
	/*
	 * Named, not counted: Terminal and Connection were removed on purpose
	 * (2026-10-10) as slices of Presets, and a floor on the count would
	 * let either come back unnoticed.
	 */
	QStringList names;
	for (const QTreeWidgetItem *item : items) names.append(item->text(0));
	QCOMPARE(names, QStringList({ QStringLiteral("Session"), QStringLiteral("Appearance"),
	                              QStringLiteral("Presets"), QStringLiteral("This device"),
	                              QStringLiteral("Licences") }));

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

	/* What each window's panes hold, every bar's tabs (sec 8.3's bar per pane). */
	QCOMPARE(first.layout_manager()->sessions(), QList<bssh_session *>({ mine, mine_shown }));
	QVERIFY2(second.layout_manager()->sessions().isEmpty(),
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

	QCOMPARE(first.layout_manager()->sessions(), QList<bssh_session *>({ mine, mine_shown }));
	QCOMPARE(second.layout_manager()->sessions(), QList<bssh_session *>({ theirs, theirs_shown }));
	bssh_pane_bar *their_bar = second.bar_for(theirs_shown);
	QVERIFY(their_bar);
	QVERIFY2(their_bar->tab_bar()->tabText(0) == QStringLiteral("theirs"),
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

	QCOMPARE(from.shown_sessions().size(), 2);
	QCOMPARE(onto.shown_sessions().size(), 0);
	QCOMPARE(from.layout_manager()->tabs(moving), QList<bssh_session *>({ moving, staying }));

	/* Onto a window showing nothing: no pane to name, so it is simply shown. */
	onto.drop_session_as_tab(moving, nullptr, -1);

	QCOMPARE(onto.shown_sessions(), QList<bssh_session *>{ moving });
	QVERIFY2(from.shown_sessions() == QList<bssh_session *>{ staying },
	          "the window it came from must let go, or the same terminal is "
	          "shown in two windows at once");
	QCOMPARE(from.layout_manager()->sessions(), QList<bssh_session *>({ staying }));

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
	window.drop_session_on(dropped, first, bssh_layout_manager::EDGE_RIGHT);
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
	narrow.drop_session_on(squeezed, only, bssh_layout_manager::EDGE_RIGHT);
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

	/* Still reachable: a tab in the pane's bar (sec 8.3's bar per pane). */
	QVERIFY2(narrow.layout_manager()->tabs(squeezed).contains(only),
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
	               bssh_session_payload(registry_index));
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
	bssh_pane_bar *row = window.bar_for(beside);
	QTabBar *tabs = row ? row->tab_bar() : nullptr;
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
	QCOMPARE(window.layout_manager()->tabs(carried).size(), 3);

	/* A different session, low in the window, delivered to the window. */
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

	/*
	 * Delivered to the WINDOW, off the tab row, it is a drop on no
	 * terminal -- and that does nothing (the holder, 2026-10-09). A drop
	 * over a terminal goes to the terminal, and the_zone_drawn_is_what_
	 * the_drop_does covers it. Accepted all the same, or the source would
	 * read "nobody wanted it" and tear out a window over this one.
	 */
	QVERIFY(on_panes.isAccepted());
	QCOMPARE(panes->pane_count(), 1);
	QVERIFY(!window.shown_sessions().contains(tiled));

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
	bssh_session *tabbed = window.registry()->create(named(QStringLiteral("tabbed")));
	window.adopt_session(tabbed);

	/* The pane's bar, aimed at; its tabs are what must not change. */
	bssh_pane_bar *row = window.bar_for(tabbed);
	QTabBar *tabs = row ? row->tab_bar() : nullptr;
	QVERIFY(tabs);
	const QList<bssh_session *> before = window.layout_manager()->sessions();

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
	QCOMPARE(window.layout_manager()->sessions(), before);

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
	const bssh_factory_reset_t factory_reset;
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

	QMenu *before = window.build_tab_menu(session, &window);
	QVERIFY(before);
	QCOMPARE(before->actions().first()->text(), QStringLiteral("Co&nnect"));
	delete before;

	session->connect_to_host();
	QVERIFY2(session->is_live(), "the fake transport should bring the session up");

	QMenu *after = window.build_tab_menu(session, &window);
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

	/*
	 * With tabs in the focused pane (sec 8.3's bar per pane), the NEW
	 * session takes the new pane and the one being looked at stays where
	 * it was, its tabs with it. Seen on the phone the other way round: the
	 * session in front left for the new pane, alone.
	 */
	window.show_as_tab(window.layout_manager()->intent().value(1));
	bssh_session *other = window.registry()->create(named(QStringLiteral("other")));
	window.adopt_session(other);
	window.layout_manager()->bring_to_front(only);
	QCoreApplication::processEvents();
	QCOMPARE(window.panes()->views().size(), 1);
	const QList<bssh_session *> tabs_before = window.layout_manager()->tabs(only);
	QCOMPARE(tabs_before.size(), 3);

	window.split_beside_current();
	QCoreApplication::processEvents();
	const QList<bssh_session *> panes = window.layout_manager()->intent();
	QCOMPARE(panes.size(), 2);
	QCOMPARE(panes.first(), only);
	QCOMPARE(window.layout_manager()->tabs(only), tabs_before);
	QVERIFY(!tabs_before.contains(panes.last()));

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
	               bssh_session_payload(onto.registry()->index_of(moving)));

	/*
	 * At the right edge, which puts it beside the pane (sec 8.3). The
	 * middle is a swap now, which leaves one pane -- this is about the
	 * ROUTE a drop takes, and an edge is the drop that makes it visible.
	 */
	/* The right zone, clear of the tab band along the very edge (sec 8.7a). */
	const QPoint at(pane->width() * 4 / 5, pane->height() / 2);
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
	bssh_session *one = window.registry()->create(named(QStringLiteral("one")));
	window.adopt_session(one);
	window.adopt_session(window.registry()->create(named(QStringLiteral("two"))));
	window.adopt_session(window.registry()->create(named(QStringLiteral("shown"))));
	QCoreApplication::processEvents();

	/* The pane's bar, with its three tabs (sec 8.3's bar per pane). */
	bssh_pane_bar *row = window.bar_for(window.layout_manager()->front_of(one));
	QTabBar *bar = row ? row->tab_bar() : nullptr;
	QVERIFY(bar);
	QCOMPARE(bar->count(), 3);

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
	const int drags = bssh_main_window::session_drags_started();
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
	QCOMPARE(window.layout_manager()->tabs(one).size(), 3);

	/*
	 * And no drag began at all. The tear-out was this test's only witness,
	 * and a drag whose release counts as a cancel -- the button still
	 * down, as Escape leaves it -- tears nothing out, so it went quiet.
	 */
	QCOMPARE(bssh_main_window::session_drags_started(), drags);

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
	const bssh_factory_reset_t factory_reset;
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
	const bssh_factory_reset_t factory_reset;
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
	const bssh_factory_reset_t factory_reset;
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
		window.drop_session_on(right, window.panes()->focused_session(), bssh_layout_manager::EDGE_RIGHT);
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
	/* A pane's own bar, as the window builds it (sec 8.3's bar per pane). */
	bssh_session_registry *registry = bssh_session_registry::shared();
	const registry_left_as_found_t tidy(registry);
	bssh_main_window window;
	bssh_session *one = registry->create(named(QStringLiteral("one")));
	window.adopt_session(one);
	bssh_pane_bar *row = window.bar_for(window.layout_manager()->front_of(one));
	QTabBar *tabs = row ? row->tab_bar() : nullptr;
	QVERIFY2(tabs, "the pane must have a tab bar");

	/* After the pane's own tab: indices are taken from what addTab returns. */
	const int web = tabs->addTab(QStringLiteral("web"));
	const int wide = tabs->addTab(QString(1024, QLatin1Char('W')));
	tabs->addTab(QStringLiteral("db"));
	tabs->resize(600, tabs->sizeHint().height());

	QVERIFY2(tabs->tabRect(wide).width() < tabs->width(),
	          qPrintable(QStringLiteral("one tab is %1px in a %2px bar")
	                             .arg(tabs->tabRect(wide).width())
	                             .arg(tabs->width())));

	const int plain = tabs->tabRect(web).width();
	tabs->setTabText(web, QStringLiteral("web"));
	QCOMPARE(tabs->tabRect(web).width(), plain);
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

	const sk_key_row_layout_t *custom =
	        sk_key_row_catalog::instance().by_id(QStringLiteral("custom"));
	QVERIFY(custom != nullptr);
	QCOMPARE(sk_key_row_spec(custom->keys), spec);

	/* Put the setting and the process-wide catalogue back, since both
	 * outlive this test. */
	settings.set_custom_key_row(original);
	if (!original.isEmpty()) {
		sk_key_row_catalog::instance().set_custom(sk_parse_key_row(original));
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
	{
		/*
		 * sec 7.1's features dialog, added here the day it was written
		 * rather than the next time somebody swept: it is fourteen
		 * checkboxes, which is more controls than any other surface in
		 * this list, and a reader meets every one of them.
		 */
		const QString nothing_off;
		bssh_term_features_editor features(nothing_off);
		complaint = unnamed_controls(QStringLiteral("terminal features"),
		                              &features, &counted);
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
	QVERIFY2(counted >= 34,
	          qPrintable(QStringLiteral("only %1 controls inspected across seven "
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

	const QList<sk_custom_slot_t> parsed = sk_parse_layout_slots(rows);
	QCOMPARE(parsed.size(), 3);

	/*
	 * A pipe is a real legend on BKSL, so the escape is the one thing
	 * the format cannot do without -- a split that ate it would drop the
	 * single key the escape exists for.
	 */
	QMap<QString, QString> bar;
	bar.insert(QStringLiteral("BKSL"), QStringLiteral("\\||\u00a6||"));
	const QList<sk_custom_slot_t> bar_parsed = sk_parse_layout_slots(bar);
	QCOMPARE(bar_parsed.size(), 1);
	QCOMPARE(bar_parsed.first().level[0], QStringLiteral("|"));

	/* A row with the wrong number of levels is dropped, and the rest of
	 * the layout stands: one mistyped line should cost that slot. */
	QMap<QString, QString> ragged;
	ragged.insert(QStringLiteral("AE01"), QStringLiteral("1|!"));
	ragged.insert(QStringLiteral("AE02"), QStringLiteral("2|@||"));
	QCOMPARE(sk_parse_layout_slots(ragged).size(), 1);

	/*
	 * Three slots cannot type ASCII, so the contract refuses them -- and
	 * says which characters are missing rather than "invalid", because a
	 * person who has transcribed 47 rows needs to know which one is
	 * wrong.
	 */
	QString why;
	QVERIFY2(!sk_set_custom_layout(QStringLiteral("Toy"), parsed, &why),
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
	QVERIFY2(!sk_keyboard_layouts().isEmpty(), "the catalogue lost its layouts");
	bool custom_offered = false;
	for (const sk_keyboard_layout_t &layout : sk_keyboard_layouts()) {
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
	for (const sk_custom_slot_t &slot : sk_shipped_layout_slots(QStringLiteral("us-intl"))) {
		whole.insert(slot.slot, sk_layout_slot_spec(slot));
	}
	QVERIFY2(whole.size() > 40, "the shipped layout came back too small to be one");

	why.clear();
	QVERIFY2(sk_set_custom_layout(QStringLiteral("Mine"),
	                                 sk_parse_layout_slots(whole), &why),
	          qPrintable(QStringLiteral("a transcription of US International was "
	                                     "refused: %1").arg(why)));

	/* Now it is offered, and selecting it works. */
	custom_offered = false;
	for (const sk_keyboard_layout_t &layout : sk_keyboard_layouts()) {
		if (layout.id == QStringLiteral("custom")) custom_offered = true;
	}
	QVERIFY2(custom_offered, "an accepted layout is not in the chooser");
	QVERIFY(sk_set_keyboard_layout(QStringLiteral("custom")));
	QCOMPARE(sk_keyboard_layout(), QStringLiteral("custom"));

	/*
	 * And the CONTROL a person actually looks at, which is one hop
	 * further than the catalogue and the hop this tree keeps losing
	 * things at: sk_keyboard_layouts() can be perfectly right while
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

	/*
	 * And the font's, added 2026-09-29. Unlike the other three it does
	 * not exist until a file is imported, so the picker offers the way
	 * to import one always, and lists the entry once there is one.
	 */
	{
		bssh_font_picker before;
		QVERIFY2(before.findChild<QPushButton *>(QStringLiteral("font_import")),
		          "a user can have their own font and no way to give it");

		const QString source = QStringLiteral(BSSH_TEST_FONT_DIR) + QStringLiteral("/Hack-Regular.ttf");
		if (QFile::exists(source)) {
			QTemporaryDir storage;
			bssh_set_custom_font_directory(storage.path());
			bssh_font_catalog &catalog = bssh_font_catalog::instance();
			const QString was = catalog.custom_family();

			QString family;
			QString why;
			QVERIFY2(bssh_import_custom_font(source, &family, &why), qPrintable(why));

			/*
			 * The FONT list by name: the scheme chooser beside it has a
			 * "custom" entry of its own, and a search of every combo box
			 * found that one and passed with the font row removed.
			 */
			bssh_font_picker after;
			QComboBox *fonts =
			        after.findChild<QComboBox *>(QStringLiteral("font-picker-family"));
			QVERIFY2(fonts, "the font picker has no font chooser");
			QVERIFY2(fonts->findData(bssh_font_catalog::custom_id()) >= 0,
			          "an imported font is not in the picker");

			catalog.set_custom_family(was);
			bssh_set_custom_font_directory(QString());
		}
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

	/*
	 * And the GROUPS, which this stopped being able to see the moment it
	 * worked. Verbs were pushed into submenus BECAUSE the top level was
	 * full -- Text Size, Pane, Terminal, Session -- so a measurement of
	 * the top level alone is now blind to exactly the place the next
	 * verb will go. A submenu that outgrows the screen has the same
	 * scroll arrows and the same entries behind a gesture nobody looks
	 * for.
	 */
	int measured = 1;
	for (QAction *action : menu->actions()) {
		QMenu *group = action->menu();
		if (!group) continue;

		++measured;
		group->ensurePolished();
		const QSize group_wanted = group->sizeHint();

		QVERIFY2(group_wanted.height() <= BSSH_PHONE_TALL_DP,
		          qPrintable(QStringLiteral("the %1 group wants %2px of a %3px screen, "
		                                     "with %4 entries")
		                             .arg(action->text())
		                             .arg(group_wanted.height())
		                             .arg(BSSH_PHONE_TALL_DP)
		                             .arg(group->actions().size())));
		QVERIFY2(group_wanted.width() <= BSSH_PHONE_DP,
		          qPrintable(QStringLiteral("the %1 group wants %2px of a %3px screen "
		                                     "in width")
		                             .arg(action->text())
		                             .arg(group_wanted.width()).arg(BSSH_PHONE_DP)));
	}

	/*
	 * The population, asserted alongside the verdict. A walk that stops
	 * finding groups reports success exactly as loudly as one that
	 * measured them all -- the vacuous pass, arriving in the gate that
	 * was written to stop this menu growing unnoticed.
	 */
	QVERIFY2(measured >= 5,
	          qPrintable(QStringLiteral("only %1 menu(s) were measured, so the groups "
	                                     "this check exists for were not walked")
	                             .arg(measured)));

	delete menu;
}

void layout_test::the_session_dialog_says_each_thing_once() {
	/*
	 * The holder's review of the GUI for redundant widgets (2026-10-22):
	 * a "Connection type" row offering SSH, greyed out, with nothing to
	 * choose; and a box titled "Load, save or delete a stored session"
	 * with a "Saved Sessions" label inside it.
	 */
	bssh_profile_store store;
	bssh_session_dialog dialog(&store);
	QVERIFY2(dialog.findChildren<QRadioButton *>().isEmpty(),
	          "a radio button with nothing to choose between");
	int saved_labels = 0;
	for (QLabel *label : dialog.findChildren<QLabel *>()) {
		QVERIFY2(!label->text().startsWith(QStringLiteral("Connection type")),
		          "the Connection type row is back");
		if (label->text() == QStringLiteral("Saved Sessions")) ++saved_labels;
	}
	int saved_boxes = 0;
	for (QGroupBox *box : dialog.findChildren<QGroupBox *>())
		if (box->title() == QStringLiteral("Saved Sessions")) ++saved_boxes;
	QCOMPARE(saved_boxes + saved_labels, 1);
}

void layout_test::a_host_can_be_given_a_reduced_terminal_without_a_text_editor() {
	/*
	 * sec 7.1: "The same string is a preset field, so a host that always
	 * wants a reduced terminal says it once." It could not be said once,
	 * or at all. The field was stored, validated on load and applied to
	 * the session, and no editor had a control for it -- alone among
	 * every field a preset carries -- so the only way to set one was to
	 * hand-edit app.ini.
	 *
	 * Three claims, and they fail apart: the dialog covers every
	 * feature, it round trips a spec, and the preset editor offers a way
	 * in. The last is the one the other two cannot see.
	 */
	{
		/*
		 * Named rather than a temporary, because
		 * `editor(QString())` declares a function -- the most vexing
		 * parse, and the compiler's message about it names neither.
		 */
		const QString nothing_off;
		bssh_term_features_editor editor(nothing_off);

		/*
		 * The POPULATION, asserted rather than the presence of a few
		 * boxes. The list is generated from the feature table, so a
		 * loop that stopped early would leave a feature this dialog
		 * silently does not govern -- and nothing else would say so.
		 */
		const QList<QCheckBox *> boxes = editor.findChildren<QCheckBox *>();
		QCOMPARE(boxes.size(), int(bssh_term_features_t::FEATURE_COUNT));

		/* An empty spec is a whole terminal, so every box is ticked. */
		for (QCheckBox *box : boxes) {
			QVERIFY2(box->isChecked(),
			          qPrintable(QStringLiteral("%1 opened unticked for a host that "
			                                     "turned nothing off")
			                             .arg(box->objectName())));
		}

		QCOMPARE(editor.spec(), QString());
		QCOMPARE(bssh_term_features_editor::summary(QString()),
		          QStringLiteral("All on"));
	}

	/*
	 * And a stored spec opens on it, named by sec 7.1's own spelling
	 * rather than by position: an enum renumbered underneath this would
	 * otherwise point the assertion at another feature and pass.
	 */
	{
		bssh_term_features_editor editor(QStringLiteral("-sixel,-hyperlinks"));

		QCheckBox *sixel =
		        editor.findChild<QCheckBox *>(QStringLiteral("feature_sixel"));
		QCheckBox *links =
		        editor.findChild<QCheckBox *>(QStringLiteral("feature_hyperlinks"));
		QCheckBox *paste =
		        editor.findChild<QCheckBox *>(QStringLiteral("feature_bracketed-paste"));
		QVERIFY(sixel && links && paste);

		QVERIFY2(!sixel->isChecked(), "a feature the spec turns off opened ticked");
		QVERIFY2(!links->isChecked(), "a feature the spec turns off opened ticked");
		QVERIFY2(paste->isChecked(), "a feature the spec leaves alone opened unticked");

		/*
		 * Round trip through the grammar rather than string equality:
		 * to_spec writes the off ones in the table's order, which is
		 * not necessarily the order somebody typed them in.
		 */
		QString ignored;
		const bssh_term_features_t back =
		        bssh_term_features_t::from_spec(editor.spec(), ignored);
		QVERIFY(!back.enabled(bssh_term_features_t::SIXEL));
		QVERIFY(!back.enabled(bssh_term_features_t::HYPERLINKS));
		QVERIFY(back.enabled(bssh_term_features_t::BRACKETED_PASTE));

		QCOMPARE(bssh_term_features_editor::summary(editor.spec()),
		          QStringLiteral("2 off"));
	}

	/*
	 * And the ROUTE, which is the half the two above cannot see: a
	 * dialog nothing opens is a dialog nobody reaches, and that was the
	 * state this whole entry describes.
	 */
	for (int touch = 0; touch <= 1; ++touch) {
		const forced_touch_t arrangement(touch);

		QTemporaryDir dir;
		bssh_profile_store store(dir.filePath(QStringLiteral("hosts.ini")));
		bssh_preset_editor editor(&store, touch ? bssh_preset_editor::TOUCH
		                                         : bssh_preset_editor::DESKTOP);

		QPushButton *button = editor.findChild<QPushButton *>(
		        QStringLiteral("preset-term-features"));
		QVERIFY2(button, "the preset editor offers no way to reach the terminal "
		                  "features, so a host's spec can only be set by editing "
		                  "app.ini");
		QVERIFY2(!button->text().isEmpty(),
		          "the button says nothing about what this host has turned off");
	}
}

void layout_test::the_divider_magnet_is_the_users_to_set_or_switch_off() {
	/*
	 * sec 8.7: "magnetic snapping to those two positions, snap radius
	 * about 5-10px and configurable". The radius was a constant, so the
	 * configurable half was never built -- and zero, which turns the
	 * magnet off, is the end somebody actually comes looking for.
	 *
	 * Two claims, and they fail differently. The setting has to round
	 * trip and refuse a number that would pin the divider; and the
	 * DIVIDER has to read it, which the setting agreeing with itself
	 * cannot show.
	 */
	bssh_app_settings store;
	const int was = store.column_snap_radius();

	QCOMPARE(store.column_snap_radius(), BSSH_COLUMN_SNAP_RADIUS);

	store.set_column_snap_radius(3);
	QCOMPARE(bssh_app_settings().column_snap_radius(), 3);

	/*
	 * Clamped rather than trusted: a radius wider than the panes holds
	 * the divider on the halfway line whatever the finger does, which
	 * reads as a divider that cannot be moved.
	 */
	store.set_column_snap_radius(9000);
	QCOMPARE(bssh_app_settings().column_snap_radius(), BSSH_MAX_COLUMN_SNAP_RADIUS);
	store.set_column_snap_radius(-4);
	QCOMPARE(bssh_app_settings().column_snap_radius(), 0);

	/*
	 * And on the way IN, which the two above cannot reach: the setter
	 * clamps as well, so a value written through it is already in range
	 * and the reader's clamp is never asked. The case the reader exists
	 * for is somebody editing app.ini, so the fixture has to write the
	 * file the way they would. Its sabotage was MISSED until this was
	 * here, with the two assertions above passing throughout.
	 */
	{
		QSettings raw(store.path(), QSettings::IniFormat);
		raw.setValue(QStringLiteral("layout/column_snap_radius"), 9000);
		raw.sync();
	}
	QCOMPARE(bssh_app_settings().column_snap_radius(), BSSH_MAX_COLUMN_SNAP_RADIUS);

	/*
	 * And the divider reads it. Two panes in a column, the splitter set
	 * a few pixels off the halfway line -- inside the default magnet and
	 * outside a switched-off one -- so the same gesture has to land in
	 * two different places depending only on the setting.
	 */
	const forced_touch_t desktop(0);

	bssh_session_registry registry;
	bssh_layout_manager layout;
	layout.set_width(400);
	layout.set_arrangement(bssh_layout_manager::COLUMN);

	QList<bssh_session *> pair;
	for (int i = 0; i < 2; ++i)
		pair.append(registry.create(named(QStringLiteral("d%1").arg(i))));

	bssh_pane_container container(&layout);
	container.show();
	container.resize(400, 600);
	layout.set_intent(pair);
	QCoreApplication::processEvents();
	QVERIFY(QTest::qWaitForWindowExposed(&container));
	QCOMPARE(container.pane_count(), 2);

	bssh_terminal_view *top = container.view_at(0);
	QVERIFY(top);
	const int cell = top->cell_height();
	QVERIFY(cell > 1);

	QSplitter *splitter = container.findChild<QSplitter *>();
	QVERIFY(splitter);

	const QList<int> sizes = splitter->sizes();
	QCOMPARE(sizes.size(), 2);
	const int total = sizes.at(0) + sizes.at(1);

	/*
	 * A position inside the default magnet whose own row-floor is NOT
	 * the halfway line's, so the two settings give different answers.
	 * Searched rather than computed: which side of the halfway line the
	 * nearest row boundary falls on depends on the widget's height and
	 * the font's cell, and a fixed offset happens to separate nothing at
	 * some of them -- which is a test that passes while asking nothing.
	 */
	const int half = total / 2;
	int near = -1;
	for (int d = 1; d <= BSSH_COLUMN_SNAP_RADIUS && near < 0; ++d) {
		if (bssh_snap_pane_extent(half - d, cell) != bssh_snap_pane_extent(half, cell))
			near = half - d;
		else if (bssh_snap_pane_extent(half + d, cell) != bssh_snap_pane_extent(half, cell))
			near = half + d;
	}
	QVERIFY2(near > 0, "no position inside the magnet floors to a different row, "
	                    "so this geometry cannot tell the settings apart");

	store.set_column_snap_radius(BSSH_COLUMN_SNAP_RADIUS);
	splitter->setSizes(QList<int>() << near << (total - near));
	container.snap_divider_to_cells();
	QCOMPARE(splitter->sizes().at(0), bssh_snap_pane_extent(half, cell));

	store.set_column_snap_radius(0);
	splitter->setSizes(QList<int>() << near << (total - near));
	container.snap_divider_to_cells();
	QVERIFY2(splitter->sizes().at(0) != bssh_snap_pane_extent(half, cell),
	          "the magnet was switched off and the divider still jumped to half");
	QCOMPARE(splitter->sizes().at(0), bssh_snap_pane_extent(near, cell));

	bssh_app_settings().set_column_snap_radius(was);
}

void layout_test::a_desktop_can_be_asked_for_the_column() {
	/*
	 * sec 8.7: "the idiom picks the arrangement's default and then lets
	 * go ... either can be set to anything -- which is the point rather
	 * than a concession: tiled has to be runnable on a phone and column
	 * on a desktop, or neither can be tested without two devices."
	 *
	 * Nothing could set it. The window called set_arrangement(COLUMN)
	 * once, in its constructor, keyed on is_touch_primary(), and offered
	 * no way back -- so on a desktop the column was reachable only by a
	 * test fixture forcing the platform to lie about itself.
	 *
	 * Asserted on the SPLITTER's orientation rather than on the setting,
	 * because the setting agreeing with itself proves nothing: what a
	 * column means is that the panes stack.
	 */
	/*
	 * The suite already runs against a sandbox HOME (test/main.cpp), so
	 * writing a device setting here does not touch anybody's app.ini.
	 */
	const forced_touch_t desktop(0);

	bssh_app_settings restore;
	const bssh_app_settings::arrangement_t was = restore.arrangement();

	{
		bssh_main_window window;
		window.resize(1200, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));

		QSplitter *splitter = window.findChild<QSplitter *>();
		QVERIFY(splitter);
		QCOMPARE(splitter->orientation(), Qt::Horizontal);
	}

	bssh_app_settings stored;
	stored.set_arrangement(bssh_app_settings::ARRANGEMENT_COLUMN);

	{
		bssh_main_window window;
		window.resize(1200, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));

		QSplitter *splitter = window.findChild<QSplitter *>();
		QVERIFY(splitter);
		QVERIFY2(splitter->orientation() == Qt::Vertical,
		          "a desktop asked for the column still tiled side by side");
	}

	/*
	 * And back to tiled, so the assertion is about the SETTING rather
	 * than about a window that happens to stack: a value that was
	 * ignored in one direction would pass the check above by accident if
	 * the default had been the column all along.
	 */
	stored.set_arrangement(bssh_app_settings::ARRANGEMENT_TILED);

	{
		bssh_main_window window;
		window.resize(1200, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));

		QSplitter *splitter = window.findChild<QSplitter *>();
		QVERIFY(splitter);
		QCOMPARE(splitter->orientation(), Qt::Horizontal);
	}

	/*
	 * And the window that is ALREADY OPEN, which is the half the two
	 * startup cases cannot see. Both paths set the arrangement -- the
	 * constructor early, so the first pane build reads the right shape,
	 * and apply_device_settings when somebody changes it -- so a test
	 * that only ever builds a fresh window passes with EITHER of them
	 * broken. Both sabotages were MISSED until this case existed, which
	 * is what a green test proving nothing looks like from the outside.
	 */
	{
		bssh_main_window window;
		window.resize(1200, 700);
		window.show();
		QVERIFY(QTest::qWaitForWindowExposed(&window));

		QSplitter *splitter = window.findChild<QSplitter *>();
		QVERIFY(splitter);
		QCOMPARE(splitter->orientation(), Qt::Horizontal);

		bssh_app_settings live;
		live.set_arrangement(bssh_app_settings::ARRANGEMENT_COLUMN);
		window.apply_device_settings();
		QCoreApplication::processEvents();

		QSplitter *after = window.findChild<QSplitter *>();
		QVERIFY(after);
		QVERIFY2(after->orientation() == Qt::Vertical,
		          "the setting changed and the open window kept its arrangement");
	}

	bssh_app_settings().set_arrangement(was);
}
