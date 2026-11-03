package se.vibes.beerssh;

import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.PowerManager;
import android.provider.Settings;

/*
 * Battery optimisation (project.md sec 5.4): whether Android has been
 * told to leave BeerSSH out of it, and the way to ask.
 *
 * The foreground service keeps a session's network in Doze already; this
 * is for what vendors' battery managers do beyond Android's own rules,
 * which can end a backgrounded session however it is held. Asked only
 * from the button on the Terminal settings page, since Play allows the
 * permission behind a clear user action.
 */
public class BsshBattery {

	public static boolean exempt(Context context) {
		PowerManager power = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
		return power == null || power.isIgnoringBatteryOptimizations(context.getPackageName());
	}

	/* Android's own yes/no dialog; the list of all apps where a vendor has none. */
	public static void request(Context context) {
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
