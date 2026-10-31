#include "ssh/working_directory.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QSysInfo>
#include <QUrl>

namespace {

QString read_link(const QString &path) {
	return QFileInfo(path).symLinkTarget();
}

QString read_line(const QString &path) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) return QString();
	return QString::fromUtf8(file.readAll()).trimmed();
}

} /* namespace */

QString bssh_working_directory_probe() {
	/*
	 * One line, in sh, inside single quotes, with no single quote, no
	 * newline and no ! in it: it is handed to whatever login shell the
	 * account has, and fish, csh and bash all pass such a string to sh
	 * untouched. Each step that cannot be answered -- a process of
	 * somebody else's, one already gone -- is skipped rather than
	 * stopping the loop.
	 *
	 * The shell is the session leader of a terminal whose environment
	 * holds our SSH_CONNECTION. Field 4 of /proc/PID/stat after the
	 * command name is the session, field 6 the terminal's foreground
	 * process group.
	 */
	static const char *const steps[] = {
		"printf \"host\\t%s\\n\" \"$(uname -n 2>/dev/null)\"",
		"[ -d /proc/self ] || exit 0",
		"printf \"proc\\t1\\n\"",
		"c=$SSH_CONNECTION",
		"[ -n \"$c\" ] || exit 0",
		"for d in /proc/[0-9]*; do "
		    "t=$(readlink \"$d/fd/0\" 2>/dev/null) || continue; "
		    "case \"$t\" in /dev/pts/*|/dev/tty*) ;; *) continue ;; esac; "
		    "tr \"\\000\" \"\\n\" < \"$d/environ\" 2>/dev/null | grep -qxF \"SSH_CONNECTION=$c\" || continue; "
		    "s=$(cat \"$d/stat\" 2>/dev/null) || continue; "
		    "set -- ${s##*) }; "
		    "[ \"${d#/proc/}\" = \"$4\" ] || continue; "
		    "printf \"shell\\t%s\\n\" \"$(readlink \"$d/cwd\" 2>/dev/null)\"; "
		    "f=/proc/$6; "
		    "printf \"fg\\t%s\\t%s\\n\" \"$(cat \"$f/comm\" 2>/dev/null)\" \"$(readlink \"$f/cwd\" 2>/dev/null)\"; "
		    "exit 0; "
		"done",
	};
	QStringList script;
	for (const char *step : steps) script << QString::fromLatin1(step);
	return QStringLiteral("sh -c '%1'").arg(script.join(QStringLiteral("; ")));
}

bssh_working_directory_t bssh_local_working_directory(qint64 shell) {
	bssh_working_directory_t found;
	found.host = QSysInfo::machineHostName();
	if (shell <= 0 || !QFileInfo::exists(QStringLiteral("/proc/self"))) return found;
	found.proc = true;

	/* As the remote script reads it: field 6 after the command name is
	 * the terminal's foreground process group. */
	const QString stat = read_line(QStringLiteral("/proc/%1/stat").arg(shell));
	const QStringList after = stat.mid(stat.lastIndexOf(QLatin1Char(')')) + 2)
	                                  .split(QLatin1Char(' '), Qt::SkipEmptyParts);
	const QString foreground = after.value(5);
	QByteArray output = "proc\t1\n";
	output += "shell\t" + read_link(QStringLiteral("/proc/%1/cwd").arg(shell)).toUtf8() + '\n';
	if (!foreground.isEmpty() && foreground != QLatin1String("-1") && foreground != QLatin1String("0")) {
		output += "fg\t" + read_line(QStringLiteral("/proc/%1/comm").arg(foreground)).toUtf8() + '\t'
		          + read_link(QStringLiteral("/proc/%1/cwd").arg(foreground)).toUtf8() + '\n';
	}
	bssh_working_directory_t parsed = bssh_parse_working_directory(output);
	parsed.host = found.host;
	return parsed;
}

