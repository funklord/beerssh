#include "platform/platform.h"

#include <QApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDBusVariant>
#include <QPalette>
#include <QStyleHints>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMenu>
#include <QSystemTrayIcon>

#include "model/command_line.h"
#include "term/colour_scheme.h"

#include <QFile>
#include <QTextStream>
#include <QStringList>

/*
 * Desktop implementation of bssh_platform (Linux primary; macOS and
 * Windows best-effort, per project.md sec 1).
 *
 * M0 is intentionally thin. What lands here later is the desktop half of
 * project.md sec 5.5: staying resident with no window open, the tray icon,
 * the prompt before backgrounding with live sessions, desktop
 * notifications, and treating suspend/resume as a network-change event.
 */

/*
 * org.freedesktop.appearance color-scheme: 0 no preference, 1 prefer
 * dark, 2 prefer light. Read once per call rather than cached, because
 * the value changes while running and this is a DBus round trip on a
 * settings page, not on a paint.
 */
/*
 * Whether a session bus is configured, answered WITHOUT calling
 * QDBusConnection::sessionBus() -- which is the whole point.
 *
 * With DBUS_SESSION_BUS_ADDRESS unset, sessionBus() does not fail: libdbus
 * AUTOLAUNCHES a dbus-daemon and connects to that. In a headless run -- a
 * test under the offscreen platform, an agent's shell, a build container --
 * there is no session to reap it, so each run leaves one orphaned with a
 * stale /tmp/dbus-* socket. Measured across this workspace's Qt projects:
 * 1031 of them, 2.2 GB, the oldest six days old, every one launched from a
 * project directory by a run that only wanted to ask the portal whether
 * the desktop was dark. Reading the env first is the fix, and every
 * project that consults the portal has the same leak to close.
 */
static bool session_bus_configured() {
	return !qEnvironmentVariableIsEmpty("DBUS_SESSION_BUS_ADDRESS");
}

static bssh_appearance_t portal_appearance() {
	if (!session_bus_configured()) return BSSH_APPEARANCE_UNKNOWN;
	if (!QDBusConnection::sessionBus().isConnected()) return BSSH_APPEARANCE_UNKNOWN;

	QDBusInterface settings(QStringLiteral("org.freedesktop.portal.Desktop"),
	                         QStringLiteral("/org/freedesktop/portal/desktop"),
	                         QStringLiteral("org.freedesktop.portal.Settings"),
	                         QDBusConnection::sessionBus());
	if (!settings.isValid()) return BSSH_APPEARANCE_UNKNOWN;

	const QDBusReply<QDBusVariant> reply =
	        settings.call(QStringLiteral("Read"), QStringLiteral("org.freedesktop.appearance"),
	                       QStringLiteral("color-scheme"));
	if (!reply.isValid()) return BSSH_APPEARANCE_UNKNOWN;

	/* Doubly wrapped: Read returns a variant holding a variant. */
	QVariant value = reply.value().variant();
	if (value.metaType().id() == QMetaType::QVariantMap) return BSSH_APPEARANCE_UNKNOWN;
	while (value.canConvert<QDBusVariant>()) value = value.value<QDBusVariant>().variant();

	switch (value.toUInt()) {
	case 1: return BSSH_APPEARANCE_DARK;
	case 2: return BSSH_APPEARANCE_LIGHT;
	default: return BSSH_APPEARANCE_UNKNOWN;
	}
}

class bssh_platform_desktop : public bssh_platform {
	Q_OBJECT

public:
	bssh_platform_desktop() : m_tray(nullptr) {
		watch_colour_scheme();

		/*
		 * Reachability is watched by the base class now, for both
		 * platforms. The case that matters here is a laptop waking from
		 * suspend: every TCP connection is already dead and looks
		 * precisely like a network flap (sec 5.5).
		 */
	}

	QString name() const override {
		/*
		 * The QPA plugin name ("wayland", "xcb", "offscreen", ...) is
		 * worth surfacing rather than a flat "desktop": the two Linux
		 * session types differ on which key chords ever reach the app
		 * (project.md sec 6.1), so a bug report wants to say which one
		 * was in use.
		 */
		return QStringLiteral("desktop/") + QGuiApplication::platformName();
	}

