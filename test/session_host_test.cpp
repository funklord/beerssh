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
#include <QSignalSpy>
#include <QTest>

#include <poll.h>
#include <signal.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
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
#ifdef __linux__
		prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif

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
	uint64_t lost_lines = 0;
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
			} else if (frame.kind == BSSH_WIRE_LOST
			           && !bssh_wire_read_lost(&frame, &offset, &lost_lines)) {
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

	/*
	 * The shape of the hole, from a known program: the first line that arrived
	 * says how many seq wrote before it. Its first bytes may be the end of a line,
	 * and the shell's own echo of nothing adds none.
	 */
	const QList<QByteArray> arrived = client.output.split('\n');
	QVERIFY(arrived.size() > 2);
	QCOMPARE(client.lost_lines + 1, arrived.at(1).trimmed().toULongLong() - 1);
}

void session_host_test::a_faithful_mode_pauses_the_program_and_loses_nothing() {
	const size_t capacity = 4096;
	host_process_t host("stty -echo -onlcr; echo READY; read go; seq 1 20000; echo finished; read wait",
	                    capacity);
	client_t client;

	QVERIFY(client.connect_to(host.path));
	client.attach(0);
	client.set_mode(BSSH_WIRE_MODE_SERIAL);

	/*
	 * Typed only once the echo is off. Typed before, the line discipline
	 * echoed the newline ahead of seq's first line -- seen on macOS, whose
	 * runner reaches stty later.
	 */
	QVERIFY2(client.wait_for("READY\n", 10000), client.output.constData());
	client.ack(client.next);
	client.output.clear();
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
	QList<uint64_t> lines;
	int exits = 0;
	int restarts = 0;
	int caught_up = 0;
	int32_t status = 0;
	QList<QByteArray> history;     /* each line's text, a '+' ahead of a continuation */
	QList<uint64_t> history_lost;
	int text_when_history_began = -1;
	bssh_session_client_t client;

	received_t() {
		bssh_session_client_events_t events = {};
		events.context = this;
		events.output = [](void *self, const uint8_t *bytes, size_t len) {
			static_cast<received_t *>(self)->text.append(reinterpret_cast<const char *>(bytes), int(len));
		};
		events.gap = [](void *self, uint64_t count, uint64_t lines) {
			received_t *me = static_cast<received_t *>(self);
			me->gaps.append(count);
			me->lines.append(lines);
			me->text.append('|');
		};
		events.exited = [](void *self, int32_t status) {
			received_t *me = static_cast<received_t *>(self);
			++me->exits;
			me->status = status;
		};
		events.restarted = [](void *self) { ++static_cast<received_t *>(self)->restarts; };
		events.caught_up = [](void *self) { ++static_cast<received_t *>(self)->caught_up; };
		events.history = [](void *self, bssh_wire_cells_t *cells, int continuation) {
			QByteArray text = continuation ? "+" : "";
			bssh_screen_cell_t cell;
			while (bssh_wire_next_cell(cells, &cell) > 0)
				text += QString(QChar::fromUcs4(cell.chars[0] ? cell.chars[0] : U' ')).toUtf8();
			received_t *me = static_cast<received_t *>(self);
			if (me->text_when_history_began < 0) me->text_when_history_began = int(me->text.size());
			me->history.append(text);
		};
		events.history_lost = [](void *self, uint64_t lines) {
			static_cast<received_t *>(self)->history_lost.append(lines);
		};
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
	received.output(0, "a\n");

	/* Line 40 at offset 100, and the client held one: 39 are gone. */
	uint8_t frame[32];
	const size_t len = bssh_wire_put_lost(frame, sizeof(frame), 100, 40);
	QCOMPARE(bssh_session_client_feed(&received.client, frame, len), 0);
	QCOMPARE(received.gaps, QList<uint64_t>{ 98 });
	QCOMPARE(received.lines, QList<uint64_t>{ 39 });
	QCOMPARE(received.client.next, uint64_t(100));
	QCOMPARE(received.client.line, uint64_t(40));

	/* Told again of what it already knows, it adds nothing. */
	QCOMPARE(bssh_session_client_feed(&received.client, frame, len), 0);
	QCOMPARE(received.gaps.size(), 1);

	/* And output from there joins up with no second gap. */
	received.output(100, "c\n");
	QCOMPARE(received.text, QByteArray("a\n|c\n"));

	/* A second hole is measured from there, not from the start. */
	const size_t again = bssh_wire_put_lost(frame, sizeof(frame), 200, 50);
	QCOMPARE(bssh_session_client_feed(&received.client, frame, again), 0);
	QCOMPARE(received.lines.last(), uint64_t(9));

	/* Bytes seen twice are lines counted once. */
	received.output(200, "d\ne\n");
	received.output(200, "d\ne\nf\n");
	QCOMPARE(received.client.line, uint64_t(53));

	/* A host's line behind the client's own is no number to show. */
	const size_t behind = bssh_wire_put_lost(frame, sizeof(frame), 300, 2);
	QCOMPARE(bssh_session_client_feed(&received.client, frame, behind), 0);
	QCOMPARE(received.lines.last(), uint64_t(0));
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

void session_host_test::a_client_that_takes_keyframes_is_sent_the_screen_and_the_lines_it_missed() {
	QTemporaryDir scratch;
	const QByteArray marker = (scratch.path() + QStringLiteral("/done")).toUtf8();
	const QByteArray command = "seq 1 200000; echo finished; touch " + marker + "; read wait";
	host_process_t host(command.constData(), 4096);

	/* Nobody attached, so the log runs on past its 4096 bytes. */
	QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(QString::fromUtf8(marker)), 20000);

	/*
	 * seq blocks on a full pty, so up to a pty buffer of it may still be on
	 * its way into the host when the marker appears. Left to arrive during
	 * the keyframe it is more history, sent and counted -- correct, and not
	 * the case this test pins down.
	 */
	QTest::qSleep(300);
	client_t link;
	QVERIFY(link.connect_to(host.path));
	uint8_t frame[32];
	link.send_raw(frame, bssh_session_client_put_keyframes(frame, sizeof(frame)));
	link.attach(0);

	received_t received;
	bssh_session_client_reset_link(&received.client);
	QElapsedTimer timer;
	timer.start();
	while (!received.caught_up && timer.elapsed() < 10000) {
		pollfd one = { link.fd, POLLIN, 0 };
		if (poll(&one, 1, 100) <= 0) continue;

		uint8_t chunk[65536];
		const ssize_t got = read(link.fd, chunk, sizeof(chunk));
		QVERIFY(got > 0);
		QCOMPARE(bssh_session_client_feed(&received.client, chunk, size_t(got)), 0);
	}
	QCOMPARE(received.caught_up, 1);

	/* Not a replay from the oldest byte held: no gap, and the place is past it all. */
	QVERIFY(received.gaps.isEmpty());
	QVERIFY(received.client.next > 1000000);

	/* Lines went past the server's limit too, and it says so without a count. */
	QCOMPARE(received.history_lost, QList<uint64_t>{ 0 });

	/* Latest first: the whole screen had come before the first line of history. */
	QVERIFY(received.text_when_history_began > 0);
	QCOMPARE(received.text_when_history_began, int(received.text.size()));

	/* The history it still held, in order and every one of them. */
	QVERIFY(received.history.size() >= BSSH_SCREEN_HISTORY_LINES);
	for (int i = 1; i < received.history.size(); ++i)
		QCOMPARE(received.history.at(i).toULongLong(), received.history.at(i - 1).toULongLong() + 1);

	/*
	 * And then the screen, and whatever the host had still to read when the
	 * test saw the program finish, which scrolls on from it: read in order,
	 * the history, what scrolled off here and the screen are one count, to
	 * the last line seq wrote.
	 */
	bssh_emulator terminal(80, 24);
	terminal.feed(received.text);
	QStringList lines;
	for (const QByteArray &line : received.history) lines.append(QString::fromUtf8(line));
	for (int i = 0; i < terminal.scrollback().count(); ++i) {
		QString text;
		for (const bssh_cell_t &cell : terminal.scrollback().line(i)) text += QChar::fromUcs4(cell.ch);
		lines.append(text);
	}
	lines += terminal.screen().to_text().split(QLatin1Char('\n'));
	quint64 last = 0;
	for (const QString &line : lines) {
		bool number = false;
		const quint64 value = line.trimmed().toULongLong(&number);
		if (!number) continue;
		if (last) QCOMPARE(value, last + 1);
		last = value;
	}
	QCOMPARE(last, quint64(200000));
	QVERIFY2(terminal.screen().to_text().contains(QStringLiteral("finished")),
	         qPrintable(terminal.screen().to_text()));
}

void session_host_test::a_live_client_far_behind_is_sent_the_screen_not_every_byte() {
	/*
	 * 30,000 lines while away, about 170 KB, all of it still in a 1 MB log.
	 * Live answers that with the lines and the screen; Serial with every
	 * byte, which is its promise; and a backlog a replay can afford is
	 * replayed in Live too.
	 */
	QTemporaryDir scratch;
	const QByteArray marker = (scratch.path() + QStringLiteral("/done")).toUtf8();
	const QByteArray command = "seq 1 30000; echo finished; touch " + marker + "; read wait";
	host_process_t host(command.constData());
	QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(QString::fromUtf8(marker)), 20000);
	QTest::qSleep(300); /* what seq still had in the pty, into the host */

	auto come_back = [&](uint8_t mode, uint64_t from, received_t &received) {
		client_t link;
		if (!link.connect_to(host.path)) return false;
		uint8_t frame[32];
		link.send_raw(frame, bssh_session_client_put_keyframes(frame, sizeof(frame)));
		link.set_mode(mode);
		link.attach(from);
		received.client.next = from;
		bssh_session_client_reset_link(&received.client);
		QElapsedTimer timer;
		timer.start();
		while (!received.caught_up && timer.elapsed() < 10000) {
			pollfd one = { link.fd, POLLIN, 0 };
			if (poll(&one, 1, 100) <= 0) continue;
			uint8_t chunk[65536];
			const ssize_t got = read(link.fd, chunk, sizeof(chunk));
			if (got <= 0 || bssh_session_client_feed(&received.client, chunk, size_t(got)) != 0) return false;
		}
		return received.caught_up == 1;
	};

	received_t live;
	QVERIFY(come_back(BSSH_WIRE_MODE_LIVE, 0, live));
	QVERIFY2(live.text.size() < 8192, "Live replayed the backlog rather than sending the screen");
	QVERIFY(live.history.size() >= BSSH_SCREEN_HISTORY_LINES);
	QCOMPARE(live.history.last().toULongLong() + 1 > 29000, true);
	bssh_emulator terminal(80, 24);
	terminal.feed(live.text);
	QVERIFY2(terminal.screen().to_text().contains(QStringLiteral("30000\nfinished")),
	         qPrintable(terminal.screen().to_text()));
	QVERIFY(live.gaps.isEmpty());

	received_t serial;
	QVERIFY(come_back(BSSH_WIRE_MODE_SERIAL, 0, serial));
	QVERIFY(serial.history.isEmpty());
	QVERIFY(serial.text.startsWith("1\r\n2\r\n3\r\n"));
	QVERIFY(serial.text.contains("29999\r\n30000\r\nfinished"));

	/* Live, a few lines behind: those bytes, not a keyframe. */
	received_t near;
	const uint64_t end = serial.client.next;
	QVERIFY(come_back(BSSH_WIRE_MODE_LIVE, end - 200, near));
	QVERIFY(near.history.isEmpty());
	QCOMPARE(near.text.size(), 200);
}

