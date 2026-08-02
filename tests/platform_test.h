#ifndef BSSH_PLATFORM_TEST_H
#define BSSH_PLATFORM_TEST_H

#include <QObject>

/*
 * The platform seam (project.md sec 3, core principle 3) and the
 * close-with-live-sessions policy (sec 5.5).
 *
 * What is testable here is the desktop half and the policy; the Android
 * half is JNI into a foreground service and is honestly not implemented,
 * which platform_android.cpp says at the call site rather than pretending
 * with a stub.
 */
class platform_test : public QObject {
	Q_OBJECT

private slots:
	void desktop_platform_is_selected_and_named();
	void keep_alive_controls_quit_on_last_window();
	void session_count_and_notify_are_safe_without_a_tray();
	void close_policy_asks_only_when_something_is_live();
	void the_window_opens_on_the_front_door();
};

#endif /* BSSH_PLATFORM_TEST_H */
