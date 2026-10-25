#include "model/session.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QIODevice>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QStringList>
#include <QUuid>

#include "input/input_router.h"
#include "term/emulator.h"
#include "term/term_features.h"
#include "model/preset.h"
#include "model/app_settings.h"

bssh_session::bssh_session(const bssh_profile_t &profile, QObject *parent)
    : QObject(parent),
      m_profile(profile),
      m_font_zoom(0),
      m_transport(nullptr),
      m_user_disconnected(false),
      m_unseen_bell(false),
      m_remote_exited(false),
      m_exit_status(0),
      m_used_key_auth(false),
      m_password_requested(false),
      m_on_screen(false),
      m_cols(80),
      m_rows(24) {
	/*
	 * The emulator exists before the connection does, so output has
	 * somewhere to land from the first byte -- and so a session that
	 * never connects still has a screen to show its error on.
	 */
	m_handoff_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
	m_emulator = new bssh_emulator(m_cols, m_rows, this);
	m_input_router = new bssh_input_router(m_emulator, this);

	/*
	 * Where this host was last left (sec 9.5), bounded like any other
	 * step. Before anything connects to this object, so it is a starting
	 * value and not a change anybody is told about.
	 */
	set_font_zoom(profile.font_zoom);

	/*
	 * The picture history the user set per terminal (sec 7.1), from the
	 * start and here rather than in a window: output -- pictures included
	 * -- lands in the emulator whether or not anything shows the session,
	 * so a terminal opened as a background tab would otherwise hold the
	 * built-in amount until something happened to rebuild a view of it.
	 * A window re-applies it to every session when the setting changes.
	 */
	m_emulator->set_image_budget(qint64(bssh_app_settings().image_history_mb()) * 1024 * 1024);

	connect(m_emulator, &bssh_emulator::bell, this, [this] {
		/*
		 * A host whose preset silences the bell (sec 10.2) -- a noisy
		 * build machine, say -- should not mark its tab either. The
		 * setting means "do not tell me", not "tell me quietly".
		 */
		if (!m_settings.value(QStringLiteral("bell_enabled")).toBool(true)) return;

		if (m_on_screen) return;

		m_unseen_bell = true;
		emit state_changed();

		/*
		 * And tell whoever is listening that it happened out of sight
		 * (sec 5.5). Emitted here rather than derived by a listener,
		 * because the two conditions that make a bell worth reporting --
		 * the host wants bells at all, and nothing is showing this
		 * session -- have both just been evaluated above.
		 */
		emit bell_while_away();
	});

	connect(m_emulator, &bssh_emulator::title_changed, this, [this](const QString &title) {
		m_remote_title = title;
		emit title_changed();
	});
}

bssh_session::~bssh_session() {
}

void bssh_session::set_font_zoom(int steps) {
	/*
	 * Bounded here as well as in the view, and not because one of them is
	 * a duplicate of the other: the view clamps against the size it is
	 * drawing, while this bound is what stops a held-down key or a fast
	 * wheel from walking the stored number away from anything the view
	 * will ever accept. Without it the two disagree after the first step
	 * past the limit, and every step back is then a step that does
	 * nothing.
	 *
	 * The range is the widest a preset's own font size may span (sec
	 * 10.5), which is the largest a zoom could ever need to be.
	 */
	const int span = BSSH_PRESET_MAX_POINT_SIZE - BSSH_PRESET_MIN_POINT_SIZE;
	const int bounded = qBound(-span, steps, span);

	if (bounded == m_font_zoom) return;

	m_font_zoom = bounded;
	emit font_zoom_changed();
}