	void set_keep_alive(bool keep_alive) override {
		/*
		 * The whole desktop mechanism: stop Qt quitting when the last
		 * window closes, and give the user a way back. Sessions belong
		 * to the registry, not to a window (sec 3), so closing the
		 * window must not disturb them.
		 */
		QApplication::setQuitOnLastWindowClosed(!keep_alive);

		if (keep_alive) {
			ensure_tray();
		} else if (m_tray) {
			m_tray->hide();
		}
	}

	bool send_to_background() override {
		/*
		 * Nothing to do here: a desktop app goes out of sight by hiding
		 * its window, and the window is the caller's. Answering false
		 * says so.
		 */
		return false;
	}

	bool can_inhibit_sleep() const override {
		return QDBusConnection::systemBus().isConnected()
		       || QDBusConnection::sessionBus().isConnected();
	}

	/*
	 * Asked two ways, because desktops listen in two places: logind's
	 * inhibitor on the system bus, which holds for as long as the file
	 * descriptor it hands back stays open, and the older
	 * org.freedesktop.PowerManagement.Inhibit on the session bus, which a
	 * power manager that is not systemd's follows and which hands back a
	 * cookie. Both asynchronously -- a machine with no system bus must not
	 * stall a window for a D-Bus timeout -- and a reply that arrives after
	 * the answer changed back is let go at once.
	 */
	void platform_set_sleep_inhibited(bool inhibit) override {
		m_want_awake = inhibit;

		if (!inhibit) {
			m_logind_lock = QDBusUnixFileDescriptor();
			if (m_pm_cookie != 0) {
				QDBusMessage release = QDBusMessage::createMethodCall(
				        QStringLiteral("org.freedesktop.PowerManagement"),
				        QStringLiteral("/org/freedesktop/PowerManagement/Inhibit"),
				        QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
				        QStringLiteral("UnInhibit"));
				release << m_pm_cookie;
				QDBusConnection::sessionBus().asyncCall(release);
				m_pm_cookie = 0;
			}
			return;
		}

		const QString why = QStringLiteral("A connected host asks to keep the computer awake");

		if (QDBusConnection::systemBus().isConnected()) {
			QDBusMessage call = QDBusMessage::createMethodCall(
			        QStringLiteral("org.freedesktop.login1"),
			        QStringLiteral("/org/freedesktop/login1"),
			        QStringLiteral("org.freedesktop.login1.Manager"),
			        QStringLiteral("Inhibit"));
			call << QStringLiteral("sleep:idle") << QStringLiteral("BeerSSH") << why
			     << QStringLiteral("block");
			auto *watcher = new QDBusPendingCallWatcher(
			        QDBusConnection::systemBus().asyncCall(call), this);
			connect(watcher, &QDBusPendingCallWatcher::finished, this,
			        [this](QDBusPendingCallWatcher *done) {
				const QDBusPendingReply<QDBusUnixFileDescriptor> reply = *done;
				if (m_want_awake && reply.isValid()) m_logind_lock = reply.value();
				done->deleteLater();
			});
		}

		if (QDBusConnection::sessionBus().isConnected()) {
			QDBusMessage call = QDBusMessage::createMethodCall(
			        QStringLiteral("org.freedesktop.PowerManagement"),
			        QStringLiteral("/org/freedesktop/PowerManagement/Inhibit"),
			        QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
			        QStringLiteral("Inhibit"));
			call << QStringLiteral("BeerSSH") << why;
			auto *watcher = new QDBusPendingCallWatcher(
			        QDBusConnection::sessionBus().asyncCall(call), this);
			connect(watcher, &QDBusPendingCallWatcher::finished, this,
			        [this](QDBusPendingCallWatcher *done) {
				const QDBusPendingReply<uint> reply = *done;
				if (reply.isValid()) {
					if (m_want_awake) {
						m_pm_cookie = reply.value();
					} else {
						QDBusMessage release = QDBusMessage::createMethodCall(
						        QStringLiteral("org.freedesktop.PowerManagement"),
						        QStringLiteral("/org/freedesktop/PowerManagement/Inhibit"),
						        QStringLiteral("org.freedesktop.PowerManagement.Inhibit"),
						        QStringLiteral("UnInhibit"));
						release << reply.value();
						QDBusConnection::sessionBus().asyncCall(release);
					}
				}
				done->deleteLater();
			});
		}
	}

