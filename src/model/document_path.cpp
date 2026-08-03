#include "model/document_path.h"

#include <QUrl>

QString bssh_normalise_document_path(const QString &path) {
	if (!path.startsWith(QStringLiteral("file://"))) return path;
	return QUrl(path).toLocalFile();
}

bool bssh_is_local_document_path(const QString &path) {
	return !path.startsWith(QStringLiteral("content://"));
}
