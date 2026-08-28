package se.vibes.beerssh;

import android.app.Activity;
import android.content.Context;
import android.hardware.biometrics.BiometricManager;
import android.hardware.biometrics.BiometricPrompt;
import android.os.Build;
import android.os.CancellationSignal;
import android.os.Handler;
import android.os.Looper;

import java.util.concurrent.Executor;

/*
 * The biometric prompt, for project.md sec 11's per-host release of a
 * stored password.
 *
 * The third Java file here and, like the other two, it exists because of
 * a platform limitation rather than a design choice: BiometricPrompt is
 * an Android UI object with no C API, so the prompt has to be raised on
 * this side of the fence. sec 5.2's rule is that Java holds no logic
 * worth testing, and this holds none -- it asks the system a question and
 * hands the answer back.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO. It does not touch the Keystore key.
 * BsshKeystore's comment records why the key requires no user
 * authentication, and binding the two together here would make that
 * decision by accident. This prompt gates whether the APP releases a
 * password it has already decrypted; it is not a second lock on the
 * cipher.
 *
 * API 29 AND ABOVE ONLY, and the option is absent below that rather than
 * approximated. BiometricPrompt itself arrived in 28, but BiometricManager
 * -- the only way to ask whether anything is actually ENROLLED -- came in
 * 29. On 28 the honest answers available are "the device has a fingerprint
 * reader" and "somebody has registered a finger", and only the second is
 * the question. sec 11 says absent rather than faked, and a prompt offered
 * to a device with no enrolled biometric is the faking: the user turns the
 * setting on and their password becomes unreachable.
 */
public class BsshBiometric {

	/* Set from C++ once the native method is registered. */
	private static native void nativeResult(long token, boolean authenticated);

	/*
	 * Whether a prompt would actually succeed on this device: hardware
	 * present, and a biometric enrolled. Both halves matter -- a reader
	 * with nothing registered raises a prompt that can only be
	 * cancelled.
	 */
	public static boolean available(Context context) {
		if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) return false;
		if (context == null) return false;

		BiometricManager manager = context.getSystemService(BiometricManager.class);
		if (manager == null) return false;

		return manager.canAuthenticate() == BiometricManager.BIOMETRIC_SUCCESS;
	}

	/*
	 * Raise the prompt. Must be called on the Android UI thread, which is
	 * the caller's business -- BiometricPrompt needs a Looper and the Qt
	 * thread has none.
	 *
	 * Every path answers exactly once, including the ones that look like
	 * they need no answer. A prompt that is dismissed without a result is
	 * a caller waiting for a callback that never comes, and the caller is
	 * holding a password it will not release until it hears something.
	 */
	public static void authenticate(Activity activity, String title, String reason,
	                                 final long token) {
		if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || activity == null) {
			nativeResult(token, false);
			return;
		}

		final Executor executor = new Executor() {
			private final Handler handler = new Handler(Looper.getMainLooper());

			@Override
			public void execute(Runnable command) {
				handler.post(command);
			}
		};

		BiometricPrompt.Builder builder = new BiometricPrompt.Builder(activity);
		builder.setTitle(title);
		builder.setDescription(reason);

		/*
		 * A way out that is not the back button. Android requires one on
		 * this API, and it is the right shape anyway: sec 11's fallback
		 * is the typed password, so cancelling here has to reach the
		 * caller as "not authenticated" rather than as silence.
		 */
		builder.setNegativeButton("Use password", executor,
		                           (dialog, which) -> nativeResult(token, false));

		BiometricPrompt prompt = builder.build();

		prompt.authenticate(new CancellationSignal(), executor,
		                     new BiometricPrompt.AuthenticationCallback() {
			@Override
			public void onAuthenticationSucceeded(BiometricPrompt.AuthenticationResult result) {
				nativeResult(token, true);
			}

			@Override
			public void onAuthenticationError(int code, CharSequence message) {
				nativeResult(token, false);
			}

			/*
			 * NOT an answer. A finger that did not match leaves the
			 * prompt up and the user may try again; reporting failure
			 * here would release the caller while the dialog is still
			 * on screen. The error callback above is what ends it.
			 */
			@Override
			public void onAuthenticationFailed() {
			}
		});
	}
}
