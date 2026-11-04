#ifndef BSSH_STORE_WRITERS_H
#define BSSH_STORE_WRITERS_H

#include <QCoreApplication>
#include <QFile>
#include <QList>
#include <QProcess>
#include <QString>
#include <QTest>

#include <memory>
#include <vector>

/*
 * Three real processes writing one store at once; see run_store_writer in
 * test/main.cpp, which is what they run. False when one could not start or
 * failed, and the reader then asks the store what survived.
 */
inline bool bssh_run_store_writers(const QString &kind, const QString &dir, int each) {
	const QStringList prefixes = { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") };

	std::vector<std::unique_ptr<QProcess>> writers;
	for (const QString &prefix : prefixes) {
		std::unique_ptr<QProcess> writer(new QProcess);
		QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
		environment.insert(QStringLiteral("BSSH_TEST_STORE_WRITER"),
		                   QStringLiteral("%1|%2|%3|%4").arg(kind, dir, prefix).arg(each));
		environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
		writer->setProcessEnvironment(environment);
		writer->setProcessChannelMode(QProcess::ForwardedChannels);
		writer->start(QCoreApplication::applicationFilePath(), QStringList());
		writers.push_back(std::move(writer));
	}

	/* All of them at the line before any starts, or they take turns. */
	for (const QString &prefix : prefixes) {
		const QString ready = dir + QStringLiteral("/ready-") + prefix;
		for (int waited = 0; !QFile::exists(ready); waited += 10) {
			if (waited > 30000) return false;
			QTest::qWait(10);
		}
	}
	if (!QFile(dir + QStringLiteral("/go")).open(QIODevice::WriteOnly)) return false;

	bool passed = true;
	for (const std::unique_ptr<QProcess> &writer : writers) {
		if (!writer->waitForFinished(60000) || writer->exitCode() != 0) passed = false;
	}
	return passed;
}

#endif
