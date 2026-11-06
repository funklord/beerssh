#include "session_host_test.h"

#include "session/session_host.h"
#include "session/wire.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

/* A session host in a child process, gone by the end of the test. */
struct host_process_t {
	QTemporaryDir dir;
	QByteArray path;
	pid_t pid = -1;

	explicit host_process_t(const char *command, size_t capacity = 1 << 20, int linger_ms = 5000) {
		path = (dir.path() + QStringLiteral("/s")).toUtf8();
		pid = fork();
		if (pid != 0) return;

		/*
		 * Dies with the test. A run the watchdog killed left this child and its
		 * shell behind, reparented and still waiting for a client.
		 */
		prctl(PR_SET_PDEATHSIG, SIGKILL);

		char *const argv[] = { const_cast<char *>("sh"), const_cast<char *>("-c"),
		                       const_cast<char *>(command), nullptr };
		uint8_t *bytes = static_cast<uint8_t *>(malloc(capacity));
		bssh_output_log_mark_t marks[64];

		bssh_session_host_options_t options;
		options.socket_path = path.constData();
		options.argv = argv;
		options.log_bytes = bytes;
		options.log_capacity = capacity;
		options.log_marks = marks;
		options.log_marks_capacity = 64;
		options.linger_ms = linger_ms;

		const int status = bssh_session_host_run(&options);
		_exit(WIFEXITED(status) ? WEXITSTATUS(status) : 99);
	}

	~host_process_t() {
		if (pid <= 0) return;
		kill(pid, SIGKILL);
		waitpid(pid, nullptr, 0);
	}

	/* Whether it ended by itself within `ms`, and how. */
	bool ended(int ms, int *code) {
		QElapsedTimer timer;
		timer.start();
		while (timer.elapsed() < ms) {
			int status = 0;
			if (waitpid(pid, &status, WNOHANG) == pid) {
				pid = -1;
				*code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
				return true;
			}
			QTest::qSleep(10);
		}
		return false;
	}
};

struct client_t {
	int fd = -1;
	QByteArray in;
	QByteArray output;
	uint64_t next = 0;
	uint64_t lost_to = 0;
	bool saw_lost = false;
	bool saw_exit = false;
	int32_t exit_status = -1;

	~client_t() { drop(); }

	void drop() {
		if (fd >= 0) close(fd);
		fd = -1;
		in.clear();
	}

	bool connect_to(const QByteArray &path) {
		drop();
		QElapsedTimer timer;
		timer.start();
		while (timer.elapsed() < 3000) {
			fd = socket(AF_UNIX, SOCK_STREAM, 0);
			sockaddr_un address;
			memset(&address, 0, sizeof(address));
			address.sun_family = AF_UNIX;
			strncpy(address.sun_path, path.constData(), sizeof(address.sun_path) - 1);
			if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0) return true;
			close(fd);
			fd = -1;
			QTest::qSleep(10);
		}
		return false;
	}

	/* The host closes on us: a read says so, and never waits past `ms`. */
	bool hung_up(int ms = 2000) {
		pollfd one = { fd, POLLIN, 0 };
		char byte = 0;
		return poll(&one, 1, ms) > 0 && read(fd, &byte, 1) == 0;
	}

	void send_raw(const uint8_t *bytes, size_t len) {
		const ssize_t wrote = ::send(fd, bytes, len, MSG_NOSIGNAL);
		Q_UNUSED(wrote);
	}

	void attach(uint64_t offset) {
		uint8_t frame[32];
		send_raw(frame, bssh_wire_put_attach(frame, sizeof(frame), offset, 80, 24));
		next = offset;
	}

	void type(const char *text) {
		uint8_t frame[256];
		send_raw(frame, bssh_wire_put(frame, sizeof(frame), BSSH_WIRE_INPUT, text, strlen(text)));
	}

	void set_mode(uint8_t mode) {
		uint8_t frame[8];
		send_raw(frame, bssh_wire_put(frame, sizeof(frame), BSSH_WIRE_MODE, &mode, 1));
	}

	void ack(uint64_t offset) {
		uint8_t frame[16];
		send_raw(frame, bssh_wire_put_offset(frame, sizeof(frame), BSSH_WIRE_ACK, offset));
	}

	/* Reads for up to `ms`, stopping early once `until` says so. */
	template <typename done_t> bool pump(int ms, done_t until) {
		QElapsedTimer timer;
		timer.start();
		while (!until()) {
			const qint64 left = ms - timer.elapsed();
			if (left <= 0) return false;

			pollfd one = { fd, POLLIN, 0 };
			if (poll(&one, 1, int(left)) <= 0) continue;

			char chunk[65536];
			const ssize_t got = read(fd, chunk, sizeof(chunk));
			if (got <= 0) return until();
			in.append(chunk, int(got));
			take();
		}
		return true;
	}

	bool wait_for(const char *text, int ms = 3000) {
		return pump(ms, [&] { return output.contains(text); });
	}

	void take() {
		for (;;) {
			bssh_wire_frame_t frame;
			uint32_t used = 0;
			if (bssh_wire_take(reinterpret_cast<uint8_t *>(in.data()), uint32_t(in.size()), &frame,
			                   &used) != 1)
				return;

			uint64_t offset = 0;
			const uint8_t *bytes = nullptr;
			uint32_t len = 0;
			if (frame.kind == BSSH_WIRE_OUTPUT && !bssh_wire_read_output(&frame, &offset, &bytes, &len)) {
				/* Offsets must join up, or a gap would pass unseen. */
				if (offset != next) output.append("<<JUMP>>");
				output.append(reinterpret_cast<const char *>(bytes), int(len));
				next = offset + len;
			} else if (frame.kind == BSSH_WIRE_LOST && !bssh_wire_read_offset(&frame, &offset)) {
				saw_lost = true;
				lost_to = offset;
				next = offset;
			} else if (frame.kind == BSSH_WIRE_EXITED) {
				saw_exit = !bssh_wire_read_exited(&frame, &exit_status);
			}
			in.remove(0, int(used));
		}
	}
};

