#include "model/piece_map.h"

#include <QFile>
#include <QMutexLocker>

#include <sys/stat.h>

bssh_piece_map::bssh_piece_map(const QString &staging, qint64 size, qint64 piece)
    : m_staging(staging), m_size(qMax<qint64>(0, size)), m_piece(qMax<qint64>(1, piece)),
      m_have(size_t(pieces()), false) {}

qint64 bssh_piece_map::on_disk() const {
	struct stat st;
	if (::stat(QFile::encodeName(m_staging).constData(), &st) != 0) return 0;
	return qint64(st.st_size);
}

qint64 bssh_piece_map::run_from(qint64 offset) const {
	if (offset < 0 || offset >= m_size) return 0;
	QMutexLocker held(&m_lock);
	if (m_in_order) return qMax<qint64>(0, qMin(on_disk(), m_size) - offset);
	qint64 index = offset / m_piece;
	const qint64 last = pieces();
	while (index < last && m_have[size_t(index)]) ++index;
	/* Nothing when the offset's own piece is missing, not a negative run. */
	return qMax<qint64>(0, qMin(index * m_piece, m_size) - offset);
}

bool bssh_piece_map::has(qint64 offset, qint64 n) const {
	if (offset < 0 || n < 0 || offset > m_size) return false;
	if (n == 0) return true;
	if (offset == m_size) return false;
	return run_from(offset) >= qMin(n, m_size - offset);
}

void bssh_piece_map::want(qint64 offset) {
	QMutexLocker held(&m_lock);
	m_wanted = qBound<qint64>(0, offset, m_size);
}

qint64 bssh_piece_map::wanted() const {
	QMutexLocker held(&m_lock);
	return m_wanted;
}

void bssh_piece_map::wait(int ms) {
	QMutexLocker held(&m_lock);
	if (m_finished) return;
	m_more.wait(&m_lock, ms);
}

qint64 bssh_piece_map::next_missing() const {
	QMutexLocker held(&m_lock);
	const qint64 last = pieces();
	if (m_count >= last) return -1;
	for (qint64 i = m_wanted / m_piece; i < last; ++i)
		if (!m_have[size_t(i)]) return i;
	for (qint64 i = 0; i < last; ++i)
		if (!m_have[size_t(i)]) return i;
	return -1;
}

void bssh_piece_map::mark(qint64 index) {
	QMutexLocker held(&m_lock);
	if (index < 0 || index >= pieces() || m_have[size_t(index)]) return;
	m_have[size_t(index)] = true;
	++m_count;
	m_more.wakeAll();
}

void bssh_piece_map::set_in_order() {
	QMutexLocker held(&m_lock);
	m_in_order = true;
}

bool bssh_piece_map::in_order() const {
	QMutexLocker held(&m_lock);
	return m_in_order;
}

void bssh_piece_map::finish() {
	QMutexLocker held(&m_lock);
	m_finished = true;
	m_more.wakeAll();
}

bool bssh_piece_map::complete() const {
	QMutexLocker held(&m_lock);
	if (m_in_order) return on_disk() >= m_size;
	return m_count >= pieces();
}