void session_host_test::a_history_line_round_trips_with_its_styles() {
	bssh_screen_cell_t cells[6];
	memset(cells, 0, sizeof(cells));
	for (bssh_screen_cell_t &cell : cells) cell.width = 1;
	cells[0].chars[0] = 'a';
	cells[0].attrs = BSSH_SCREEN_BOLD | BSSH_SCREEN_DIM;
	cells[0].fg.kind = BSSH_SCREEN_COLOR_INDEXED;
	cells[0].fg.index = 9;
	cells[1].chars[0] = 0x4E2D; /* a wide character, and its far half */
	cells[1].width = 2;
	cells[2].chars[0] = 0xFFFFFFFFu;
	cells[3].chars[0] = 0; /* never written */
	cells[4].chars[0] = 'z';
	cells[4].bg.kind = BSSH_SCREEN_COLOR_RGB;
	cells[4].bg.red = 1;
	cells[4].bg.green = 2;
	cells[4].bg.blue = 3;
	cells[5].chars[0] = 0x1F600;
	cells[5].attrs = BSSH_SCREEN_UNDERLINE;

	uint8_t out[256];
	const size_t len = bssh_wire_put_history(out, sizeof(out), 123456, 1, cells, 6);
	QVERIFY(len > 0);

	bssh_wire_frame_t frame;
	uint32_t used = 0;
	QCOMPARE(bssh_wire_take(out, uint32_t(len), &frame, &used), 1);
	QCOMPARE(size_t(used), len);
	QCOMPARE(int(frame.kind), int(BSSH_WIRE_HISTORY));

	uint64_t number = 0;
	int continuation = 0;
	bssh_wire_cells_t reader;
	QCOMPARE(bssh_wire_read_history(&frame, &number, &continuation, &reader), 0);
	QCOMPARE(number, uint64_t(123456));
	QCOMPARE(continuation, 1);

	/* Every cell but the far half, which the width stands for. */
	const int expected[] = { 0, 1, 3, 4, 5 };
	for (int at : expected) {
		bssh_screen_cell_t cell;
		QCOMPARE(bssh_wire_next_cell(&reader, &cell), 1);
		QCOMPARE(cell.chars[0], cells[at].chars[0]);
		QCOMPARE(int(cell.width), int(cells[at].width));
		QCOMPARE(int(cell.attrs), int(cells[at].attrs));
		QVERIFY(!memcmp(&cell.fg, &cells[at].fg, sizeof(cell.fg)));
		QVERIFY(!memcmp(&cell.bg, &cells[at].bg, sizeof(cell.bg)));
	}
	bssh_screen_cell_t cell;
	QCOMPARE(bssh_wire_next_cell(&reader, &cell), 0);

	/* Too little room for the line: what fits goes, whole runs only, and reads back. */
	const size_t short_len = bssh_wire_put_history(out, 20, 7, 0, cells, 6);
	QVERIFY(short_len > 0 && short_len <= 20);
	QCOMPARE(bssh_wire_take(out, uint32_t(short_len), &frame, &used), 1);
	QCOMPARE(bssh_wire_read_history(&frame, &number, &continuation, &reader), 0);
	int read = 0;
	int step;
	while ((step = bssh_wire_next_cell(&reader, &cell)) > 0) ++read;
	QCOMPARE(step, 0);
	QVERIFY(read > 0 && read < 5);
}

