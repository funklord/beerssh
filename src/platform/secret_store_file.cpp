#include "platform/secret_store.h"

#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

#include <openssl/evp.h>
#include <openssl/rand.h>

/*
 * The passphrase-derived secret store (project.md sec 11).
 *
 * This is the fallback for a machine with no secret service running --
 * headless boxes, minimal window managers, a container -- which on Linux
 * is common enough to plan for rather than treat as an edge case. Where a
 * keyring IS running, libsecret is the better backend and this one should
 * not be selected; that backend needs libsecret-1-dev and is not built
 * yet, which backend_name() says out loud rather than implying otherwise.
 *
 * **The crypto is OpenSSL's, not ours.** PBKDF2-HMAC-SHA256 to turn a
 * passphrase into a key, AES-256-GCM to encrypt with authentication.
 * Writing either by hand would be the one place in this project where
 * getting it subtly wrong is both easy and invisible.
 *
 * The whole store is one encrypted blob rather than a file of encrypted
 * rows: it is a handful of short strings, and a per-record format would
 * leak how many credentials exist and which profile each belongs to
 * through the file layout alone.
 */

namespace {

const int BSSH_KEY_BYTES = 32;      /* AES-256 */
const int BSSH_SALT_BYTES = 16;
const int BSSH_NONCE_BYTES = 12;    /* GCM standard */
const int BSSH_TAG_BYTES = 16;

/*
 * PBKDF2 iterations. High on purpose: this runs once per unlock, so the
 * cost is paid by a person who typed their passphrase and by an attacker
 * for every guess. Argon2id would be better still and is what sec 11
 * names -- OpenSSL 3 exposes it through EVP_KDF, and moving to it is a
 * format-version bump rather than a rewrite, which is why the file
 * carries a version byte.
 */
const int BSSH_PBKDF2_ITERATIONS = 600000;

const char BSSH_MAGIC[4] = {'B', 'S', 'S', 'H'};
const quint8 BSSH_FORMAT_VERSION = 1;

QString store_path() {
	const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
	QDir().mkpath(dir);
	return dir + QStringLiteral("/secrets.enc");
}

QByteArray derive_key(const QString &passphrase, const QByteArray &salt) {
	QByteArray key(BSSH_KEY_BYTES, 0);
	const QByteArray passphrase_utf8 = passphrase.toUtf8();

	if (PKCS5_PBKDF2_HMAC(passphrase_utf8.constData(), passphrase_utf8.size(),
	                       reinterpret_cast<const unsigned char *>(salt.constData()),
	                       salt.size(), BSSH_PBKDF2_ITERATIONS, EVP_sha256(),
	                       key.size(), reinterpret_cast<unsigned char *>(key.data())) != 1) {
		return QByteArray();
	}
	return key;
}

QByteArray encrypt(const QByteArray &plaintext, const QByteArray &key, QByteArray &nonce_out) {
	nonce_out.resize(BSSH_NONCE_BYTES);
	if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce_out.data()), BSSH_NONCE_BYTES) != 1) {
		return QByteArray();
	}

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx) return QByteArray();

	QByteArray out(plaintext.size() + BSSH_TAG_BYTES, 0);
	int length = 0;
	int total = 0;
	bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
	                              reinterpret_cast<const unsigned char *>(key.constData()),
	                              reinterpret_cast<const unsigned char *>(nonce_out.constData())) == 1;

	if (ok) {
		ok = EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &length,
		                        reinterpret_cast<const unsigned char *>(plaintext.constData()),
		                        plaintext.size()) == 1;
		total = length;
	}
	if (ok) {
		ok = EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + total,
		                          &length) == 1;
		total += length;
	}
	if (ok) {
		/* The tag is what makes a wrong passphrase a detectable failure. */
		ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, BSSH_TAG_BYTES,
		                          out.data() + total) == 1;
		total += BSSH_TAG_BYTES;
	}

	EVP_CIPHER_CTX_free(ctx);
	if (!ok) return QByteArray();

	out.resize(total);
	return out;
}