void bssh_session::set_settings(const QJsonObject &settings) {
	if (settings == m_settings) return;

	m_settings = settings;

	/*
	 * Straight through to the transport (sec 5.3). The interval is a
	 * per-host setting because the right value is a property of the
	 * network in between: a box behind a mobile carrier's NAT needs to
	 * speak up every minute to keep its mapping, and one on the LAN does
	 * not need to speak at all.
	 */
	if (m_transport) {
		m_transport->set_keepalive(
		        m_settings.value(QStringLiteral("keepalive_seconds")).toInt());
		m_transport->set_term(m_settings.value(QStringLiteral("term")).toString());
		m_emulator->set_term_name(m_settings.value(QStringLiteral("term")).toString());
		m_transport->set_compression(
		        m_settings.value(QStringLiteral("compression")).toBool());
		m_transport->set_identity_file(m_profile.identity_file);
		m_transport->set_proxy_jump(m_profile.proxy_jump);
	}


	/*
	 * Which of sec 7.1's extensions this session speaks. Applied in both
	 * places for the reason set_term_name records right below: settings
	 * resolve before a transport exists, so a setter applied only in the
	 * transport block is silently missed for a session that never
	 * reconnects.
	 */
	{
		bssh_term_features_t features;
		if (!bssh_term_features_t::process_override(features)) {
			QString ignored;
			features = bssh_term_features_t::from_spec(
			        m_settings.value(QStringLiteral("term_features")).toString(),
			        ignored);
		}
		m_emulator->set_features(features);
	}

	/*
	 * How much history this host keeps (sec 10.2). On the emulator rather
	 * than on a view, because it is session state: scrolling back through
	 * a pane that was just rebuilt by a layout change must still find the
	 * history (sec 3, core principle 2).
	 */
	/*
	 * ABSENT and ZERO are different, and toInt() reports both as 0.
	 *
	 * This read the value and applied it only when positive, so "0 lines"
	 * -- which BSSH_PRESET_MIN_SCROLLBACK offers and the preset editor's
	 * spin box invites -- was discarded and the emulator kept its 5000
	 * default. The app went on holding history the user had just asked it
	 * not to keep, which for a terminal is a privacy setting rather than
	 * a preference.
	 *
	 * The case the guard protected against cannot occur: every caller
	 * passes a RESOLVED object and preset_t::to_json() always inserts
	 * this key, so an absent one means no settings were applied at all.
	 * Asking whether the key is THERE separates the two properly.
	 */
	if (m_emulator && m_settings.contains(QStringLiteral("scrollback_lines"))) {
		m_emulator->set_scrollback_lines(
		        m_settings.value(QStringLiteral("scrollback_lines")).toInt());
	}

	emit settings_changed();
}

QString bssh_session::title() const {
	if (!m_remote_title.isEmpty()) return m_remote_title;
	if (!m_profile.name.isEmpty()) return m_profile.name;
	return m_profile.target();
}

bssh_ssh_session::state_t bssh_session::state() const {
	return m_transport ? m_transport->state() : bssh_ssh_session::DISCONNECTED;
}

bool bssh_session::is_live() const {
	const bssh_ssh_session::state_t current = state();
	return current != bssh_ssh_session::DISCONNECTED && current != bssh_ssh_session::FAILED;
}