void session_host_test::a_malformed_history_line_is_refused() {
	/* A number, no continuation, and then one run's bytes. */
	auto first_cell = [](const QByteArray &run) {
		QByteArray body("\x05\x00", 2);
		body += run;
		uint8_t out[64];
		const size_t len = bssh_wire_put(out, sizeof(out), BSSH_WIRE_HISTORY, body.constData(), size_t(body.size()));
		bssh_wire_frame_t frame;
		uint32_t used = 0;
		if (bssh_wire_take(out, uint32_t(len), &frame, &used) != 1) return -9;
		uint64_t number = 0;
		int continuation = 0;
		bssh_wire_cells_t reader;
		if (bssh_wire_read_history(&frame, &number, &continuation, &reader)) return -8;
		bssh_screen_cell_t cell;
		return bssh_wire_next_cell(&reader, &cell);
	};

	QCOMPARE(first_cell(QByteArray("\x00\x00\x00\x01\x82\x01", 6)), 1);  /* 'A', for a start */
	QCOMPARE(first_cell(QByteArray("\x80\x00\x00\x01\x82\x01", 6)), -1); /* the top attribute bit */
	QCOMPARE(first_cell(QByteArray("\x00\x03\x00\x01\x82\x01", 6)), -1); /* a colour kind nobody knows */
	QCOMPARE(first_cell(QByteArray("\x00\x00\x00\x00\x82\x01", 6)), -1); /* a run of nothing */
	QCOMPARE(first_cell(QByteArray("\x00\x00\x00\x01", 4)), -1);         /* a run with no cell */
	QCOMPARE(first_cell(QByteArray("\x00\x02\x01", 3)), -1);             /* a colour cut short */
	QCOMPARE(first_cell(QByteArray("\x00\x00\x00\x01\x80\x80\x88\x01", 8)), -1); /* past U+10FFFF */

	/* And the client refuses such a line whole, rather than half of it. */
	received_t received;
	QByteArray body("\x05\x00\x00\x00\x00\x02\x82\x01", 8);
	uint8_t out[64];
	const size_t len = bssh_wire_put(out, sizeof(out), BSSH_WIRE_HISTORY, body.constData(), size_t(body.size()));
	QCOMPARE(bssh_session_client_feed(&received.client, out, len), -1);
	QVERIFY(received.history.isEmpty());
}

