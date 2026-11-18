#ifndef BSSH_FUZZNET_LINK_H
#define BSSH_FUZZNET_LINK_H

/*
 * A client's end of a session over fuzznet (project.md sec 5.6): a device
 * paired to the host (fuzznet's `device/`, its image text), speaking to the
 * host's UDP port, with the reliable stream of stream.h inside the words it
 * says and hears. What goes in is what an SSH exec channel to the session
 * helper carries -- the command line, then the session's frames -- and what
 * comes out is the helper's output.
 *
 * Opaque, because its insides are fuzznet's headers, which only this file's
 * C includes: a caller in C++ sees none of them. Blocking where fuzznet's
 * device is -- pump() waits up to the time it is given -- so a GUI runs it
 * on a thread of its own.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct bssh_fuzznet_link bssh_fuzznet_link_t;

/*
 * A link to the host at `address` (IPv4, dotted) `port`, as the device the
 * image text `image` describes. NULL, with `why` saying so, when the image
 * is not one or no socket could be had. `deliver` is handed the host's
 * stream in order, from pump().
 */
bssh_fuzznet_link_t *bssh_fuzznet_link_open(const char *image, const char *address, uint16_t port,
                                            void (*deliver)(void *context, const uint8_t *bytes,
                                                            size_t len),
                                            void *context, const char **why);
void bssh_fuzznet_link_close(bssh_fuzznet_link_t *link);

/* As much of `bytes` as the stream has room for; how much that was. */
size_t bssh_fuzznet_link_write(bssh_fuzznet_link_t *link, const uint8_t *bytes, size_t len);

/* Bytes written and not yet acknowledged. */
size_t bssh_fuzznet_link_pending(const bssh_fuzznet_link_t *link);

/*
 * Sends what the stream has to send and hears the host for up to `wait_ms`,
 * delivering what arrives. 0, or -1 when the device could not speak at all
 * -- no session with the host to be had.
 */
int bssh_fuzznet_link_pump(bssh_fuzznet_link_t *link, unsigned wait_ms);

#ifdef __cplusplus
}
#endif

#endif
