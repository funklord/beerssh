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