void session_host_test::the_client_takes_history_in_order_and_says_what_went() {
	received_t received;
	received.output(0, "old\n");
	uint8_t out[256];
	auto feed = [&](size_t len) { return bssh_session_client_feed(&received.client, out, len); };
	auto line = [&](uint64_t number, char ch) {
		bssh_screen_cell_t cell;
		memset(&cell, 0, sizeof(cell));
		cell.chars[0] = uint32_t(ch);
		cell.width = 1;
		return bssh_wire_put_history(out, sizeof(out), number, 0, &cell, 1);
	};

	QCOMPARE(feed(bssh_wire_put_keyframe(out, sizeof(out), 10, 0)), 0);
	QCOMPARE(feed(line(10, 'a')), 0);
	QCOMPARE(feed(line(10, 'a')), 0); /* sent twice: taken once */
	QCOMPARE(feed(line(13, 'd')), 0); /* two went between */
	QCOMPARE(received.history, (QList<QByteArray>{ "a", "d" }));
	QCOMPARE(received.history_lost, QList<uint64_t>{ 2 });

	/* The screen, in parts: the place moves only with the last. */
	size_t taken = 0;
	const uint8_t *paint = reinterpret_cast<const uint8_t *>("NOW");
	QCOMPARE(feed(bssh_wire_put_repaint(out, sizeof(out), 900, 40, 0, paint, 2, &taken)), 0);
	QCOMPARE(received.client.next, uint64_t(4));
	QCOMPARE(feed(bssh_wire_put_repaint(out, sizeof(out), 900, 40, 1, paint + 2, 1, &taken)), 0);
	QCOMPARE(received.client.next, uint64_t(900));
	QCOMPARE(received.client.line, uint64_t(40));
	QCOMPARE(received.text, QByteArray("old\nNOW"));
	QVERIFY(received.gaps.isEmpty());

	/* Output from there joins up. */
	received.output(900, "next");
	QCOMPARE(received.text, QByteArray("old\nNOWnext"));

	/* A keyframe saying lines went before it says so, count unknown. */
	QCOMPARE(feed(bssh_wire_put_keyframe(out, sizeof(out), 50, 1)), 0);
	QCOMPARE(received.history_lost, (QList<uint64_t>{ 2, 0 }));

	/*
	 * Cut short by a lost link, before its last part: the place has not
	 * moved, so the host sends it again from where the client stood, and
	 * the lines that did arrive are not taken twice, nor the loss said twice.
	 */
	QCOMPARE(feed(line(50, 'p')), 0);
	QCOMPARE(feed(line(51, 'q')), 0);
	const uint64_t stood = received.client.next;
	bssh_session_client_reset_link(&received.client);
	received.hello();
	QCOMPARE(received.client.next, stood);
	QCOMPARE(feed(bssh_wire_put_keyframe(out, sizeof(out), 50, 1)), 0);
	QCOMPARE(feed(line(50, 'p')), 0);
	QCOMPARE(feed(line(51, 'q')), 0);
	QCOMPARE(feed(line(52, 'r')), 0);
	QCOMPARE(feed(bssh_wire_put_repaint(out, sizeof(out), 2000, 90, 1, nullptr, 0, &taken)), 0);
	QCOMPARE(received.history.mid(2), (QList<QByteArray>{ "p", "q", "r" }));
	QCOMPARE(received.history_lost, (QList<uint64_t>{ 2, 0 }));
	QCOMPARE(received.client.next, uint64_t(2000));
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
		profile.resume_choice = wanted ? bssh_profile_t::RESUME_ALWAYS : bssh_profile_t::RESUME_NEVER;
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

	/* The attach frame's offset: the first of its kind the client wrote. */
	uint64_t attached_at() const {
		QByteArray bytes = fake()->written();
		for (;;) {
			bssh_wire_frame_t frame;
			uint32_t used = 0;
			uint64_t offset = 0;
			uint16_t columns = 0;
			uint16_t rows = 0;
			if (bssh_wire_take(reinterpret_cast<uint8_t *>(bytes.data()), uint32_t(bytes.size()), &frame,
			                   &used) != 1)
				return UINT64_MAX;
			if (frame.kind == BSSH_WIRE_ATTACH)
				return bssh_wire_read_attach(&frame, &offset, &columns, &rows) ? UINT64_MAX : offset;
			bytes.remove(0, int(used));
		}
	}
};

}