QByteArray decrypt(const QByteArray &ciphertext, const QByteArray &key, const QByteArray &nonce) {
	if (ciphertext.size() < BSSH_TAG_BYTES) return QByteArray();

	const int body = ciphertext.size() - BSSH_TAG_BYTES;

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx) return QByteArray();

	QByteArray out(body, 0);
	int length = 0;
	int total = 0;

	bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
	                              reinterpret_cast<const unsigned char *>(key.constData()),
	                              reinterpret_cast<const unsigned char *>(nonce.constData())) == 1;
	if (ok) {
		ok = EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &length,
		                        reinterpret_cast<const unsigned char *>(ciphertext.constData()),
		                        body) == 1;
		total = length;
	}
	if (ok) {
		ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, BSSH_TAG_BYTES,
		                          const_cast<char *>(ciphertext.constData()) + body) == 1;
	}
	if (ok) {
		/*
		 * Fails on a wrong passphrase AND on a tampered file, which is
		 * the difference between an AEAD and plain encryption: without
		 * the tag, a modified store would decrypt to garbage that some
		 * caller would then treat as a credential.
		 */
		ok = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + total,
		                          &length) == 1;
		total += length;
	}

	EVP_CIPHER_CTX_free(ctx);
	if (!ok) return QByteArray();

	out.resize(total);
	return out;
}

} /* namespace */

class bssh_secret_store_file : public bssh_secret_store {
public:
	bssh_secret_store_file() : m_unlocked(false) {}

	QString backend_name() const override {
		/*
		 * Named plainly, because the UI shows it: this is a file on disk
		 * protected by a passphrase, and calling it anything grander
		 * would invite the user to trust it like a keyring.
		 */
		return QStringLiteral("passphrase-encrypted file (no keyring running)");
	}

	bool is_hardware_backed() const override { return false; }
	bool is_unlocked() const override { return m_unlocked; }
	bool needs_passphrase() const override { return true; }

	bool unlock(const QString &passphrase) override {
		QFile file(store_path());

		if (!file.exists()) {
			/* First use: the passphrase given now is the one that opens
			 * it from here on. */
			m_salt.resize(BSSH_SALT_BYTES);
			if (RAND_bytes(reinterpret_cast<unsigned char *>(m_salt.data()),
			                BSSH_SALT_BYTES) != 1) {
				return false;
			}
			m_key = derive_key(passphrase, m_salt);
			if (m_key.isEmpty()) return false;

			m_secrets = QJsonObject();
			m_unlocked = true;
			return flush();
		}

		if (!file.open(QIODevice::ReadOnly)) return false;
		const QByteArray blob = file.readAll();
		file.close();

		const int header = int(sizeof(BSSH_MAGIC)) + 1 + BSSH_SALT_BYTES + BSSH_NONCE_BYTES;
		if (blob.size() < header) return false;
		if (blob.left(int(sizeof(BSSH_MAGIC))) != QByteArray(BSSH_MAGIC, sizeof(BSSH_MAGIC))) {
			return false;
		}

		int offset = int(sizeof(BSSH_MAGIC));
		const quint8 version = quint8(blob.at(offset++));
		if (version != BSSH_FORMAT_VERSION) return false;

		m_salt = blob.mid(offset, BSSH_SALT_BYTES);
		offset += BSSH_SALT_BYTES;
		const QByteArray nonce = blob.mid(offset, BSSH_NONCE_BYTES);
		offset += BSSH_NONCE_BYTES;

		const QByteArray key = derive_key(passphrase, m_salt);
		if (key.isEmpty()) return false;

		const QByteArray plaintext = decrypt(blob.mid(offset), key, nonce);
		if (plaintext.isEmpty()) return false;   /* wrong passphrase, or tampered */

		m_secrets = QJsonDocument::fromJson(plaintext).object();
		m_key = key;
		m_unlocked = true;
		return true;
	}

