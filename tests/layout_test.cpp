#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSignalSpy>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QtTest>

#include "model/profile.h"
#include "platform/platform.h"
#include <QTemporaryDir>
#include "model/profile_store.h"
#include "ui/host_list.h"
#include "ui/host_editor.h"
#include "ui/preset_editor.h"
#include "ui/app_settings_view.h"
#include "ui/licences_view.h"
#include "ui/font_picker.h"
#include "ui/password_prompt.h"
#include "ui/session_dialog.h"
#include "ui/ssh_config_import.h"
#include <QCheckBox>
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
#include "layout_test.h"
#include "input/input_router.h"
#include "model/app_settings.h"
#include "model/session.h"
#include "model/session_registry.h"
#include "term/emulator.h"
#include "ui/key_row.h"
#include "ui/key_row_layout.h"
#include "ui/layout_manager.h"
#include "ui/pane_container.h"
#include "ui/terminal_view.h"

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
struct forced_touch_t {
	explicit forced_touch_t(int touch) { bssh_platform::force_touch_primary(touch); }
	~forced_touch_t() { bssh_platform::force_touch_primary(-1); }
};

const int BSSH_PHONE_DP = 360;
const int BSSH_PHONE_TALL_DP = 640;

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

void check_fits(const QString &name, QWidget *widget)
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
 * The small desktop window sec 8.2 already contemplates: it names tiling
 * a window into a half-screen column, which on a 1366px laptop is about
 * 680 and lands in the MEDIUM size class. 800x600 is the round number
 * safely inside that, and the classic floor besides.
 */
const int BSSH_SMALL_DESKTOP_W = 800;
const int BSSH_SMALL_DESKTOP_H = 600;

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
	bssh_main_window window(QStringLiteral("nowhere.example"));
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
	window.adopt_session(a);
	window.adopt_session(b);
	QCOMPARE(registry->count(), 2);

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
	QCOMPARE(registry->count(), 2);

	QMouseEvent middle(QEvent::MouseButtonRelease, on_second, tabs->mapToGlobal(on_second),
	                    Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
	QApplication::sendEvent(tabs, &middle);

	/* The one under the pointer went, and the other did not. */
	QCOMPARE(registry->count(), 1);
	QCOMPARE(registry->at(0), a);
	Q_UNUSED(b);

	/*
	 * The registry is process-wide now (sec 8.4), so a session left here
	 * is one the next test inherits. Cleaned up explicitly rather than by
	 * the window going away, which no longer owns it.
	 */
	registry->close(a);
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

	window.adopt_session(registry->create(named(QStringLiteral("a"))));

	QMenu *menu = window.build_tab_menu(0, &window);
	QVERIFY2(menu, "a tab with a session behind it must have a menu");

	const QList<QAction *> actions = menu->actions();
	QCOMPARE(actions.size(), 4);

	QCOMPARE(actions.at(0)->text(), QStringLiteral("&Duplicate"));
	QCOMPARE(actions.at(1)->text(), QStringLiteral("Show &beside current"));

	/*
	 * The separator earns its place: Close is the one verb here that
	 * cannot be undone, and a menu that puts it flush against the others
	 * is one a slipped thumb closes a session with.
	 */
	QVERIFY2(actions.at(2)->isSeparator(), "the destructive verb needs separating");
	QCOMPARE(actions.at(3)->text(), QStringLiteral("&Close"));

	for (const QAction *action : actions) {
		if (action->isSeparator()) continue;
		QVERIFY2(action->text().contains(QLatin1Char('&')),
		          qPrintable(action->text() + QStringLiteral(" has no access key")));
	}

	/*
	 * Duplicate does not depend on room for a pane, which the split does
	 * -- it opens a tab. A window with no room must still offer it.
	 */
	QVERIFY2(actions.at(0)->isEnabled(), "Duplicate must not depend on the window's width");

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
	 * (tests/main.cpp); restored afterwards regardless, since it is a
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
		check_fits(QStringLiteral("key row: ") + layout.id, &row);
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

	bssh_session *mine = first.registry()->create(named(QStringLiteral("mine")));
	first.adopt_session(mine);

	QTabBar *first_tabs = first.findChild<QTabBar *>();
	QTabBar *second_tabs = second.findChild<QTabBar *>();
	QVERIFY(first_tabs && second_tabs);

	QCOMPARE(first_tabs->count(), 1);
	QVERIFY2(second_tabs->count() == 0,
	          "the other window must not show a session it was never given");

	/* Both still see it in the registry -- one process, one set. */
	QCOMPARE(second.registry()->count(), 1);

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

	QTabBar *tabs = window.findChild<QTabBar *>();
	QVERIFY(tabs);
	QCOMPARE(tabs->count(), 2);

	bssh_main_window *torn = window.tear_out_tab(1);
	QVERIFY2(torn, "a tab that is not the last must tear out");
	torn->setAttribute(Qt::WA_DeleteOnClose, false);

	/* The session is the SAME object, still registered, still carrying
	 * what was on its screen. */
	QCOMPARE(shared->count(), 2);
	QCOMPARE(shared->index_of(go) >= 0, true);
	QVERIFY(go->emulator()->screen().to_text().contains(QStringLiteral("torn-out-marker")));

	/* And it is shown in the new window and no longer in the old one. */
	QTabBar *torn_tabs = torn->findChild<QTabBar *>();
	QVERIFY(torn_tabs);
	QCOMPARE(tabs->count(), 1);
	QCOMPARE(torn_tabs->count(), 1);

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
	QCOMPARE(from_tabs->count(), 2);
	QCOMPARE(onto_tabs->count(), 0);

	onto.drop_session(moving, false);

	QCOMPARE(onto_tabs->count(), 1);
	QVERIFY2(from_tabs->count() == 1,
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
	narrow.resize(360, 640);
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

	QTabBar *narrow_tabs = narrow.findChild<QTabBar *>();
	QVERIFY(narrow_tabs);
	QVERIFY2(narrow_tabs->count() == 2,
	          "a tile that will not fit must still arrive as a tab");

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
	QCOMPARE(tabs->count(), 3);

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