void session_host_test::a_host_not_asked_is_not_probed() {
	kept_t kept(false);
	QVERIFY(kept.fake()->side_commands().filter(QStringLiteral("beerssh-session")).isEmpty());
	QCOMPARE(kept.fake()->shells_opened(), 1);
	QVERIFY(!kept.fake()->raw_channel());
}

void session_host_test::a_kept_session_shows_a_hole_as_lines() {
	kept_t kept;
	kept.answer_probe();
	kept.host_says(kept.hello());

	auto lost = [](uint64_t offset, uint64_t lines) {
		uint8_t frame[32];
		const size_t len = bssh_wire_put_lost(frame, sizeof(frame), offset, lines);
		return QByteArray(reinterpret_cast<const char *>(frame), int(len));
	};

	kept.host_says(lost(1000, 37));
	QVERIFY2(kept.screen().contains(QStringLiteral("[37 lines lost]")), qPrintable(kept.screen()));

	kept.host_says(lost(1100, 38));
	QVERIFY2(kept.screen().contains(QStringLiteral("[1 line lost]")), qPrintable(kept.screen()));

	/* A full-screen program writes no newlines, and bytes are all there is. */
	kept.host_says(lost(1200, 38));
	QVERIFY2(kept.screen().contains(QStringLiteral("[100 bytes lost]")), qPrintable(kept.screen()));
}

