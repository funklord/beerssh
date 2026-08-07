#include "model/window_title.h"

#include "model/session.h"

QString bssh_expand_window_title(const QString &format, const bssh_session *session) {
	const QString app = QStringLiteral("BeerSSH");

	if (!session) return app;

	const bssh_profile_t &profile = session->profile();

	QString out;
	out.reserve(format.size() + 32);

	for (int i = 0; i < format.size(); ++i) {
		const QChar c = format.at(i);

		if (c != QLatin1Char('%') || i + 1 >= format.size()) {
			out += c;
			continue;
		}

		const QChar what = format.at(++i);
		switch (what.unicode()) {
		case 't': out += session->title(); break;
		case 'n': out += profile.name; break;
		case 'u': out += profile.user; break;
		case 'h': out += profile.host; break;
		case 'p': out += QString::number(profile.port); break;
		case 'a': out += app; break;
		case '%': out += QLatin1Char('%'); break;
		default:
			/*
			 * An unknown placeholder is left as written rather than
			 * swallowed. Somebody who typed %z wants to see that it
			 * did nothing; a title that silently loses characters is
			 * one they cannot debug from the outside.
			 */
			out += QLatin1Char('%');
			out += what;
			break;
		}
	}

	return out;
}
