#include "session_host_test.h"

#include "fake_ssh_session.h"
#include "model/conn_manager.h"
#include "model/session.h"
#include "model/session_registry.h"
#include "session/session_client.h"
#include "term/emulator.h"
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
		QElapsedTimer timer;
		timer.start();
		while (timer.elapsed() < ms) {
			pollfd one = { fd, POLLIN, 0 };
			char bytes[256];
			if (poll(&one, 1, 100) > 0 && read(fd, bytes, sizeof(bytes)) <= 0) return true;
		}
		return false;
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

namespace {

struct received_t {
	QByteArray text;
	QList<uint64_t> gaps;
	int exits = 0;
	int restarts = 0;
	int caught_up = 0;
	int32_t status = 0;
	bssh_session_client_t client;

	received_t() {
		bssh_session_client_events_t events;
		events.context = this;
		events.output = [](void *self, const uint8_t *bytes, size_t len) {
			static_cast<received_t *>(self)->text.append(reinterpret_cast<const char *>(bytes), int(len));
		};
		events.gap = [](void *self, uint64_t count) {
			received_t *me = static_cast<received_t *>(self);
			me->gaps.append(count);
			me->text.append('|');
		};
		events.exited = [](void *self, int32_t status) {
			received_t *me = static_cast<received_t *>(self);
			++me->exits;
			me->status = status;
		};
		events.restarted = [](void *self) { ++static_cast<received_t *>(self)->restarts; };
		events.caught_up = [](void *self) { ++static_cast<received_t *>(self)->caught_up; };
		bssh_session_client_init(&client, &events);
		hello();
	}

	void hello() {
		uint8_t frame[32];
		bssh_session_client_feed(&client, frame, bssh_wire_put_hello(frame, sizeof(frame)));
	}

	int output(uint64_t offset, const char *text_in) {
		uint8_t frame[256];
		const size_t len = bssh_wire_put_output(frame, sizeof(frame), offset,
		                                        reinterpret_cast<const uint8_t *>(text_in), strlen(text_in));
		return bssh_session_client_feed(&client, frame, len);
	}
};

}

void session_host_test::the_client_delivers_output_split_anywhere() {
	received_t received;
	uint8_t frames[64];
	size_t len = bssh_wire_put_output(frames, sizeof(frames), 0, reinterpret_cast<const uint8_t *>("ab"), 2);
	len += bssh_wire_put_output(frames + len, sizeof(frames) - len, 2, reinterpret_cast<const uint8_t *>("cd"), 2);
	len += bssh_wire_put_exited(frames + len, sizeof(frames) - len, 9);

	for (size_t i = 0; i < len; ++i) QCOMPARE(bssh_session_client_feed(&received.client, frames + i, 1), 0);
	QCOMPARE(received.text, QByteArray("abcd"));
	QCOMPARE(received.exits, 1);
	QCOMPARE(received.status, 9);
	QCOMPARE(received.client.next, uint64_t(4));

	/* And all at once, which is how a busy connection delivers them. */
	received_t together;
	QCOMPARE(bssh_session_client_feed(&together.client, frames, len), 0);
	QCOMPARE(together.text, QByteArray("abcd"));
	QCOMPARE(together.exits, 1);
}

void session_host_test::the_client_never_joins_text_across_a_gap() {
	received_t received;
	QCOMPARE(received.output(0, "ab"), 0);
	QCOMPARE(received.output(7, "cd"), 0);

	QCOMPARE(received.text, QByteArray("ab|cd"));
	QCOMPARE(received.gaps, QList<uint64_t>{ 5 });
	QCOMPARE(received.client.next, uint64_t(9));
}

void session_host_test::the_client_is_told_what_a_lost_frame_took() {
	received_t received;
	received.output(0, "ab");

	uint8_t frame[16];
	const size_t len = bssh_wire_put_offset(frame, sizeof(frame), BSSH_WIRE_LOST, 100);
	QCOMPARE(bssh_session_client_feed(&received.client, frame, len), 0);
	QCOMPARE(received.gaps, QList<uint64_t>{ 98 });
	QCOMPARE(received.client.next, uint64_t(100));

	/* Told again of what it already knows, it adds nothing. */
	QCOMPARE(bssh_session_client_feed(&received.client, frame, len), 0);
	QCOMPARE(received.gaps.size(), 1);

	/* And output from there joins up with no second gap. */
	received.output(100, "cd");
	QCOMPARE(received.text, QByteArray("ab|cd"));
}

