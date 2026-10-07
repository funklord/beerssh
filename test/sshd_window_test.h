#ifndef BSSH_SSHD_WINDOW_TEST_H
#define BSSH_SSHD_WINDOW_TEST_H

#include <QObject>

/*
 * beersshd's Android screen (project.md sec 19), on the desktop over a
 * platform the test controls: storage access, the Wi-Fi address, the
 * foreground service and Android's time limit are all answers it can
 * change, which a phone does not let a test do.
 */
class sshd_window_test : public QObject {
	Q_OBJECT

private slots:
	void without_access_it_asks_and_does_not_start();
	void it_serves_the_platform_root_on_the_platform_address();
	void the_time_limit_stops_the_server_and_says_why();
	void losing_wifi_stops_and_a_new_address_restarts();
	void saved_keys_are_counted_by_the_login_parse();
	void the_keys_box_follows_the_file_but_not_over_edits();
};

#endif /* BSSH_SSHD_WINDOW_TEST_H */
