#ifndef BSSH_UI_SHOTS_H
#define BSSH_UI_SHOTS_H

#include <QObject>

/*
 * Renders every screen to a PNG so somebody can look at them
 * (project.md sec 12.3).
 *
 * Not a test. It asserts almost nothing and cannot fail a build; what it
 * does is make "look at the thing" a command rather than an afternoon.
 *
 * It exists because looking is what kept working. Every layout defect in
 * this project so far was found by rendering a screen and looking at it,
 * and several of them had passed every test that existed at the time: the
 * front door embedded the TOUCH preset editor for want of one enum, the
 * host and preset editors overlapped their own rows on a phone, a
 * connection error blanked the app because a status label was allowed to
 * decide the window's width. Each was then pinned with a test -- but the
 * test was written after the picture, not before it.
 *
 * The measurements it prints alongside are the ones worth watching: the
 * minimum size each surface demands. A surface that starts demanding more
 * than the screen it is meant for is the shape most of those bugs had.
 */
class bssh_ui_shots : public QObject {
	Q_OBJECT

private slots:
	void render_touch_surfaces();
	void render_desktop_surfaces();
	void render_front_door_pages();
};

#endif /* BSSH_UI_SHOTS_H */
