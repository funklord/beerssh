#ifndef BSSH_PLATFORM_H
#define BSSH_PLATFORM_H

#include <QObject>
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
 *   M5  network_changed()         signal; ConnectivityManager callbacks on
 *                                 Android, QNetworkInformation on desktop
 *   M6  secret_store()            bssh_secret_store, per project.md sec 11
 */
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
	virtual bool is_touch_primary() const = 0;

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

signals:
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