void session_host_test::the_client_drops_what_it_has_already_seen() {
	received_t received;
	received.output(0, "abcd");

	received.output(2, "cdef");
	QCOMPARE(received.text, QByteArray("abcdef"));

	received.output(1, "bc");
	QCOMPARE(received.text, QByteArray("abcdef"));
	QCOMPARE(received.client.next, uint64_t(6));
}

void session_host_test::the_client_keeps_its_place_across_a_new_link() {
	received_t received;
	received.output(0, "abcd");

	/* Half a frame, and then the connection goes. */
	uint8_t frame[32];
	const size_t len = bssh_wire_put_output(frame, sizeof(frame), 4, reinterpret_cast<const uint8_t *>("efgh"), 4);
	QCOMPARE(bssh_session_client_feed(&received.client, frame, 3), 0);
	bssh_session_client_reset_link(&received.client);
	QCOMPARE(received.client.next, uint64_t(4));

	/* The new link proves itself, and starts from where the old one was. */
	received.hello();
	QCOMPARE(bssh_session_client_feed(&received.client, frame, len), 0);
	QCOMPARE(received.text, QByteArray("abcdefgh"));
}

void session_host_test::the_client_acknowledges_when_due_and_not_before() {
	received_t received;
	received.output(0, "abc");

	uint8_t out[16];
	QCOMPARE(bssh_session_client_ack_due(&received.client, 4, out, sizeof(out)), size_t(0));

	received.output(3, "d");
	const size_t len = bssh_session_client_ack_due(&received.client, 4, out, sizeof(out));
	QVERIFY(len > 0);

	bssh_wire_frame_t frame;
	uint32_t used = 0;
	QCOMPARE(bssh_wire_take(out, uint32_t(len), &frame, &used), 1);
	uint64_t offset = 0;
	QCOMPARE(bssh_wire_read_offset(&frame, &offset), 0);
	QCOMPARE(offset, uint64_t(4));

	QCOMPARE(bssh_session_client_ack_due(&received.client, 4, out, sizeof(out)), size_t(0));
}

void session_host_test::the_client_refuses_a_peer_not_speaking_the_protocol() {
	/* A .bashrc's banner, printed ahead of the program on the far end. */
	received_t received;
	bssh_session_client_reset_link(&received.client);
	const QByteArray banner("Welcome to the host, this is not a frame at all\r\n");
	QCOMPARE(bssh_session_client_feed(&received.client, reinterpret_cast<const uint8_t *>(banner.constData()),
	                                  size_t(banner.size())),
	         -1);
	QVERIFY(received.text.isEmpty());

	/* Refused at the first wrong byte, without waiting for more. */
	bssh_session_client_reset_link(&received.client);
	const uint8_t wrong[] = { BSSH_WIRE_OUTPUT };
	QCOMPARE(bssh_session_client_feed(&received.client, wrong, 1), -1);

	/* And a hello that stops short is only not finished. */
	bssh_session_client_reset_link(&received.client);
	uint8_t hello[32];
	const size_t len = bssh_wire_put_hello(hello, sizeof(hello));
	QCOMPARE(bssh_session_client_feed(&received.client, hello, len - 1), 0);
	QCOMPARE(bssh_session_client_feed(&received.client, hello + len - 1, 1), 0);
	QCOMPARE(received.client.said_hello, 1);
}

void session_host_test::a_client_and_a_real_host_agree_from_the_first_byte() {
	host_process_t host("echo from-the-program; read wait");
	client_t link;
	QVERIFY(link.connect_to(host.path));
	link.attach(0);

	received_t received;
	bssh_session_client_reset_link(&received.client);

	QElapsedTimer timer;
	timer.start();
	while (!received.text.contains("from-the-program") && timer.elapsed() < 3000) {
		pollfd one = { link.fd, POLLIN, 0 };
		if (poll(&one, 1, 100) <= 0) continue;

		uint8_t chunk[4096];
		const ssize_t got = read(link.fd, chunk, sizeof(chunk));
		QVERIFY(got > 0);
		QCOMPARE(bssh_session_client_feed(&received.client, chunk, size_t(got)), 0);
	}
	QVERIFY(received.text.contains("from-the-program"));
}

