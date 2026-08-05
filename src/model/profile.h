#ifndef BSSH_PROFILE_H
#define BSSH_PROFILE_H

#include <QJsonObject>
#include <QString>

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

	bssh_profile_t()
	    : id(0), port(22), has_stored_password(false), preset_id(0), local(false) {}

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
};

#endif /* BSSH_PROFILE_H */
