#include "media/player_choice.h"

#include <QDesktopServices>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include "media/media_player.h"
#include "model/app_settings.h"

namespace {

struct known_player_t {
	const char *id;
	const char *label;
};

/* hydra's list, in hydra's order. */
const known_player_t known_players[] = {
	{ "mpv", "mpv" },
	{ "vlc", "VLC" },
	{ "smplayer", "SMPlayer" },
	{ "ffplay", "ffplay" },
	{ "mplayer2", "mplayer2" },
	{ "mplayer", "mplayer" },
};

/* hydra's terminals, each with the option after which it takes a program
 * and its arguments as separate words. */
struct known_terminal_t {
	const char *program;
	const char *option;
};
const known_terminal_t known_terminals[] = {
	{ "x-terminal-emulator", "-e" },
	{ "xterm", "-e" },
	{ "konsole", "-e" },
	{ "gnome-terminal", "--" },
	{ "xfce4-terminal", "-x" },
	{ "mate-terminal", "-x" },
	{ "terminator", "-x" },
	{ "qterminal", "-e" },
	{ "alacritty", "-e" },
	{ "urxvt", "-e" },
	{ "kitty", "" },
	{ "foot", "" },
};

QStringList words(const QString &line) {
	return line.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
}

/* A program by name on PATH, or an absolute path that is executable. */
QString find_program(const QString &name) {
	if (name.isEmpty()) return QString();
	const QString found = QStandardPaths::findExecutable(name);
	if (!found.isEmpty()) return found;
	return QFileInfo(name).isExecutable() ? name : QString();
}

} /* namespace */

QList<bssh_player_entry_t> bssh_players(const QString &custom_command) {
	QList<bssh_player_entry_t> out;
	if (bssh_in_app_player()) out.append({ QStringLiteral("builtin"), QStringLiteral("BeerSSH's player"), QString(), true });
	out.append({ QStringLiteral("system"), QStringLiteral("The system's player"), QString(), true });
#ifndef Q_OS_ANDROID
	for (const known_player_t &known : known_players) {
		bssh_player_entry_t entry;
		entry.id = QString::fromLatin1(known.id);
		entry.label = QString::fromLatin1(known.label);
		entry.path = QStandardPaths::findExecutable(entry.id);
		entry.installed = !entry.path.isEmpty();
		out.append(entry);
	}
	const QStringList parts = words(custom_command);
	bssh_player_entry_t custom;
	custom.id = QStringLiteral("custom");
	custom.label = parts.isEmpty() ? QStringLiteral("Custom…")
	                               : QStringLiteral("Custom… (%1)").arg(parts.first());
	custom.path = find_program(parts.value(0));
	custom.installed = !custom.path.isEmpty();
	out.append(custom);
#else
	Q_UNUSED(custom_command);
#endif
	return out;
}

QString bssh_player_default() {
	return bssh_in_app_player() ? QStringLiteral("builtin") : QStringLiteral("system");
}

QString bssh_player_in_use() {
	const QString chosen = bssh_app_settings().player();
	if (chosen.isEmpty()) return bssh_player_default();
	/* builtin chosen in a build without it: the default, not nothing. */
	if (chosen == QLatin1String("builtin") && !bssh_in_app_player()) return bssh_player_default();
	return chosen;
}

QStringList bssh_player_arguments(const QString &id, const QString &custom, const QString &target) {
	if (id == QLatin1String("custom")) {
		QStringList parts = words(custom);
		if (!parts.isEmpty()) parts.removeFirst();
		const QString url = QUrl::fromLocalFile(target).toString();
		bool substituted = false;
		for (QString &part : parts) {
			if (!part.contains(QLatin1String("%U"))) continue;
			part.replace(QLatin1String("%U"), url);
			substituted = true;
		}
		/* No placeholder: appended, as somebody typing a program name means. */
		if (!substituted) parts << url;
		return parts;
	}
	if (id == QLatin1String("mpv")) return { QStringLiteral("--force-window=yes"), target };
	if (id == QLatin1String("ffplay")) return { QStringLiteral("-autoexit"), target };
	if (id == QLatin1String("mplayer") || id == QLatin1String("mplayer2"))
		return { QStringLiteral("-cache"), QStringLiteral("8192"), target };
	return { target };
}

bool bssh_player_wants_terminal(const QString &id) {
	if (id == QLatin1String("mplayer") || id == QLatin1String("mplayer2")) return true;
	return id == QLatin1String("custom") && bssh_app_settings().player_custom_in_terminal();
}

QStringList bssh_player_terminal(const QString &configured) {
	QStringList parts = words(configured);
	/* One configured but not there falls back to another rather than to
	 * none: "a terminal, this one" is nearer to a different terminal. */
	if (!parts.isEmpty()) {
		const QString path = find_program(parts.first());
		if (!path.isEmpty()) {
			parts.first() = path;
			return parts;
		}
	}
	for (const known_terminal_t &known : known_terminals) {
		const QString path = QStandardPaths::findExecutable(QString::fromLatin1(known.program));
		if (path.isEmpty()) continue;
		QStringList out{ path };
		if (*known.option) out << QString::fromLatin1(known.option);
		return out;
	}
	return {};
}

QStringList bssh_player_command(const QString &id, const QString &path, QString *error) {
	const bssh_app_settings settings;
	const QString custom = settings.player_custom_command();
	QString program;
	if (id == QLatin1String("custom")) {
		program = find_program(words(custom).value(0));
		if (program.isEmpty()) {
			if (error) {
				*error = custom.isEmpty() ? QStringLiteral("No custom player command is set")
				                          : QStringLiteral("%1 is not installed").arg(words(custom).first());
			}
			return {};
		}
	} else {
		program = QStandardPaths::findExecutable(id);
		if (program.isEmpty()) {
			if (error) *error = QStringLiteral("%1 is not installed").arg(id);
			return {};
		}
	}
	QStringList line;
	if (bssh_player_wants_terminal(id)) {
		line = bssh_player_terminal(settings.player_terminal());
		if (line.isEmpty()) {
			if (error) *error = QStringLiteral("%1 needs a terminal, and none is installed").arg(id);
			return {};
		}
	}
	line << program << bssh_player_arguments(id, custom, path);
	return line;
}

bool bssh_play_externally(const QString &path, QString *error) {
	const QString id = bssh_player_in_use();
	if (id == QLatin1String("builtin") || id == QLatin1String("system")) {
		if (QDesktopServices::openUrl(QUrl::fromLocalFile(path))) return true;
		if (error) *error = QStringLiteral("The system found no player for this file");
		return false;
	}
	const QStringList line = bssh_player_command(id, path, error);
	if (line.isEmpty()) return false;
	if (QProcess::startDetached(line.first(), line.mid(1))) return true;
	if (error) *error = QStringLiteral("%1 could not be started").arg(line.first());
	return false;
}