void session_host_test::a_client_asking_past_the_end_is_told_it_is_a_new_session() {
	host_process_t host("echo a-new-shell; read wait");
	client_t link;
	QVERIFY(link.connect_to(host.path));

	/* As after a reboot: the client's place is in a session that is gone. */
	link.attach(5000);

	received_t received;
	received.client.next = 5000;
	bssh_session_client_reset_link(&received.client);

	QElapsedTimer timer;
	timer.start();
	while (!received.text.contains("a-new-shell") && timer.elapsed() < 3000) {
		pollfd one = { link.fd, POLLIN, 0 };
		if (poll(&one, 1, 100) <= 0) continue;

		uint8_t chunk[4096];
		const ssize_t got = read(link.fd, chunk, sizeof(chunk));
		QVERIFY(got > 0);
		QCOMPARE(bssh_session_client_feed(&received.client, chunk, size_t(got)), 0);
	}
	QVERIFY2(received.text.contains("a-new-shell"), "what the new shell wrote was taken as seen");
	QCOMPARE(received.restarts, 1);
	QVERIFY(received.gaps.isEmpty());
}

void session_host_test::a_client_is_told_when_it_has_caught_up() {
	host_process_t host("echo written-before; read wait");
	client_t link;

	/* Long enough for the line to be in the log before anyone asks. */
	QTest::qSleep(300);
	QVERIFY(link.connect_to(host.path));
	link.attach(0);

	received_t received;
	bssh_session_client_reset_link(&received.client);

	QElapsedTimer timer;
	timer.start();
	while (!received.caught_up && timer.elapsed() < 3000) {
		pollfd one = { link.fd, POLLIN, 0 };
		if (poll(&one, 1, 100) <= 0) continue;

		uint8_t chunk[4096];
		const ssize_t got = read(link.fd, chunk, sizeof(chunk));
		QVERIFY(got > 0);
		QCOMPARE(bssh_session_client_feed(&received.client, chunk, size_t(got)), 0);
	}
	QCOMPARE(received.caught_up, 1);
	QVERIFY2(received.text.contains("written-before"), "said before what it was to deliver");
}

void session_host_test::a_hangup_ends_the_program_and_the_host() {
	host_process_t host(ECHO_SHELL, 4096, 60000);
	client_t link;

	QVERIFY(link.connect_to(host.path));
	link.attach(0);
	QVERIFY(link.wait_for("ready:"));

	/* Well before the linger: nobody is coming back. */
	uint8_t frame[4];
	link.send_raw(frame, bssh_wire_put(frame, sizeof(frame), BSSH_WIRE_HANGUP, nullptr, 0));
	link.drop();

	int code = -1;
	QVERIFY(host.ended(5000, &code));
}

namespace {

/* A session over the fake transport, one fake per connection. */
struct kept_t {
	bssh_factory_reset_t factory_reset;
	QList<bssh_fake_ssh_session *> fakes;
	bssh_session_registry registry;
	bssh_session *session = nullptr;

	explicit kept_t(bool wanted = true, const QByteArray &restored = QByteArray()) {
		bssh_ssh_session::set_factory([this](QObject *parent) -> bssh_ssh_session * {
			bssh_fake_ssh_session *fake = new bssh_fake_ssh_session(parent);
			fakes.append(fake);
			return fake;
		});

		bssh_profile_t profile;
		profile.name = QStringLiteral("kept");
		profile.host = QStringLiteral("kept.example");
		profile.user = QStringLiteral("nabbe");
		profile.resume_session = wanted;
		session = registry.create(profile);
		registry.conn_manager_for(session)->set_auto_reconnect(false);
		session->restore_kept_name(restored);
		session->connect_to_host();
		fakes.last()->simulate_key_auth_success();
	}

	bssh_fake_ssh_session *fake() const { return fakes.last(); }

	void answer_probe() { fake()->simulate_side_output("beerssh-session 1\n"); }

	void host_says(const QByteArray &frame) { fake()->emit_chunk(frame); }

	QByteArray hello() const {
		uint8_t frame[32];
		const size_t len = bssh_wire_put_hello(frame, sizeof(frame));
		return QByteArray(reinterpret_cast<const char *>(frame), int(len));
	}

	QByteArray output(uint64_t offset, const char *text) const {
		uint8_t frame[256];
		const size_t len = bssh_wire_put_output(frame, sizeof(frame), offset,
		                                        reinterpret_cast<const uint8_t *>(text), strlen(text));
		return QByteArray(reinterpret_cast<const char *>(frame), int(len));
	}