	void lock() override {
		m_unlocked = false;
		m_secrets = QJsonObject();

		/*
		 * Overwritten before release. QByteArray does not scrub, so this
		 * is best-effort rather than a guarantee -- worth doing, not
		 * worth trusting.
		 */
		m_key.fill('\0');
		m_key.clear();
	}

	bool store(const QString &id, const QString &secret) override {
		if (!m_unlocked) return false;

		/*
		 * Written first, kept only if the write succeeded.
		 *
		 * This changed m_secrets and then flushed, returning what the
		 * flush said -- so a failed write left memory and disk
		 * disagreeing, with the caller told it had failed. The store
		 * either holds the secret or it does not, and the answer has to
		 * be the one it gave.
		 */
		const QJsonObject before = m_secrets;
		m_secrets.insert(id, secret);
		if (flush()) return true;

		m_secrets = before;
		return false;
	}

	QString load(const QString &id) const override {
		if (!m_unlocked) return QString();
		return m_secrets.value(id).toString();
	}

	bool erase(const QString &id) override {
		if (!m_unlocked) return false;

		/*
		 * The same, and this is the direction that costs. A user asks to
		 * forget a password, is told it could not be done, and it is
		 * gone from memory anyway: the app cannot use it, and the next
		 * successful write for any OTHER id persists the erasure that
		 * was reported as failed.
		 */
		const QJsonObject before = m_secrets;
		m_secrets.remove(id);
		if (flush()) return true;

		m_secrets = before;
		return false;
	}

private:
	bool flush() {
		QByteArray nonce;
		const QByteArray ciphertext =
		        encrypt(QJsonDocument(m_secrets).toJson(QJsonDocument::Compact), m_key, nonce);
		if (ciphertext.isEmpty()) return false;

		QByteArray blob(BSSH_MAGIC, sizeof(BSSH_MAGIC));
		blob.append(char(BSSH_FORMAT_VERSION));
		blob.append(m_salt);
		blob.append(nonce);
		blob.append(ciphertext);

		/*
		 * Replaced atomically, not rewritten in place. This opened the
		 * real file with Truncate, so the existing store was destroyed
		 * BEFORE the replacement was written -- and an interrupted or
		 * failed write then left a truncated blob.
		 *
		 * What that costs is not the one secret being saved. unlock()
		 * rejects a short blob, so EVERY credential is gone, and the UI
		 * says "either it is wrong, or the store has been modified" --
		 * sending somebody to look for an attacker, or to doubt their
		 * own passphrase, when this file truncated itself.
		 *
		 * QSaveFile writes a temporary beside it and renames over the
		 * target on commit(), so a failure at any point leaves the old
		 * store exactly as it was.
		 */
		QSaveFile file(store_path());

		/*
		 * And where a temporary cannot be created -- a writable file in
		 * a directory that is not -- fall back to writing in place
		 * rather than refusing. That is the behaviour this replaces, so
		 * declining it would trade one failure for another; atomicity is
		 * gained wherever the directory allows it and nothing is lost
		 * where it does not.
		 */
		file.setDirectWriteFallback(true);

		if (!file.open(QIODevice::WriteOnly)) return false;

		/*
		 * Owner-only, set on the temporary before it is renamed, so the
		 * secrets are never briefly readable by anybody else. A rename
		 * carries the permissions with it -- asserted by a test, because
		 * a temporary created with the default mode would make this
		 * file world-readable and nothing else would notice.
		 */
		file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);

		if (file.write(blob) != blob.size()) return false;

		/* commit() flushes, renames and reports -- close() would not. */
		return file.commit();
	}

	bool m_unlocked;
	QByteArray m_key;
	QByteArray m_salt;
	QJsonObject m_secrets;
};

bssh_secret_store *bssh_make_file_secret_store() {
	static bssh_secret_store_file store;
	return &store;
}