void bssh_session::connect_to_host() {
	if (m_transport) return;

	m_asked_to_connect = true;

	m_user_disconnected = false;
	m_remote_exited = false;
	m_exit_status = 0;
	m_used_key_auth = false;
	m_password_requested = false;
	/*
	 * A local shell is a different transport, not a different session
	 * (sec 2, sec 3): everything above this line -- the emulator, the
	 * router, the scrollback, the tab -- is the same object either way,
	 * which is what keeps the terminal core free of any idea of a PTY.
	 *
	 * The injected factory still wins for both, so a test can fake a
	 * local session exactly as it fakes an SSH one.
	 */
	m_transport = m_profile.local
	        ? bssh_ssh_session::create_local(m_profile.shell,
	                                          m_profile.working_directory, this)
	        : bssh_ssh_session::create(this);

	/*
	 * The interval is applied HERE as well as in set_settings, because the
	 * two arrive in the other order: a session is created and has its
	 * settings resolved long before anyone presses connect, so at
	 * set_settings time there is no transport to tell. Applying it in only
	 * one of the two places leaves every first connection running without
	 * a keepalive -- which is precisely the connection a phone is most
	 * likely to lose.
	 */
	m_transport->set_keepalive(
	        m_settings.value(QStringLiteral("keepalive_seconds")).toInt());
	m_transport->set_term(m_settings.value(QStringLiteral("term")).toString());

	/*
	 * The emulator's copy too, and for the same reason the line above is
	 * here: settings resolve before anyone presses connect, so at
	 * set_settings() time there is no transport and that whole block is
	 * skipped. Every other member of it is re-applied here; this one was
	 * missed, so a preset naming a TERM of its own put it in the
	 * environment while XTGETTCAP went on answering "xterm-256color" --
	 * the terminal naming itself one thing to the shell and another when
	 * asked, which sec 7.1 calls worse than declining to answer.
	 */
	m_emulator->set_term_name(m_settings.value(QStringLiteral("term")).toString());

	/*
	 * Which of sec 7.1's extensions this session speaks. Applied in both
	 * places for the reason set_term_name records right below: settings
	 * resolve before a transport exists, so a setter applied only in the
	 * transport block is silently missed for a session that never
	 * reconnects.
	 */
	{
		bssh_term_features_t features;
		if (!bssh_term_features_t::process_override(features)) {
			QString ignored;
			features = bssh_term_features_t::from_spec(
			        m_settings.value(QStringLiteral("term_features")).toString(),
			        ignored);
		}
		m_emulator->set_features(features);
	}

	m_transport->set_compression(
	        m_settings.value(QStringLiteral("compression")).toBool());

	/*
	 * From the PROFILE, not the settings: a key path identifies one
	 * machine's access, so sec 10.3 keeps it off the shareable preset.
	 * Set in both places for the reason above -- this is the one that
	 * covers the first connection.
	 */
	m_transport->set_identity_file(m_profile.identity_file);
	m_transport->set_proxy_jump(m_profile.proxy_jump);

	/*
	 * What to run instead of a shell (sec 8.6). From the profile like the
	 * two above, and applied here rather than at create_local() because
	 * the SSH transport wants it too -- the difference between the two is
	 * theirs to keep, not this function's.
	 */
	m_transport->set_command(m_profile.command);

	/* Counted per connection: the question is always about this one. */
	m_bytes_in = 0;
	m_bytes_out = 0;
	m_last_in = QDateTime();
	m_last_out = QDateTime();
	m_connected_at = QDateTime();
	wire_transport();
	m_transport->connect_to_host(m_profile.host, m_profile.port, m_profile.user);
}

/*
 * What the session hears from its transport, and what its emulator sends
 * back. Shared by connect_to_host() and import_handoff(): an imported
 * transport arrives CONNECTED already, before this is wired, so the
 * handler below that opens a shell on CONNECTED does not open a second
 * one over the shell that moved.
 */
