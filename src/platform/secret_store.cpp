#include "platform/secret_store.h"

bssh_secret_store::bssh_secret_store(QObject *parent) : QObject(parent) {
}

bssh_secret_store::~bssh_secret_store() {
}

/*
 * The test override, and where instance() reads it from.
 *
 * Here rather than in either selector so both platforms share one
 * definition: the Android selector consults the same function, and a
 * second copy of a one-line accessor is a second thing to be wrong.
 */
namespace {
bssh_secret_store *g_override = nullptr;
}

bssh_secret_store *bssh_secret_store_override() {
	return g_override;
}

void bssh_secret_store::set_instance(bssh_secret_store *store) {
	g_override = store;
}
