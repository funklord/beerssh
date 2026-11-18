#include "stream_test.h"

#include <QByteArray>
#include <QTest>

#include <algorithm>
#include <cstring>
#include <map>
#include <vector>

#include "session/generated/situ.h"
#include "session/stream.h"

namespace {

const size_t MSS = 1100;
/* Any number both ends agree on; a stream begun again draws another. */
const uint64_t EPOCH = 0x5eed;

/* Deterministic, so a failure is the same failure on the next run. */
struct random_t {
	uint64_t state;
	explicit random_t(uint64_t seed) : state(seed * 6364136223846793005ull + 1442695040888963407ull) {}
	unsigned below(unsigned n) {
		state = state * 6364136223846793005ull + 1442695040888963407ull;
		return unsigned((state >> 33) % n);
	}
};

struct link_t {
	unsigned loss_percent = 0;
	unsigned duplicate_percent = 0;
	unsigned delay_ms = 25;
	unsigned jitter_ms = 0;
	bool dead = false;
	/* Segments to lose by their count, one way, A to B. */
	std::vector<unsigned> lose_from_a;
};

struct end_t {
	bssh_stream_t stream;
	std::vector<uint8_t> ring = std::vector<uint8_t>(64 * 1024);
	QByteArray to_write;
	size_t written = 0;
	QByteArray received;
	unsigned sent = 0;

	explicit end_t() { bssh_stream_init(&stream, ring.data(), ring.size(), MSS, 1 << 20, EPOCH); }
};

void deliver(void *context, const uint8_t *bytes, size_t len) {
	static_cast<QByteArray *>(context)->append(reinterpret_cast<const char *>(bytes), int(len));
}

QByteArray pattern(int size, char seed) {
	QByteArray bytes(size, Qt::Uninitialized);
	for (int i = 0; i < size; ++i) bytes[i] = char(seed + i * 7 + i / 251);
	return bytes;
}

/*
 * Runs both ends until each has received everything the other wrote, or
 * `limit_ms` of simulated time passes; returns the simulated time taken.
 */
uint64_t run(end_t &a, end_t &b, const link_t &link, uint64_t seed, uint64_t limit_ms,
             std::vector<uint64_t> *a_send_times = nullptr) {
	random_t random(seed);
	std::multimap<uint64_t, std::pair<int, QByteArray>> flight; /* arrival time, (to end, bytes) */
	uint64_t now = 0;
	uint8_t out[MSS + BSSH_STREAM_HEADER_MAX];

	end_t *ends[2] = { &a, &b };
	while (now <= limit_ms) {
		for (int side = 0; side < 2; ++side) {
			end_t &end = *ends[side];
			if (end.written < size_t(end.to_write.size())) {
				end.written += bssh_stream_write(&end.stream,
				                                 reinterpret_cast<const uint8_t *>(end.to_write.constData())
				                                         + end.written,
				                                 size_t(end.to_write.size()) - end.written);
			}
			for (;;) {
				const size_t len = bssh_stream_poll(&end.stream, now, out, sizeof out);
				if (!len) break;
				const unsigned number = end.sent++;
				if (side == 0 && a_send_times) a_send_times->push_back(now);
				if (link.dead) continue;
				if (side == 0 && std::find(link.lose_from_a.begin(), link.lose_from_a.end(), number)
				                         != link.lose_from_a.end())
					continue;
				if (random.below(100) < link.loss_percent) continue;
				const QByteArray bytes(reinterpret_cast<const char *>(out), int(len));
				const uint64_t at = now + link.delay_ms + (link.jitter_ms ? random.below(link.jitter_ms) : 0);
				flight.insert({ at, { 1 - side, bytes } });
				if (random.below(100) < link.duplicate_percent) flight.insert({ at + 3, { 1 - side, bytes } });
			}
		}

		if (a.received.size() == b.to_write.size() && b.received.size() == a.to_write.size()
		    && !bssh_stream_pending(&a.stream) && !bssh_stream_pending(&b.stream))
			return now;

		/* To the next thing that happens: an arrival or a timer. */
		uint64_t next = limit_ms + 1;
		if (!flight.empty()) next = std::min(next, flight.begin()->first);
		for (end_t *end : ends) {
			const int64_t wait = bssh_stream_wait(&end->stream, now);
			if (wait >= 0) next = std::min(next, now + uint64_t(wait));
		}
		now = std::max(next, now);
		while (!flight.empty() && flight.begin()->first <= now) {
			const auto arrival = *flight.begin();
			flight.erase(flight.begin());
			end_t &to = *ends[arrival.second.first];
			const QByteArray &bytes = arrival.second.second;
			if (bssh_stream_receive(&to.stream, reinterpret_cast<const uint8_t *>(bytes.constData()),
			                        size_t(bytes.size()), now, deliver, &to.received) != 0)
				return UINT64_MAX;
		}
	}
	return UINT64_MAX;
}

} /* namespace */

void stream_test::a_clean_link_carries_both_ways_without_resending() {
	end_t a, b;
	a.to_write = pattern(300000, 'a');
	b.to_write = pattern(120000, 'b');
	const uint64_t took = run(a, b, link_t(), 1, 60000);
	QVERIFY(took != UINT64_MAX);
	QCOMPARE(b.received, a.to_write);
	QCOMPARE(a.received, b.to_write);
	QCOMPARE(a.stream.retransmitted, uint64_t(0));
	QCOMPARE(b.stream.retransmitted, uint64_t(0));
}

/*
 * The case the layer exists for. Every seed is a different pattern of loss,
 * delay, reordering and repetition, and each must end with exactly the bytes
 * written, in order -- not merely as many of them.
 */
