#ifndef BSSH_PRESET_EDITOR_TEST_H
#define BSSH_PRESET_EDITOR_TEST_H

#include <QObject>

/*
 * Tests for editing the middle layer (project.md sec 10).
 *
 * The rules here are the ones that protect a user from the preset
 * system's own power: an edit reaches every host that follows it, so it
 * has to be counted before it happens, and the shipped starting points
 * have to survive being opened by someone who then pressed Save.
 */
class preset_editor_test : public QObject {
	Q_OBJECT

private slots:
	void lists_every_preset_including_the_shipped_ones();
	void a_shipped_preset_cannot_be_saved_over();
	void cloning_a_shipped_preset_yields_an_editable_copy();
	void the_host_count_is_shown_before_the_edit();
	void saving_changes_what_following_hosts_resolve_to();
	void an_overridden_field_does_not_follow_the_preset();
	void a_preset_round_trips_through_an_exported_file();
	void an_exported_preset_names_nobodys_infrastructure();
	void an_import_never_claims_an_id_or_shipped_status();
	void an_untouched_font_stays_unset_rather_than_being_pinned();
};

#endif /* BSSH_PRESET_EDITOR_TEST_H */
