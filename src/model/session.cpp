#include "model/session.h"

#include "input/input_router.h"
#include "term/emulator.h"
#include "term/term_features.h"

bssh_session::bssh_session(const bssh_profile_t &profile, QObject *parent)
    : QObject(parent),
      m_profile(profile),
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
	m_emulator = new bssh_emulator(m_cols, m_rows, this);
	m_input_router = new bssh_input_router(m_emulator, this);

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
	});

	connect(m_emulator, &bssh_emulator::title_changed, this, [this](const QString &title) {
		m_remote_title = title;
		emit title_changed();
	});
}

bssh_session::~bssh_session() {
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

	connect(m_transport, &bssh_ssh_session::bytes_received,
	         m_emulator, &bssh_emulator::feed);
	connect(m_transport, &bssh_ssh_session::failed, this, [this](const QString &reason) {
		note_connection_lost();
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
		 * A reconnect can answer for itself if a password was given
		 * before; a first connection has to ask. Keeping the password
		 * only once it has actually been needed is narrower than
		 * keeping it always, which is what this did before keys were
		 * tried.
		 */
		if (!m_password.isEmpty()) {
			m_transport->authenticate_password(m_password);
			return;
		}

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
		if (!reply.isEmpty() && m_transport) m_transport->send(reply);
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
				m_transport->send(command.toUtf8() + '\n');
			}
		}
		emit state_changed();
	});

	m_transport->connect_to_host(m_profile.host, m_profile.port, m_profile.user);
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

void bssh_session::supply_password(const QString &password) {
	m_password = password;
	if (m_transport) m_transport->authenticate_password(password);
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