	QString screen() const { return session->emulator()->screen().to_text(); }

	/* Every frame the client wrote, kinds in order. */
	QList<int> kinds_written() const {
		QByteArray bytes = fake()->written();
		QList<int> kinds;
		for (;;) {
			bssh_wire_frame_t frame;
			uint32_t used = 0;
			if (bssh_wire_take(reinterpret_cast<uint8_t *>(bytes.data()), uint32_t(bytes.size()), &frame,
			                   &used) != 1)
				return kinds;
			kinds.append(frame.kind);
			bytes.remove(0, int(used));
		}
	}

	/* The attach frame's offset, which it writes first. */
	uint64_t attached_at() const {
		QByteArray bytes = fake()->written();
		bssh_wire_frame_t frame;
		uint32_t used = 0;
		uint64_t offset = 0;
		uint16_t columns = 0;
		uint16_t rows = 0;
		if (bssh_wire_take(reinterpret_cast<uint8_t *>(bytes.data()), uint32_t(bytes.size()), &frame, &used) != 1
		    || frame.kind != BSSH_WIRE_ATTACH || bssh_wire_read_attach(&frame, &offset, &columns, &rows))
			return UINT64_MAX;
		return offset;
	}
};

}

void session_host_test::a_host_not_asked_is_not_probed() {
	kept_t kept(false);
	QVERIFY(kept.fake()->side_commands().filter(QStringLiteral("beerssh-session")).isEmpty());
	QCOMPARE(kept.fake()->shells_opened(), 1);
	QVERIFY(!kept.fake()->raw_channel());
}

void session_host_test::a_host_without_the_program_gets_a_plain_shell() {
	kept_t kept;

	/* Asked first, and no shell until it is answered. */
	QVERIFY(!kept.fake()->side_commands().filter(QStringLiteral("beerssh-session version")).isEmpty());
	QCOMPARE(kept.fake()->shells_opened(), 0);

	/* The shell's answer to a command it does not have is no output at all. */
	kept.fake()->simulate_side_output(QByteArray());
	QCOMPARE(kept.fake()->shells_opened(), 1);
	QVERIFY(!kept.fake()->raw_channel());
	QVERIFY(kept.fake()->command().isEmpty());

	/* And bytes are bytes: nothing here is a frame. */
	kept.host_says("plain-output");
	QVERIFY(kept.screen().contains(QStringLiteral("plain-output")));
}

void session_host_test::a_host_with_the_program_gets_a_kept_session() {
	kept_t kept;
	kept.answer_probe();

	QCOMPARE(kept.fake()->shells_opened(), 1);
	QVERIFY(kept.fake()->raw_channel());
	QCOMPARE(kept.fake()->command().value(0), QStringLiteral("beerssh-session"));
	QCOMPARE(kept.fake()->command().value(1), QStringLiteral("attach"));
	QVERIFY(!kept.fake()->command().value(2).isEmpty());
	QCOMPARE(kept.attached_at(), uint64_t(0));

	kept.host_says(kept.hello() + kept.output(0, "from-the-host"));
	QVERIFY(kept.screen().contains(QStringLiteral("from-the-host")));

	/* What is typed and the size both go as frames, not as bytes on the channel. */
	kept.session->emulator()->send_paste(QStringLiteral("typed"));
	kept.session->resize(100, 30);
	const QList<int> kinds = kept.kinds_written();
	QVERIFY(kinds.contains(BSSH_WIRE_INPUT));
	QVERIFY(kinds.contains(BSSH_WIRE_RESIZE));
	QCOMPARE(kept.fake()->resize_count(), 0);
}

void session_host_test::a_reconnect_returns_to_the_same_place_with_modes_kept() {
	kept_t kept;
	kept.answer_probe();
	const QString name = kept.fake()->command().value(2);

	/* Inside an editor when the link drops: the alternate screen. */
	kept.host_says(kept.hello() + kept.output(0, "\033[?1049hin-the-editor"));
	QVERIFY(kept.session->emulator()->screen().to_text().contains(QStringLiteral("in-the-editor")));
	const uint64_t held = strlen("\033[?1049hin-the-editor");

	kept.fake()->simulate_connection_lost();
	kept.session->reconnect();
	kept.fake()->simulate_key_auth_success();
	kept.answer_probe();

	QCOMPARE(kept.fake()->command().value(2), name);
	QCOMPARE(kept.attached_at(), held);

	/* Still in the editor: a plain reconnect would have reset the modes. */
	QVERIFY(kept.screen().contains(QStringLiteral("in-the-editor")));
	kept.host_says(kept.hello() + kept.output(held, "-and-on"));
	QVERIFY(kept.screen().contains(QStringLiteral("in-the-editor-and-on")));
}