/* One line, then echoes each line back, saying who it is. */
const char *const ECHO_SHELL =
        "stty -echo; echo ready:$$; while read line; do echo got:$$:$line; done";

}

void session_host_test::a_frame_round_trips_and_says_how_much_it_used() {
	uint8_t bytes[64];
	const size_t len = bssh_wire_put_attach(bytes, sizeof(bytes), 300, 80, 24);
	bytes[len] = 0xff;

	bssh_wire_frame_t frame;
	uint32_t used = 0;
	QCOMPARE(bssh_wire_take(bytes, uint32_t(len) + 1, &frame, &used), 1);
	QCOMPARE(used, uint32_t(len));
	QCOMPARE(int(frame.kind), int(BSSH_WIRE_ATTACH));

	uint64_t offset = 0;
	uint16_t columns = 0;
	uint16_t rows = 0;
	QCOMPARE(bssh_wire_read_attach(&frame, &offset, &columns, &rows), 0);
	QCOMPARE(offset, uint64_t(300));
	QCOMPARE(columns, uint16_t(80));
	QCOMPARE(rows, uint16_t(24));

	const uint8_t text[] = { 'h', 'i' };
	const size_t out_len = bssh_wire_put_output(bytes, sizeof(bytes), 1ull << 40, text, 2);
	QCOMPARE(bssh_wire_take(bytes, uint32_t(out_len), &frame, &used), 1);
	const uint8_t *body = nullptr;
	uint32_t body_len = 0;
	QCOMPARE(bssh_wire_read_output(&frame, &offset, &body, &body_len), 0);
	QCOMPARE(offset, uint64_t(1ull << 40));
	QCOMPARE(body_len, uint32_t(2));
	QCOMPARE(body[1], uint8_t('i'));
}

void session_host_test::a_frame_can_arrive_a_byte_at_a_time() {
	uint8_t bytes[64];
	const size_t len = bssh_wire_put_offset(bytes, sizeof(bytes), BSSH_WIRE_ACK, 1000000);

	bssh_wire_frame_t frame;
	uint32_t used = 0;
	for (uint32_t have = 0; have < len; ++have) QCOMPARE(bssh_wire_take(bytes, have, &frame, &used), 0);
	QCOMPARE(bssh_wire_take(bytes, uint32_t(len), &frame, &used), 1);
}

void session_host_test::a_kind_nobody_knows_is_a_frame_to_skip() {
	const uint8_t text[] = { 7 };
	uint8_t bytes[16];
	const size_t len = bssh_wire_put(bytes, sizeof(bytes), 200, text, 1);

	bssh_wire_frame_t frame;
	uint32_t used = 0;
	QCOMPARE(bssh_wire_take(bytes, uint32_t(len), &frame, &used), 1);
	QCOMPARE(int(frame.kind), 200);
	QCOMPARE(frame.body_len, uint32_t(1));
}

