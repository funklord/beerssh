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
 * What a writer's exit code means, as run_store_writer in test/main.cpp
 * returns them.
 */
inline QString bssh_store_writer_failure(int code) {
	switch (code) {
	case 2: return QStringLiteral("a malformed spec");
	case 3: return QStringLiteral("the host list would not open");
	case 4: return QStringLiteral("a save was refused");
	case 5: return QStringLiteral("no go within 30 s");
	case 6: return QStringLiteral("the store would not unlock");
	default: return QStringLiteral("exit code %1").arg(code);
	}
}

/*
 * Three real processes writing one store at once; see run_store_writer in
 * test/main.cpp, which is what they run. Empty when every one finished
 * cleanly, and the reader then asks the store what survived; otherwise
 * which writer failed and how, with what it printed. A bare false once
 * cost a Windows CI failure its reason (2026-11-17, project.md sec 8.4).
 *
 * `said`, given, gets what every writer printed, the ones that succeeded
 * too: on 2026-11-24 all three succeeded and a secret was still lost, at
 * load 30, and the warning that would say why -- a lock waited for past
 * its patience, the change going ahead anyway -- is printed by a writer
 * that then exits cleanly. The caller shows it when what it reads back is
 * short.
 */
inline QString bssh_run_store_writers(const QString &kind, const QString &dir, int each,
                                      QString *said_all = nullptr) {
	const QStringList prefixes = { QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c") };

	std::vector<std::unique_ptr<QProcess>> writers;
	for (const QString &prefix : prefixes) {
		std::unique_ptr<QProcess> writer(new QProcess);
		QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
		environment.insert(QStringLiteral("BSSH_TEST_STORE_WRITER"),
		                   QStringLiteral("%1|%2|%3|%4").arg(kind, dir, prefix).arg(each));
		environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
		writer->setProcessEnvironment(environment);
		writer->setProcessChannelMode(QProcess::MergedChannels);
		writer->start(QCoreApplication::applicationFilePath(), QStringList());
		writers.push_back(std::move(writer));
	}

	/* All of them at the line before any starts, or they take turns. */
	for (int i = 0; i < int(prefixes.size()); ++i) {
		const QString ready = dir + QStringLiteral("/ready-") + prefixes.at(i);
		for (int waited = 0; !QFile::exists(ready); waited += 10) {
			if (waited > 30000) {
				writers.at(size_t(i))->kill();
				writers.at(size_t(i))->waitForFinished(5000);
				return QStringLiteral("writer %1 was never ready (%2): %3")
				        .arg(prefixes.at(i), writers.at(size_t(i))->errorString(),
				             QString::fromLocal8Bit(writers.at(size_t(i))->readAll()));
			}
			QTest::qWait(10);
		}
	}
	if (!QFile(dir + QStringLiteral("/go")).open(QIODevice::WriteOnly))
		return QStringLiteral("the go file could not be made");

	QStringList failures;
	for (int i = 0; i < int(prefixes.size()); ++i) {
		QProcess &writer = *writers.at(size_t(i));
		const bool finished = writer.waitForFinished(60000);
		const QString said = QString::fromLocal8Bit(writer.readAll()).trimmed();
		if (said_all && !said.isEmpty())
			*said_all += QStringLiteral("[writer %1] %2\n").arg(prefixes.at(i), said.right(1200));
		if (!finished)
			failures << QStringLiteral("writer %1 still running after 60 s").arg(prefixes.at(i));
		else if (writer.exitStatus() != QProcess::NormalExit)
			failures << QStringLiteral("writer %1 crashed").arg(prefixes.at(i));
		else if (writer.exitCode() != 0)
			failures << QStringLiteral("writer %1: %2").arg(prefixes.at(i), bssh_store_writer_failure(writer.exitCode()));
		else
			continue;
		if (!said.isEmpty()) failures.last() += QStringLiteral(" -- it said: ") + said.right(600);
	}
	return failures.join(QStringLiteral("; "));
}

#endif