bool bssh_copy_into(const QStringList &paths, const QString &directory, QStringList &written,
                    QString &why, const std::function<void(qint64, qint64)> &progress) {
	const QDir into(directory.isEmpty() ? QDir::homePath() : directory);
	struct item_t { QString from; QString to; bool directory; };
	QList<item_t> items;
	qint64 total = 0;
	for (const QString &path : paths) {
		const QFileInfo info(path);
		if (!info.exists()) continue;
		const QString name = bssh_free_name(info.fileName(), [&into](const QString &candidate) {
			return QFileInfo::exists(into.filePath(candidate)) || QFileInfo(into.filePath(candidate)).isSymLink();
		});
		if (name.isEmpty()) continue;
		written << name;
		const QString to = into.filePath(name);
		if (!info.isDir()) {
			items.append({ info.absoluteFilePath(), to, false });
			total += info.size();
			continue;
		}
		items.append({ info.absoluteFilePath(), to, true });
		QDirIterator inside(info.absoluteFilePath(), QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
		                    QDirIterator::Subdirectories);
		while (inside.hasNext()) {
			const QFileInfo each(inside.next());
			if (each.isSymLink() && each.isDir()) continue;
			const QString relative = QDir(info.absoluteFilePath()).relativeFilePath(each.absoluteFilePath());
			items.append({ each.absoluteFilePath(), to + QLatin1Char('/') + relative, each.isDir() });
			if (!each.isDir()) total += each.size();
		}
	}

	qint64 done = 0;
	progress(0, total);
	for (const item_t &item : items) {
		if (item.directory) {
			if (!QDir().mkdir(item.to)) {
				why = QStringLiteral("could not make %1").arg(item.to);
				return false;
			}
			continue;
		}
		QFile from(item.from);
		QFile to(item.to);
		if (!from.open(QIODevice::ReadOnly)) {
			why = QStringLiteral("could not read %1").arg(item.from);
			return false;
		}
		/* NewOnly: the free name found above is not overwritten either. */
		if (!to.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
			why = QStringLiteral("could not write %1").arg(item.to);
			return false;
		}
		while (!from.atEnd()) {
			const QByteArray piece = from.read(1024 * 1024);
			if (to.write(piece) != piece.size()) {
				why = QStringLiteral("writing %1 failed").arg(item.to);
				return false;
			}
			done += piece.size();
			progress(done, total);
		}
		to.setPermissions(from.permissions());
	}
	return true;
}

bool bssh_is_onward_program(const QString &name) {
	static const QStringList onward = {
		QStringLiteral("ssh"), QStringLiteral("slogin"), QStringLiteral("autossh"),
		QStringLiteral("mosh-client"), QStringLiteral("mosh"), QStringLiteral("telnet"),
		QStringLiteral("rlogin"), QStringLiteral("rsh"), QStringLiteral("et"),
	};
	return onward.contains(name.trimmed());
}

bssh_working_directory_t bssh_parse_working_directory(const QByteArray &output) {
	bssh_working_directory_t found;
	QString shell;
	QString foreground_path;
	for (const QByteArray &raw : output.split('\n')) {
		const QList<QByteArray> fields = raw.split('\t');
		const QByteArray key = fields.value(0);
		if (key == "host") found.host = QString::fromUtf8(fields.value(1));
		else if (key == "proc") found.proc = true;
		else if (key == "shell") shell = QString::fromUtf8(fields.value(1));
		else if (key == "fg") {
			found.foreground = QString::fromUtf8(fields.value(1));
			foreground_path = QString::fromUtf8(fields.value(2));
		}
	}
	found.elsewhere = bssh_is_onward_program(found.foreground);

	/*
	 * The foreground's directory where it can be read: a sub-shell the
	 * user cd'd in, or the editor they are in, is "here" more than the
	 * login shell behind it. The shell's when the foreground is not
	 * ours to read -- after sudo -i, say -- or there is none.
	 */
	const QString chosen = foreground_path.isEmpty() ? shell : foreground_path;
	if (chosen.startsWith(QLatin1Char('/'))) found.path = chosen;
	return found;
}

QString bssh_reported_directory(const QString &report, const QString &host) {
	const QUrl url(report);
	if (!url.isValid() || url.scheme() != QLatin1String("file")) return QString();
	const QString from = url.host();
	if (!from.isEmpty() && from != QLatin1String("localhost")
	    && from.compare(host, Qt::CaseInsensitive) != 0)
		return QString();
	const QString path = url.path();
	return path.startsWith(QLatin1Char('/')) ? path : QString();
}

QString bssh_free_name(const QString &name, const std::function<bool(const QString &)> &taken) {
	if (!taken(name)) return name;
	const int dot = name.lastIndexOf(QLatin1Char('.'));
	const bool has_extension = dot > 0;
	const QString stem = has_extension ? name.left(dot) : name;
	const QString extension = has_extension ? name.mid(dot) : QString();
	for (int n = 1; n < 10000; ++n) {
		const QString candidate = QStringLiteral("%1 (%2)%3").arg(stem).arg(n).arg(extension);
		if (!taken(candidate)) return candidate;
	}
	return QString();
}
