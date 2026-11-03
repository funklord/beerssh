package se.vibes.beersshd;

import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Environment;
import android.os.PowerManager;
import android.provider.Settings;

/*
 * Shared storage and the permission that reaches it (project.md sec 19).
 *
 * Three questions the C++ side cannot answer itself: where shared storage
 * is, whether "All files access" has been granted, and how to send the
 * user to the one screen that grants it. Nothing here reads a file.
 */
public class SshdStorage {

	/* The root served: what a file manager calls internal storage. */
	public static String root() {
		return Environment.getExternalStorageDirectory().getAbsolutePath();
	}

	public static boolean hasAccess() {
		return Environment.isExternalStorageManager();
	}

	/*
	 * The per-app page first, which is the toggle itself; the list of all
	 * apps if a vendor's Settings has no per-app page, which some do not.
	 */
	public static void requestAccess(Context context) {
		Intent intent = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
			Uri.parse("package:" + context.getPackageName()));
		intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
		try {
			context.startActivity(intent);
		} catch (ActivityNotFoundException error) {
			Intent list = new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION);
			list.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
			context.startActivity(list);
		}
	}

	/*
	 * Whether Android has been told to leave beersshd out of battery
	 * optimisation (project.md sec 5.4). A foreground service already
	 * keeps the network in Doze; this is for what vendors' battery
	 * managers do beyond Android's own rules, which is what a server
	 * reachable all day most has to fear.
	 */
	public static boolean batteryExempt(Context context) {
		PowerManager power = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
		return power == null || power.isIgnoringBatteryOptimizations(context.getPackageName());
	}

	/*
	 * Android's own yes/no dialog for this app, asked only from the
	 * button the window shows -- Play allows the permission behind a
	 * clear user action (sec 5.4). The list of all apps where a vendor
	 * has no such dialog.
	 */
	public static void requestBatteryExemption(Context context) {
		Intent intent = new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
			Uri.parse("package:" + context.getPackageName()));
		intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
		try {
			context.startActivity(intent);
		} catch (ActivityNotFoundException error) {
			Intent list = new Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS);
			list.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
			context.startActivity(list);
		}
	}
}
