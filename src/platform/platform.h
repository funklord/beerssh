#ifndef BSSH_PLATFORM_H
#define BSSH_PLATFORM_H

#include <QObject>
#include <QPalette>
#include <Qt>
#include <QString>

/*
 * bssh_platform -- everything that differs between Android and desktop,
 * per project.md sec 3, core principle 3. One implementation per platform
 * in its own file, selected by a scope in beerssh.pro; no #ifdef for
 * platform differences anywhere outside src/platform/.
 *
 * M0 declares only what the skeleton actually uses. The rest is listed
 * here rather than stubbed out, so the intended shape is visible without
 * pretending the methods exist yet:
 *
 *   M4  keep_process_alive(bool)  foreground service / desktop tray residency
 *   M4  set_session_count(int)    notification text, from the registry count
 *   M5  network_changed()         signal; QNetworkInformation on both, whose
 *                                 Android backend is ConnectivityManager
 *   M6  secret_store()            bssh_secret_store, per project.md sec 11
 */
/*
 * Light or dark, as this project's own type (project.md sec 6.2).
 *
 * Deliberately NOT Qt::ColorScheme, which arrived in Qt 6.5 and is
 * therefore absent from the Qt the CI runner ships -- Ubuntu 24.04 is on
 * 6.4.2. The same rule the SSH layer follows applies here: a library type
 * in an interface pins every caller to that library's version, and this
 * one silently pinned the whole app to a Qt newer than the one it is
 * built against.
 *
 * Named for appearance rather than "colour scheme" because a terminal
 * colour scheme (sec 10.2) is an unrelated thing and having two of them
 * under one name is how they get confused.
 */
enum bssh_appearance_t {
	BSSH_APPEARANCE_UNKNOWN,
	BSSH_APPEARANCE_LIGHT,
	BSSH_APPEARANCE_DARK
};

class bssh_platform : public QObject {
	Q_OBJECT

public:
	explicit bssh_platform(QObject *parent = nullptr);
	~bssh_platform() override;

	/* The single instance, of whichever implementation was built in. */
	static bssh_platform *instance();

	/* Short identifier for the about screen and the M0 status line. */
	virtual QString name() const = 0;

	/*
	 * Whether touch is the primary input, which is what gates the
	 * on-screen key row (project.md sec 6.2). Deliberately a capability
	 * question rather than an OS question: "is this Android" gets a
	 * convertible laptop in tablet mode and a phone with a Bluetooth
	 * keyboard wrong in opposite directions.
	 */
	bool is_touch_primary() const;

	/*
	 * Forces the answer, or -1 to go back to asking the platform.
	 *
	 * A testing seam, in the same spirit as bssh_ssh_session::set_factory,
	 * and it earns its place: fourteen widgets decide their arrangement
	 * from this one question, and a desktop build could not render or
	 * test any of their touch forms. `make shots` was labelling a picture
	 * "touch-licences" while showing the DESKTOP arrangement squeezed
	 * into 360px -- a tool whose whole purpose is that you can trust what
	 * you see, showing something the app never does.
	 *
	 * Never called by the app. Threading an explicit arrangement through
	 * fourteen constructors would be a large change made entirely for the
	 * benefit of a renderer.
	 */
	static void force_touch_primary(int touch);

protected:
	virtual bool platform_is_touch_primary() const = 0;

public:

	/*
	 * Keep the process alive with no window on screen (sec 5.1 / 5.5).
	 *
	 * The same call, two entirely different mechanisms: on Android it
	 * starts and stops the foreground service, because a backgrounded app
	 * cannot hold a socket open; on desktop the process simply keeps
	 * running, and the work is stopping Qt from quitting on the last
	 * window and putting an icon in the tray so the user can get back.
	 * Callers state the intent and never learn which happened.
	 */
	virtual void set_keep_alive(bool keep_alive) = 0;

	/*
	 * How many sessions are connected -- notification text on Android,
	 * the tray tooltip on desktop. Taken from the session registry, never
	 * from the view state: panes and tabs come and go, sessions are what
	 * matter (sec 5.2).
	 */
	virtual void set_session_count(int count) = 0;

	/* Reconnecting, reconnect failed, a bell in an unfocused pane. */
	virtual void notify(const QString &title, const QString &body) = 0;

