#ifndef BSSH_LIVE_SSH_TEST_H
#define BSSH_LIVE_SSH_TEST_H

#include <QObject>

/*
 * The SSH transport against a real server (project.md sec 12.3).
 *
 * Everything else in tests/ runs against bssh_fake_ssh_session, which is
 * what makes the suite runnable with no server and no device -- and what
 * leaves exactly one layer unexercised: the libssh binding itself. A fake
 * proves the emulator handles bytes; it cannot prove an option reached
 * libssh, or that libssh did what the option name suggests.
 *
 * That gap was not theoretical. IdentityFile was wired through to
 * SSH_OPTIONS_ADD_IDENTITY, shipped, and given a widget on both front
 * doors before anything had checked that a named key actually
 * authenticates.
 *
 * NOT part of `make test`: it needs an sshd. tools/check-ssh.sh starts a
 * throwaway one on a spare port with a generated key and runs this
 * against it; `make check-ssh` is the entry point. The port and key path
 * arrive in BSSH_TEST_SSH_PORT and BSSH_TEST_SSH_KEY, and the tests skip
 * rather than fail when they are unset, so running the binary by hand
 * cannot report a false failure.
 */
class live_ssh_test : public QObject {
	Q_OBJECT

private slots:
	void a_named_key_authenticates();
	void the_same_key_works_through_a_tilde();
	void without_the_key_it_does_not_get_in();
	void a_missing_key_fails_rather_than_hangs();

	/*
	 * sec 11's trust-on-first-use, against a server whose host key can
	 * actually change. The failure this guards against is the one sec 11
	 * names: a warning that ends up silently pre-accepted. A fake can be
	 * told to emit host_key_changed; only a real server can prove the
	 * binding notices when it happens.
	 */
	void an_unknown_host_is_held_until_accepted();
	void accepting_once_is_remembered();
	void a_changed_host_key_is_refused();
	void a_host_openssh_already_trusts_is_not_asked_about();
	void accepting_writes_a_hashed_entry_openssh_can_use();

	/*
	 * sec 5.3's dead-peer detection, against a link whose packets really
	 * vanish. A closed socket is not a dropped link: a peer that goes
	 * away cleanly sends a FIN and is noticed at once, which is the case
	 * that was never in doubt. This needs the packets to disappear, so
	 * tools/check-keepalive.sh black-holes the port with iptables.
	 */
	void a_black_holed_link_is_noticed();

	/*
	 * sec 5.3's reconnection machine, driven by a link that really drops
	 * and then really comes back. The backoff schedule is a pure function
	 * and unit-tested as one; what a unit test cannot show is that the
	 * FSM engages on a genuine drop and gets a session back afterwards.
	 */
	void a_dropped_session_reconnects_when_the_link_returns();
};

#endif /* BSSH_LIVE_SSH_TEST_H */
