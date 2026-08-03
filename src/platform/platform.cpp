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
	 *
	 * Only when there IS a GUI application. QGuiApplication::palette()
	 * builds the platform theme on first call, which dereferences an
	 * event dispatcher a QCoreApplication does not have -- so this
	 * segfaults rather than returning a default. Anything console-shaped
	 * that reaches the platform layer, and every non-GUI test, hits it.
	 */
	if (qobject_cast<QGuiApplication *>(QCoreApplication::instance())) {
		m_startup_palette = QGuiApplication::palette();
	}

}

bssh_platform::~bssh_platform() {
}
