#ifndef BSSH_LIVE_SSH_TEST_H
#define BSSH_LIVE_SSH_TEST_H

#include <QObject>

/*
 * The SSH transport against a real server (project.md sec 12.3).
 *
 * Everything else in test/ runs against bssh_fake_ssh_session, which is
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
 * NOT part of `make test`: it needs an sshd. tool/check-ssh.sh starts a
 * throwaway one on a spare port with a generated key and runs this
 * against it; `make check-ssh` is the entry point. The port and key path
 * arrive in BSSH_TEST_SSH_PORT and BSSH_TEST_SSH_KEY, and the tests skip
 * rather than fail when they are unset, so running the binary by hand
 * cannot report a false failure.
 */
class live_ssh_test : public QObject {
	Q_OBJECT

private slots:
	void a_live_ssh_session_moves_to_another_session();
	void a_session_through_a_jump_host_moves_too();
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
	void a_revoked_key_is_refused_not_offered();

	/*
	 * sec 5.3's dead-peer detection, against a link whose packets really
	 * vanish. A closed socket is not a dropped link: a peer that goes
	 * away cleanly sends a FIN and is noticed at once, which is the case
	 * that was never in doubt. This needs the packets to disappear, so
	 * tool/check-keepalive.sh black-holes the port with iptables.
	 */
	void a_black_holed_link_is_noticed();

	/*
	 * sec 5.3's reconnection machine, driven by a link that really drops
	 * and then really comes back. The backoff schedule is a pure function
	 * and unit-tested as one; what a unit test cannot show is that the
	 * FSM engages on a genuine drop and gets a session back afterwards.
	 */
	void a_dropped_session_reconnects_when_the_link_returns();

	/*
	 * sec 10.5's ProxyJump. The thing worth proving is not that the
	 * option is stored -- a unit test does that -- but that an SSH
	 * session really runs end-to-end inside a direct-tcpip channel that
	 * another SSH session is carrying, with the target's key checked and
	 * the target's shell answering. Nothing but a real server can show
	 * that, and the failure mode if the pump is wrong is a handshake that
	 * hangs rather than one that errors.
	 */
	/*
	 * The size the remote is told. Every full-screen program there --
	 * vim, less, top -- lays itself out from it, so a wrong one is not a
	 * cosmetic bug: it is a screen drawn to the wrong shape. Only a real
	 * pty can answer, which is why this is here and not in the emulator
	 * tests, and it is asked of the SHELL rather than of anything this
	 * app kept a copy of.
	 */
	/*
	 * sec 8.6's -e over SSH. Worth a real server rather than a fake for
	 * the reason IdentityFile was: a fake proves the words were stored,
	 * and only a server can show that the exec request was made instead
	 * of a shell request, that the PTY was still asked for, and that the
	 * far end's own shell did the splitting the protocol leaves to it.
	 */
	void a_remote_command_runs_instead_of_a_shell();

	void the_remote_is_told_the_size_it_was_given();
	void a_resize_reaches_the_remote();
	void a_send_to_a_remote_that_is_not_reading_still_lets_go();

	void a_host_is_reachable_through_a_jump_host();
	void an_unknown_jump_host_is_refused_rather_than_prompted_about();

	/*
	 * A tunnel costs a socket pair, and libssh does NOT close a
	 * descriptor handed to it with SSH_OPTIONS_FD -- measured, not
	 * assumed. Believing otherwise leaked one per attempt, and a host
	 * behind a bastion on a flaky link retries for as long as it takes.
	 */
	void a_tunnel_gives_back_every_descriptor_it_took();

	/*
	 * When the BASTION goes away, the session on the far side of it has
	 * to notice. The pump's three bastion-side exits used to leave the
	 * socket pair open, so the target session held a descriptor that was
	 * open and permanently silent: never readable, never an error, never
	 * a drop. The UI said connected, the terminal was frozen, and the
	 * reconnect machine (sec 5.3) was never told.
	 */
	void a_dead_jump_host_is_noticed_by_the_session();

	/*
	 * And the harder half: a link that goes SILENT rather than dying.
	 * A bastion that is killed sends a FIN and libssh notices; a bastion
	 * behind a phone that lost signal sends nothing at all, and the
	 * kernel only notices because of sec 5.3's socket options -- which
	 * the tunnel's own socket did not have.
	 */
	void a_black_holed_jump_host_is_noticed();
};

#endif /* BSSH_LIVE_SSH_TEST_H */