void session_host_test::a_length_past_the_limit_is_refused_at_once() {
	/* Otherwise a reader buffers towards a frame that may never be legal. */
	uint8_t huge[] = { BSSH_WIRE_OUTPUT, 0xff, 0xff, 0xff, 0x7f, 0 };
	bssh_wire_frame_t frame;
	uint32_t used = 0;
	QCOMPARE(bssh_wire_take(huge, sizeof(huge), &frame, &used), -1);

	/* And the same with no end to the length yet: 4 bytes say enough. */
	QCOMPARE(bssh_wire_take(huge, 3, &frame, &used), 0);
	QCOMPARE(bssh_wire_take(huge, 4, &frame, &used), -1);

	/* 65537 fits three bytes of length and still is not allowed. */
	uint8_t over[] = { BSSH_WIRE_OUTPUT, 0x81, 0x80, 0x04 };
	QCOMPARE(bssh_wire_take(over, sizeof(over), &frame, &used), -1);

	static uint8_t body[BSSH_WIRE_BODY_MAX + 1];
	static uint8_t out[BSSH_WIRE_FRAME_MAX + 16];
	QCOMPARE(bssh_wire_put(out, sizeof(out), BSSH_WIRE_INPUT, body, sizeof(body)), size_t(0));
	QVERIFY(bssh_wire_put(out, sizeof(out), BSSH_WIRE_INPUT, body, BSSH_WIRE_BODY_MAX) > 0);
}

void session_host_test::a_varint_with_a_second_spelling_is_refused() {
	/* 0x81 0x00 is 1 spelled in two bytes. */
	uint8_t length[] = { BSSH_WIRE_ACK, 0x81, 0x00, 5 };
	bssh_wire_frame_t frame;
	uint32_t used = 0;
	QCOMPARE(bssh_wire_take(length, sizeof(length), &frame, &used), -1);

	uint8_t offset[] = { BSSH_WIRE_ACK, 2, 0x85, 0x00 };
	QCOMPARE(bssh_wire_take(offset, sizeof(offset), &frame, &used), 1);
	uint64_t value = 0;
	QCOMPARE(bssh_wire_read_offset(&frame, &value), -1);

	/* The one body read by hand, since it runs on past its offset. */
	uint8_t output[] = { BSSH_WIRE_OUTPUT, 3, 0x85, 0x00, 'x' };
	QCOMPARE(bssh_wire_take(output, sizeof(output), &frame, &used), 1);
	const uint8_t *bytes = nullptr;
	uint32_t len = 0;
	QCOMPARE(bssh_wire_read_output(&frame, &value, &bytes, &len), -1);
}

void session_host_test::a_shell_survives_losing_its_client() {
	host_process_t host(ECHO_SHELL);
	client_t client;

	QVERIFY(client.connect_to(host.path));
	client.attach(0);
	QVERIFY(client.wait_for("ready:"));
	const int at = client.output.indexOf("ready:") + 6;
	const QByteArray shell_pid = client.output.mid(at, client.output.indexOf('\r', at) - at);
	QVERIFY(!shell_pid.isEmpty());

	/* The connection drops. The same process answers on the next one. */
	const uint64_t held = client.next;
	client.drop();
	QVERIFY(client.connect_to(host.path));
	client.attach(held);
	client.type("hello\n");

	const QByteArray answer = "got:" + shell_pid + ":hello";
	QVERIFY2(client.wait_for(answer.constData()), client.output.constData());
	QVERIFY(!client.output.contains("<<JUMP>>"));
}

void session_host_test::output_written_while_detached_arrives_on_return() {
	host_process_t host("stty -echo; read go; sleep 0.2; echo written-while-away; read wait");
	client_t client;

	QVERIFY(client.connect_to(host.path));
	client.attach(0);
	client.type("\n");
	client.drop();

	/* Written with nobody attached, and from offset 0 nothing is missing. */
	QTest::qSleep(400);
	QVERIFY(client.connect_to(host.path));
	client.attach(0);
	QVERIFY2(client.wait_for("written-while-away"), client.output.constData());
	QVERIFY(!client.saw_lost);
}

