package se.vibes.beerssh;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;

/*
 * HTTPS for the font download (project.md sec 9.4): a resource's length,
 * and a byte range of it.
 *
 * Here and not in C++ because Qt on Android has no TLS backend -- it
 * dlopens a libssl the platform does not offer to apps, and the kit ships
 * none -- so every QNetworkAccessManager request to an https address fails
 * with "TLS initialization failed". Measured on the emulator 2026-09-28,
 * after the download had passed every desktop test. The alternative was
 * bbq-predictor's: shared OpenSSL in the APK, native libraries extracted,
 * Qt's TLS plugin loaded by hand -- every native library on disk twice.
 *
 * So it is sec 5.2's exception, the way BsshKeystore is: the platform
 * forces the side of the fence, and nothing worth testing crosses it.
 * The system's certificate store and redirects come with the platform;
 * which ranges to ask for, what they must contain and every check on the
 * bytes stay in bssh_font_download, which the suite drives.
 *
 * Called from a worker thread, never the UI thread -- Android refuses
 * network access there outright. Failures come back as a null or -1 with
 * the reason in lastError(), per thread, so an exception never has to
 * cross into native code.
 */
public class BsshHttp {

	private static final int TIMEOUT_MS = 30000;

	/*
	 * The most a whole-resource fetch will hold. Only a licence is fetched
	 * whole, and those are kilobytes; this refuses a server that answers
	 * with something else rather than filling the heap.
	 */
	private static final long WHOLE_LIMIT = 16L * 1024 * 1024;

	private static final ThreadLocal<String> error = new ThreadLocal<>();

	public static String lastError() {
		String reason = error.get();
		return reason == null ? "" : reason;
	}

	private static HttpURLConnection open(String address, String method) throws Exception {
		HttpURLConnection connection = (HttpURLConnection) new URL(address).openConnection();
		connection.setRequestMethod(method);
		connection.setConnectTimeout(TIMEOUT_MS);
		connection.setReadTimeout(TIMEOUT_MS);
		connection.setInstanceFollowRedirects(true);

		/*
		 * Without this the platform asks for gzip and unpacks it
		 * silently, and a byte range of the packed stream is not a byte
		 * range of the file.
		 */
		connection.setRequestProperty("Accept-Encoding", "identity");
		return connection;
	}

	private static String describe(Exception e) {
		String message = e.getMessage();
		return message == null ? e.getClass().getSimpleName() : message;
	}

	/* The length, or -1. */
	public static long size(String address) {
		error.remove();
		HttpURLConnection connection = null;
		try {
			connection = open(address, "HEAD");
			int code = connection.getResponseCode();
			if (code != HttpURLConnection.HTTP_OK) {
				error.set("the server answered " + code);
				return -1;
			}
			long length = connection.getContentLengthLong();
			if (length < 0) error.set("the server did not say how long it is");
			return length;
		} catch (Exception e) {
			error.set(describe(e));
			return -1;
		} finally {
			if (connection != null) connection.disconnect();
		}
	}

	/*
	 * `length` bytes from `from`, or the whole resource when `length` is
	 * negative. A range must come back as 206: a server that ignores the
	 * range and starts sending the whole 165 MB is refused, not read.
	 */
	public static byte[] fetch(String address, long from, long length) {
		error.remove();
		HttpURLConnection connection = null;
		try {
			connection = open(address, "GET");
			if (length >= 0)
				connection.setRequestProperty("Range", "bytes=" + from + "-" + (from + length - 1));

			int code = connection.getResponseCode();
			int wanted = length >= 0 ? HttpURLConnection.HTTP_PARTIAL : HttpURLConnection.HTTP_OK;
			if (code != wanted) {
				error.set(code == HttpURLConnection.HTTP_OK
				          ? "the server ignored the byte range"
				          : "the server answered " + code);
				return null;
			}

			long limit = length >= 0 ? length : WHOLE_LIMIT;
			ByteArrayOutputStream out = new ByteArrayOutputStream(
			        (int) Math.min(limit, 1024 * 1024));
			byte[] buffer = new byte[64 * 1024];
			long total = 0;

			try (InputStream in = connection.getInputStream()) {
				int read;
				while ((read = in.read(buffer)) > 0) {
					total += read;
					if (total > limit) {
						error.set("the server sent more than was asked for");
						return null;
					}
					out.write(buffer, 0, read);
				}
			}
			return out.toByteArray();
		} catch (Exception e) {
			error.set(describe(e));
			return null;
		} finally {
			if (connection != null) connection.disconnect();
		}
	}
}
