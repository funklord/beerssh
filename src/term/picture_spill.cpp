#include "term/picture_spill.h"

#include <QDir>
#include <QFile>
#include <QLockFile>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtEndian>

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <zlib.h>

#include <cstring>

namespace {

const int BSSH_SPILL_KEY_BYTES = 32;     /* AES-256 */
const int BSSH_SPILL_NONCE_BYTES = 12;
const int BSSH_SPILL_TAG_BYTES = 16;

/* Magic, width, height, QImage::Format, bytes per line. */
const int BSSH_SPILL_HEADER_BYTES = 20;
const char BSSH_SPILL_MAGIC[4] = {'B', 'S', 'P', '1'};

/*
 * The largest picture this will read back. Far beyond anything the
 * parsers accept (graphics.cpp caps what a remote may send), so it only
 * ever refuses a header that is not one of ours.
 */
const qint64 BSSH_SPILL_MAX_BYTES = qint64(1) << 30;

const QString BSSH_SPILL_PREFIX = QStringLiteral("terminal-");
const QString BSSH_SPILL_LOCK = QStringLiteral("owner.lock");

/*
 * The serial is both the nonce and the additional data. Unique per key
 * because the store never reuses a serial, and binding it into the tag
 * is what makes a file copied onto another picture's name fail to open
 * rather than show the wrong picture.
 */
QByteArray nonce_for(quint64 serial) {
	QByteArray nonce(BSSH_SPILL_NONCE_BYTES, 0);
	qToBigEndian(serial, nonce.data() + 4);
	return nonce;
}

QByteArray seal(const QByteArray &plain, const QByteArray &key, quint64 serial) {
	const QByteArray nonce = nonce_for(serial);

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx) return QByteArray();

	QByteArray out(plain.size() + BSSH_SPILL_TAG_BYTES, 0);
	int length = 0;
	int total = 0;

	bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
	                              reinterpret_cast<const unsigned char *>(key.constData()),
	                              reinterpret_cast<const unsigned char *>(nonce.constData())) == 1;
	if (ok) {
		ok = EVP_EncryptUpdate(ctx, nullptr, &length,
		                        reinterpret_cast<const unsigned char *>(nonce.constData()),
		                        nonce.size()) == 1;
	}
	if (ok) {
		ok = EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &length,
		                        reinterpret_cast<const unsigned char *>(plain.constData()),
		                        plain.size()) == 1;
		total = length;
	}
	if (ok) {
		ok = EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + total,
		                          &length) == 1;
		total += length;
	}
	if (ok) {
		ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, BSSH_SPILL_TAG_BYTES,
		                          out.data() + total) == 1;
		total += BSSH_SPILL_TAG_BYTES;
	}

	EVP_CIPHER_CTX_free(ctx);
	if (!ok) return QByteArray();

	out.resize(total);
	return out;
}

QByteArray open_sealed(const QByteArray &sealed, const QByteArray &key, quint64 serial) {
	if (sealed.size() < BSSH_SPILL_TAG_BYTES) return QByteArray();

	const QByteArray nonce = nonce_for(serial);
	const int body = sealed.size() - BSSH_SPILL_TAG_BYTES;

	EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
	if (!ctx) return QByteArray();

	QByteArray out(body, 0);
	int length = 0;
	int total = 0;

	bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr,
	                              reinterpret_cast<const unsigned char *>(key.constData()),
	                              reinterpret_cast<const unsigned char *>(nonce.constData())) == 1;
	if (ok) {
		ok = EVP_DecryptUpdate(ctx, nullptr, &length,
		                        reinterpret_cast<const unsigned char *>(nonce.constData()),
		                        nonce.size()) == 1;
	}
	if (ok) {
		ok = EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char *>(out.data()), &length,
		                        reinterpret_cast<const unsigned char *>(sealed.constData()),
		                        body) == 1;
		total = length;
	}
	if (ok) {
		ok = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, BSSH_SPILL_TAG_BYTES,
		                          const_cast<char *>(sealed.constData()) + body) == 1;
	}
	if (ok) {
		/* The tag: a wrong key, a damaged file or a renamed one all end here. */
		ok = EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char *>(out.data()) + total,
		                          &length) == 1;
		total += length;
	}

	EVP_CIPHER_CTX_free(ctx);
	if (!ok) return QByteArray();

	out.resize(total);
	return out;
}

/*
 * The pixels as they are, compressed at zlib's fastest level. A PNG would
 * be smaller and costs several times as long, and this runs on the thread
 * that is scrolling -- spilling is on the way out of the screen and
 * reading is on the way back in.
 */
QByteArray pack(const QImage &image) {
	const qint64 raw = qint64(image.bytesPerLine()) * image.height();
	if (raw <= 0 || raw > BSSH_SPILL_MAX_BYTES) return QByteArray();

	uLongf packed_size = compressBound(uLong(raw));
	QByteArray out(BSSH_SPILL_HEADER_BYTES + int(packed_size), 0);

	std::memcpy(out.data(), BSSH_SPILL_MAGIC, 4);
	qToLittleEndian(qint32(image.width()), out.data() + 4);
	qToLittleEndian(qint32(image.height()), out.data() + 8);
	qToLittleEndian(qint32(image.format()), out.data() + 12);
	qToLittleEndian(qint32(image.bytesPerLine()), out.data() + 16);

	if (compress2(reinterpret_cast<Bytef *>(out.data()) + BSSH_SPILL_HEADER_BYTES, &packed_size,
	              image.constBits(), uLong(raw), 1) != Z_OK) {
		return QByteArray();
	}

	out.resize(BSSH_SPILL_HEADER_BYTES + int(packed_size));
	return out;
}

