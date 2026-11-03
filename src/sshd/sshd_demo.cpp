#include "sshd/sshd_demo.h"

#include <QFile>

namespace {

/* Keys nobody holds: the public halves of two made for this demo, whose private halves were deleted. */
const char *const DEMO_KEYS =
        "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIEiII+xcBaB4UuGLMfstYxUoAOMyjbigKcv25MA+ZIjT backup@nas\n"
        "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIGagHMeAMa4o63tHkCnoMtr2P8qFqK5DfCOGL50eaiLC alex@laptop\n";

const char *const DEMO_ADDRESS = "192.168.1.23";

} /* namespace */

bssh_sshd_demo_platform::bssh_sshd_demo_platform() {
	QFile keys(m_dir.path() + QStringLiteral("/authorized_keys"));
	if (keys.open(QIODevice::WriteOnly)) keys.write(DEMO_KEYS);
}

QHostAddress bssh_sshd_demo_platform::listen_address() const {
	return QHostAddress(QString::fromLatin1(DEMO_ADDRESS));
}

QString bssh_sshd_demo_control::status_line() const {
	if (!serving) return tr("Stopped. Needs All files access to serve storage.");
	return tr("Serving /storage/emulated/0 at %1, port 8022 -- 1 connection(s)")
	        .arg(QString::fromLatin1(DEMO_ADDRESS));
}

QString bssh_sshd_demo_control::fingerprint() const {
	if (!serving) return QString();
	return QStringLiteral("SHA256:q3Vb7nTmY2kR9xLw4HcP8sJ1fZ6aDgE0uNoC5iWtXrM");
}

void bssh_sshd_demo_control::tell() {
	/* Before noon, to sit with a demo clock (the play-store skill). */
	emit logged(QTime(9, 41, 2), tr("Listening on %1:8022").arg(QString::fromLatin1(DEMO_ADDRESS)));
	emit logged(QTime(9, 58, 17), QStringLiteral("192.168.1.10: backup started SFTP"));
	emit logged(QTime(10, 4, 51), QStringLiteral("192.168.1.10: disconnected"));
	emit logged(QTime(11, 30, 6), QStringLiteral("192.168.1.15: alex started SFTP"));
}