void bssh_session::wire_transport() {

	connect(m_transport, &bssh_ssh_session::bytes_received,
	         this, [this](const QByteArray &bytes) {
		m_bytes_in += bytes.size();
		m_last_in = QDateTime::currentDateTimeUtc();
	});
	connect(m_transport, &bssh_ssh_session::bytes_received,
	         m_emulator, &bssh_emulator::feed);
	connect(m_transport, &bssh_ssh_session::failed, this, [this](const QString &reason) {
		note_connection_lost();

		/*
		 * Hung up on after a refused password: said as that. A server
		 * counting failures closes the connection, and what libssh makes
		 * of the closed socket -- "Socket error: No such file or
		 * directory", seen on the phone against beersshd -- names neither
		 * the password nor what to do next. Nothing reconnects after a
		 * refusal (conn_manager), so the way on is the user's.
		 */
		if (m_password_refused) {
			emit failed(QStringLiteral("The server refused the password and closed the "
			                            "connection. Open the host again to try another."));
			return;
		}
		emit failed(reason);
	});
	connect(m_transport, &bssh_ssh_session::shell_exited,
	         this, &bssh_session::note_remote_exit);

	connect(m_transport, &bssh_ssh_session::key_passphrase_required,
	         this, &bssh_session::needs_key_passphrase);
	connect(m_transport, &bssh_ssh_session::host_key_unknown,
	         this, &bssh_session::host_key_unknown);
	connect(m_transport, &bssh_ssh_session::host_key_changed,
	         this, &bssh_session::host_key_changed);
	connect(m_transport, &bssh_ssh_session::host_key_revoked,
	         this, &bssh_session::host_key_revoked);

	connect(m_transport, &bssh_ssh_session::password_required, this, [this] {
		m_password_requested = true;

		/*
		 * A reconnect can answer for itself with a password this host
		 * ACCEPTED before; a first connection has to ask. Kept only once
		 * the server took it -- it used to be kept as soon as it was
		 * typed, so a wrong one was replayed by every reconnect.
		 */
		if (!m_password.isEmpty() && !was_refused(m_password)) {
			m_offered = m_password;
			m_transport->authenticate_password(m_password);
			return;
		}

		emit needs_password();
	});

	/*
	 * Refused: remembered, never offered again, and the user asked --
	 * on this same connection, which is still authenticating.
	 */
	connect(m_transport, &bssh_ssh_session::password_rejected, this, [this] {
		if (m_refused_salt.isEmpty()) {
			quint32 salt[4];
			QRandomGenerator::system()->fillRange(salt);
			m_refused_salt = QByteArray(reinterpret_cast<const char *>(salt), sizeof(salt));
		}
		if (!m_offered.isEmpty()) m_refused.insert(refused_key(m_offered));
		if (m_offered == m_password) m_password.clear();
		m_offered.clear();
		m_password_refused = true;
		emit needs_password();
	});

	/*
	 * The emulator's replies -- cursor reports, device attributes, and
	 * every keystroke the router encodes -- go back out the transport.
	 * Wired once here rather than in a window, so they keep flowing while
	 * no view is attached at all.
	 */
	connect(m_emulator, &bssh_emulator::output_ready, this, [this] {
		const QByteArray reply = m_emulator->take_output();
		if (!reply.isEmpty()) send_out(reply);
	});

	/*
	 * The tmux and screen listing coming back. Nothing found is nothing
	 * shown, and the question stays open: a reconnect lists again, in case
	 * one has been started there since.
	 */
	connect(m_transport, &bssh_ssh_session::side_output_ready, this,
	         [this](const QByteArray &output) {
		if (!m_mux_listing) return;
		m_mux_listing = false;
		if (m_mux_answered || !m_mux_attach.isEmpty()) return;
		const QList<bssh_mux_session_t> found = bssh_mux_parse(output);
		if (found.isEmpty()) return;
		m_mux_pending = found;
		emit multiplexers_found(found);
	});

	connect(m_transport, &bssh_ssh_session::state_changed,
	         this, [this](bssh_ssh_session::state_t state) {
		if (state == bssh_ssh_session::CONNECTED) {
			/*
			 * Reached without a password ever being asked for means the
			 * key worked -- and such a session holds no credential at
			 * all, so it can reconnect after a drop with nothing kept in
			 * memory (sec 11).
			 */
			if (!m_password_requested) m_used_key_auth = true;

			m_connected_at = QDateTime::currentDateTimeUtc();
			++m_connections;

			/* In: what was offered is the password worth replaying. */
			if (!m_offered.isEmpty()) m_password = m_offered;
			m_offered.clear();
			m_password_refused = false;
			m_transport->open_shell(m_cols, m_rows);

			/*
			 * The on-connect command (sec 10.2), and sec 5.3's practical
			 * answer to "a reconnect is a new shell": pointing it at
			 * tmux is what turns a dropped connection into a session
			 * that comes back with the work still in it.
			 *
			 * Sent on every connect, INCLUDING a reconnect, because a
			 * reconnect is exactly when it matters -- the new shell is
			 * the one that needs re-attaching.
			 */
			const QString command =
			        m_settings.value(QStringLiteral("on_connect_command")).toString();
			if (!command.isEmpty()) {
				send_out(command.toUtf8() + '\n');
			}

			/*
			 * tmux and screen (sec 10.2a), only into a shell: a session
			 * running a command of its own (sec 8.6's -e) has nothing to
			 * type into. A choice made is typed again on every connect;
			 * until one is, what is there is listed beside the shell,
			 * which opened above, so the prompt is not kept waiting.
			 */
			/*
			 * A listing still out belongs to the connection before this
			 * one. Over SSH its channel went with that connection and will
			 * never answer, and left set this would keep every connection
			 * after it from listing.
			 */
			m_mux_listing = false;

			if (m_profile.command.isEmpty()) {
				if (!m_mux_attach.isEmpty()) {
					send_out(m_mux_attach.toUtf8() + '\n');
				} else if (!m_mux_answered && !m_mux_listing &&
				           bssh_app_settings().offer_multiplexer_sessions()) {
					m_mux_listing = true;
					m_transport->run_on_the_side(bssh_mux_listing_command());
				}
			}
		}
		emit state_changed();
	});

}

