#include "sshd/sshd_platform.h"

#include <QDir>
#include <QNetworkInterface>

namespace {

/*
 * The desktop's answers. The window runs here for testing and as a
 * convenience; the command line is the desktop's real front end, and
 * it takes its root and address as arguments instead.
 */
class desktop_platform : public bssh_sshd_platform {
public:
	QString storage_root() const override { return QDir::homePath(); }
	bool has_storage_access() const override { return true; }
	void request_storage_access() override {}

	/* The first address another machine could reach, or loopback when there is none. */
	QHostAddress listen_address() const override {
		for (const QNetworkInterface &interface : QNetworkInterface::allInterfaces()) {
			const auto flags = interface.flags();
			if (!(flags & QNetworkInterface::IsUp) || !(flags & QNetworkInterface::IsRunning)) continue;
			if (flags & QNetworkInterface::IsLoopBack) continue;
			for (const QNetworkAddressEntry &entry : interface.addressEntries()) {
				if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) return entry.ip();
			}
		}
		return QHostAddress(QHostAddress::LocalHost);
	}

	void set_serving(bool, const QString &) override {}
};

} /* namespace */

bssh_sshd_platform *bssh_sshd_platform::instance() {
	static desktop_platform platform;
	return &platform;
}