	/*
	 * Where the curated font set lives (sec 9.4).
	 *
	 * The two platforms answer this from genuinely different places -- an
	 * installed share directory or a source checkout on desktop, the
	 * packaged assets on Android, with the rest of the set arriving later
	 * through an asset pack -- and sec 3 says a difference like that
	 * belongs behind this interface rather than in an #ifdef inside the
	 * renderer. bssh_font_catalog asks once at startup and never learns
	 * which kind of path it got.
	 *
	 * May be empty when nothing is bundled, which is not an error: the
	 * catalog then falls back to system-installed families and, failing
	 * that, down sec 9.3's chain.
	 */
	virtual QString font_directory() const = 0;

	/*
	 * The user's OpenSSH directory (~/.ssh), or empty where that is not a
	 * meaningful place (project.md sec 10.6, sec 11).
	 *
	 * A platform question rather than a path question, which is why it is
	 * asked here rather than derived from HomeLocation. Android HAS a
	 * writable home -- the app's own sandbox -- so probing whether ~/.ssh
	 * can be created succeeds there and produces a .ssh directory inside
	 * the APK's private storage that no ssh client will ever read. Empty
	 * means "this platform does not have one", and every caller falls
	 * back to the app's own storage.
	 */
	virtual QString openssh_directory() const = 0;

	/*
	 * A document this app was ASKED to open, or empty (project.md sec
	 * 10.5). A preset file, in practice -- the only file type BeerSSH
	 * knows how to be handed.
	 *
	 * The two platforms are handed one in genuinely different ways: a
	 * path on the command line from a desktop file manager, an intent
	 * carrying a content:// URI on Android. That is the difference sec 3
	 * says belongs behind this interface, and it is also the answer to a
	 * problem the other direction could not solve -- picking a file from
	 * inside the app does not work on Android, while being GIVEN one
	 * does, and being given one is how Android expects this to work
	 * anyway.
	 */
	virtual QString launch_document() const = 0;

	/*
	 * Whether the system is asking for a light or dark interface, or
	 * Unknown when nothing will say (project.md sec 6.2).
	 *
	 * A platform question, and a surprisingly awkward one: Qt's own
	 * QStyleHints::colorScheme() is authoritative where a platform theme
	 * is wired up and returns Unknown where it is not -- which is a plain
	 * Linux session with no platform-theme plugin engaged, where the
	 * desktop's preference is still perfectly readable from the XDG
	 * portal. Answering "Unknown" there and drawing a light window on a
	 * dark desktop is the failure this method exists to avoid, so each
	 * platform is allowed more than one way to find out.
	 *
	 * Qt's setColorScheme() is deliberately NOT used to apply the answer:
	 * measured on this tree's own Qt 6.8.2, it is ignored on Linux under
	 * both offscreen and xcb, with and without a platform theme -- no
	 * signal, no palette change. The palette is set by bssh_theme
	 * instead, which works the same everywhere.
	 */
	virtual bssh_appearance_t system_appearance() const = 0;

protected:
	/*
	 * The palette the PLATFORM handed us, captured at construction --
	 * before bssh_theme has applied one of its own.
	 *
	 * The last-resort half of the detection above reads a palette's
	 * lightness, and reading the CURRENT one is a trap: the app
	 * overwrites it with its own choice, so "what is the system asking
	 * for" would answer with what the app last decided. Force the theme
	 * to Light once and the fallback reports Light as the system
	 * preference for the rest of the session.
	 */
	QPalette startup_palette() const { return m_startup_palette; }

public:

private:
	QPalette m_startup_palette;

public:

signals:
	/*
	 * The system's light/dark preference changed while running. Following
	 * it live is most of what "follows the system" means to a user: a
	 * theme that is only right at startup is one they will call broken.
	 */
	void appearance_changed();

	/*
	 * Connectivity changed and any live session should re-check itself:
	 * ConnectivityManager callbacks on Android, QNetworkInformation on
	 * desktop -- where the case that matters is a laptop waking up, which
	 * kills every TCP connection while looking exactly like a flap.
	 *
	 * The reconnection machine that consumes this is bssh_conn_manager at
	 * M5; this is the platform half of it, and it is emitted whether or
	 * not anything is listening yet.
	 */
	void network_changed();
};

#endif /* BSSH_PLATFORM_H */