void bssh_session::disconnect_from_host() {
	m_user_disconnected = true;

	/*
	 * Announced before the transport is touched, because the reconnect
	 * manager decides from the state change that is about to follow. Told
	 * afterwards it would already have scheduled an attempt, and the
	 * session would say "reconnecting..." about a connection nobody asked
	 * it to make.
	 */
	emit user_disconnected();

	if (m_transport) m_transport->disconnect_from_host();
}

void bssh_session::set_profile(const bssh_profile_t &profile) {
	if (profile.id != m_profile.id) return;

	const QString before = title();
	m_profile = profile;

	/*
	 * The title is derived from the profile (sec 8.3), so it can change
	 * with it -- but only when the remote has not set one of its own
	 * through OSC, which title() already decides.
	 */
	if (title() != before) emit title_changed();
}

void bssh_session::reconnect() {
	if (m_user_disconnected) return;

	/*
	 * A fresh transport rather than a revived one: the old libssh
	 * session's socket, channel and state are gone, and reusing the
	 * object would mean reasoning about which parts of it survived a
	 * failure. The screen and scrollback are untouched, so the user keeps
	 * their history across the drop -- but the remote shell is genuinely
	 * new (sec 5.3), and every MODE the old shell's programs set has to
	 * go with it. Left standing they belong to nobody: a stale mouse mode
	 * writes a report onto the new shell's command line at every click,
	 * and a link that dropped inside an editor comes back with the shell
	 * drawing into the alternate buffer.
	 */
	if (m_emulator) m_emulator->reset_modes();

	if (m_transport) {
		m_transport->disconnect_from_host();
		m_transport->deleteLater();
		m_transport = nullptr;
	}

	connect_to_host();
}

/*
 * Offered, and kept only if the server takes it (see CONNECTED above).
 *
 * A password the server has already refused is not sent: it cannot work,
 * and every refusal is a failure a server may be counting towards
 * blackholing this address (sec 11). The user is asked instead, which is
 * the same answer a refusal gets.
 */
void bssh_session::supply_password(const QString &password) {
	if (was_refused(password)) {
		m_password_refused = true;
		emit needs_password();
		return;
	}

	m_offered = password;
	if (m_transport) m_transport->authenticate_password(password);
}

bool bssh_session::was_refused(const QString &password) const {
	return !m_refused.isEmpty() && m_refused.contains(refused_key(password));
}

/* Salted by the first refusal, which is the first time one is needed. */
QByteArray bssh_session::refused_key(const QString &password) const {
	return QCryptographicHash::hash(m_refused_salt + password.toUtf8(),
	                                QCryptographicHash::Sha256);
}

void bssh_session::supply_key_passphrase(const QString &passphrase) {
	if (m_transport) m_transport->supply_key_passphrase(passphrase);
}

void bssh_session::accept_host_key() {
	if (m_transport) m_transport->accept_host_key();
}

void bssh_session::note_remote_exit(int exit_status) {
	m_remote_exited = true;
	m_exit_status = exit_status;

	emit shell_exited(exit_status);
	emit state_changed();
}

void bssh_session::note_connection_lost() {
	m_remote_exited = false;
	emit state_changed();
}

void bssh_session::mark_seen() {
	if (!m_unseen_bell) return;

	m_unseen_bell = false;
	emit state_changed();
}

void bssh_session::set_on_screen(bool on_screen) {
	m_on_screen = on_screen;
	if (on_screen) mark_seen();
}

void bssh_session::resize(int cols, int rows) {
	if (cols <= 0 || rows <= 0) return;
	if (cols == m_cols && rows == m_rows) return;

	m_cols = cols;
	m_rows = rows;

	m_emulator->resize(cols, rows);
	if (m_transport) m_transport->resize(cols, rows);
}

