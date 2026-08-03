#include "model/ssh_config.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include "model/document_path.h"

namespace {

struct directive_t {
	QString key;     /* lowercased -- OpenSSH keywords are case-insensitive */
	QString value;
};

struct block_t {
	QStringList patterns;
	QList<directive_t> directives;
};

/*
 * Strips a comment. OpenSSH treats everything after an unquoted '#' as
 * one, and a '#' inside a quoted value as text -- a path with a fragment
 * in it is rare but a config that loses half a line is not something to
 * debug by eye.
 */
QString without_comment(const QString &line) {
	bool quoted = false;
	for (int i = 0; i < line.size(); ++i) {
		const QChar ch = line.at(i);
		if (ch == QLatin1Char('"')) quoted = !quoted;
		else if (ch == QLatin1Char('#') && !quoted) return line.left(i);
	}
	return line;
}

QString unquote(const QString &value) {
	if (value.size() >= 2 && value.startsWith(QLatin1Char('"')) &&
		   value.endsWith(QLatin1Char('"'))) {
		return value.mid(1, value.size() - 2);
	}
	return value;
}

/*
 * "keyword value", "keyword=value" and "keyword = value" are all the same
 * line to OpenSSH. Accepting only the first is how an import silently
 * skips the entries of anyone who writes the second.
 */
bool split_line(const QString &raw, QString &key, QString &value) {
	const QString line = without_comment(raw).trimmed();
	if (line.isEmpty()) return false;

	int split = -1;
	for (int i = 0; i < line.size(); ++i) {
		const QChar ch = line.at(i);
		if (ch.isSpace() || ch == QLatin1Char('=')) {
			split = i;
			break;
		}
	}
	if (split < 0) return false;

	key = line.left(split).toLower();

	QString rest = line.mid(split);
	/* Eat the separator run, which may be spaces around an '='. */
	int start = 0;
	bool seen_equals = false;
	while (start < rest.size()) {
		const QChar ch = rest.at(start);
		if (ch.isSpace()) { ++start; continue; }
		if (ch == QLatin1Char('=') && !seen_equals) { seen_equals = true; ++start; continue; }
		break;
	}

	value = unquote(rest.mid(start).trimmed());
	return !key.isEmpty() && !value.isEmpty();
}

bool is_pattern(const QString &text) {
	return text.contains(QLatin1Char('*')) || text.contains(QLatin1Char('?')) ||
		      text.startsWith(QLatin1Char('!'));
}

/* OpenSSH globbing: '*' any run, '?' one character. Anchored both ends. */
bool glob_matches(const QString &pattern, const QString &name) {
	QString expression = QRegularExpression::escape(pattern);
	expression.replace(QStringLiteral("\\*"), QStringLiteral(".*"));
	expression.replace(QStringLiteral("\\?"), QStringLiteral("."));

	const QRegularExpression regex(QStringLiteral("\\A(?:%1)\\z").arg(expression),
		                              QRegularExpression::CaseInsensitiveOption);
	return regex.match(name).hasMatch();
}

/*
 * Whether a block's patterns select this alias. A negated pattern that
 * matches vetoes the block outright, however many positive ones also
 * match -- that is what `Host !bastion *.internal` is for.
 */
bool block_applies(const block_t &block, const QString &alias) {
	bool positive = false;

	for (const QString &pattern : block.patterns) {
		if (pattern.startsWith(QLatin1Char('!'))) {
			if (glob_matches(pattern.mid(1), alias)) return false;
		} else if (glob_matches(pattern, alias)) {
			positive = true;
		}
	}

	return positive;
}

quint16 to_port(const QString &value, quint16 fallback) {
	bool ok = false;
	const uint number = value.toUInt(&ok);
	if (!ok || number == 0 || number > 65535) return fallback;
	return static_cast<quint16>(number);
}

} /* namespace */

