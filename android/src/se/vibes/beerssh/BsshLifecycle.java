package se.vibes.beerssh;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.os.Bundle;

/*
 * Tells the hang watchdog (project.md sec 5.1) when the window comes in
 * front of the user and when it goes away.
 *
 * Java because ActivityLifecycleCallbacks is an interface only Java can
 * implement -- sec 5.2's rule, Java only where the platform forces it, and
 * this holds no logic: two callbacks, one native call each.
 *
 * WHY NOT QT'S OWN STATE, which is what the watchdog read first. Qt learns
 * of a resume in QtActivityBase.onResume() AFTER super.onResume(), and its
 * thread applies it asynchronously -- after the window work where the
 * deadlock is suspected -- so a wedged Qt thread never hears that the app
 * came back. And on pause Qt stops delivering timers before it posts the
 * state, so the watchdog's heartbeat stopped still saying "foreground".
 * These callbacks run on Android's UI thread: onActivityResumed from
 * super.onResume(), before any of Qt's resume work, and onActivityPaused
 * from onPause(). Neither goes near Qt's event loop.
 */
public class BsshLifecycle implements Application.ActivityLifecycleCallbacks {

	/* Registered from C++ before watch() is called. */
	private static native void nativeForeground(boolean foreground);

	/*
	 * A Context rather than an Activity, because Qt's context is whichever
	 * of the two exists: the callbacks are the APPLICATION's either way,
	 * and they report the activity whenever it comes and goes.
	 */
	public static void watch(Context context) {
		Application application = (Application) context.getApplicationContext();
		application.registerActivityLifecycleCallbacks(new BsshLifecycle());
	}

	@Override
	public void onActivityResumed(Activity activity) {
		nativeForeground(true);
	}

	@Override
	public void onActivityPaused(Activity activity) {
		nativeForeground(false);
	}

	@Override
	public void onActivityCreated(Activity activity, Bundle state) {}

	@Override
	public void onActivityStarted(Activity activity) {}

	@Override
	public void onActivityStopped(Activity activity) {}

	@Override
	public void onActivitySaveInstanceState(Activity activity, Bundle state) {}

	@Override
	public void onActivityDestroyed(Activity activity) {}
}
