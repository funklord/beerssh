#include "output_log_test.h"

#include "session/output_log.h"

#include <QByteArray>
#include <QTest>

namespace {

struct small_log_t {
	uint8_t bytes[8];
	bssh_output_log_mark_t marks[4];
	bssh_output_log_t log;

	small_log_t() { bssh_output_log_init(&log, bytes, sizeof(bytes), marks, 4); }

	size_t append(const char *text, uint64_t at_ms = 1) {
		return bssh_output_log_append(&log, reinterpret_cast<const uint8_t *>(text), strlen(text), at_ms);
	}

	QByteArray read(uint64_t offset, size_t capacity = 64) {
		QByteArray out(int(capacity), '\0');
		size_t len = 0;
		bssh_output_log_read(&log, offset, reinterpret_cast<uint8_t *>(out.data()), capacity, &len);
		return out.left(int(len));
	}

	bssh_output_log_answer_t answer(uint64_t offset) {
		uint8_t out[1];
		size_t len = 0;
		return bssh_output_log_read(&log, offset, out, 1, &len);
	}
};

}

void output_log_test::reads_back_what_was_written_across_the_wrap() {
	small_log_t small;
	QCOMPARE(small.append("abcdef"), size_t(6));
	QCOMPARE(small.read(0), QByteArray("abcdef"));

	/* Offsets 8 and 9 land at the front of the array. */
	QCOMPARE(small.append("ghij"), size_t(4));
	QCOMPARE(small.read(2), QByteArray("cdefghij"));
	QCOMPARE(small.read(6, 3), QByteArray("ghi"));
}

void output_log_test::says_gone_and_absent_apart() {
	small_log_t small;
	QCOMPARE(small.answer(0), BSSH_OUTPUT_LOG_ABSENT);

	small.append("abcdefghij");
	QCOMPARE(small.answer(1), BSSH_OUTPUT_LOG_GONE);
	QCOMPARE(small.answer(2), BSSH_OUTPUT_LOG_OK);
	QCOMPARE(small.answer(9), BSSH_OUTPUT_LOG_OK);
	QCOMPARE(small.answer(10), BSSH_OUTPUT_LOG_ABSENT);
}

void output_log_test::live_evicts_the_oldest_and_counts_what_the_client_lost() {
	small_log_t small;
	small.append("abcdef");
	bssh_output_log_ack(&small.log, 4);

	/* 2 evicted that the client had, and 0 it lacked. */
	small.append("ghij");
	QCOMPARE(small.log.start, uint64_t(2));
	QCOMPARE(small.log.lost, uint64_t(0));

	/* Up to offset 6: 4 and 5 were never the client's. */
	small.append("klmn");
	QCOMPARE(small.log.start, uint64_t(6));
	QCOMPARE(small.log.lost, uint64_t(2));

	/* Counted once, not again from the old acknowledgement. */
	small.append("o");
	QCOMPARE(small.log.lost, uint64_t(3));
}

void output_log_test::the_log_knows_the_line_it_starts_at() {
	/*
	 * sec 5.6: a person can picture lines and not bytes. Counted as they leave,
	 * since nothing else remembers them. small_log_t holds 8 bytes.
	 */
	small_log_t small;
	small.append("a\nb\nc\nd\n");
	QCOMPARE(small.log.start_line, uint64_t(0));

	/* Pushes out "a\nb", so one newline has left. */
	small.append("xyz");
	QCOMPARE(small.log.start, uint64_t(3));
	QCOMPARE(small.log.start_line, uint64_t(1));

	/* 5 bytes out, "\nc\nd\n". */
	small.append("12345");
	QCOMPARE(small.log.start, uint64_t(8));
	QCOMPARE(small.log.start_line, uint64_t(4));

	/* A write larger than the log pushes out part of itself as well. */
	small.append("p\nq\nr\ns\nt\nu\n");
	QCOMPARE(small.log.start, uint64_t(20));
	QCOMPARE(small.log.start_line, uint64_t(6));

	/* Across the end of the ring: offsets 6 to 9 sit at both ends of it. */
	small_log_t wrapped;
	wrapped.append("abcdef\n\n");
	wrapped.append("\n\nzzzz");
	QCOMPARE(wrapped.log.start_line, uint64_t(0));
	wrapped.append("wxyz");
	QCOMPARE(wrapped.log.start, uint64_t(10));
	QCOMPARE(wrapped.log.start_line, uint64_t(4));

	/* In a faithful log too: acknowledged bytes leave, and they had lines. */
	small_log_t faithful;
	bssh_output_log_set_faithful(&faithful.log, 1);
	faithful.append("a\nb\nc\nd\n");
	bssh_output_log_ack(&faithful.log, 8);
	faithful.append("e\nf\n");
	QCOMPARE(faithful.log.start_line, uint64_t(2));
}