void bssh_session::send_out(const QByteArray &bytes) {
	if (!m_transport || bytes.isEmpty()) return;

	/* Mid-move the connection is stopped and belongs to nobody for a
	 * moment: a write now would advance a sequence number the receiver
	 * also holds. Held, and carried (export_handoff). */
	if (m_moving) {
		m_held_output.append(bytes);
		return;
	}
	m_bytes_out += bytes.size();
	m_last_out = QDateTime::currentDateTimeUtc();
	m_transport->send(bytes);
}

namespace {

/* "never", or how long ago, to the second: the question is "recently?". */
QString ago(const QDateTime &then, const QDateTime &now) {
	if (!then.isValid()) return QStringLiteral("never");
	qint64 seconds = qMax<qint64>(0, then.secsTo(now));
	if (seconds < 60) return QStringLiteral("%1 s ago").arg(seconds);
	const qint64 minutes = seconds / 60;
	seconds %= 60;
	if (minutes < 60) return QStringLiteral("%1 min %2 s ago").arg(minutes).arg(seconds);
	return QStringLiteral("%1 h %2 min ago").arg(minutes / 60).arg(minutes % 60);
}

const char *state_name(bssh_ssh_session::state_t state) {
	switch (state) {
	case bssh_ssh_session::DISCONNECTED:   return "disconnected";
	case bssh_ssh_session::RESOLVING:      return "resolving";
	case bssh_ssh_session::CONNECTING:     return "connecting";
	case bssh_ssh_session::AUTHENTICATING: return "authenticating";
	case bssh_ssh_session::CONNECTED:      return "connected";
	case bssh_ssh_session::FAILED:         return "failed";
	}
	return "unknown";
}

} /* namespace */

QString bssh_session::details(const QDateTime &now) const {
	QStringList lines;
	lines << QStringLiteral("State: %1").arg(QLatin1String(state_name(state())));
	lines << QStringLiteral("Connected: %1, connection %2 of this tab")
	                 .arg(ago(m_connected_at, now)).arg(m_connections);
	lines << QStringLiteral("Received: %1 bytes, last %2")
	                 .arg(m_bytes_in).arg(ago(m_last_in, now));
	lines << QStringLiteral("Sent: %1 bytes, last %2")
	                 .arg(m_bytes_out).arg(ago(m_last_out, now));

	/*
	 * Anything that changes what a key sends. A program that turns one of
	 * these on and dies, or is detached from, leaves every key encoded
	 * for it -- which reads as a keyboard gone wrong.
	 */
	QStringList keys;
	if (m_emulator) {
		if (m_emulator->kitty_keyboard_flags() != 0)
			keys << QStringLiteral("kitty keyboard flags %1").arg(m_emulator->kitty_keyboard_flags());
		if (m_emulator->modify_other_keys_level() != 0)
			keys << QStringLiteral("modifyOtherKeys %1").arg(m_emulator->modify_other_keys_level());
		if (m_emulator->mouse_reporting()) keys << QStringLiteral("mouse reporting");
		if (m_emulator->alternate_screen()) keys << QStringLiteral("alternate screen");
	}
	if (m_input_router) {
		const struct { quint8 bit; const char *name; } held[] = {
			{ bssh_emulator::MOD_CTRL, "Ctrl" },
			{ bssh_emulator::MOD_ALT, "Alt" },
			{ bssh_emulator::MOD_SHIFT, "Shift" },
		};
		for (const auto &each : held) {
			const auto state = m_input_router->modifier_state(each.bit);
			if (state == bssh_input_router::MODIFIER_ONCE)
				keys << QStringLiteral("%1 armed").arg(QLatin1String(each.name));
			else if (state == bssh_input_router::MODIFIER_LOCKED)
				keys << QStringLiteral("%1 locked").arg(QLatin1String(each.name));
			else if (state != bssh_input_router::MODIFIER_OFF)
				keys << QStringLiteral("%1 held").arg(QLatin1String(each.name));
		}
	}
	lines << QStringLiteral("Keyboard: %1")
	                 .arg(keys.isEmpty() ? QStringLiteral("plain") : keys.join(QStringLiteral(", ")));

	return lines.join(QLatin1Char('\n'));
}

