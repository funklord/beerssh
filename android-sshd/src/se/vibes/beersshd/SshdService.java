package se.vibes.beersshd;

import android.Manifest;
import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.IBinder;

/*
 * The foreground service that keeps beersshd's process alive while it
 * listens (project.md sec 19).
 *
 * The same shape as BeerSSH's BsshService, and for the same reasons,
 * which that file states at length: a stop is delivered as a START
 * carrying ACTION_STOP, so the five-second startForeground() contract is
 * always honoured before the service goes; and it is never sticky, since
 * a service restarted without the process it keeps alive serves nothing.
 *
 * What is new is onTimeout. dataSync is limited on Android 15 to six
 * hours in twenty-four; when that runs out Android calls onTimeout and
 * expects the service gone within seconds, or the app is killed. The
 * server is told first, so it stops listening rather than having the
 * process taken from under it, and the notification says why it stopped.
 */
public class SshdService extends Service {

	private static final String CHANNEL_ID = "beersshd_server";
	private static final int NOTIFICATION_ID = 1;
	private static final String ACTION_STOP = "se.vibes.beersshd.STOP";

	private static String s_status = "Serving files";

	/* Registered from C++; the server stops when Android's time is up. */
	private static native void nativeTimedOut();

	public static void start(Context context, String status) {
		s_status = status;
		context.startForegroundService(new Intent(context, SshdService.class));
	}

	/*
	 * Asks to show the notification, once it would matter: at the first
	 * Start. Android 13 hides a foreground service's notification until
	 * this is granted, and the notification is the user's only sign the
	 * phone is serving files. Below 13 there is nothing to ask.
	 *
	 * On the activity's own thread, because a request from any other has
	 * been seen to be dropped (BeerSSH, project.md sec 5.1).
	 */
	public static void askToNotify(Context context) {
		if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU || !(context instanceof Activity)) {
			return;
		}
		final Activity activity = (Activity) context;
		if (activity.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
				== PackageManager.PERMISSION_GRANTED) {
			return;
		}
		activity.runOnUiThread(() -> activity.requestPermissions(
			new String[] { Manifest.permission.POST_NOTIFICATIONS }, 1));
	}

	public static void stop(Context context) {
		Intent intent = new Intent(context, SshdService.class);
		intent.setAction(ACTION_STOP);
		context.startForegroundService(intent);
	}

	public static void updateStatus(Context context, String status) {
		s_status = status;
		NotificationManager manager =
			(NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
		if (manager != null) {
			manager.notify(NOTIFICATION_ID, buildNotification(context, status));
		}
	}

	@Override
	public int onStartCommand(Intent intent, int flags, int startId) {
		createChannel();
		startForeground(NOTIFICATION_ID, buildNotification(this, s_status));

		if (intent == null || ACTION_STOP.equals(intent.getAction())) {
			stopForeground(STOP_FOREGROUND_REMOVE);
			stopSelf();
		}
		return START_NOT_STICKY;
	}

	@Override
	public void onTimeout(int startId, int fgsType) {
		try {
			nativeTimedOut();
		} catch (UnsatisfiedLinkError error) {
			/* No native side to tell: stopping is still owed to Android. */
		}
		stopForeground(STOP_FOREGROUND_REMOVE);
		stopSelf();
	}

	@Override
	public IBinder onBind(Intent intent) {
		return null;
	}

	private void createChannel() {
		NotificationManager manager =
			(NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
		if (manager == null || manager.getNotificationChannel(CHANNEL_ID) != null) {
			return;
		}

		NotificationChannel channel = new NotificationChannel(
			CHANNEL_ID, "File server", NotificationManager.IMPORTANCE_LOW);
		channel.setDescription("Shown while beersshd is listening for SFTP connections");
		channel.setShowBadge(false);
		manager.createNotificationChannel(channel);
	}

	private static Notification buildNotification(Context context, String status) {
		Intent intent = new Intent(context, org.qtproject.qt.android.bindings.QtActivity.class);
		intent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
		PendingIntent pending = PendingIntent.getActivity(context, 0, intent,
			PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE);

		return new Notification.Builder(context, CHANNEL_ID)
			.setContentTitle("beersshd")
			.setContentText(status)
			.setSmallIcon(android.R.drawable.stat_sys_upload_done)
			.setContentIntent(pending)
			.setOngoing(true)
			.build();
	}
}
