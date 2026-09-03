package se.vibes.beerssh;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.IBinder;

/*
 * The foreground service (project.md sec 5.1, sec 5.2).
 *
 * This is the ONLY Java in the project, and it is here because Android has
 * no way to run a background component without a manifest-registered
 * Service subclass -- a platform limitation, not a design choice. sec 5.2
 * is explicit that this file should be "boilerplate plus one notification
 * plus two calls", and it is: it holds no networking, no protocol, no
 * session state and no UI logic. The SSH connections live in the C++
 * process this keeps alive; this class does not know they exist.
 *
 * Everything it does could be described as: tell Android this process is
 * doing something the user asked for, and show them a notification saying
 * so, because Android will not allow the first without the second.
 */
public class BsshService extends Service {

	private static final String CHANNEL_ID = "beerssh_sessions";
	private static final int NOTIFICATION_ID = 1;

	/*
	 * Stopping is a START. It reads backwards, and it is the whole fix for
	 * a crash on the most ordinary error path there is.
	 *
	 * startForegroundService() opens a contract: this service has about
	 * five seconds to call startForeground(), or Android throws
	 * RemoteServiceException and kills the process. A plain stopService()
	 * arriving first does NOT release that contract -- it destroys the
	 * service before onStartCommand ever runs, so startForeground() is
	 * never called and the app dies. That is exactly what a connection
	 * that fails immediately looks like: the session goes live, the
	 * service is asked to start, the connection is refused a moment
	 * later, and the service is asked to stop before it existed.
	 *
	 * So a stop is delivered as another start, carrying this action.
	 * onStartCommand honours the contract first and stops itself second.
	 */
	private static final String ACTION_STOP = "se.vibes.beerssh.STOP";

	/* Text for the notification, set before start() and on each update. */
	private static String s_status = "Connected";

	/*
	 * Called from C++ (bssh_platform_android). Static, taking the context
	 * explicitly, so the native side needs no reference to an instance
	 * and no lifecycle knowledge -- it says "keep running" and this
	 * translates that into Android's terms.
	 */
	public static void start(Context context, String status) {
		s_status = status;

		Intent intent = new Intent(context, BsshService.class);
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
			context.startForegroundService(intent);
		} else {
			context.startService(intent);
		}
	}

	public static void stop(Context context) {
		Intent intent = new Intent(context, BsshService.class);
		intent.setAction(ACTION_STOP);
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
			context.startForegroundService(intent);
		} else {
			context.startService(intent);
		}
	}

	/*
	 * Updates the notification without restarting anything. The count
	 * comes from the session registry (sec 5.2) -- panes and tabs come and
	 * go, sessions are what this reports.
	 */
	public static void updateStatus(Context context, String status) {
		s_status = status;

		NotificationManager manager =
			(NotificationManager) context.getSystemService(Context.NOTIFICATION_SERVICE);
		if (manager == null) {
			return;
		}

		manager.notify(NOTIFICATION_ID, buildNotification(context, status));
	}

	@Override
	public int onStartCommand(Intent intent, int flags, int startId) {
		createChannel();

		/*
		 * A null intent means ANDROID started this, not the app: it is
		 * the redelivery a system-restarted service gets. Nothing good
		 * can come of continuing.
		 *
		 * The process this service exists to keep alive is gone, and the
		 * SSH sessions went with it -- plain SSH cannot resume one
		 * (sec 5.3), so there is nothing left to hold open. What is left
		 * is a process with a service and no Qt: it shows a notification
		 * saying "Connected" when nothing is, and the launcher can no
		 * longer bring the app up, because Android hands the launch to
		 * this process and the activity comes up over a runtime that
		 * never initialised. Measured on a device 2026-09-04: the app
		 * became unstartable and stayed that way until a force-stop.
		 *
		 * startForeground() is still called first, because the contract
		 * described above applies to this start as much as any other.
		 */
		startForeground(NOTIFICATION_ID, buildNotification(this, s_status));
		if (intent == null) {
			stopForeground(true);
			stopSelf();
			return START_NOT_STICKY;
		}

		if (ACTION_STOP.equals(intent.getAction())) {
			stopForeground(true);
			stopSelf();
			/*
			 * NOT sticky: this one was asked to go away, so Android must
			 * not helpfully bring it back.
			 */
			return START_NOT_STICKY;
		}

		/*
		 * NOT sticky, and the comment this replaces argued itself out of
		 * its own conclusion: it said the sessions do not come back with
		 * the service, "so this is about the service's own lifecycle".
		 * A service whose only job is to keep a process alive for
		 * sessions that no longer exist has no lifecycle worth
		 * preserving.
		 *
		 * What sticky actually bought was a resurrection: the Qt main
		 * loop aborted, Android recreated the service in a fresh process
		 * with no Qt in it, and the app could not be started from the
		 * launcher again -- no crash dialog, because the process was
		 * technically alive. sec 5.1 wants the service to outlive the
		 * UI going to the BACKGROUND; it must not outlive the UI dying.
		 */
		return START_NOT_STICKY;
	}

	@Override
	public void onDestroy() {
		stopForeground(true);
		super.onDestroy();
	}

	@Override
	public IBinder onBind(Intent intent) {
		/* Nothing binds to this: the C++ side is in the same process and
		 * talks to it through the static methods above. */
		return null;
	}

	private void createChannel() {
		if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) {
			return;
		}

		NotificationManager manager =
			(NotificationManager) getSystemService(Context.NOTIFICATION_SERVICE);
		if (manager == null || manager.getNotificationChannel(CHANNEL_ID) != null) {
			return;
		}

		/*
		 * Low importance: this notification exists because Android
		 * requires one, not because the user needs to be interrupted. It
		 * should sit quietly in the shade and say what is running.
		 */
		NotificationChannel channel = new NotificationChannel(
			CHANNEL_ID, "SSH sessions", NotificationManager.IMPORTANCE_LOW);
		channel.setDescription("Shown while BeerSSH is holding SSH sessions open");
		channel.setShowBadge(false);
		manager.createNotificationChannel(channel);
	}

	private static Notification buildNotification(Context context, String status) {
		Intent intent = new Intent(context, org.qtproject.qt.android.bindings.QtActivity.class);
		intent.setFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);

		int flags = PendingIntent.FLAG_UPDATE_CURRENT;
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M) {
			flags |= PendingIntent.FLAG_IMMUTABLE;
		}
		PendingIntent pending = PendingIntent.getActivity(context, 0, intent, flags);

		Notification.Builder builder;
		if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
			builder = new Notification.Builder(context, CHANNEL_ID);
		} else {
			builder = new Notification.Builder(context);
		}

		return builder
			.setContentTitle("BeerSSH")
			.setContentText(status)
			.setSmallIcon(android.R.drawable.stat_sys_upload_done)
			.setContentIntent(pending)
			.setOngoing(true)
			.build();
	}
}
