#ifndef BSSH_GRAPHICS_INTERNAL_H
#define BSSH_GRAPHICS_INTERNAL_H

#include <QMap>
#include <QString>

#include "term/image_store.h"

/*
 * Parsers for the three inline-image protocols (project.md sec 7.1).
 *
 * Split out from the emulator so they can be tested directly on a byte
 * string, without a VTerm, a screen or a widget -- these are pure
 * functions from protocol bytes to a placement, which is the easiest
 * possible thing to test and the hardest thing to get right by eye.
 */
struct bssh_graphics_result_t {
	enum action_t {
		NOTHING,        /* unparseable, or a protocol we decline */
		PLACE,
		CONTINUE,       /* a chunk arrived; more follows */
		DELETE_IMAGES,

		/*
		 * a=q -- kitty's "can you hear me". The client sends a tiny
		 * image with the query action and expects an acknowledgement;
		 * no image is stored and nothing is drawn. It carries an id so
		 * the answer can be matched to the question.
		 */
		ACKNOWLEDGE
	};

	action_t action;
	int id;
	bssh_image_placement_t placement;

	bssh_graphics_result_t() : action(NOTHING), id(0) {}
};

/*
 * State for a chunked transmission -- a large image does not arrive in one
 * APC. Both halves matter: the payload accumulates, and so do the controls,
 * because the format, dimensions and id are sent only on the FIRST chunk.
 * Keeping just the bytes and re-reading the controls from the final chunk
 * gets a default-format decode of a PNG, which fails silently.
 */
/*
 * The most base64 a single kitty transmission may accumulate.
 *
 * bssh_accumulate bounds ONE APC payload at 8 MB and clears on the
 * initial fragment. It does not bound a TRANSMISSION, which m=1 stitches
 * together from any number of complete, in-budget payloads -- and the
 * only things that ever emptied it were m=0, which a remote need never
 * send, and a reset -- and the reset half of that was a claim rather
 * than a fact until 2026-09-22, since the soft reset goes through
 * libvterm and this buffer is ours. bssh_emulator::reset_modes clears
 * it now, which is what sec 6.4's Reset Terminal is for. So the wire could hand this 8 MB at a time for as
 * long as it liked: the product of two bounds, which is not a bound.
 *
 * 32 MB of base64 is about 24 MB decoded, which is already past the image
 * store's whole 32 MB budget once decoded to pixels -- so nothing a real
 * client sends comes near it. A 4K screenshot is a few megabytes.
 */
const qint64 BSSH_MAX_TRANSMISSION_BYTES = 32 * 1024 * 1024;

struct bssh_graphics_transmission_t {
	QByteArray data;
	QMap<QString, QString> controls;

	/*
	 * This transmission went past BSSH_MAX_TRANSMISSION_BYTES and is
	 * being thrown away.
	 *
	 * Held until m=0 rather than only clearing the buffer, because the
	 * chunks that follow belong to the same doomed image: accumulating
	 * them again would leave a tail in the buffer for the NEXT
	 * transmission to be prepended to, which decodes to nothing and
	 * reads as the terminal having stopped accepting images.
	 */
	bool abandoned = false;
};

bssh_graphics_result_t bssh_parse_kitty(const QByteArray &payload,
                                         bssh_graphics_transmission_t &transmission,
                                         int cell_width, int cell_height);

bssh_graphics_result_t bssh_parse_iterm2(const QByteArray &payload,
                                          int cell_width, int cell_height);

bssh_graphics_result_t bssh_parse_sixel(const QByteArray &payload,
                                         int cell_width, int cell_height);

#endif /* BSSH_GRAPHICS_INTERNAL_H */
