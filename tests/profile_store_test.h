#ifndef BSSH_PROFILE_STORE_TEST_H
#define BSSH_PROFILE_STORE_TEST_H

#include <QObject>

/*
 * Saved hosts (project.md sec 10.6) and the two front doors (sec 8.5).
 *
 * The store is worth testing hard because it is the first thing in this
 * project that persists: a bug here outlives the process and takes the
 * user's profiles with it.
 */
class profile_store_test : public QObject {
	Q_OBJECT

private slots:
	void saves_and_lists_profiles();
	void updates_rather_than_duplicating();
	void removes_profiles();
	void survives_being_reopened();
	void schema_has_nowhere_to_put_a_secret();
	void layout_round_trips();
	void layout_forgets_deleted_hosts();
	void an_old_database_gains_the_layout_table();
	void the_stored_password_flag_round_trips();
	void front_door_follows_the_input_device();
};

#endif /* BSSH_PROFILE_STORE_TEST_H */
