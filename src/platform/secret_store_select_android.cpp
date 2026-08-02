#include "platform/secret_store.h"

/*
 * Which secret store Android uses (project.md sec 11).
 *
 * **The passphrase file, for now, and that is a known shortfall rather
 * than the design.** sec 11 wants Keystore-wrapped blobs, hardware-backed
 * where the device offers it, with optional biometric release -- all of
 * which is JNI work that has to be written against a device, like the
 * foreground service it would sit beside.
 *
 * The file store is portable (Qt and OpenSSL, nothing platform-specific)
 * and it is honest: backend_name() says it is a passphrase-encrypted file,
 * so a user is not told their credentials are in the Keystore when they
 * are not.
 */
bssh_secret_store *bssh_secret_store::instance() {
	static bssh_secret_store *store = bssh_make_file_secret_store();
	return store;
}
