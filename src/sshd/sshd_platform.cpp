#include "sshd/sshd_platform.h"

#include <QStandardPaths>

QString bssh_sshd_platform::data_directory() const {
	return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}