void stream_test::a_bad_link_still_delivers_every_byte_once_in_order() {
	link_t link;
	link.loss_percent = 20;
	link.duplicate_percent = 5;
	link.delay_ms = 40;
	link.jitter_ms = 60;
	for (uint64_t seed = 1; seed <= 12; ++seed) {
		end_t a, b;
		a.to_write = pattern(80000, char('a' + seed));
		b.to_write = pattern(20000, char('k' + seed));
		const uint64_t took = run(a, b, link, seed, 600000);
		QVERIFY2(took != UINT64_MAX, qPrintable(QStringLiteral("seed %1: a got %2 of %3, b got %4 of %5")
		                                                .arg(seed).arg(a.received.size()).arg(b.to_write.size())
		                                                .arg(b.received.size()).arg(a.to_write.size())));
		QCOMPARE(b.received, a.to_write);
		QCOMPARE(a.received, b.to_write);
		QVERIFY(a.stream.retransmitted > 0);
	}
}

/*
 * A hole in a run of segments: the segments after it come back as the same
 * acknowledgement three times, and the hole goes again then -- well inside
 * the first timeout, which is a second before anything is measured.
 */
void stream_test::one_lost_segment_is_sent_again_before_the_timeout() {
	end_t a, b;
	a.to_write = pattern(int(MSS) * 40, 'x');
	link_t link;
	link.lose_from_a = { 5 };
	const uint64_t took = run(a, b, link, 7, 60000);
	QVERIFY(took != UINT64_MAX);
	QCOMPARE(b.received, a.to_write);
	QVERIFY2(took < BSSH_STREAM_RTO_INITIAL_MS, qPrintable(QString::number(took)));
	QVERIFY(b.stream.dropped_ahead > 0);
}

/* Nothing comes back: the tries spread out, doubling, and stop crowding a dead link. */
void stream_test::a_dead_link_is_tried_less_and_less_often() {
	end_t a, b;
	a.to_write = pattern(100, 'k');
	link_t link;
	link.dead = true;
	std::vector<uint64_t> times;
	QCOMPARE(run(a, b, link, 3, 120000, &times), UINT64_MAX);
	QVERIFY2(times.size() >= 5 && times.size() <= 10, qPrintable(QString::number(times.size())));
	for (size_t i = 2; i < times.size(); ++i) {
		const uint64_t before = times[i - 1] - times[i - 2];
		const uint64_t gap = times[i] - times[i - 1];
		QVERIFY2(gap >= 2 * before || gap >= BSSH_STREAM_RTO_MAX_MS,
		         qPrintable(QStringLiteral("%1 then %2").arg(before).arg(gap)));
	}
}

void stream_test::the_round_trip_is_measured() {
	end_t a, b;
	a.to_write = pattern(30000, 'r');
	link_t link;
	link.delay_ms = 50;
	QVERIFY(run(a, b, link, 5, 60000) != UINT64_MAX);
	QVERIFY(a.stream.have_rtt);
	QVERIFY2(a.stream.srtt_ms >= 95 && a.stream.srtt_ms <= 110, qPrintable(QString::number(a.stream.srtt_ms)));
	QVERIFY(a.stream.rto_ms >= BSSH_STREAM_RTO_MIN_MS);
}

void stream_test::a_malformed_segment_is_refused_and_changes_nothing() {
	end_t a;
	QByteArray got;
	uint8_t segment[32];

	QCOMPARE(bssh_stream_receive(&a.stream, segment, 0, 0, deliver, &got), -1);

	/* A varint that never ends. */
	std::memset(segment, 0x80, sizeof segment);
	QCOMPARE(bssh_stream_receive(&a.stream, segment, 3, 0, deliver, &got), -1);

	/* Acknowledging bytes nobody sent. */
	size_t len = situ_varint_put(segment, 10, EPOCH);
	len += situ_varint_put(segment + len, 10, 0);
	len += situ_varint_put(segment + len, 10, 5);
	len += situ_varint_put(segment + len, 10, 1000);
	const bssh_stream_t before = a.stream;
	QCOMPARE(bssh_stream_receive(&a.stream, segment, len, 0, deliver, &got), -1);
	QVERIFY(std::memcmp(&before, &a.stream, sizeof before) == 0);
	QVERIFY(got.isEmpty());

	/*
	 * Another stream's, which is what a stream begun again after a drop
	 * meets: the old one's segments, still arriving. Otherwise good.
	 */
	len = situ_varint_put(segment, 10, EPOCH + 1);
	len += situ_varint_put(segment + len, 10, 0);
	len += situ_varint_put(segment + len, 10, 0);
	len += situ_varint_put(segment + len, 10, 1000);
	std::memcpy(segment + len, "hi", 2);
	uint64_t epoch = 0;
	QCOMPARE(bssh_stream_segment_epoch(segment, len + 2, &epoch), 0);
	QCOMPARE(epoch, EPOCH + 1);
	QCOMPARE(bssh_stream_receive(&a.stream, segment, len + 2, 0, deliver, &got), -1);
	QVERIFY(std::memcmp(&before, &a.stream, sizeof before) == 0);
	QVERIFY(got.isEmpty());

	/* And a good one, data and all, to show the refusals were not for everything. */
	len = situ_varint_put(segment, 10, EPOCH);
	len += situ_varint_put(segment + len, 10, 0);
	len += situ_varint_put(segment + len, 10, 0);
	len += situ_varint_put(segment + len, 10, 1000);
	std::memcpy(segment + len, "hi", 2);
	QCOMPARE(bssh_stream_segment_epoch(segment, len + 2, &epoch), 0);
	QCOMPARE(epoch, EPOCH);
	QCOMPARE(bssh_stream_receive(&a.stream, segment, len + 2, 0, deliver, &got), 0);
	QCOMPARE(got, QByteArray("hi"));
}