void session_host_test::a_kept_session_puts_the_lines_it_missed_into_its_scrollback() {
	kept_t kept;
	kept.answer_probe();

	/* It asks for keyframes, ahead of the attach that a gap answers. */
	const QList<int> kinds = kept.kinds_written();
	QVERIFY(kinds.indexOf(BSSH_WIRE_KEYFRAMES) >= 0);
	QVERIFY(kinds.indexOf(BSSH_WIRE_KEYFRAMES) < kinds.indexOf(BSSH_WIRE_ATTACH));

	/* And its mode, which decides how the host answers a backlog at the attach. */
	QVERIFY(kinds.indexOf(BSSH_WIRE_MODE) >= 0);
	QVERIFY(kinds.indexOf(BSSH_WIRE_MODE) < kinds.indexOf(BSSH_WIRE_ATTACH));

	uint8_t out[256];
	auto frame = [&](size_t len) { return QByteArray(reinterpret_cast<const char *>(out), int(len)); };
	auto line = [&](uint64_t number, const char *text, int continuation) {
		bssh_screen_cell_t cells[16];
		memset(cells, 0, sizeof(cells));
		const int count = int(strlen(text));
		for (int i = 0; i < count; ++i) {
			cells[i].chars[0] = uint32_t(text[i]);
			cells[i].width = 1;
		}
		cells[0].fg.kind = BSSH_SCREEN_COLOR_INDEXED;
		cells[0].fg.index = 2;
		return frame(bssh_wire_put_history(out, sizeof(out), number, continuation, cells, count));
	};

	kept.host_says(kept.hello() + kept.output(0, "before\r\n"));
	const int had = kept.session->emulator()->scrollback().count();
	size_t taken = 0;
	const char *paint = "\x1b[H\x1b[2Jthe-screen-now";
	kept.host_says(frame(bssh_wire_put_keyframe(out, sizeof(out), 5, 1)) + line(5, "five", 0)
	               + line(6, "six", 1)
	               + frame(bssh_wire_put_repaint(out, sizeof(out), 9000, 100, 1,
	                                             reinterpret_cast<const uint8_t *>(paint), strlen(paint),
	                                             &taken)));

	const bssh_scrollback &history = kept.session->emulator()->scrollback();
	auto text_of = [&](int index) {
		QString text;
		for (const bssh_cell_t &cell : history.line(index)) text += QChar::fromUcs4(cell.ch);
		return text.trimmed();
	};
	QCOMPARE(history.count(), had + 3);
	QCOMPARE(text_of(had), QStringLiteral("[earlier lines lost]"));
	QCOMPARE(text_of(had + 1), QStringLiteral("five"));
	QCOMPARE(text_of(had + 2), QStringLiteral("six"));
	QVERIFY(history.is_continuation(had + 2));

	/* Its colour is the host's, by the rule this terminal reads its own with. */
	QCOMPARE(int(history.line(had + 1).first().fg.kind), int(bssh_color_t::INDEXED));
	QCOMPARE(int(history.line(had + 1).first().fg.index), 2);

	QVERIFY2(kept.screen().contains(QStringLiteral("the-screen-now")), qPrintable(kept.screen()));
	QVERIFY(!kept.screen().contains(QStringLiteral("before")));

	/* And output from where the screen stands joins up, with no hole drawn. */
	kept.host_says(kept.output(9000, " and-after"));
	QVERIFY2(kept.screen().contains(QStringLiteral("the-screen-now and-after")), qPrintable(kept.screen()));
	QVERIFY(!kept.screen().contains(QStringLiteral("lost]")));
}