	QSize platform_screen_size() const override {
		/*
		 * None to follow: a desktop's windows are sized by the user, and
		 * the arrangement stays whatever it was chosen to be.
		 */
		return QSize();
	}

	bool watch_foreground(std::function<void(bool)>) override {
		/*
		 * Nothing to watch with: a desktop has windows rather than an app
		 * lifecycle, and the one caller -- the hang watchdog -- does not
		 * arm here anyway, since SIGQUIT on a desktop is a core dump.
		 */
		return false;
	}

	bool can_hide_from_screenshots() const override {
		/*
		 * No. X11 has no such concept, Wayland's screencopy protocols do
		 * not offer an opt-out a client can rely on, and neither does
		 * macOS or Windows in a form worth promising. sec 11 says not to
		 * pretend otherwise in the UI, so this answers honestly and the
		 * setting is not offered here at all.
		 */
		return false;
	}

	void set_hide_from_screenshots(bool hide) override {
		Q_UNUSED(hide);
	}

	void set_session_count(int count) override {
		if (!m_tray) return;

		m_tray->setToolTip(count == 1 ? QStringLiteral("BeerSSH -- 1 session connected")
		                              : QStringLiteral("BeerSSH -- %1 sessions connected").arg(count));
	}

	void platform_notify(const QString &title, const QString &body) override {
		/*
		 * Qt routes this through the platform's own notification service
		 * where one exists, and falls back to a balloon otherwise. A
		 * tray icon is required for it either way, which is why this
		 * creates one rather than silently doing nothing.
		 */
		ensure_tray();
		if (m_tray) m_tray->showMessage(title, body);
	}

	QString platform_terminfo_directory() const override {
		/*
		 * The system's, and deliberately not ours. A desktop has
		 * /usr/share/terminfo with thousands of entries maintained by
		 * somebody whose job that is; shipping one entry beside it
		 * would shadow the real database for exactly one TERM and
		 * leave every other name worse off.
		 *
		 * Empty means "say nothing", so TERMINFO is left unset and the
		 * library looks where it always looks.
		 */
		return QString();
	}

	QString font_directory() const override {
		/*
		 * Three places, in the order that makes a developer's checkout
		 * behave like an installation rather than a special case:
		 *
		 *   next to the binary   -- a build tree, and any self-contained
		 *                           layout that ships fonts beside the app
		 *   ../share/beerssh     -- an installed prefix
		 *   BSSH_FONT_DIR        -- an override, for running against a
		 *                           font set somewhere else entirely
		 *
		 * The override goes first because that is what an override means.
		 * Nothing here is fatal if missing: sec 9.4 lets the catalog fall
		 * back to families the distribution already installed, which on a
		 * Linux desktop is usually most of the set.
		 */
		const QByteArray override_dir = qgetenv("BSSH_FONT_DIR");
		if (!override_dir.isEmpty()) return QString::fromLocal8Bit(override_dir);

		const QString app_dir = QCoreApplication::applicationDirPath();
		const QStringList candidates = {
			app_dir + QStringLiteral("/fonts"),
			app_dir + QStringLiteral("/../share/beerssh/fonts"),
			app_dir + QStringLiteral("/../../fonts")
		};

		for (const QString &candidate : candidates) {
			if (QDir(candidate).exists()) return QDir(candidate).canonicalPath();
		}

		return QString();
	}


	/*
	 * Three sources, in decreasing order of authority, because no single
	 * one of them answers everywhere (see the interface).
	 */
	/*
	 * Both sources are watched, because either may be the one that
	 * notices: Qt's hint where a platform theme is engaged, and the
	 * portal's SettingChanged where it is not. Both funnel into one
	 * signal, so a duplicate is a harmless repaint rather than a
	 * behaviour.
	 */
	void watch_colour_scheme() {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
		connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
		         this, [this](Qt::ColorScheme) { emit appearance_changed(); });
#endif

		/*
		 * Only where a bus is configured: connect() would otherwise
		 * autolaunch a dbus-daemon that nothing reaps -- see
		 * session_bus_configured() above and the leak it records.
		 */
		if (!session_bus_configured()) return;

