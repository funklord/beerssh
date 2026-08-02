#ifndef BSSH_SESSION_REGISTRY_TEST_H
#define BSSH_SESSION_REGISTRY_TEST_H

#include <QObject>

/*
 * The registry (project.md sec 3, core principle 2).
 *
 * The point of these is the one property the whole architecture rests on:
 * a session is not a widget's possession. If that holds, tabs, splits,
 * activity recreation and background survival are bookkeeping; if it ever
 * stops holding, all four break at once and none of them obviously.
 */
class session_registry_test : public QObject {
	Q_OBJECT

private slots:
	void creates_and_lists_sessions();
	void session_outlives_the_view_that_showed_it();
	void closing_announces_before_destroying();
	void live_count_ignores_dead_sessions();
	void title_falls_back_from_osc_to_profile();
	void the_transport_reports_which_kind_of_ending_it_was();
	void a_password_is_requested_only_when_keys_fail();
	void a_host_key_is_never_trusted_without_an_answer();
	void an_encrypted_key_is_offered_before_a_password();
};

#endif /* BSSH_SESSION_REGISTRY_TEST_H */
