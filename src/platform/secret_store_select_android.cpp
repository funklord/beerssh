#include "platform/secret_store.h"

/* Defined in secret_store.cpp, beside the setter. */
bssh_secret_store *bssh_secret_store_override();
void bssh_secret_store_count_lookup();

/*
 * Which secret store Android uses (project.md sec 11).
 *
 * **The Keystore where the device will give us a key, the passphrase file
 * otherwise.** Chosen at RUNTIME rather than at build time, for the same
 * reason the desktop choice is: whether a working key store exists is a
 * property of the device and its state, not of the build. A phone whose
 * Keystore refuses to generate a key -- it happens, and it happens after
 * an OS update -- must still be able to save a password rather than
 * silently failing to.
 *
 * The Keystore is preferred wherever it works because the user types
 * nothing: the key is generated in the Keystore, cannot be extracted, and
 * on hardware that offers one lives in a TEE or a secure element. The
 * fallback's key is only as good as a passphrase somebody had to invent
 * and then type on a phone keyboard often enough to make short.
 *
 * Both name themselves honestly, which is what sec 11 asks for and what
 * "This device" now shows: neither says "encrypted" and leaves the user to
 * assume the stronger one.
 */
bssh_secret_store *bssh_secret_store::instance() {
	bssh_secret_store_count_lookup();
	if (bssh_secret_store_override()) return bssh_secret_store_override();

	static bssh_secret_store *store = []() -> bssh_secret_store * {
		if (bssh_secret_store *keystore = bssh_make_keystore_secret_store()) return keystore;
		return bssh_make_file_secret_store();
	}();
	return store;
}
