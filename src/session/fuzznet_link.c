/*
 * A client's end of a session over fuzznet (fuzznet_link.h).
 */

#define _DEFAULT_SOURCE

#include "fuzznet_link.h"

#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "stream.h"

#include "device/device.h"
#include "net/udp.h"
#include "session/aead_monocypher.h"
#include "session/hash_monocypher.h"
#include "session/random_system.h"

/* A word's payload, less the stream's header, with room to spare: as beersshd's. */
#define SEGMENT_DATA 960u
#define RING 65536u

struct bssh_fuzznet_link {
	fzn_device_t device;
	fzn_hash_ops_t hash;
	fzn_aead_ops_t aead;
	fzn_random_ops_t rng;
	fzn_bearer_t bearer;
	fzn_bearer_join_t join;
	fzn_bearer_slot_t slot;
	fzn_udp_link_t udp;

	bssh_stream_t stream;
	uint8_t ring[RING];
	void (*deliver)(void *context, const uint8_t *bytes, size_t len);
	void *context;
};

static uint64_t monotonic_ms(void *ctx) {
	struct timespec ts;
	(void)ctx;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* A word the host said: a segment of its stream. One that is not is dropped. */
static void heard(void *ctx, const uint8_t *bytes, size_t len) {
	bssh_fuzznet_link_t *link = ctx;
	(void)bssh_stream_receive(&link->stream, bytes, len, monotonic_ms(NULL), link->deliver,
	                          link->context);
}

bssh_fuzznet_link_t *bssh_fuzznet_link_open(const char *image, const char *address, uint16_t port,
                                            void (*deliver)(void *context, const uint8_t *bytes,
                                                            size_t len),
                                            void *context, const char **why) {
	static const char *ignored;
	if (!why) why = &ignored;
	bssh_fuzznet_link_t *link = calloc(1, sizeof(*link));
	if (!link) {
		*why = "no memory";
		return NULL;
	}
	link->udp.fd = -1;
	if (!image || !fzn_device_image_from_text(image, strlen(image), &link->device.image)) {
		*why = "not a device image";
		free(link);
		return NULL;
	}
	if (fzn_udp_resolve(AF_INET, address, port, &link->udp.to) != FZN_UDP_OK
	    || fzn_udp_bind(AF_INET, NULL, 0u, &link->udp.fd) != FZN_UDP_OK) {
		*why = "no UDP socket to that address";
		if (link->udp.fd >= 0) close(link->udp.fd);
		free(link);
		return NULL;
	}
	fzn_hash_monocypher_init(&link->hash);
	fzn_aead_monocypher_init(&link->aead);
	fzn_random_system_init(&link->rng);
	fzn_udp_bearer(&link->bearer, &link->udp);
	link->join.held = &link->slot;
	link->join.n_held = 1u;
	link->join.unit = link->bearer.unit;

	fzn_device_t *d = &link->device;
	d->hash = &link->hash;
	d->aead = &link->aead;
	d->rng = &link->rng;
	d->bearer = &link->bearer;
	d->join = &link->join;
	d->uptime_ms = monotonic_ms;
	/* A session asked for waits this long a try, and tries this often. */
	d->wait_ms = 400u;
	d->tries = 10u;
	d->life_s = 60u;
	d->heard = heard;
	d->heard_ctx = link;

	/* A new stream each link, so the host can tell it from the last one. */
	uint64_t epoch = 0;
	if (getentropy(&epoch, sizeof(epoch)) != 0) {
		*why = "no random number for the stream";
		close(link->udp.fd);
		free(link);
		return NULL;
	}
	bssh_stream_init(&link->stream, link->ring, RING, SEGMENT_DATA, RING, epoch);
	link->deliver = deliver;
	link->context = context;
	return link;
}

void bssh_fuzznet_link_close(bssh_fuzznet_link_t *link) {
	if (!link) return;
	if (link->udp.fd >= 0) close(link->udp.fd);
	/* What it held is the session's keys: gone with it. */
	memset(link, 0, sizeof(*link));
	free(link);
}

size_t bssh_fuzznet_link_write(bssh_fuzznet_link_t *link, const uint8_t *bytes, size_t len) {
	return bssh_stream_write(&link->stream, bytes, len);
}

size_t bssh_fuzznet_link_pending(const bssh_fuzznet_link_t *link) {
	return bssh_stream_pending(&link->stream);
}

int bssh_fuzznet_link_up(const bssh_fuzznet_link_t *link) {
	return link->device.session_up;
}

unsigned bssh_fuzznet_link_backoffs(const bssh_fuzznet_link_t *link) {
	return link->stream.backoffs;
}

int bssh_fuzznet_link_pump(bssh_fuzznet_link_t *link, unsigned wait_ms) {
	uint8_t segment[SEGMENT_DATA + BSSH_STREAM_HEADER_MAX];
	size_t n;
	while ((n = bssh_stream_poll(&link->stream, monotonic_ms(NULL), segment, sizeof(segment))) > 0) {
		if (fzn_device_say(&link->device, segment, n) != FZN_DEVICE_OK) return -1;
	}
	/* Until the stream next has something to do, within what was asked. */
	const int64_t due = bssh_stream_wait(&link->stream, monotonic_ms(NULL));
	unsigned listen = wait_ms;
	if (due >= 0 && (uint64_t)due < listen) listen = (unsigned)due;
	if (!link->device.session_up) {
		/*
		 * Nothing to hear in before a session stands: an empty word asks
		 * for one, and tells the host where this device is, so it can
		 * speak first (fuzznet's sec 935).
		 */
		if (fzn_device_say(&link->device, segment, 0u) != FZN_DEVICE_OK) return -1;
	}
	if (listen) fzn_device_listen(&link->device, listen);
	return 0;
}
