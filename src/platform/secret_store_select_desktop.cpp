#include "platform/secret_store.h"

/* Defined in secret_store.cpp, beside the setter. */
bssh_secret_store *bssh_secret_store_override();
void bssh_secret_store_count_lookup();

/*
 * Which secret store the desktop uses (project.md sec 11).
 *
 * A file per platform rather than a conditional inside one: the selection
 * differs by platform, so it lives where every other platform difference
 * does (sec 3, core principle 3).
 */
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
	bssh_secret_store_count_lookup();
	if (bssh_secret_store_override()) return bssh_secret_store_override();

	static bssh_secret_store *selected = nullptr;
	static bool resolved = false;

	if (!resolved) {
		selected = bssh_make_libsecret_store();
		if (!selected) selected = bssh_make_file_secret_store();
		resolved = true;
	}

	return selected;
}
