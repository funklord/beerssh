/*
 * A program on a pty, its output through bssh_emulator: the BeerSSH side of
 * tool/check-xterm.py, run as BSSH_TEST_PTY_HOST=COLSxROWS beerssh_test
 * program args. Answers DECRQCRA from the cells, which the app does not:
 * letting a remote read the screen back is not a feature it wants. Ends
 * when the program does.
 */
#ifndef BSSH_TEST_PTY_HOST_H
#define BSSH_TEST_PTY_HOST_H

#include <QByteArray>
#include <QCoreApplication>
#include <QList>

#include <poll.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "term/emulator.h"

/* ESC [ params * y at `at`: its parameters and length, or false. */
static bool parse_checksum_request(const QByteArray &bytes, int at, int *length, QList<int> *parameters) {
	QByteArray number;
	parameters->clear();
	for (int i = at + 2; i < bytes.size(); i++) {
		const char c = bytes[i];
		if (c >= '0' && c <= '9') {
			number += c;
		} else if (c == ';') {
			parameters->append(number.toInt());
			number.clear();
		} else if (c == '*' && i + 1 < bytes.size() && bytes[i + 1] == 'y') {
			parameters->append(number.toInt());
			*length = i + 2 - at;
			return true;
		} else {
			return false;
		}
	}
	return false;
}

static unsigned checksum_of(const bssh_screen &screen, const QList<int> &request) {
	unsigned sum = 0;
	const int top = request.value(2, 1), left = request.value(3, 1);
	const int bottom = request.value(4, screen.rows()), right = request.value(5, screen.cols());
	for (int y = qMax(top, 1); y <= qMin(bottom, screen.rows()); y++) {
		for (int x = qMax(left, 1); x <= qMin(right, screen.cols()); x++) {
			const bssh_cell_t &cell = screen.cell(y - 1, x - 1);
			if (!(cell.attrs & bssh_cell_t::UNWRITTEN)) sum += cell.ch;
		}
	}
	return sum;
}

/* Where to cut what has arrived: a request may be only half here. */
static int complete_length(const QByteArray &bytes) {
	const int escape = bytes.lastIndexOf('\x1b');
	if (escape < 0 || bytes.size() - escape > 40) return bytes.size();
	for (int i = escape + 1; i < bytes.size(); i++) {
		const char c = bytes[i];
		if (!((c >= '0' && c <= '9') || c == ';' || c == '[' || c == '*')) return bytes.size();
	}
	return escape;
}

static int run_pty_host(const QByteArray &size, char **command) {
	const QList<QByteArray> sides = size.split('x');
	const int cols = sides.value(0).toInt(), rows = sides.value(1).toInt();
	bssh_emulator emulator(cols, rows);

	int master = -1;
	struct winsize window = {quint16(rows), quint16(cols), 0, 0};
	const pid_t child = forkpty(&master, nullptr, nullptr, &window);
	if (child == 0) {
		execvp(command[0], command);
		_exit(127);
	}

	const auto flush = [&] {
		const QByteArray output = emulator.take_output();
		if (!output.isEmpty() && write(master, output.constData(), size_t(output.size())) < 0) _exit(1);
	};

	QByteArray pending;
	char data[65536];
	for (;;) {
		struct pollfd ready = {master, POLLIN, 0};
		if (poll(&ready, 1, 2000) == 0) {
			if (waitpid(child, nullptr, WNOHANG) == child) break;
			continue;
		}
		const ssize_t count = read(master, data, sizeof data);
		if (count <= 0) break;
		pending.append(data, int(count));

		for (int at = pending.indexOf("\x1b["); at >= 0; at = pending.indexOf("\x1b[", at + 2)) {
			int length = 0;
			QList<int> request;
			if (!parse_checksum_request(pending, at, &length, &request)) continue;

			emulator.feed(pending.left(at));
			flush();
			char reply[64];
			const int size_of_reply = snprintf(reply, sizeof reply, "\x1bP%d!~%04X\x1b\\", request.value(0),
			                                   checksum_of(emulator.screen(), request) & 0xffff);
			if (write(master, reply, size_t(size_of_reply)) < 0) _exit(1);
			pending.remove(0, at + length);
			at = -2;
		}

		const int keep = complete_length(pending);
		emulator.feed(pending.left(keep));
		flush();
		pending.remove(0, keep);
		QCoreApplication::processEvents();
	}
	return 0;
}

#endif
