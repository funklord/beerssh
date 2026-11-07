#ifndef BSSH_SESSION_CLIENT_H
#define BSSH_SESSION_CLIENT_H

/*
 * The client's half of the session protocol (project.md sec 5.6): frames in,
 * terminal bytes out, and where to ask from after a drop. It knows no
 * transport and no terminal, so BeerSSH's SSH path and a fuzznet shell share
 * it.
 *
 * THE OFFSET IS WHAT SURVIVES A DROP. `next` is the first byte not yet seen,
 * and it is kept across connections: bssh_session_client_reset_link() forgets
 * only a half-read frame, and the attach that follows asks from `next`.
 *
 * A GAP IS NEVER JOINED OVER. Output that lands past `next` means bytes went
 * missing, whatever the reason, and the caller is told how many before the
 * bytes that follow them: terminal bytes on either side of a hole are not one
 * stream, and fed as one they draw something the program never wrote. Output
 * that overlaps what was seen, which a resend after a drop can do, has the
 * seen part removed.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BSSH_SESSION_CLIENT_BUFFER 65600u

typedef struct bssh_session_client_events {
	void *context;
	void (*output)(void *context, const uint8_t *bytes, size_t len);
	/*
	 * `count` bytes are gone for good, and `next` now stands after them. `lines`
	 * is how many newlines were among them, which is what a person can picture,
	 * and 0 when that is not known or a full-screen program wrote none.
	 */
	void (*gap)(void *context, uint64_t count, uint64_t lines);
	void (*exited)(void *context, int32_t status);
	/* The session asked for is gone and this is a new one: a reboot, say. */
	void (*restarted)(void *context);
	/* What was written before this attach has all been delivered. */
	void (*caught_up)(void *context);
} bssh_session_client_events_t;

typedef struct bssh_session_client {
	bssh_session_client_events_t events;
	uint64_t next;
	uint64_t line;   /* newlines before `next`, from the session's start */
	uint64_t acked;
	uint8_t in[BSSH_SESSION_CLIENT_BUFFER];
	uint32_t in_used;
	int said_hello;  /* this link has proved it speaks the protocol */
} bssh_session_client_t;

void bssh_session_client_init(bssh_session_client_t *client,
                              const bssh_session_client_events_t *events);

/*
 * Bytes from the connection. Returns 0, or -1 when the peer is not speaking
 * this protocol, which a caller takes as the cue for plain SSH.
 */
int bssh_session_client_feed(bssh_session_client_t *client, const uint8_t *bytes, size_t len);

/* The connection went. What was seen stands; a half-read frame does not. */
void bssh_session_client_reset_link(bssh_session_client_t *client);

/* An acknowledgement if `every` bytes have arrived since the last one, or 0.
 * Writes to `out` as the bssh_wire_put functions do. */
size_t bssh_session_client_ack_due(bssh_session_client_t *client, uint64_t every, uint8_t *out,
                                   size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
