#include "platform/secret_store.h"

#include <QDir>
#include <QFile>
#include <QJniEnvironment>
#include <QJniObject>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

/*
 * The Android Keystore backend (project.md sec 11).
 *
 * The same shape as the passphrase file beside it -- one encrypted blob
 * holding a JSON object, rather than a file of encrypted rows, because a
 * per-record layout leaks how many credentials exist and which profile
 * each belongs to through the file layout alone -- and one difference
 * that is the whole point: the key is not derived from anything the user
 * typed. It is generated inside the Keystore, cannot be extracted, and on
 * hardware that offers one it lives in a TEE or a secure element.
 *
 * So this store needs no passphrase and is unlocked whenever the app is
 * running. That is not a weaker position than the file's: the file's key
 * is only as good as a passphrase somebody had to invent, and it has to
 * be typed on a phone keyboard often enough to make it short.
 *
 * The encryption itself happens in Java (BsshKeystore) because the key
 * never leaves the Keystore -- there is no handle to hand to OpenSSL.
 * Everything else, the file and the JSON, stays here.
 */

namespace {

const char *const BSSH_KEYSTORE_CLASS = "se/vibes/beerssh/BsshKeystore";

QString store_path() {
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	QDir().mkpath(dir);

	/*
	 * A different name from the passphrase store's secrets.enc, and
	 * deliberately: a device that falls back after having used this one
	 * must not find a file it cannot read where it expects one it can.
	 */
	return dir + QStringLiteral("/secrets.ks");
}

QByteArray call_bytes(const char *method, const QByteArray &argument) {
	QJniEnvironment env;

	jbyteArray in = env->NewByteArray(argument.size());
	if (!in) return QByteArray();
	env->SetByteArrayRegion(in, 0, argument.size(),
	                         reinterpret_cast<const jbyte *>(argument.constData()));

	const QJniObject result = QJniObject::callStaticObjectMethod(
	        BSSH_KEYSTORE_CLASS, method, "([B)[B", in);
	env->DeleteLocalRef(in);

	if (!result.isValid()) return QByteArray();

	jbyteArray out = result.object<jbyteArray>();
	const jsize size = env->GetArrayLength(out);
	QByteArray bytes(size, 0);
	env->GetByteArrayRegion(out, 0, size, reinterpret_cast<jbyte *>(bytes.data()));
	return bytes;
}

} /* namespace */

class bssh_secret_store_keystore : public bssh_secret_store {
public:
	bssh_secret_store_keystore()
	    : m_hardware(QJniObject::callStaticMethod<jboolean>(BSSH_KEYSTORE_CLASS,
	                                                          "isHardwareBacked", "()Z")),
	      m_loaded(false) {}

	QString backend_name() const override {
		/*
		 * Which Keystore this is, said out loud. "Android Keystore"
		 * alone would let a user on an emulator or an old device -- where
		 * the key is held by software with no hardware behind it -- read
		 * the same reassurance as somebody with a secure element. sec 11
		 * spends a paragraph refusing exactly that averaging.
		 */
		return m_hardware
		               ? QStringLiteral("Android Keystore (hardware-backed)")
		               : QStringLiteral("Android Keystore (software-backed on this device)");
	}

	bool is_hardware_backed() const override { return m_hardware; }

	/*
	 * Always, and with nothing to type. The key is released to this app
	 * by the system, so there is no locked state for a user to resolve --
	 * which is why needs_passphrase() is false and unlock() ignores what
	 * it is given.
	 */
	bool is_unlocked() const override { return true; }
	bool needs_passphrase() const override { return false; }

	bool unlock(const QString &) override {
		ensure_loaded();
		return true;
	}

	void lock() override {
		/*
		 * Drops what is in memory. It does NOT make the store
		 * unreadable, and cannot: the key comes back the moment it is
		 * asked for. Reported honestly by is_unlocked() staying true
		 * rather than pretending to a locked state this backend does not
		 * have.
		 */
		m_secrets = QJsonObject();
		m_loaded = false;
	}

	bool store(const QString &id, const QString &secret) override {
		ensure_loaded();
		m_secrets.insert(id, secret);
		return flush();
	}

	QString load(const QString &id) const override {
		const_cast<bssh_secret_store_keystore *>(this)->ensure_loaded();
		return m_secrets.value(id).toString();
	}

	bool erase(const QString &id) override {
		ensure_loaded();
		if (!m_secrets.contains(id)) return true;

		m_secrets.remove(id);
		return flush();
	}

private:
	void ensure_loaded() {
		if (m_loaded) return;
		m_loaded = true;

		QFile file(store_path());
		if (!file.exists() || !file.open(QIODevice::ReadOnly)) return;

		const QByteArray blob = file.readAll();
		file.close();

		/*
		 * An empty answer means the blob was not written by this
		 * device's key -- restored from another phone's backup, or
		 * tampered with. Treated as an empty store rather than as an
		 * error the user has to resolve: there is nothing they could do,
		 * and the passwords behind it are recoverable by typing them
		 * again.
		 */
		const QByteArray plaintext = call_bytes("unwrap", blob);
		if (plaintext.isEmpty()) return;

		m_secrets = QJsonDocument::fromJson(plaintext).object();
	}

	bool flush() {
		const QByteArray plaintext = QJsonDocument(m_secrets).toJson(QJsonDocument::Compact);
		const QByteArray blob = call_bytes("wrap", plaintext);
		if (blob.isEmpty()) return false;

		QFile file(store_path());
		if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;

		const bool ok = file.write(blob) == blob.size();
		file.close();

		/*
		 * The app's own directory is already private to this uid, but
		 * the file is narrowed anyway: this is the one file in the
		 * project whose contents are a credential, even wrapped.
		 */
		if (ok) file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
		return ok;
	}

	bool m_hardware;
	bool m_loaded;
	QJsonObject m_secrets;
};

bssh_secret_store *bssh_make_keystore_secret_store() {
	/*
	 * Asked before anything is built: a device whose Keystore is broken
	 * or refuses to generate a key falls back to the passphrase file
	 * rather than to a store that cannot save anything.
	 */
	if (!QJniObject::callStaticMethod<jboolean>(BSSH_KEYSTORE_CLASS, "isAvailable", "()Z")) {
		return nullptr;
	}
	return new bssh_secret_store_keystore();
}
