package se.vibes.beerssh;

import android.app.Activity;
import android.app.Application;
import android.content.Context;
import android.graphics.PixelFormat;
import android.media.Image;
import android.media.ImageReader;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.qtproject.qt.android.BsshSurfaceFeed;

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

	/* The activity while it is stopped, and null otherwise. */
	private static volatile Activity stopped;

	/* Made on first need and kept: Qt may still be drawing into it. */
	private static ImageReader spare;

	/*
	 * A Context rather than an Activity, because Qt's context is whichever
	 * of the two exists: the callbacks are the APPLICATION's either way,
	 * and they report the activity whenever it comes and goes.
	 */
	public static void watch(Context context) {
		Application application = (Application) context.getApplicationContext();
		application.registerActivityLifecycleCallbacks(new BsshLifecycle());
	}

	/*
	 * Frees Qt's main thread from a wait for a surface that cannot come
	 * (project.md sec 5.6). Qt waits with no timeout, and only a surface ends
	 * the wait, and a stopped activity gets none until it is shown again. So
	 * it gets one nobody sees. The suspend queued behind it then takes it
	 * away, and starting again brings the real one. From the hang watchdog,
	 * on its thread; it does nothing while the activity is started, since a
	 * wait there ends by itself.
	 */
	public static void release() {
		final Activity activity = stopped;
		if (activity == null) return;
		activity.runOnUiThread(() -> {
			if (stopped != activity) return;
			if (spare == null) {
				spare = ImageReader.newInstance(64, 64, PixelFormat.RGBA_8888, 2);
				spare.setOnImageAvailableListener(reader -> {
					final Image image = reader.acquireLatestImage();
					if (image != null) image.close();
				}, new Handler(Looper.getMainLooper()));
			}
			final int windows = BsshSurfaceFeed.feed(activity.getWindow().getDecorView(),
			                                         spare.getSurface());
			Log.w("beerssh", "main loop silent while stopped: spare surface to "
			                 + windows + " window(s)");
		});
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
	public void onActivityStarted(Activity activity) {
		stopped = null;
	}

	@Override
	public void onActivityStopped(Activity activity) {
		stopped = activity;
	}

	@Override
	public void onActivitySaveInstanceState(Activity activity, Bundle state) {}

	@Override
	public void onActivityDestroyed(Activity activity) {
		if (stopped == activity) stopped = null;
	}
}