/* ---- moving a session to another process (sec 8.4) -------------------- */

namespace {

const char BSSH_SESSION_HANDOFF_MAGIC[] = "bssh-session";
const quint32 BSSH_SESSION_HANDOFF_FORMAT = 1;

void write_profile(QDataStream &out, const bssh_profile_t &profile) {
	out << qint32(profile.id) << profile.name << profile.host << profile.user << profile.port
	    << profile.identity_file << profile.proxy_jump << profile.has_stored_password
	    << profile.require_biometric << qint32(profile.font_zoom) << profile.keep_scrollback
	    << profile.keep_awake << profile.folder << qint32(profile.preset_id)
	    << QJsonDocument(profile.overrides).toJson(QJsonDocument::Compact) << profile.local
	    << profile.shell << profile.command << profile.working_directory << profile.hold;
}

void read_profile(QDataStream &in, bssh_profile_t &profile) {
	qint32 id = 0, font_zoom = 0, preset_id = 0;
	QByteArray overrides;
	in >> id >> profile.name >> profile.host >> profile.user >> profile.port
	   >> profile.identity_file >> profile.proxy_jump >> profile.has_stored_password
	   >> profile.require_biometric >> font_zoom >> profile.keep_scrollback
	   >> profile.keep_awake >> profile.folder >> preset_id >> overrides >> profile.local
	   >> profile.shell >> profile.command >> profile.working_directory >> profile.hold;
	profile.id = id;
	profile.font_zoom = font_zoom;
	profile.preset_id = preset_id;
	profile.overrides = QJsonDocument::fromJson(overrides).object();
}

}  // namespace

bool bssh_session::export_handoff(QByteArray &state, QList<int> &fds, QString *why) {
	state.clear();
	fds.clear();
	if (!m_transport || m_transport->state() != bssh_ssh_session::CONNECTED) {
		if (why) *why = QStringLiteral("the session is not connected");
		return false;
	}

	m_moving = true;
	QByteArray transport;
	if (!m_transport->export_transport(transport, fds, why)) {
		m_moving = false;
		const QByteArray held = m_held_output;
		m_held_output.clear();
		send_out(held);
		return false;
	}

	/*
	 * What the transport had already read and queued for this thread, into
	 * the terminal now: the stop was between packets on the connection,
	 * and those bytes are on this side of it.
	 */
	QCoreApplication::sendPostedEvents(m_transport, QEvent::MetaCall);

	QDataStream out(&state, QIODevice::WriteOnly);
	out.setVersion(QDataStream::Qt_6_0);
	out << QByteArray(BSSH_SESSION_HANDOFF_MAGIC) << BSSH_SESSION_HANDOFF_FORMAT;
	write_profile(out, m_profile);
	out << QJsonDocument(m_settings).toJson(QJsonDocument::Compact) << qint32(m_font_zoom)
	    << qint32(m_cols) << qint32(m_rows) << m_remote_title << m_password
	    << m_used_key_auth << m_password_requested << m_unseen_bell
	    << m_bytes_in << m_bytes_out << m_last_in << m_last_out << m_connected_at
	    << qint32(m_connections);
	out << m_transport->kind() << transport << m_emulator->export_state();
	/* Last, after the terminal's export, which can itself produce output
	 * that is held. */
	out << m_held_output;
	return true;
}

void bssh_session::release_handoff() {
	if (!m_moving || !m_transport) return;
	m_transport->release_transport();
	m_held_output.clear();
	/* Inert from here. Not DISCONNECTED by request: nobody asked for the
	 * connection to end, and it has not. */
	m_user_disconnected = true;
}

void bssh_session::resume_handoff() {
	if (!m_moving) return;
	m_moving = false;
	if (m_transport) m_transport->resume_transport();
	const QByteArray held = m_held_output;
	m_held_output.clear();
	send_out(held);
}