void session_host_test::a_resize_while_away_does_not_bring_lines_back_twice() {
	/*
	 * A phone turned while its link was down: the terminal narrows on its
	 * own, and its reflow pushes rows of the screen it had into its history.
	 * The host, told the new size at the attach, reflows its copy too and
	 * sends what went off the top -- those rows among them. Taken on top of
	 * the client's own, they would read twice. The host here is the real
	 * screen model, fed what the client was fed.
	 */
	kept_t kept;
	kept.answer_probe();
	kept.session->resize(80, 24);
	QByteArray written;
	for (int i = 1; i <= 40; ++i)
		written += "line " + QByteArray::number(i) + " " + QByteArray(i % 2 ? 50 : 8, 'x') + "\r\n";
	bssh_screen_model_t *host = bssh_screen_model_new(24, 80);
	QVERIFY(host);
	bssh_screen_model_feed(host, reinterpret_cast<const uint8_t *>(written.constData()), size_t(written.size()));
	uint8_t big[8192];
	kept.host_says(kept.hello()
	               + QByteArray(reinterpret_cast<const char *>(big),
	                            int(bssh_wire_put_output(big, sizeof(big), 0,
	                                                     reinterpret_cast<const uint8_t *>(written.constData()),
	                                                     size_t(written.size())))));
	QVERIFY(kept.screen().contains(QStringLiteral("line 40")));
	const uint64_t at = uint64_t(written.size());

	kept.fake()->simulate_connection_lost();
	kept.session->resize(40, 24); /* turned, while away */
	kept.session->reconnect();
	kept.fake()->simulate_key_auth_success();
	kept.answer_probe();

	/* The attach tells the host the size, and it answers with a keyframe. */
	bssh_screen_model_resize(host, 24, 40);
	uint64_t from = 0, first = 0, count = 0;
	const int known = bssh_screen_model_history_after(host, at, &from);
	bssh_screen_model_history(host, &first, &count);
	QByteArray frames = kept.hello();
	uint8_t out[4096];
	frames += QByteArray(reinterpret_cast<const char *>(out), int(bssh_wire_put_keyframe(out, sizeof(out), from, !known)));
	bssh_screen_cell_t cells[256];
	for (uint64_t n = from; n < first + count; ++n) {
		int continuation = 0;
		const int width = bssh_screen_model_history_line(host, n, cells, 256, &continuation);
		frames += QByteArray(reinterpret_cast<const char *>(out),
		                     int(bssh_wire_put_history(out, sizeof(out), n, continuation, cells, width)));
	}
	const size_t length = bssh_screen_model_repaint(host, nullptr, 0);
	QByteArray repaint(int(length), Qt::Uninitialized);
	bssh_screen_model_repaint(host, reinterpret_cast<uint8_t *>(repaint.data()), length);
	size_t taken = 0;
	frames += QByteArray(reinterpret_cast<const char *>(out),
	                     int(bssh_wire_put_repaint(out, sizeof(out), at, 40, 1,
	                                               reinterpret_cast<const uint8_t *>(repaint.constData()),
	                                               size_t(repaint.size()), &taken)));
	QCOMPARE(taken, size_t(repaint.size()));
	kept.host_says(frames);

	/* Line by line, joined where a row continues the one above: once each, in order. */
	auto logical = [](QStringList rows, QList<bool> continues) {
		QStringList lines;
		for (int i = 0; i < rows.size(); ++i) {
			if (continues.at(i) && !lines.isEmpty()) lines.last() += rows.at(i);
			else lines.append(rows.at(i));
		}
		for (QString &line : lines) line = line.trimmed();
		return lines;
	};
	const bssh_scrollback &mine = kept.session->emulator()->scrollback();
	QStringList rows;
	QList<bool> continues;
	for (int i = 0; i < mine.count(); ++i) {
		QString text;
		for (const bssh_cell_t &cell : mine.line(i)) if (cell.width) text += QChar::fromUcs4(cell.ch);
		rows << text;
		continues << mine.is_continuation(i);
	}
	QStringList theirs_rows;
	QList<bool> theirs_continues;
	for (uint64_t n = first; n < first + count; ++n) {
		int continuation = 0;
		const int width = bssh_screen_model_history_line(host, n, cells, 256, &continuation);
		QString text;
		for (int c = 0; c < width; ++c)
			if (cells[c].chars[0] != 0xFFFFFFFFu) text += QChar::fromUcs4(cells[c].chars[0] ? cells[c].chars[0] : U' ');
		theirs_rows << text;
		theirs_continues << (continuation != 0);
	}
	QCOMPARE(logical(rows, continues), logical(theirs_rows, theirs_continues));
	bssh_screen_model_free(host);
}

