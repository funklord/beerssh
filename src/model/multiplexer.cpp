#include "model/multiplexer.h"

#include <QStringList>

namespace {

/* Between tmux's lines and screen's, so neither can be read as the other. */
const char BSSH_MUX_SCREEN_MARK[] = "@@bssh-screen@@";

/* Single-quoted for the shell the line is typed into. */
QString quoted(const QString &word) {
	QString inner = word;
	inner.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
	return QLatin1Char('\'') + inner + QLatin1Char('\'');
}

} /* namespace */

QString bssh_mux_listing_script() {
	/*
	 * tmux in a format of our own -- name, windows, attached clients, a
	 * literal tab between -- so nothing depends on the wording of its
	 * default listing, which has changed between releases. screen has no
	 * such option, so its own -ls is read below. Either one missing is
	 * silence on stderr and nothing on stdout, which reads as "none".
	 */
	return QStringLiteral("tmux list-sessions -F \"#{session_name}\t#{session_windows}\t"
	                      "#{session_attached}\" 2>/dev/null; echo %1; screen -ls 2>/dev/null")
	        .arg(QLatin1String(BSSH_MUX_SCREEN_MARK));
}

QString bssh_mux_listing_command() {
	return QStringLiteral("sh -c ") + quoted(bssh_mux_listing_script());
}

QList<bssh_mux_session_t> bssh_mux_parse(const QByteArray &output) {
	QList<bssh_mux_session_t> sessions;
	bool in_screen = false;

	const QStringList lines = QString::fromUtf8(output).split(QLatin1Char('\n'));
	for (QString line : lines) {
		if (line.endsWith(QLatin1Char('\r'))) line.chop(1);
		if (line.trimmed() == QLatin1String(BSSH_MUX_SCREEN_MARK)) {
			in_screen = true;
			continue;
		}

		if (!in_screen) {
			/* name, windows, attached clients -- exactly three fields. */
			const QStringList fields = line.split(QLatin1Char('\t'));
			if (fields.size() != 3 || fields.at(0).isEmpty()) continue;
			bool windows_ok = false;
			bool attached_ok = false;
			const int windows = fields.at(1).toInt(&windows_ok);
			const int attached = fields.at(2).toInt(&attached_ok);
			if (!windows_ok || !attached_ok) continue;

			bssh_mux_session_t session;
			session.kind = bssh_mux_session_t::TMUX;
			session.name = fields.at(0);
			session.detail = windows == 1 ? QStringLiteral("1 window")
			                              : QStringLiteral("%1 windows").arg(windows);
			session.attached = attached > 0;
			sessions.append(session);
			continue;
		}

		/*
		 * screen -ls: one tab-indented line per session, "pid.name", then
		 * an optional date, then the state in parentheses --
		 *   "\t12345.work\t(10/23/2026 09:17:00 AM)\t(Detached)"
		 * The heading and the "N Sockets in ..." footer are not indented.
		 */
		if (!line.startsWith(QLatin1Char('\t'))) continue;
		QStringList fields = line.split(QLatin1Char('\t'), Qt::SkipEmptyParts);
		if (fields.isEmpty()) continue;

		const QString id = fields.first().trimmed();
		const int dot = id.indexOf(QLatin1Char('.'));
		bool pid_ok = false;
		if (dot <= 0) continue;
		id.left(dot).toLongLong(&pid_ok);
		if (!pid_ok) continue;

		QString state = fields.last().trimmed();
		if (state.startsWith(QLatin1Char('(')) && state.endsWith(QLatin1Char(')')))
			state = state.mid(1, state.size() - 2);

		bssh_mux_session_t session;
		session.kind = bssh_mux_session_t::SCREEN;
		session.name = id;
		session.detail = state;
		session.attached = state.contains(QLatin1String("Attached"), Qt::CaseInsensitive);
		sessions.append(session);
	}
	return sessions;
}

QString bssh_mux_attach_command(const bssh_mux_session_t &session) {
	if (session.kind == bssh_mux_session_t::TMUX)
		return QStringLiteral("tmux attach -t ") + quoted(QLatin1Char('=') + session.name);

	/*
	 * -x joins a session even while it is attached elsewhere, which is
	 * what coming back after a dropped connection needs: the old one may
	 * not have noticed it is gone yet.
	 */
	return QStringLiteral("screen -x ") + quoted(session.name);
}

QString bssh_mux_label(const bssh_mux_session_t &session) {
	const QString kind = session.kind == bssh_mux_session_t::TMUX ? QStringLiteral("tmux")
	                                                               : QStringLiteral("screen");
	QString detail = session.detail;
	if (session.kind == bssh_mux_session_t::TMUX && session.attached)
		detail += QStringLiteral(", attached");
	return detail.isEmpty() ? QStringLiteral("%1  %2").arg(kind, session.name)
	                        : QStringLiteral("%1  %2  (%3)").arg(kind, session.name, detail);
}
