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
	void deleting_a_host_takes_its_password_with_it();
	void a_password_a_locked_store_kept_is_swept_at_the_next_unlock();
	void a_sweep_does_not_erase_a_password_a_reused_id_now_owns();
	void a_save_that_cannot_reach_the_disk_is_not_reported_as_saved();
	void a_device_setting_that_cannot_be_written_says_so();
	void a_hand_typed_port_out_of_range_falls_back_rather_than_wrapping();
	void a_hand_typed_override_meets_the_same_bounds_as_a_preset();
	void saves_and_lists_profiles();
	void updates_rather_than_duplicating();
	void removes_profiles();
	void survives_being_reopened();
	void schema_has_nowhere_to_put_a_secret();
	void layout_round_trips();
	void layout_forgets_deleted_hosts();
	void an_old_database_gains_the_layout_table();
	void the_stored_password_flag_round_trips();
	void the_starter_presets_are_seeded_once();
	void a_host_follows_a_preset_and_may_override_it();
	void a_preset_in_use_cannot_be_deleted();
	void a_shipped_preset_cannot_be_edited();
	void the_host_editor_builds_a_profile();
	void front_door_follows_the_input_device();
	void an_identity_file_survives_the_round_trip();
	void a_jump_host_survives_the_round_trip();
	void the_touch_editor_resolves_through_the_preset();
	void the_touch_editor_saves_the_colour_scheme();
	void a_list_follows_a_host_added_in_another_window();
	void the_biometric_flag_survives_the_round_trip();
	void the_capability_decides_whether_the_option_exists();
	void the_touch_front_door_can_edit_a_host_it_created();
	void repeated_imports_get_distinguishable_names();
	void an_explicit_directory_is_never_left();
	void an_existing_host_list_is_migrated_once();
};

#endif /* BSSH_PROFILE_STORE_TEST_H */
