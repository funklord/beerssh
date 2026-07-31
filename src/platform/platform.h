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
};

#endif /* BSSH_PLATFORM_H */
