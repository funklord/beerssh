/*
 * One question over fuzznet, as BeerSSH's client asks it (project.md sec
 * 5.6): tool/check-fuzznet.sh's other end. Given a device image and a host's
 * UDP port, it sends the session helper's probe, "beerssh-session version",
 * and prints what comes back.
 *
 * What stops it: a 20 s deadline, whatever is heard.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "session/fuzznet_link.h"

static char got[256];
static size_t got_len;

static void deliver(void *context, const uint8_t *bytes, size_t len) {
	(void)context;
	if (len > sizeof(got) - 1 - got_len) len = sizeof(got) - 1 - got_len;
	memcpy(got + got_len, bytes, len);
	got_len += len;
}

static time_t now(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec;
}

int main(int argc, char **argv) {
	if (argc != 4) {
		fprintf(stderr, "usage: bssh-fuzznet-probe IMAGE ADDRESS PORT\n");
		return 2;
	}
	const char *why = "";
	bssh_fuzznet_link_t *link = bssh_fuzznet_link_open(argv[1], argv[2],
	                                                   (uint16_t)strtoul(argv[3], NULL, 10),
	                                                   deliver, NULL, &why);
	if (!link) {
		fprintf(stderr, "bssh-fuzznet-probe: %s\n", why);
		return 1;
	}
	static const char question[] = "beerssh-session version\n";
	bssh_fuzznet_link_write(link, (const uint8_t *)question, sizeof(question) - 1);
	const time_t until = now() + 20;
	while (now() < until && !memchr(got, '\n', got_len)) {
		if (bssh_fuzznet_link_pump(link, 200u) != 0) {
			fprintf(stderr, "bssh-fuzznet-probe: no session with the host\n");
			bssh_fuzznet_link_close(link);
			return 1;
		}
	}
	bssh_fuzznet_link_close(link);
	if (!memchr(got, '\n', got_len)) {
		fprintf(stderr, "bssh-fuzznet-probe: nothing came back\n");
		return 1;
	}
	fwrite(got, 1, got_len, stdout);
	return 0;
}
