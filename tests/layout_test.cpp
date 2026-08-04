#include <QScrollArea>
#include <QSignalSpy>
#include <QtTest>

#include "model/profile.h"
#include <QTemporaryDir>
#include "model/profile_store.h"
#include "ui/host_list.h"
#include "ui/host_editor.h"
#include "ui/preset_editor.h"
#include "ui/app_settings_view.h"
#include "ui/licences_view.h"
#include "ui/font_picker.h"
#include "ui/password_prompt.h"
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
		bssh_preset_editor editor(&store, true);
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
