package se.vibes.beerssh;

import android.os.Build;
import android.security.keystore.KeyGenParameterSpec;
import android.security.keystore.KeyInfo;
import android.security.keystore.KeyProperties;

import java.security.KeyStore;

import javax.crypto.Cipher;
import javax.crypto.KeyGenerator;
import javax.crypto.SecretKey;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.GCMParameterSpec;

/*
 * The Android Keystore, for wrapping the credential store (project.md
 * sec 11).
 *
 * The second Java file in the project, and it is here for the same reason
 * as the first: a platform limitation rather than a design choice. The
 * Keystore's whole point is that the key never leaves it -- there is no
 * handle to hand to OpenSSL, and no C API that reaches it -- so the
 * encryption has to happen on this side of the fence. sec 5.2's rule is
 * that Java holds no logic worth testing, and this holds none: it wraps
 * bytes and unwraps them.
 *
 * What it protects against, and what it does not. The key is generated in
 * the Keystore and is not extractable, so the encrypted store cannot be
 * decrypted by copying the file off the device -- an attacker needs the
 * device AND the app's identity. On hardware that offers it the key sits
 * in a TEE or a secure element and even a rooted system cannot read it
 * out; isHardwareBacked() reports which case this device is, and the
 * answer is shown to the user rather than assumed.
 *
 * No user authentication is required to use the key. Biometric release is
 * sec 11's [OPEN] item and needs a decision about what happens on a
 * reconnect at 3am with nobody holding the phone; requiring it here would
 * make that decision by accident.
 */
public class BsshKeystore {

	private static final String PROVIDER = "AndroidKeyStore";
	private static final String ALIAS = "beerssh_secrets";
	private static final String TRANSFORMATION = "AES/GCM/NoPadding";

	/* GCM's standard nonce, and the tag length javax.crypto appends. */
	private static final int IV_BYTES = 12;
	private static final int TAG_BITS = 128;

	/*
	 * The key, created on first use. Generated rather than derived: there
	 * is no passphrase here, which is the entire difference between this
	 * backend and the file one.
	 */
	private static SecretKey key() throws Exception {
		KeyStore store = KeyStore.getInstance(PROVIDER);
		store.load(null);

		KeyStore.Entry existing = store.getEntry(ALIAS, null);
		if (existing instanceof KeyStore.SecretKeyEntry) {
			return ((KeyStore.SecretKeyEntry) existing).getSecretKey();
		}

		KeyGenerator generator =
			KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, PROVIDER);
		generator.init(new KeyGenParameterSpec.Builder(
				ALIAS,
				KeyProperties.PURPOSE_ENCRYPT | KeyProperties.PURPOSE_DECRYPT)
			.setBlockModes(KeyProperties.BLOCK_MODE_GCM)
			.setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
			.setKeySize(256)
			.build());

		return generator.generateKey();
	}

	/*
	 * Whether this device will give us a key at all. Called before the
	 * backend is chosen, so a device whose Keystore is broken or absent
	 * falls back to the passphrase file rather than failing to store
	 * anything.
	 */
	public static boolean isAvailable() {
		try {
			return key() != null;
		} catch (Exception e) {
			return false;
		}
	}

	/*
	 * Whether the key is in secure hardware. Asked of the key itself
	 * rather than guessed from the device: an emulator and a phone with a
	 * secure element both have an "Android Keystore", and only one of
	 * them means what the user would take that to mean.
	 */
	public static boolean isHardwareBacked() {
		try {
			SecretKey secret = key();
			SecretKeyFactory factory =
				SecretKeyFactory.getInstance(secret.getAlgorithm(), PROVIDER);
			KeyInfo info = (KeyInfo) factory.getKeySpec(secret, KeyInfo.class);

			if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
				return info.getSecurityLevel() != KeyProperties.SECURITY_LEVEL_SOFTWARE;
			}
			return info.isInsideSecureHardware();
		} catch (Exception e) {
			/* Understating it is the safe direction to be wrong in. */
			return false;
		}
	}

	/* Returns iv || ciphertext-with-tag, or null. */
	public static byte[] wrap(byte[] plaintext) {
		try {
			Cipher cipher = Cipher.getInstance(TRANSFORMATION);
			cipher.init(Cipher.ENCRYPT_MODE, key());

			byte[] iv = cipher.getIV();
			byte[] body = cipher.doFinal(plaintext);

			byte[] out = new byte[iv.length + body.length];
			System.arraycopy(iv, 0, out, 0, iv.length);
			System.arraycopy(body, 0, out, iv.length, body.length);
			return out;
		} catch (Exception e) {
			return null;
		}
	}

	/*
	 * Returns the plaintext, or null for anything that is not exactly
	 * what wrap() produced with this device's key. The GCM tag makes a
	 * tampered or foreign store a detectable failure rather than garbage
	 * some caller would treat as a credential.
	 */
	public static byte[] unwrap(byte[] blob) {
		try {
			if (blob == null || blob.length <= IV_BYTES) return null;

			byte[] iv = new byte[IV_BYTES];
			System.arraycopy(blob, 0, iv, 0, IV_BYTES);

			Cipher cipher = Cipher.getInstance(TRANSFORMATION);
			cipher.init(Cipher.DECRYPT_MODE, key(), new GCMParameterSpec(TAG_BITS, iv));
			return cipher.doFinal(blob, IV_BYTES, blob.length - IV_BYTES);
		} catch (Exception e) {
			return null;
		}
	}
}
