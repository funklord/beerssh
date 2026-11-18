#include "sshd/fuzznet_pair.h"

#include <QDir>
#include <QProcess>
#include <QTemporaryDir>

namespace {

/* One of fuzznet's steps: its last line of output, or empty with `why` set. */
QString step(const QString &program, const QStringList &arguments, QString *why) {
	QProcess process;
	process.start(program, QStringList{ QStringLiteral("--fuzznet") } + arguments);
	if (!process.waitForFinished(30000)) {
		process.kill();
		process.waitForFinished(2000);
		*why = QStringLiteral("%1 did not finish").arg(arguments.value(1));
		return QString();
	}
	if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
		const QString said = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
		*why = said.isEmpty() ? QStringLiteral("%1 failed").arg(arguments.value(1)) : said;
		return QString();
	}
	const QStringList lines = QString::fromLatin1(process.readAllStandardOutput())
	                                  .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
	if (lines.isEmpty()) *why = QStringLiteral("%1 said nothing").arg(arguments.value(1));
	return lines.isEmpty() ? QString() : lines.last().trimmed();
}

} /* namespace */

QString bssh_fuzznet_pair(const QString &program, const QStringList &options, QString *why) {
	QString host_dir;
	QStringList rest;
	for (const QString &option : options) {
		if (option.startsWith(QLatin1String("--fuzznet-dir="))) host_dir = option.mid(14);
		else rest << option;
	}
	/* None named: fuzznet's own default store, where a host started without one keeps it. */
	if (host_dir.isEmpty()) {
		const QString data = qEnvironmentVariable("XDG_DATA_HOME");
		const QString home = qEnvironmentVariable("HOME");
		if (!data.isEmpty()) host_dir = data + QStringLiteral("/fuzznet");
		else if (!home.isEmpty()) host_dir = home + QStringLiteral("/.local/share/fuzznet");
	}
	if (host_dir.isEmpty()) {
		*why = QStringLiteral("--fuzznet-pair needs --fuzznet-dir=DIR, this host's fuzznet store");
		return QString();
	}
	/*
	 * Not made here: an estate root is a choice about who this host trusts,
	 * and a mistyped directory would quietly become a new one.
	 */
	if (!QDir(host_dir).exists() || QDir(host_dir).isEmpty()) {
		*why = QStringLiteral("no fuzznet host in %1: make one with beersshd --fuzznet "
		                      "--fuzznet-dir=%1 --new-root")
		               .arg(host_dir);
		return QString();
	}

	/* The device's own store, for the length of the pairing: made first, as
	 * fuzznet keeps nothing in a directory that is not there. */
	QTemporaryDir device;
	if (!device.isValid()) {
		*why = QStringLiteral("no room for the device's store: %1").arg(device.errorString());
		return QString();
	}
	const QString at_device = QStringLiteral("--fuzznet-dir=") + device.path();
	const QString at_host = QStringLiteral("--fuzznet-dir=") + host_dir;

	const QString prekey = step(program, QStringList{ at_device, QStringLiteral("--prekey") } + rest, why);
	if (prekey.isEmpty()) return QString();
	const QString card = step(program, QStringList{ at_host, QStringLiteral("--pair=") + prekey } + rest, why);
	if (card.isEmpty()) return QString();
	const QString gateway = step(program, QStringList{ at_device, QStringLiteral("--accept=") + card } + rest, why);
	if (gateway.isEmpty()) return QString();
	const QString image =
	        step(program, QStringList{ at_device, QStringLiteral("--device-image=") + gateway } + rest, why);
	if (!image.startsWith(QLatin1String("FZD1:"))) {
		if (why->isEmpty()) *why = QStringLiteral("fuzznet gave no device image");
		return QString();
	}
	return image;
}