		QDBusConnection::sessionBus().connect(
		        QString(), QStringLiteral("/org/freedesktop/portal/desktop"),
		        QStringLiteral("org.freedesktop.portal.Settings"),
		        QStringLiteral("SettingChanged"), this, SLOT(on_portal_setting_changed(QString, QString, QDBusVariant)));
	}

	QString openssh_directory() const override {
		const QString home = QDir::homePath();
		if (home.isEmpty()) return QString();
		return home + QStringLiteral("/.ssh");
	}

	double system_font_scale() const override {
		/*
		 * Nothing to add: Qt applies the desktop's own font size to the
		 * default application font already, so the interface is already
		 * at the size the user chose. Returning 1.0 is not "unsupported",
		 * it is "already applied".
		 */
		return 1.0;
	}

	bssh_appearance_t system_appearance() const override {
		/*
		 * 1. Qt, where a platform theme is engaged. Correct on Windows,
		 *    macOS, and any Linux session with the KDE or GTK platform
		 *    theme plugin loaded.
		 */
		/*
		 * Qt's hint, where the Qt being built against has one:
		 * QStyleHints::colorScheme() arrived in 6.5 and the CI runner
		 * ships 6.4. Everything below still answers without it, which is
		 * why the chain has three sources rather than one.
		 */
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
		const Qt::ColorScheme hinted = QGuiApplication::styleHints()->colorScheme();
		if (hinted == Qt::ColorScheme::Dark) return BSSH_APPEARANCE_DARK;
		if (hinted == Qt::ColorScheme::Light) return BSSH_APPEARANCE_LIGHT;
#endif

		/*
		 * 2. The XDG desktop portal, which is where the preference
		 *    actually lives on Linux and answers even when Qt does not:
		 *    measured on this machine, styleHints said Unknown while the
		 *    portal said "prefer dark", which is a light window on a dark
		 *    desktop for anyone whose distribution has not wired the
		 *    platform theme up.
		 */
		const bssh_appearance_t from_portal = portal_appearance();
		if (from_portal != BSSH_APPEARANCE_UNKNOWN) return from_portal;

		/*
		 * 4. What the desktop wrote down, asked BEFORE the palette
		 *    below and not after it. The palette rung always answers --
		 *    a palette is either lighter or darker than its text -- so
		 *    a rung placed after it can never run. On the desktop this
		 *    exists for, that ordering would report light and this
		 *    would be dead code. Authority, not sequence: a statement
		 *    the desktop wrote down outranks an inference drawn from
		 *    whatever Qt happened to hand us.
		 */
		const bssh_appearance_t written =
		    bssh_appearance_from_scheme_files(bssh_scheme_sources());
		if (written != BSSH_APPEARANCE_UNKNOWN) return written;

		/*
		 * 5. The palette we were given. If something themed this app,
		 *    its window colour says which way round it meant it -- a
		 *    weaker signal than being told, but a true one, and it costs
		 *    nothing when the others have already answered.
		 */
		const QColor window = startup_palette().color(QPalette::Window);
		return window.lightness() < 128 ? BSSH_APPEARANCE_DARK : BSSH_APPEARANCE_LIGHT;
	}

private slots:
	void on_portal_setting_changed(const QString &space, const QString &key,
	                                const QDBusVariant &value) {
		Q_UNUSED(value);
		if (space != QStringLiteral("org.freedesktop.appearance")) return;
		if (key != QStringLiteral("color-scheme")) return;
		emit appearance_changed();
	}

