#include "platform/platform.h"

#include <QGuiApplication>

/*
 * Only the parts of bssh_platform that are the same everywhere live here.
 * instance() is deliberately NOT one of them -- each platform file defines
 * it, returning its own concrete type, which is what keeps the selection a
 * build-time question rather than a runtime branch.
 */

bssh_platform::bssh_platform(QObject *parent) : QObject(parent) {
	/*
	 * Captured now, before anything has applied a palette of its own;
	 * see startup_palette().
	 */
	m_startup_palette = QGuiApplication::palette();

}

bssh_platform::~bssh_platform() {
}