void output_log_test::a_write_larger_than_the_log_keeps_its_tail() {
	small_log_t small;
	small.append("ab");
	QCOMPARE(small.append("0123456789"), size_t(10));
	QCOMPARE(small.log.end, uint64_t(12));
	QCOMPARE(small.log.start, uint64_t(4));
	QCOMPARE(small.read(4), QByteArray("23456789"));
}

void output_log_test::faithful_never_overwrites_what_the_client_lacks() {
	small_log_t small;
	bssh_output_log_set_faithful(&small.log, 1);

	QCOMPARE(small.append("abcdef"), size_t(6));
	QCOMPARE(small.append("ghijkl"), size_t(2));
	QCOMPARE(small.append("ij"), size_t(0));
	QCOMPARE(small.log.lost, uint64_t(0));
	QCOMPARE(small.read(0), QByteArray("abcdefgh"));

	/* Three acknowledged is three that may be replaced, and no more. */
	bssh_output_log_ack(&small.log, 3);
	QCOMPARE(small.append("ijkl"), size_t(3));
	QCOMPARE(small.read(3), QByteArray("defghijk"));
	QCOMPARE(small.log.lost, uint64_t(0));
}

void output_log_test::an_ack_never_moves_backwards_or_past_the_end() {
	small_log_t small;
	small.append("abcd");

	bssh_output_log_ack(&small.log, 3);
	bssh_output_log_ack(&small.log, 1);
	QCOMPARE(small.log.acked, uint64_t(3));

	bssh_output_log_ack(&small.log, 100);
	QCOMPARE(small.log.acked, uint64_t(4));
}

void output_log_test::moving_to_faithful_guards_what_is_still_held() {
	small_log_t small;
	small.append("abcdefghijkl");
	QCOMPARE(small.log.start, uint64_t(4));

	/* The client has nothing, and 0 to 3 are gone: room is measured from 4. */
	bssh_output_log_set_faithful(&small.log, 1);
	QCOMPARE(small.append("m"), size_t(0));
	QCOMPARE(small.read(4), QByteArray("efghijkl"));

	bssh_output_log_ack(&small.log, 6);
	QCOMPARE(small.append("mnop"), size_t(2));

	/* And back: Live takes everything again. */
	bssh_output_log_set_faithful(&small.log, 0);
	QCOMPARE(small.append("op"), size_t(2));
}

void output_log_test::marks_coalesce_and_lose_only_the_oldest_timing() {
	small_log_t small;
	uint64_t at = 0;
	QCOMPARE(bssh_output_log_time_of(&small.log, 0, &at), -1);

	small.append("a", 10);
	small.append("b", 10);
	QCOMPARE(small.log.marks_used, size_t(1));

	small.append("c", 20);
	QCOMPARE(bssh_output_log_time_of(&small.log, 1, &at), 0);
	QCOMPARE(at, uint64_t(10));
	QCOMPARE(bssh_output_log_time_of(&small.log, 2, &at), 0);
	QCOMPARE(at, uint64_t(20));

	/* Four marks fit, so the fifth takes the first. */
	small.append("d", 30);
	small.append("e", 40);
	small.append("f", 50);
	QCOMPARE(small.log.marks_used, size_t(4));
	QCOMPARE(bssh_output_log_time_of(&small.log, 0, &at), -1);
	QCOMPARE(bssh_output_log_time_of(&small.log, 3, &at), 0);
	QCOMPARE(at, uint64_t(30));
	QCOMPARE(bssh_output_log_time_of(&small.log, 5, &at), 0);
	QCOMPARE(at, uint64_t(50));
}

void output_log_test::refuses_to_start_with_nowhere_to_keep_anything() {
	bssh_output_log_t log;
	uint8_t bytes[1];
	bssh_output_log_mark_t marks[1];

	QCOMPARE(bssh_output_log_init(&log, nullptr, 1, marks, 1), -1);
	QCOMPARE(bssh_output_log_init(&log, bytes, 0, marks, 1), -1);
	QCOMPARE(bssh_output_log_init(&log, bytes, 1, nullptr, 1), -1);
	QCOMPARE(bssh_output_log_init(&log, bytes, 1, marks, 0), -1);
	QCOMPARE(bssh_output_log_init(&log, bytes, 1, marks, 1), 0);
}