void session_host_test::a_client_too_far_behind_is_told_what_was_lost() {
	QTemporaryDir scratch;
	const QByteArray marker = (scratch.path() + QStringLiteral("/done")).toUtf8();
	const QByteArray command = "seq 1 200000; echo finished; touch " + marker + "; read wait";
	host_process_t host(command.constData(), 4096);
	client_t client;

	/* Nobody attached, so the log runs on past its 4096 bytes. */
	QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(QString::fromUtf8(marker)), 20000);
	QVERIFY(client.connect_to(host.path));
	client.attach(0);
	QVERIFY2(client.wait_for("finished"), client.output.right(200).constData());
	QVERIFY(client.saw_lost);
	QVERIFY(client.lost_to > 1000000);
	QVERIFY(!client.output.contains("<<JUMP>>"));
}

void session_host_test::a_faithful_mode_pauses_the_program_and_loses_nothing() {
	const size_t capacity = 4096;
	host_process_t host("stty -echo -onlcr; read go; seq 1 20000; echo finished; read wait", capacity);
	client_t client;

	QVERIFY(client.connect_to(host.path));
	client.attach(0);
	client.set_mode(BSSH_WIRE_MODE_SERIAL);
	client.type("\n");

	/* No acknowledgement, so the program can get no further than the log. */
	QVERIFY(client.pump(10000, [&] { return uint64_t(client.output.size()) >= capacity; }));
	client.pump(300, [] { return false; });
	QCOMPARE(uint64_t(client.output.size()), uint64_t(capacity));
	QVERIFY(!client.output.contains("finished"));

	/* Acknowledged, it finishes, and every line arrives in order. */
	QVERIFY(client.pump(10000, [&] {
		client.ack(client.next);
		return client.output.contains("finished");
	}));
	QVERIFY(!client.saw_lost);
	QVERIFY(!client.output.contains("<<JUMP>>"));

	QByteArray expected;
	for (int i = 1; i <= 20000; ++i) expected += QByteArray::number(i) + '\n';
	QVERIFY(client.output.startsWith(expected));
}

void session_host_test::a_second_client_replaces_the_first() {
	host_process_t host(ECHO_SHELL);
	client_t first;
	client_t second;

	QVERIFY(first.connect_to(host.path));
	first.attach(0);
	QVERIFY(first.wait_for("ready:"));

	QVERIFY(second.connect_to(host.path));
	second.attach(0);
	second.type("mine\n");
	QVERIFY(second.wait_for("mine"));

	/* The first hears the socket close, after what it had been sent. */
	QVERIFY(first.pump(2000, [&] {
		pollfd one = { first.fd, POLLIN | POLLHUP, 0 };
		return poll(&one, 1, 0) > 0 && recv(first.fd, nullptr, 0, MSG_PEEK) == 0
		       && (one.revents & POLLHUP);
	}));
}

void session_host_test::a_program_ending_is_reported_and_ends_the_host() {
	host_process_t host("echo bye; exit 7");
	client_t client;

	QVERIFY(client.connect_to(host.path));
	client.attach(0);
	QVERIFY(client.pump(3000, [&] { return client.saw_exit; }));
	QVERIFY(client.output.contains("bye"));
	QVERIFY(WIFEXITED(client.exit_status));
	QCOMPARE(WEXITSTATUS(client.exit_status), 7);

	int code = -1;
	QVERIFY(host.ended(3000, &code));
	QCOMPARE(code, 7);

	/* Nothing left to connect to. */
	QVERIFY(!QFile::exists(QString::fromUtf8(host.path)));
}

void session_host_test::nobody_coming_back_ends_the_host_after_the_linger() {
	host_process_t host("exit 3", 4096, 100);

	int code = -1;
	QVERIFY(host.ended(3000, &code));
	QCOMPARE(code, 3);
}

void session_host_test::garbage_loses_the_client_and_not_the_session() {
	host_process_t host(ECHO_SHELL);
	client_t client;

	QVERIFY(client.connect_to(host.path));
	const uint8_t garbage[] = { 1, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
	client.send_raw(garbage, sizeof(garbage));

	QVERIFY(client.hung_up());

	/* A frame well-formed outside and not inside: a body too short. */
	QVERIFY(client.connect_to(host.path));
	const uint8_t empty_attach[] = { BSSH_WIRE_ATTACH, 0 };
	client.send_raw(empty_attach, sizeof(empty_attach));
	QVERIFY(client.hung_up());

	QVERIFY(client.connect_to(host.path));
	client.attach(0);
	QVERIFY(client.wait_for("ready:"));
}
