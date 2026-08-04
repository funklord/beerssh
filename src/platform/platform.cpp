#include "platform/platform.h"

#include <QGuiApplication>
#include <QNetworkInformation>

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

	/*
	 * Network changes, on every platform, from one place (sec 5.3).
	 *
	 * This used to be desktop-only, and the Android side had a comment
	 * promising ConnectivityManager callbacks and no code -- so the
	 * platform sec 5 exists FOR was the one that waited out the backoff
	 * and the TCP timeout on a Wi-Fi-to-mobile handover, while a laptop
	 * waking from suspend reconnected at once.
	 *
	 * No JNI and no second Java file were needed after all: Qt ships an
	 * Android backend for QNetworkInformation that is ConnectivityManager
	 * underneath, which is what sec 5.3 asked for by name. Having asked
	 * the same question twice and answered it once, it is now asked here
	 * rather than in either platform file.
	 *
	 * Best-effort: a system with no backend simply reconnects on the
	 * keepalive timeout instead, which is slower and not broken.
	 */
	if (QNetworkInformation::loadDefaultBackend()) {
		QNetworkInformation *info = QNetworkInformation::instance();
		if (info) {
			connect(info, &QNetworkInformation::reachabilityChanged,
			         this, [this](QNetworkInformation::Reachability) {
				emit network_changed();
			});
		}
	}
}

bssh_platform::~bssh_platform() {
}
