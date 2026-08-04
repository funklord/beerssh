#ifndef BSSH_SECRET_STORE_H
#define BSSH_SECRET_STORE_H

#include <QObject>
#include <QString>

/*
 * bssh_secret_store -- where credentials live (project.md sec 11).
 *
 * Callers get an opaque id back and never see a key mechanism: on Android
 * this becomes Keystore-wrapped blobs, on desktop the platform's secret
 * service where one is running, and a passphrase-derived encrypted file
 * where none is. The profile database holds ids only, and its schema has
 * nowhere to put a secret (sec 10.6) — that separation is the point.
 *
 * **The backend is not an implementation detail to hide.** A
 * hardware-backed keystore and a file encrypted with a passphrase the user
 * typed are not equivalent security, and averaging them into a reassuring
 * "encrypted" is how someone ends up trusting the weaker one. The UI is
 * expected to show backend_name() and is_hardware_backed().
 */
class bssh_secret_store : public QObject {
	Q_OBJECT

public:
	explicit bssh_secret_store(QObject *parent = nullptr);
	~bssh_secret_store() override;

	/*
	 * The store for this machine: the platform's secret service when one
	 * is actually running, the passphrase-derived file otherwise. Chosen
	 * at RUNTIME, not at build time, because "is a keyring running" is a
	 * property of the session rather than of the build -- the same binary
	 * meets both cases, and a headless login of a desktop install is the
	 * common one.
	 */
	static bssh_secret_store *instance();

	/* Shown to the user, verbatim. Never softened. */
	virtual QString backend_name() const = 0;
	virtual bool is_hardware_backed() const = 0;

	/*
	 * Whether the store can be read right now. A passphrase-derived store
	 * starts locked; a keyring-backed one is unlocked by the session.
	 */
	virtual bool is_unlocked() const = 0;
	virtual bool needs_passphrase() const = 0;

	/*
	 * Unlocks, or creates the store on first use. Wrong passphrase
	 * returns false rather than yielding garbage -- the AEAD tag makes
	 * that a detectable failure rather than a silent one.
	 */
	virtual bool unlock(const QString &passphrase) = 0;
	virtual void lock() = 0;

	virtual bool store(const QString &id, const QString &secret) = 0;

	/* Empty when absent, locked, or unreadable -- deliberately not
	 * distinguished at the call site, so no caller branches on it. */
	virtual QString load(const QString &id) const = 0;

	virtual bool erase(const QString &id) = 0;
};

/*
 * The backends, built directly. instance() picks between them; tests use
 * them by name, because a test that goes through instance() asserts
 * whatever happens to be running on the machine it runs on.
 */
bssh_secret_store *bssh_make_file_secret_store();
bssh_secret_store *bssh_make_libsecret_store();   /* nullptr when unavailable */
bssh_secret_store *bssh_make_keystore_secret_store();  /* Android; nullptr when unavailable */

#endif /* BSSH_SECRET_STORE_H */