public:
	QString launch_document() const override {
		/*
		 * A file named on the command line, which is what a desktop file
		 * manager passes when BeerSSH is the handler for a .ini, and
		 * what a shell passes when someone types it.
		 *
		 * Through the one command-line parser (sec 8.6), which is the
		 * point of this being three lines. It used to be a second scan
		 * of argv living here, with its own idea of what an option
		 * looked like and its own rule for telling a path from a
		 * hostname -- and the two could disagree: this one required the
		 * file to EXIST, so a mistyped preset path fell through to
		 * main()'s loop and was dialled as a hostname.
		 */
		return bssh_command_line().document;
	}

	/*
	 * Absent, not faked (sec 11). Linux, macOS and Windows all have some
	 * user-verification API, but none of them is reachable through Qt and
	 * each is a different piece of platform work -- so until one is
	 * written this answers honestly and the option is offered nowhere.
	 */
	bool platform_has_biometric() const override {
		return false;
	}

	void authenticate(const QString &reason, std::function<void(bool)> done) override {
		Q_UNUSED(reason);

		/* Never reached while has_biometric() is false, and false rather
		 * than true so that a caller which forgets to check fails
		 * CLOSED -- it withholds a password rather than releasing one. */
		if (done) done(false);
	}

	bool platform_is_touch_primary() const override {
		/*
		 * False for now, not forever. A convertible in tablet mode is a
		 * real desktop case for touch sizing and the on-screen key row,
		 * and the answer for it is sec 8.7's `metrics` dimension (auto,
		 * on, off) -- which that section holds back until a feature needs
		 * it rather than building it as a refactor. The key row this used
		 * to wait for exists; the setting is what remains.
		 */
		return false;
	}

private:
	/*
	 * Built once, SHOWN every time -- two questions that were one early
	 * return.
	 *
	 * set_keep_alive(false) hides the icon when the last session goes,
	 * and bssh_main_window calls set_keep_alive(live > 0) on every
	 * connect and disconnect. So the second session of a run got no icon
	 * at all: this saw m_tray already existed and returned, and the icon
	 * stayed hidden for the rest of the process. That is sec 5.5's route
	 * back to a hidden window, gone after the first time the session
	 * count touches zero.
	 *
	 * notify() inherited it, handing showMessage() an invisible icon,
	 * which Qt drops without a word -- so every reconnect and bell notice
	 * went missing too.
	 */
	void ensure_tray() {
		if (!QSystemTrayIcon::isSystemTrayAvailable()) return;

		if (m_tray) {
			m_tray->show();
			return;
		}

		m_tray = new QSystemTrayIcon(this);
		m_tray->setToolTip(QStringLiteral("BeerSSH"));

		/*
		 * The application icon, said out loud. QSystemTrayIcon does not
		 * inherit the one main.cpp sets on QGuiApplication, and Qt warns
		 * "No Icon set" and shows nothing -- a way back nobody can see is
		 * the same as no way back. main.cpp sets the window icon for the
		 * same reason and this did not follow it.
		 */
		m_tray->setIcon(QGuiApplication::windowIcon());

		QMenu *menu = new QMenu();
		menu->addAction(QStringLiteral("Quit BeerSSH"), qApp, &QCoreApplication::quit);
		m_tray->setContextMenu(menu);

		m_tray->show();
	}

	QSystemTrayIcon *m_tray;

	/* The sleep inhibition, as each of the two places that grant one returns it. */
	bool m_want_awake = false;
	QDBusUnixFileDescriptor m_logind_lock;
	uint m_pm_cookie = 0;
};

bssh_platform *bssh_platform::instance() {
	static bssh_platform_desktop platform;
	return &platform;
}

#include "platform_desktop.moc"

/*
 * "0,42,78" -> a colour, or an invalid one. TDE and KDE 3 write plain
 * decimal triples; anything else is not this format and is not guessed at.
 */
/*
 * "#232323" -> a colour, or an invalid one. LXQt writes hex where TDE
 * writes decimal triples: two spellings of one statement, so both are
 * read rather than one being preferred.
 */
static QColor bssh_colour_from_hex(const QString &value) {
	const QString s = value.trimmed();
	if (s.size() != 7 || !s.startsWith(QLatin1Char('#'))) return QColor();
	bool ok = false;
	const uint n = s.mid(1).toUInt(&ok, 16);
	if (!ok) return QColor();
	return QColor(int((n >> 16) & 0xff), int((n >> 8) & 0xff), int(n & 0xff));
}

static QColor bssh_colour_from_triple(const QString &value) {
	const QStringList parts = value.split(QLatin1Char(','));
	if (parts.size() != 3) return QColor();
	bool ok_r = false, ok_g = false, ok_b = false;
	const int r = parts.at(0).trimmed().toInt(&ok_r);
	const int g = parts.at(1).trimmed().toInt(&ok_g);
	const int b = parts.at(2).trimmed().toInt(&ok_b);
	if (!ok_r || !ok_g || !ok_b) return QColor();
	if (r < 0 || r > 255 || g < 0 || g > 255 || b < 0 || b > 255) return QColor();
	return QColor(r, g, b);
}