void session_host_test::a_banner_ahead_of_the_program_falls_back_on_the_same_connection() {
	kept_t kept;
	kept.answer_probe();

	kept.host_says("Welcome to the host, have a nice day\r\n");
	QCOMPARE(kept.fake()->restarts().size(), 1);
	QVERIFY(!kept.fake()->raw_channel());

	/* Not tried again on a later connection of this session. */
	kept.fake()->simulate_connection_lost();
	kept.session->reconnect();
	kept.fake()->simulate_key_auth_success();
	QCOMPARE(kept.fake()->shells_opened(), 1);
	QVERIFY(kept.fake()->side_commands().filter(QStringLiteral("beerssh-session")).isEmpty());

	/* And it is a terminal again: bytes go as they are. */
	kept.fake()->clear_written();
	kept.session->emulator()->send_paste(QStringLiteral("typed"));
	QCOMPARE(kept.fake()->written(), QByteArray("typed"));
}

void session_host_test::closing_a_kept_session_hangs_it_up() {
	kept_t kept;
	kept.answer_probe();
	kept.host_says(kept.hello());

	bssh_fake_ssh_session *fake = kept.fake();
	kept.session->disconnect_from_host();
	QCOMPARE(int(fake->written().at(fake->written().size() - 2)), int(BSSH_WIRE_HANGUP));
}

void session_host_test::a_new_shell_after_a_reboot_is_shown_and_said() {
	kept_t kept;
	kept.answer_probe();
	kept.host_says(kept.hello() + kept.output(0, "old-shell"));

	kept.fake()->simulate_connection_lost();
	kept.session->reconnect();
	kept.fake()->simulate_key_auth_success();
	kept.answer_probe();

	uint8_t frame[16];
	const size_t len = bssh_wire_put_offset(frame, sizeof(frame), BSSH_WIRE_RESTARTED, 0);
	kept.host_says(kept.hello() + QByteArray(reinterpret_cast<const char *>(frame), int(len))
	               + kept.output(0, "new-shell"));

	QVERIFY(kept.screen().contains(QStringLiteral("a new shell")));
	QVERIFY(kept.screen().contains(QStringLiteral("new-shell")));
}

void session_host_test::a_restored_session_finds_its_shell_and_replays_in_silence() {
	kept_t kept(true, "from-the-saved-layout");
	QCOMPARE(kept.session->kept_name(), QByteArray("from-the-saved-layout"));
	kept.answer_probe();

	QCOMPARE(kept.fake()->command().value(2), QStringLiteral("from-the-saved-layout"));
	QCOMPARE(kept.attached_at(), uint64_t(0));

	/* A question from the log, long since answered: DA, which draws nothing. */
	kept.fake()->clear_written();
	kept.host_says(kept.hello() + kept.output(0, "from-before\033[c"));
	QVERIFY(kept.screen().contains(QStringLiteral("from-before")));
	QVERIFY2(kept.fake()->written().isEmpty(), "answered a question from the log into the live shell");

	/* Caught up, the same question is a program asking now. */
	uint8_t frame[4];
	const size_t len = bssh_wire_put(frame, sizeof(frame), BSSH_WIRE_CAUGHT_UP, nullptr, 0);
	kept.host_says(QByteArray(reinterpret_cast<const char *>(frame), int(len)));
	kept.host_says(kept.output(14, "\033[c"));
	QVERIFY(kept.kinds_written().contains(BSSH_WIRE_INPUT));
}

void session_host_test::a_session_is_named_before_it_is_asked_anything() {
	/* A name that came after the probe could be lost with the process. */
	kept_t kept;
	QVERIFY(!kept.session->kept_name().isEmpty());
	QCOMPARE(kept.fake()->shells_opened(), 0);

	kept_t not_asked(false);
	QVERIFY(not_asked.session->kept_name().isEmpty());
}
