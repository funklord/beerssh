#ifndef BSSH_PROFILE_H
#define BSSH_PROFILE_H

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

/*
 * bssh_profile_t -- one host you connect to (project.md sec 10).
 *
 * Deliberately the non-secret half only. Passwords, private keys and key
 * references live in the secret store (sec 11) behind an id, and never in
 * here: this struct is the thing that gets listed, edited, exported and
 * shown on screen, and the easiest way to leak a credential is to put it
 * somewhere that gets rendered.
 *
 * The preset a profile references (sec 10.1) is not here yet -- presets
 * arrive at M10. The field order is already the one sec 10.5 wants for an
 * OpenSSH config import, so that mapping stays cheap.
 */
struct bssh_profile_t {
	/* Row id in the store; 0 means "not saved yet". */
	int id;

	/* What the user called it. PuTTY's "saved session" name. */
	QString name;

	QString host;
	QString user;
	quint16 port;

	/*
	 * A specific private key to offer, or empty for the default
	 * identities (sec 10.5). A PATH, never key material: the file stays
	 * where it is, is read only by libssh at authentication time, and is
	 * no more a secret than the name of the host it opens -- which is
	 * what lets it live in this struct at all under sec 10.3.
	 *
	 * This is what an imported OpenSSH `IdentityFile` becomes. Without
	 * it, a host whose key is not one of the four default names falls
	 * through to a password prompt it may have no way to answer.
	 */
	QString identity_file;

	/*
	 * The bastion to reach this host through, as OpenSSH's ProxyJump
	 * writes it: [user@]host[:port], empty for a direct connection
	 * (sec 10.5). One hop.
	 *
	 * A hostname, like the host field beside it, and no more a secret
	 * than that one -- which is what lets it live here under sec 10.3.
	 * This is what an imported OpenSSH `ProxyJump` becomes.
	 */
	QString proxy_jump;

	/*
	 * Whether a password for this host exists in the secret store. NOT a
	 * secret -- it is the hint that lets the app know it is worth asking
	 * for the store's passphrase, without unlocking the store to find
	 * out. Storing this in the profile row keeps sec 10.3's rule intact:
	 * the database still has nowhere to put a credential.
	 */
	bool has_stored_password;

	/*
	 * Whether releasing this host's remembered password needs the user to
	 * prove it is them first -- BiometricPrompt on Android (sec 11).
	 *
	 * PER HOST, and off by default, because the question sec 11 blocked
	 * this on has no single answer: a reconnect at 3am with nobody
	 * holding the phone is right for the box running tmux and wrong for
	 * the production database, and they are not the same host. Off by
	 * default keeps every existing profile behaving exactly as it did.
	 *
	 * Not a secret, like the flag above, so it lives in the profile row
	 * without giving the database anywhere to put a credential (sec 10.3).
	 * It only ever gates a password the store already holds: a
	 * key-authenticated session never reads the store at all, so this
	 * cannot make one fail to reconnect.
	 */
	bool require_biometric;

	/*
	 * The preset this host follows (sec 10.1), and the fields where it
	 * disagrees. Overrides are sparse on purpose: a field absent here
	 * takes the preset's value, which is what makes editing a preset
	 * reach every host that has not spoken for that field.
	 */
	int preset_id;
	QJsonObject overrides;

	/*
	 * A local shell rather than an SSH connection (sec 2, sec 3). The
	 * desktop's default session is one of these; host, user, port and
	 * everything under sec 11 are meaningless for it.
	 */
	bool local;

	/* What the local shell runs. Empty means $SHELL, then /bin/sh. */
	QString shell;

	/*
	 * What to run instead of a shell, argv-style (sec 8.6's -e). Empty is
	 * the ordinary case: the login shell locally, the remote's own shell
	 * over SSH.
	 *
	 * Beside `shell` because it answers the same question -- what does
	 * this session run -- and because putting it here is what lets it
	 * reach the transport without a second channel from main() down
	 * through the window, the registry and the session.
	 */
	QStringList command;

	/* Where a local shell starts (sec 8.6's --workdir); empty inherits. */
	QString working_directory;

	/*
	 * Keep the pane when what it was running ends, rather than closing it
	 * (sec 8.6's --hold).
	 *
	 * A launch-time policy rather than a property of the host, and here
	 * anyway: the window learns a session has ended from a signal that
	 * carries the session, and a session carries its profile. The
	 * alternative was a fourth thing for main() to thread through the
	 * registry. Never written to the store -- profile_store saves the
	 * fields it names, and this is not one of them.
	 */
	bool hold;

	bssh_profile_t()
	    : id(0), port(22), has_stored_password(false), require_biometric(false),
	      preset_id(0), local(false),
	      hold(false) {}

	/*
	 * A local profile is valid without a host, which is the whole point
	 * of it -- there is nothing to connect to.
	 */
	bool is_valid() const { return local || !host.isEmpty(); }

	/*
	 * The shell on this machine (sec 2). One place, because two callers
	 * want it -- what the desktop opens with, and the row at the top of
	 * the host list -- and a second spelling of "what a local session is"
	 * would be a second thing to keep in step.
	 *
	 * The name becomes the tab label and the window title until the shell
	 * sets its own through OSC (sec 8.3), so it is not left empty.
	 */
	static bssh_profile_t local_shell() {
		bssh_profile_t profile;
		profile.local = true;
		profile.name = QStringLiteral("Local shell");
		return profile;
	}

	/* "user@host" or "user@host:port" when the port is not the default. */
	QString target() const {
		QString text = user.isEmpty() ? host : user + QLatin1Char('@') + host;
		if (port != 22) text += QLatin1Char(':') + QString::number(port);
		return text;
	}

	/*
	 * target()'s inverse: the [user@]host[:port] somebody typed on the
	 * command line (sec 8.6), as a profile with nothing else set.
	 *
	 * Here rather than in the window that used to hold it, next to the
	 * function it undoes, because two things parse one now -- the window
	 * and main() -- and a destination that round-trips differently
	 * depending on which of them read it is a bug waiting for a port
	 * number.
	 *
	 * The user falls back to this account's, which is what ssh does with
	 * a bare hostname.
	 */
	static bssh_profile_t from_target(const QString &target) {
		bssh_profile_t profile;
		QString rest = target;

		const int at = rest.lastIndexOf(QLatin1Char('@'));
		if (at >= 0) {
			profile.user = rest.left(at);
			rest = rest.mid(at + 1);
		}
		const int colon = rest.lastIndexOf(QLatin1Char(':'));
		if (colon >= 0) {
			profile.port = static_cast<quint16>(rest.mid(colon + 1).toUShort());
			rest = rest.left(colon);
		}
		profile.host = rest;
		if (profile.user.isEmpty()) profile.user = qEnvironmentVariable("USER");

		return profile;
	}
};

#endif /* BSSH_PROFILE_H */
