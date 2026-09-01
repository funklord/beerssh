#include <QtTest>

#include <libssh/libssh.h>

#include "host_key_test.h"
#include "ssh/ssh_session_libssh.h"

void host_key_test::a_recorded_and_unchanged_key_is_trusted() {
	QCOMPARE(bssh_host_key_verdict(SSH_KNOWN_HOSTS_OK), BSSH_HOST_KEY_TRUSTED);
}

/*
 * Two spellings of "not recorded". NOT_FOUND is the case where there is no
 * known-hosts file yet, and libssh says of it in as many words: "the host
 * is thus unknown".
 */
void host_key_test::an_unrecorded_host_is_a_first_contact() {
	QCOMPARE(bssh_host_key_verdict(SSH_KNOWN_HOSTS_UNKNOWN), BSSH_HOST_KEY_FIRST_CONTACT);
	QCOMPARE(bssh_host_key_verdict(SSH_KNOWN_HOSTS_NOT_FOUND), BSSH_HOST_KEY_FIRST_CONTACT);
}

/*
 * CHANGED is the famous one and was always handled. OTHER was not: "the
 * server gave us a key of a type while we had an other type recorded", of
 * which libssh's own header says "It is a possible attack" -- and it fell
 * through to the first-contact prompt, whose button reads "Accept and
 * connect".
 *
 * The warning dialog's words fit it exactly, which is why both map here
 * rather than OTHER earning a third dialog: the key IS not the one
 * recorded.
 */
void host_key_test::a_key_that_does_not_match_is_a_mismatch_whichever_way() {
	QCOMPARE(bssh_host_key_verdict(SSH_KNOWN_HOSTS_CHANGED), BSSH_HOST_KEY_MISMATCH);
	QCOMPARE(bssh_host_key_verdict(SSH_KNOWN_HOSTS_OTHER), BSSH_HOST_KEY_MISMATCH);
}

/*
 * The one that matters most, and the reason this file exists.
 *
 * An unreadable or malformed known-hosts file means the check DID NOT
 * RUN. That was reported as "this host is not in the store", under a
 * dialog inviting the user to accept the key -- so a trust decision was
 * offered on the strength of a verification that never happened. "I could
 * not tell" and "it is not there" are different answers and were rendered
 * identically.
 *
 * Asserted as "not first contact" as well as "unverified", because the
 * defect was never that the verdict was unnamed -- it was that it was
 * named the SAME as the ordinary case.
 */
void host_key_test::a_check_that_could_not_run_is_never_a_first_contact() {
	QCOMPARE(bssh_host_key_verdict(SSH_KNOWN_HOSTS_ERROR), BSSH_HOST_KEY_UNVERIFIED);
	QVERIFY2(bssh_host_key_verdict(SSH_KNOWN_HOSTS_ERROR) != BSSH_HOST_KEY_FIRST_CONTACT,
	          "a failed check must not be offered to the user as a new host");
}

/*
 * A libssh that grows a seventh verdict must not have it silently mean
 * "trusted". The value is deliberately one this enum does not define.
 */
void host_key_test::an_unrecognised_verdict_refuses_rather_than_trusts() {
	QCOMPARE(bssh_host_key_verdict(4242), BSSH_HOST_KEY_UNVERIFIED);
	QCOMPARE(bssh_host_key_verdict(-99), BSSH_HOST_KEY_UNVERIFIED);
}

void host_key_test::an_accepted_credential_is_accepted() {
	QCOMPARE(bssh_auth_outcome(SSH_AUTH_SUCCESS), BSSH_AUTH_ACCEPTED);
}

void host_key_test::only_denied_means_the_credential_was_wrong() {
	QCOMPARE(bssh_auth_outcome(SSH_AUTH_DENIED), BSSH_AUTH_REJECTED);

	/*
	 * The whole point, stated as the property rather than value by
	 * value: exactly one of libssh's six results means "what you typed
	 * was wrong", and every other one used to say so too.
	 */
	const int others[] = { SSH_AUTH_SUCCESS, SSH_AUTH_PARTIAL, SSH_AUTH_INFO,
		                SSH_AUTH_AGAIN, SSH_AUTH_ERROR };
	for (int rc : others) {
		QVERIFY2(bssh_auth_outcome(rc) != BSSH_AUTH_REJECTED,
		          "only SSH_AUTH_DENIED may be reported as a wrong credential");
	}
}

/*
 * `AuthenticationMethods password,keyboard-interactive` returns PARTIAL
 * for a password that was RIGHT: the server took it and wants another
 * factor. It was reported as "Authentication failed", so the user retyped
 * a correct password indefinitely.
 *
 * The key paths already fell through to the next prompt on this, which is
 * the correct move and was an accident of testing only for SUCCESS -- so
 * the same value was handled sensibly in one place and as a failure in
 * another, which is the asymmetry that gave the bug away.
 */
void host_key_test::a_partial_acceptance_is_not_a_failure() {
	QCOMPARE(bssh_auth_outcome(SSH_AUTH_PARTIAL), BSSH_AUTH_NEEDS_MORE);
}

/*
 * A transport or protocol fault is not a verdict on the credential, and
 * telling the user their password failed sends them to fix the one thing
 * that was not broken. The same collapse as the known-hosts ERROR above,
 * one layer along.
 */
void host_key_test::an_auth_error_is_not_a_rejected_credential() {
	QCOMPARE(bssh_auth_outcome(SSH_AUTH_ERROR), BSSH_AUTH_ERRORED);
}

/*
 * INFO and AGAIN cannot occur on these calls -- INFO belongs to
 * keyboard-interactive and AGAIN to a non-blocking session -- so what
 * matters is only that neither can be mistaken for a way through.
 */
void host_key_test::an_unrecognised_auth_result_is_not_an_acceptance() {
	QCOMPARE(bssh_auth_outcome(SSH_AUTH_INFO), BSSH_AUTH_ERRORED);
	QCOMPARE(bssh_auth_outcome(SSH_AUTH_AGAIN), BSSH_AUTH_ERRORED);
	QCOMPARE(bssh_auth_outcome(4242), BSSH_AUTH_ERRORED);
	QVERIFY(bssh_auth_outcome(4242) != BSSH_AUTH_ACCEPTED);
}
