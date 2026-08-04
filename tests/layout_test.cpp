#include <QAbstractButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSignalSpy>
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
#include <QTabBar>
#include <QMetaMethod>
#include "ui/long_press.h"
#include "ui/main_window.h"
#include "fake_ssh_session.h"
#include "layout_test.h"
#include "model/session.h"
#include "model/session_registry.h"
#include "term/emulator.h"
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
