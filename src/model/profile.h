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

	bssh_profile_t() : id(0), port(22), has_stored_password(false), preset_id(0) {}

	bool is_valid() const { return !host.isEmpty(); }

	/* "user@host" or "user@host:port" when the port is not the default. */
	QString target() const {
		QString text = user.isEmpty() ? host : user + QLatin1Char('@') + host;
		if (port != 22) text += QLatin1Char(':') + QString::number(port);
		return text;
	}
};

#endif /* BSSH_PROFILE_H */
