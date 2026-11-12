#include "media/media_bytes.h"

#include "model/piece_map.h"

#include <QFile>
#include <QMutexLocker>

#include <sys/stat.h>

bssh_media_bytes::bssh_media_bytes(const QString &staging, qint64 size) : m_staging(staging), m_size(size) {}

bssh_media_bytes::bssh_media_bytes(std::shared_ptr<bssh_piece_map> pieces)
    : m_staging(pieces ? pieces->staging() : QString()), m_size(pieces ? pieces->size() : 0),
      m_pieces(std::move(pieces)) {}

qint64 bssh_media_bytes::on_disk() const {
	struct stat st;
	if (::stat(QFile::encodeName(m_staging).constData(), &st) != 0) return 0;
	return qint64(st.st_size);
}

qint64 bssh_media_bytes::read(qint64 offset, uint8_t *out, qint64 n) {
	if (offset < 0 || n < 0 || !out) return -1;
	if (m_size >= 0 && offset >= m_size) return -1;
	bool finished;
	{
		QMutexLocker held(&m_lock);
		finished = m_finished;
	}
	/* Read what has arrived after the flag: a copy that ended whole has
	 * every byte on disk by the time it says so. */
	qint64 here;
	if (m_pieces) {
		const qint64 run = m_pieces->run_from(offset);
		if (run == 0) {
			m_pieces->want(offset);
			return finished ? -1 : 0;
		}
		here = offset + run;
	} else {
		here = on_disk();
	}
	if (offset >= here) return finished ? -1 : 0;
	QFile file(m_staging);
	if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) return finished ? -1 : 0;
	const qint64 got = file.read(reinterpret_cast<char *>(out), qMin(n, here - offset));
	if (got > 0) return got;
	return finished ? -1 : 0;
}

bool bssh_media_bytes::has(qint64 offset, qint64 n) const {
	if (offset < 0 || n < 0) return false;
	if (m_pieces) return m_pieces->has(offset, n);
	return offset + n <= on_disk();
}

void bssh_media_bytes::want(qint64 offset) {
	if (m_pieces) m_pieces->want(offset);
	QMutexLocker held(&m_lock);
	m_wanted = offset;
}

qint64 bssh_media_bytes::wanted() const {
	QMutexLocker held(&m_lock);
	return m_wanted;
}

void bssh_media_bytes::wait(int ms) {
	/* The map wakes on each piece; its own lock, not this one. */
	if (m_pieces && !is_finished()) {
		m_pieces->wait(ms);
		return;
	}
	QMutexLocker held(&m_lock);
	if (m_finished) return;
	m_more.wait(&m_lock, ms);
}

void bssh_media_bytes::arrived() {
	QMutexLocker held(&m_lock);
	m_more.wakeAll();
}

void bssh_media_bytes::finished(bool whole) {
	if (m_pieces) m_pieces->finish();
	QMutexLocker held(&m_lock);
	m_finished = true;
	m_whole = whole;
	m_more.wakeAll();
}

bool bssh_media_bytes::is_finished() const {
	QMutexLocker held(&m_lock);
	return m_finished;
}