QList<bssh_ssh_config_host_t> bssh_ssh_config::parse(const QString &text) {
	QList<block_t> blocks;
	bool inside_match = false;

	const QStringList lines = text.split(QLatin1Char('\n'));
	for (const QString &raw : lines) {
		QString key;
		QString value;
		if (!split_line(raw, key, value)) continue;

		if (key == QStringLiteral("host")) {
			inside_match = false;

			block_t block;
			block.patterns = value.split(QRegularExpression(QStringLiteral("\\s+")),
			                              Qt::SkipEmptyParts);
			blocks.append(block);
			continue;
		}

		/*
		 * Match depends on the command line, the local user, and possibly
		 * the exit status of a program. None of that is knowable while
		 * reading the file, so its contents are skipped rather than
		 * guessed at and attributed to whichever Host came before it --
		 * which would hand the user a profile the config never described.
		 */
		if (key == QStringLiteral("match")) {
			inside_match = true;
			continue;
		}

		if (inside_match || blocks.isEmpty()) continue;

		directive_t directive;
		directive.key = key;
		directive.value = value;
		blocks.last().directives.append(directive);
	}

	/* Concrete aliases, in the order the file introduces them. */
	QStringList aliases;
	for (const block_t &block : blocks) {
		for (const QString &pattern : block.patterns) {
			if (is_pattern(pattern)) continue;
			if (!aliases.contains(pattern, Qt::CaseInsensitive)) aliases.append(pattern);
		}
	}

	QList<bssh_ssh_config_host_t> hosts;
	for (const QString &alias : aliases) {
		bssh_ssh_config_host_t host;
		host.alias = alias;

		/*
		 * FIRST value wins, which is OpenSSH's rule and the reverse of
		 * what this file format looks like it should do. Every applying
		 * block is still visited, because a `Host *` block contributes
		 * whatever the more specific ones did not claim.
		 */
		QSet<QString> taken;
		for (const block_t &block : blocks) {
			if (!block_applies(block, alias)) continue;

			for (const directive_t &directive : block.directives) {
				if (taken.contains(directive.key)) continue;
				taken.insert(directive.key);

				if (directive.key == QStringLiteral("hostname")) host.hostname = directive.value;
				else if (directive.key == QStringLiteral("user")) host.user = directive.value;
				else if (directive.key == QStringLiteral("port")) host.port = to_port(directive.value, 22);
				else if (directive.key == QStringLiteral("identityfile")) host.identity_file = directive.value;
				else if (directive.key == QStringLiteral("proxyjump")) host.proxy_jump = directive.value;
			}
		}

		/*
		 * No HostName means the alias IS the hostname, and %h inside one
		 * means the alias too -- `HostName %h.internal` is how a whole
		 * estate gets addressed from a single wildcard block.
		 */
		if (host.hostname.isEmpty()) host.hostname = alias;
		host.hostname.replace(QStringLiteral("%h"), alias);

		if (host.is_valid()) hosts.append(host);
	}

	return hosts;
}

QList<bssh_ssh_config_host_t> bssh_ssh_config::read(const QString &raw_path, bool *ok) {
	if (ok) *ok = false;

	QFile file(bssh_normalise_document_path(raw_path));
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return QList<bssh_ssh_config_host_t>();

	const QString text = QString::fromUtf8(file.readAll());
	file.close();

	if (ok) *ok = true;
	return parse(text);
}

QString bssh_ssh_config::default_path() {
	return QDir::homePath() + QStringLiteral("/.ssh/config");
}

bssh_profile_t bssh_ssh_config_host_t::to_profile() const {
	bssh_profile_t profile;
	profile.name = alias;
	profile.host = hostname;
	profile.user = user;
	profile.port = port;

	/*
	 * Carried across now that the transport can offer a named key
	 * (SSH_OPTIONS_ADD_IDENTITY). ProxyJump still cannot be honoured and
	 * is still only reported.
	 */
	profile.identity_file = identity_file;
	return profile;
}
