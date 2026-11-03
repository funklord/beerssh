package se.vibes.beersshd;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/*
 * Whether the user has the server switched on, and what a reboot does
 * with that (project.md sec 19.6).
 *
 * beersshd is to be reachable whenever it is on, and a phone restarts --
 * an update at night, a flat battery -- without asking. With "Serve
 * again after the phone restarts" on, the server is started here, in its
 * own process, with no tap (the holder, 2026-11-03: "it restarts if
 * configured to do so"). With it off, one notification says the server
 * stopped; a tap opens the app, which starts it again because it is still
 * switched on -- a silent failure, found when a backup is needed, turned
 * into one the user sees the same morning.
 *
 * Kept outside Qt's settings because this reads it at boot, with no Qt
 * loaded.
 */
public class SshdBoot extends BroadcastReceiver {

	private static final String PREFERENCES = "beersshd";
	private static final String SWITCHED_ON = "switched_on";
	private static final String START_AT_BOOT = "start_at_boot";
	private static final String CHANNEL_ID = "beersshd_reboot";
	private static final int NOTIFICATION_ID = 2;

	/*
	 * A file rather than a preference, because two processes change it:
	 * the app's Start and Stop, and the :server process's Stop in the
	 * notification. SharedPreferences are read once per process and kept,
	 * so the app would go on seeing "on" after the notification's Stop and
	 * start the server again. One byte, read fresh every time.
	 */
	private static java.io.File switchedOnFile(Context context) {
		return new java.io.File(context.getFilesDir(), SWITCHED_ON);
	}

	public static boolean switchedOn(Context context) {
		return switchedOnFile(context).exists();
	}

	public static void setSwitchedOn(Context context, boolean on) {
		java.io.File file = switchedOnFile(context);
		try {
			if (on) file.createNewFile();
			else file.delete();
		} catch (java.io.IOException failed) {
			/* Unwritable app storage: nothing better to do than leave it. */
		}
	}

	public static boolean startAtBoot(Context context) {
		return context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
			.getBoolean(START_AT_BOOT, false);
	}

	public static void setStartAtBoot(Context context, boolean on) {
		context.getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)
			.edit().putBoolean(START_AT_BOOT, on).apply();
	}

	/*
	 * The restart notice is over once the server is serving again, however
	 * it came back -- a tap on the notice, the app opened from the
	 * launcher. SshdService.start calls this; left to auto-cancel, opening
	 * the app any other way left it up beside the serving notification.
	 */
	public static void clearNotice(Context context) {
		NotificationManager manager =
			(NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
		if (manager != null) manager.cancel(NOTIFICATION_ID);
	}

	/*
	 * Already serving: the broadcast came late. Seen on a loaded emulator,
	 * where BOOT_COMPLETED arrived a minute after boot and after the app
	 * had been opened and started -- and posted "stopped when the phone
	 * restarted" beside a server that was running. getRunningServices
	 * still answers for the app's own services.
	 */
	@SuppressWarnings("deprecation")
	private static boolean serverRunning(Context context) {
		android.app.ActivityManager manager =
			(android.app.ActivityManager) context.getSystemService(Context.ACTIVITY_SERVICE);
		if (manager == null) return false;
		for (android.app.ActivityManager.RunningServiceInfo info
				: manager.getRunningServices(Integer.MAX_VALUE)) {
			if (SshdService.class.getName().equals(info.service.getClassName())) return true;
		}
		return false;
	}

	@Override
	public void onReceive(Context context, Intent intent) {
		if (!Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction()) || !switchedOn(context)
				|| serverRunning(context)) {
			return;
		}

		/*
		 * Configured to: serve again with no tap. A BOOT_COMPLETED
		 * receiver may start a connectedDevice foreground service on
		 * Android 15 (its restrictions name dataSync and five others),
		 * and the server comes up in its own process waiting for Wi-Fi.
		 * If Android refuses, the notice below is still owed.
		 */
		if (startAtBoot(context)) {
			try {
				SshdService.start(context, "Waiting for Wi-Fi");
				return;
			} catch (RuntimeException refused) {
				/* Fall through to the notice. */
			}
		}

		NotificationManager manager =
			(NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
		if (manager == null) return;

		if (manager.getNotificationChannel(CHANNEL_ID) == null) {
			NotificationChannel channel = new NotificationChannel(
				CHANNEL_ID, "After a restart", NotificationManager.IMPORTANCE_DEFAULT);
			channel.setDescription("Says when the phone restarted while beersshd was serving");
			manager.createNotificationChannel(channel);
		}

		Intent open = new Intent(context, org.qtproject.qt.android.bindings.QtActivity.class);
		open.setFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
		PendingIntent pending = PendingIntent.getActivity(context, 0, open,
			PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

		manager.notify(NOTIFICATION_ID, new Notification.Builder(context, CHANNEL_ID)
			.setContentTitle("beersshd stopped when the phone restarted")
			.setContentText("Tap to serve files again.")
			.setSmallIcon(android.R.drawable.stat_sys_upload_done)
			.setContentIntent(pending)
			.setAutoCancel(true)
			.build());
	}
}