void session_host_test::a_backlog_is_drawn_but_not_answered() {
	/*
	 * What a program wrote while nobody watched, sent before the host says
	 * it has caught up: drawn, but a question in it was asked long ago and
	 * its answer would be typed into whatever runs now, and its bell rang
	 * for nobody. What the user types meanwhile still goes. After the host
	 * has caught up, the same bytes are answered and ring. Two questions,
	 * since the two are answered by different code: device attributes by
	 * this terminal's own reply, the cursor's place by libvterm's.
	 */
	kept_t kept;
	kept.answer_probe();
	QSignalSpy rang(kept.session, &bssh_session::bell_rang);
	auto inputs = [&kept] { return int(kept.kinds_written().count(BSSH_WIRE_INPUT)); };
	const int before = inputs();

	kept.host_says(kept.hello() + kept.output(0, "from-the-backlog\x1b[c\x1b[6n\x07"));
	QVERIFY(kept.screen().contains(QStringLiteral("from-the-backlog")));
	QCOMPARE(inputs(), before);
	QCOMPARE(rang.count(), 0);

	kept.session->emulator()->send_paste(QStringLiteral("typed"));
	const int typed = inputs();
	QVERIFY(typed > before);

	uint8_t frame[8];
	const size_t len = bssh_wire_put(frame, sizeof(frame), BSSH_WIRE_CAUGHT_UP, nullptr, 0);
	const qsizetype end = qsizetype(strlen("from-the-backlog\x1b[c\x1b[6n\x07"));
	kept.host_says(QByteArray(reinterpret_cast<const char *>(frame), int(len)) + kept.output(uint64_t(end), "\x1b[c\x1b[6n\x07"));
	QVERIFY(inputs() > typed);
	QCOMPARE(rang.count(), 1);
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

void session_host_test::keeping_every_update_switches_during_a_session() {
	kept_t kept;
	QVERIFY(!kept.session->is_kept());
	kept.answer_probe();
	QVERIFY(kept.session->is_kept());

	const auto modes_sent = [&kept] {
		QByteArray bytes = kept.fake()->written();
		QList<int> modes;
		for (;;) {
			bssh_wire_frame_t frame;
			uint32_t used = 0;
			if (bssh_wire_take(reinterpret_cast<uint8_t *>(bytes.data()), uint32_t(bytes.size()), &frame,
			                   &used) != 1)
				return modes;
			uint8_t mode = 0;
			if (frame.kind == BSSH_WIRE_MODE && !bssh_wire_read_mode(&frame, &mode)) modes.append(mode);
			bytes.remove(0, int(used));
		}
	};

	/* On the same connection, at once. */
	QCOMPARE(modes_sent(), QList<int>{ BSSH_WIRE_MODE_LIVE });
	kept.session->set_keeps_every_update(true);
	QCOMPARE(modes_sent(), QList<int>({ BSSH_WIRE_MODE_LIVE, BSSH_WIRE_MODE_SERIAL }));
	QVERIFY(kept.session->keeps_every_update());

	/* And it holds across a drop, for a host that may have started anew. */
	kept.fake()->simulate_connection_lost();
	kept.session->reconnect();
	kept.fake()->simulate_key_auth_success();
	kept.answer_probe();
	QCOMPARE(modes_sent(), QList<int>{ BSSH_WIRE_MODE_SERIAL });
}

void session_host_test::a_mode_set_with_nothing_kept_waits_for_the_attach() {
	kept_t kept;
	kept.session->set_keeps_every_update(true);
	QVERIFY2(kept.fake()->written().isEmpty(), "a frame reached a shell that is not this protocol");
}
