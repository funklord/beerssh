#include "platform/secret_store.h"

bssh_secret_store::bssh_secret_store(QObject *parent) : QObject(parent) {
}

bssh_secret_store::~bssh_secret_store() {
}

bssh_secret_store *bssh_secret_store::instance() {
	/*
	 * Runtime selection, resolved once (sec 11). The keyring is preferred
	 * because the login session has already unlocked it -- the user types
	 * nothing and the secret is protected by whatever protects the rest
	 * of their session. The file store is the answer when no service
	 * answers, which is a real and common case rather than an edge one.
	 *
	 * Asking once and remembering matters: a store that re-probed on
	 * every access could answer from the keyring before the session bus
	 * appeared and from the file afterwards, and secrets written to one
	 * would be invisible in the other.
	 */
	static bssh_secret_store *selected = nullptr;
	static bool resolved = false;

	if (!resolved) {
		selected = bssh_make_libsecret_store();
		if (!selected) selected = bssh_make_file_secret_store();
		resolved = true;
	}

	return selected;
}
