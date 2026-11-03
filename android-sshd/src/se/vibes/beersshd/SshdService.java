package se.vibes.beersshd;

import android.Manifest;
import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.IBinder;

import org.qtproject.qt.android.bindings.QtService;

/*
 * The foreground service that IS beersshd's server (project.md sec 19.6):
 * it runs in its own :server process, and as a QtService it loads Qt
 * there and runs main with --serve, which keeps the server going with no
 * window. The app's window drives it from the app's process with start
 * and stop below, and reads what it reports from two files. A process of
 * its own is what lets it start at boot: a Qt that a service started in
 * the app's process makes QtActivityBase restart the whole process when
 * the app is opened, which would drop every transfer.
 *
 * The same shape as BeerSSH's BsshService, and for the same reasons,
 * which that file states at length: a stop is delivered as a START
 * carrying ACTION_STOP, so the five-second startForeground() contract is
 * always honoured before the service goes; and it is never sticky, since
 * a service restarted without the process it keeps alive serves nothing.
 *
 * What is new is onTimeout. The type is connectedDevice, which has no
 * time limit, so Android should never call it; it is kept from when the
 * type was dataSync, whose six hours in twenty-four did fire it on the
 * Fold (project.md sec 19.6), because a type Android limits later would
 * otherwise kill the app instead of stopping the server. When it comes,
 * Android expects the service gone within seconds. The server is told
 * first, so it stops listening rather than having the process taken from
 * under it, and the notification says why it stopped.
 */
public class SshdService extends QtService {

	private static final String CHANNEL_ID = "beersshd_server";
	private static final int NOTIFICATION_ID = 1;
	private static final String ACTION_STOP = "se.vibes.beersshd.STOP";

	/* Per process: the :server process starts with this, whatever its starter set. */
	private static String s_status = "Starting";

	/* Registered from C++; the server stops when Android's time is up. */
	private static native void nativeTimedOut();

	public static void start(Context context, String status) {
		s_status = status;
		SshdBoot.clearNotice(context);
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

	/*
	 * Foreground before Qt is loaded, which QtService's onCreate does
	 * synchronously. Seen at boot on the API 36 emulator: loading took
	 * long enough that Android logged "Timeout executing service" and the
	 * service never became foreground (startForegroundCount=0) although
	 * the server inside it was listening -- a process Android may freeze
	 * or kill at will. The contract from startForegroundService() runs
	 * from the start call, so it is met here, first.
	 */
	@Override
	public void onCreate() {
		createChannel();
		try {
			startForeground(NOTIFICATION_ID, buildNotification(this, s_status));
		} catch (IllegalStateException | SecurityException refused) {
			/* onStartCommand tries again and reports it. */
		}
		super.onCreate();
	}

	@Override
	public int onStartCommand(Intent intent, int flags, int startId) {
		createChannel();

		/*
		 * Android may refuse: ForegroundServiceStartNotAllowedException (an
		 * IllegalStateException) for a start it counts as from the
		 * background, or when a limited type's time is spent, and a
		 * SecurityException where a type's prerequisite is missing.
		 * Uncaught, any of them is a crash on Start -- and on Stop, which
		 * comes through here too. Caught, the server is told the same thing
		 * onTimeout tells it: Android will not keep it running. Written
		 * from the documentation; not yet seen on a device (project.md
		 * sec 19.6).
		 */
		try {
			startForeground(NOTIFICATION_ID, buildNotification(this, s_status));
		} catch (IllegalStateException | SecurityException refused) {
			if (intent != null && !ACTION_STOP.equals(intent.getAction())) {
				tellNativeTimedOut();
			}
			stopSelf();
			return START_NOT_STICKY;
		}

		if (intent == null || ACTION_STOP.equals(intent.getAction())) {
			stopForeground(STOP_FOREGROUND_REMOVE);
			stopSelf();
		}
		return START_NOT_STICKY;
	}

	@Override
	public void onTimeout(int startId, int fgsType) {
		tellNativeTimedOut();
		stopForeground(STOP_FOREGROUND_REMOVE);
		stopSelf();
	}

	private static void tellNativeTimedOut() {
		try {
			nativeTimedOut();
		} catch (UnsatisfiedLinkError error) {
			/* No native side to tell: stopping is still owed to Android. */
		}
	}

	@Override
	public IBinder onBind(Intent intent) {
		return null;
	}

	/*
	 * Stopping the service is ending the :server process, and with it the
	 * server -- by killing it rather than through QtService's onDestroy.
	 * That one terminates Qt and calls System.exit(0), and on the API 36
	 * emulator the process was still there afterwards, its listener
	 * closed and Qt shut down: the next Start then created the service in
	 * it again, Qt would not start a second time, and Android's main
	 * thread waited for it for ever. Nothing here needs an orderly exit --
	 * the kernel closes the sockets, and a status naming a dead process is
	 * read as stopped (bssh_sshd_remote).
	 */
	@Override
	public void onDestroy() {
		stopForeground(STOP_FOREGROUND_REMOVE);
		android.os.Process.killProcess(android.os.Process.myPid());
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