QImage unpack(const QByteArray &packed) {
	if (packed.size() < BSSH_SPILL_HEADER_BYTES) return QImage();
	if (std::memcmp(packed.constData(), BSSH_SPILL_MAGIC, 4) != 0) return QImage();

	const qint32 width = qFromLittleEndian<qint32>(packed.constData() + 4);
	const qint32 height = qFromLittleEndian<qint32>(packed.constData() + 8);
	const qint32 format = qFromLittleEndian<qint32>(packed.constData() + 12);
	const qint32 bytes_per_line = qFromLittleEndian<qint32>(packed.constData() + 16);

	if (width <= 0 || height <= 0 || bytes_per_line <= 0) return QImage();
	if (format <= QImage::Format_Invalid || format >= QImage::NImageFormats) return QImage();

	const qint64 raw = qint64(bytes_per_line) * height;
	if (raw > BSSH_SPILL_MAX_BYTES) return QImage();

	QImage image(width, height, QImage::Format(format));
	if (image.isNull() || image.bytesPerLine() != bytes_per_line) return QImage();

	uLongf out_size = uLongf(raw);
	if (uncompress(image.bits(), &out_size,
	               reinterpret_cast<const Bytef *>(packed.constData()) + BSSH_SPILL_HEADER_BYTES,
	               uLong(packed.size() - BSSH_SPILL_HEADER_BYTES)) != Z_OK) {
		return QImage();
	}
	if (qint64(out_size) != raw) return QImage();

	return image;
}

} /* namespace */

bssh_picture_spill::bssh_picture_spill(const QString &base) : m_base(base) {
}

bssh_picture_spill::~bssh_picture_spill() {
	/*
	 * The lock first, then the directory. Safe in that order because
	 * sweep_abandoned leaves a directory with no lock file alone; the
	 * other order has QTemporaryDir delete the lock file from under
	 * QLockFile, which then complains that it could not remove its own.
	 */
	m_lock.reset();
	m_dir.reset();
}

QString bssh_picture_spill::default_base() {
	const QString cache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
	if (cache.isEmpty()) return QString();
	return cache + QStringLiteral("/pictures");
}

QString bssh_picture_spill::directory() const {
	return m_dir ? m_dir->path() : QString();
}

QString bssh_picture_spill::path_of(quint64 serial) const {
	return m_dir->path() + QLatin1Char('/') + QString::number(serial);
}

bool bssh_picture_spill::ensure_directory() {
	if (m_dir) return true;
	if (m_base.isEmpty()) return false;

	if (m_key.isEmpty()) {
		m_key.resize(BSSH_SPILL_KEY_BYTES);
		if (RAND_bytes(reinterpret_cast<unsigned char *>(m_key.data()), BSSH_SPILL_KEY_BYTES) != 1) {
			m_key.clear();
			return false;
		}
	}

	if (!QDir().mkpath(m_base)) return false;
	QFile::setPermissions(m_base, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);

	/* QTemporaryDir makes it owner-only, and removes it on the way out. */
	std::unique_ptr<QTemporaryDir> dir(
	        new QTemporaryDir(m_base + QLatin1Char('/') + BSSH_SPILL_PREFIX + QStringLiteral("XXXXXX")));
	if (!dir->isValid()) return false;

	std::unique_ptr<QLockFile> lock(new QLockFile(dir->path() + QLatin1Char('/') + BSSH_SPILL_LOCK));
	lock->setStaleLockTime(0);
	if (!lock->tryLock(0)) return false;

	m_dir = std::move(dir);
	m_lock = std::move(lock);
	return true;
}

bool bssh_picture_spill::write(quint64 serial, const QImage &image) {
	if (image.isNull() || !ensure_directory()) return false;

	const QByteArray sealed = seal(pack(image), m_key, serial);
	if (sealed.isEmpty()) return false;

	QFile file(path_of(serial));
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate,
	               QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
		return false;
	}
	if (file.write(sealed) != sealed.size()) {
		file.close();
		file.remove();
		return false;
	}
	file.close();

	remove(serial);
	m_sizes.insert(serial, sealed.size());
	m_bytes += sealed.size();
	return true;
}

QImage bssh_picture_spill::read(quint64 serial) const {
	if (!m_dir || !m_sizes.contains(serial)) return QImage();

	QFile file(path_of(serial));
	if (!file.open(QIODevice::ReadOnly)) return QImage();

	return unpack(open_sealed(file.readAll(), m_key, serial));
}

void bssh_picture_spill::remove(quint64 serial) {
	const auto found = m_sizes.constFind(serial);
	if (found == m_sizes.constEnd()) return;

	m_bytes -= found.value();
	m_sizes.erase(found);
	QFile::remove(path_of(serial));
}

int bssh_picture_spill::sweep_abandoned(const QString &base) {
	if (base.isEmpty()) return 0;

	const QDir root(base);
	if (!root.exists()) return 0;

	int removed = 0;
	const QStringList names =
	        root.entryList(QStringList() << (BSSH_SPILL_PREFIX + QLatin1Char('*')),
	                       QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);

	for (const QString &name : names) {
		const QString path = root.filePath(name);
		const QString lock_path = path + QLatin1Char('/') + BSSH_SPILL_LOCK;

		/*
		 * No lock file is not "abandoned": it is also what a directory
		 * looks like in the instant between another instance making it
		 * and locking it. Leaving an empty directory is the cheap error.
		 */
		if (!QFile::exists(lock_path)) continue;

		QLockFile lock(lock_path);
		lock.setStaleLockTime(0);

		/* Succeeds only when the owner is gone: a live one holds it. */
		if (!lock.tryLock(0)) continue;
		lock.unlock();

		if (QDir(path).removeRecursively()) ++removed;
	}

	return removed;
}
