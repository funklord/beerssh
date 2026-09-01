#ifndef BSSH_PRESET_TEST_H
#define BSSH_PRESET_TEST_H

#include <QObject>

/*
 * Presets and the three-layer resolution (project.md sec 10).
 *
 * The security-relevant test here is the export one. sec 10.3 makes
 * "a preset cannot hold a secret" a structural property rather than a
 * promise the export function keeps, and a test that reads the exported
 * keys is what stops a future field quietly breaking it.
 */
class preset_test : public QObject {
	Q_OBJECT

private slots:
	void an_imported_preset_cannot_exceed_the_editor_s_ranges();
	void a_steady_cursor_is_the_default();
	void an_exported_preset_contains_nothing_private();
	void export_and_import_round_trip();
	void an_imported_preset_cannot_claim_to_be_builtin();
	void the_exported_file_contains_nothing_private();
	void an_ini_round_trip_keeps_every_field();
	void a_hostile_ini_cannot_claim_to_be_builtin();
	void the_preset_beats_the_defaults();
	void the_host_beats_the_preset();
	void an_override_equal_to_the_preset_is_not_an_override();
	void reverting_a_field_follows_the_preset_again();
	void editing_a_preset_reaches_every_host_that_did_not_override();
	void the_starter_presets_differ_where_it_matters();
};

#endif /* BSSH_PRESET_TEST_H */