bssh_session *bssh_session::import_handoff(const QByteArray &state, const QList<int> &fds,
                                           QObject *parent, QString *why) {
	QDataStream in(state);
	in.setVersion(QDataStream::Qt_6_0);
	QByteArray magic;
	quint32 format = 0;
	in >> magic >> format;
	if (in.status() != QDataStream::Ok || magic != BSSH_SESSION_HANDOFF_MAGIC ||
	    format != BSSH_SESSION_HANDOFF_FORMAT) {
		if (why) *why = QStringLiteral("the session's state is not one this BeerSSH reads");
		return nullptr;
	}

	bssh_profile_t profile;
	read_profile(in, profile);
	QByteArray settings, transport, terminal, held;
	qint32 font_zoom = 0, cols = 0, rows = 0, connections = 0;
	QString title, password, kind;
	bool used_key_auth = false, password_requested = false, unseen_bell = false;
	qint64 bytes_in = 0, bytes_out = 0;
	QDateTime last_in, last_out, connected_at;
	in >> settings >> font_zoom >> cols >> rows >> title >> password >> used_key_auth
	   >> password_requested >> unseen_bell >> bytes_in >> bytes_out >> last_in >> last_out
	   >> connected_at >> connections;
	in >> kind >> transport >> terminal >> held;
	if (in.status() != QDataStream::Ok || !in.atEnd() || cols <= 0 || rows <= 0) {
		if (why) *why = QStringLiteral("the session's state is damaged");
		return nullptr;
	}

	bssh_session *session = new bssh_session(profile, parent);
	session->set_settings(QJsonDocument::fromJson(settings).object());
	session->set_font_zoom(font_zoom);
	session->resize(cols, rows);
	if (!session->m_emulator->import_state(terminal)) {
		if (why) *why = QStringLiteral("the terminal's state does not fit this BeerSSH");
		delete session;
		return nullptr;
	}

	bssh_ssh_session *carried = bssh_ssh_session::create_for_import(kind, session);
	if (!carried || !carried->import_transport(transport, fds, why)) {
		if (why && !carried) *why = QStringLiteral("this BeerSSH cannot take a %1 connection").arg(kind);
		delete session;
		return nullptr;
	}

	session->m_transport = carried;
	session->m_transport->set_keepalive(
	        session->m_settings.value(QStringLiteral("keepalive_seconds")).toInt());
	session->m_transport->set_term(session->m_settings.value(QStringLiteral("term")).toString());
	session->m_transport->set_identity_file(profile.identity_file);
	session->m_transport->set_proxy_jump(profile.proxy_jump);
	session->m_transport->set_command(profile.command);
	session->m_asked_to_connect = true;
	session->m_remote_title = title;
	session->m_password = password;
	session->m_used_key_auth = used_key_auth;
	session->m_password_requested = password_requested;
	session->m_unseen_bell = unseen_bell;
	session->m_bytes_in = bytes_in;
	session->m_bytes_out = bytes_out;
	session->m_last_in = last_in;
	session->m_last_out = last_out;
	session->m_connected_at = connected_at;
	session->m_connections = connections;
	session->wire_transport();
	session->send_out(held);
	return session;
}

void bssh_session::choose_multiplexer(const bssh_mux_session_t &chosen) {
	m_mux_answered = true;
	m_mux_attach = bssh_mux_attach_command(chosen);
	if (m_transport && is_live()) send_out(m_mux_attach.toUtf8() + '\n');
	if (!m_mux_pending.isEmpty()) {
		m_mux_pending.clear();
		emit multiplexers_settled();
	}
}

void bssh_session::decline_multiplexers() {
	m_mux_answered = true;
	if (!m_mux_pending.isEmpty()) {
		m_mux_pending.clear();
		emit multiplexers_settled();
	}
}

void bssh_session::choose_multiplexers(const QList<bssh_mux_session_t> &chosen) {
	if (chosen.isEmpty()) {
		decline_multiplexers();
		return;
	}
	choose_multiplexer(chosen.first());
	if (chosen.size() > 1) emit more_multiplexers_wanted(chosen.mid(1));
}

void bssh_session::preset_multiplexer(const bssh_mux_session_t &chosen) {
	m_mux_answered = true;
	m_mux_attach = bssh_mux_attach_command(chosen);
}