QStringList bssh_scheme_sources() {
	const QString home = QDir::homePath();
	const QByteArray xdg = qgetenv("XDG_CONFIG_HOME");
	const QString cfg = xdg.isEmpty() ? home + QStringLiteral("/.config")
	                                   : QString::fromLocal8Bit(xdg);

	QStringList sources;
	sources << cfg + QStringLiteral("/kdeglobals");
	sources << home + QStringLiteral("/.trinity/share/config/kdeglobals");
	sources << home + QStringLiteral("/.kde/share/config/kdeglobals");
	/*
	 * LXQt writes the APPLIED palette into its own config, under
	 * [Palette]. The files under <data>/lxqt/palettes/ are the library
	 * its "Load Palette" dialog reads from, loaded only when
	 * palette_override is on, and nothing records which one is active --
	 * so consulting them can answer about a palette nobody is using.
	 */
	sources << cfg + QStringLiteral("/lxqt/lxqt.conf");
	return sources;
}

bssh_appearance_t bssh_appearance_from_scheme_files(const QStringList &sources) {
	for (const QString &path : sources) {
		QFile file(path);
		if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) continue;

		QTextStream in(&file);
		QString section;
		QColor background;
		QColor foreground;

		while (!in.atEnd()) {
			const QString line = in.readLine().trimmed();
			if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
				section = line.mid(1, line.size() - 2);
				continue;
			}
			/*
			 * Sections matter in both dialects and for the same reason:
			 * the same key names appear in per-application sections, and
			 * taking whichever came last would answer about some other
			 * program's colours.
			 */
			const bool general =
			    section.compare(QStringLiteral("General"), Qt::CaseInsensitive) == 0;
			const bool palette =
			    section.compare(QStringLiteral("Palette"), Qt::CaseInsensitive) == 0;
			if (!general && !palette) continue;
			const int eq = line.indexOf(QLatin1Char('='));
			if (eq < 0) continue;
			const QString key = line.left(eq).trimmed();
			const QString value = line.mid(eq + 1).trimmed();
			if (general) {
				/* TDE and KDE 3. */
				if (key.compare(QStringLiteral("windowBackground"),
				                 Qt::CaseInsensitive) == 0)
					background = bssh_colour_from_triple(value);
				else if (key.compare(QStringLiteral("windowForeground"),
				                      Qt::CaseInsensitive) == 0)
					foreground = bssh_colour_from_triple(value);
				continue;
			}
			/* LXQt. */
			if (key.compare(QStringLiteral("window_color"),
			                 Qt::CaseInsensitive) == 0)
				background = bssh_colour_from_hex(value);
			else if (key.compare(QStringLiteral("window_text_color"),
			                      Qt::CaseInsensitive) == 0)
				foreground = bssh_colour_from_hex(value);
		}

		/* This file had no answer; the next one may. */
		if (!background.isValid() || !foreground.isValid()) continue;

		/*
		 * bssh_relative_luminance rather than a second luminance of our
		 * own: colour_scheme.h already exposes it "because three callers
		 * need them and a fourth copy is how they stop agreeing", and
		 * this is the fourth caller. It is the gamma-correct WCAG form,
		 * where harmonization.md names the raw weighted sum; both answer
		 * the same question -- is the background darker than the text --
		 * and the tree's existing one is the better of the two.
		 */
		const double back = bssh_relative_luminance(background.rgb());
		const double fore = bssh_relative_luminance(foreground.rgb());
		if (back < fore) return BSSH_APPEARANCE_DARK;
		if (back > fore) return BSSH_APPEARANCE_LIGHT;
		/* Identical luminance states nothing. */
		return BSSH_APPEARANCE_UNKNOWN;
	}

	/*
	 * ABSTAIN RATHER THAN GUESS. The two errors are not symmetric: a
	 * wrong light answer is merely plain, while a wrong dark one is
	 * unreadable text on a pale background. No readable file means no
	 * opinion, and the caller keeps its own default.
	 */
	return BSSH_APPEARANCE_UNKNOWN;
}
