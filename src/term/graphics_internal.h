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
struct bssh_graphics_transmission_t {
	QByteArray data;
	QMap<QString, QString> controls;
};

bssh_graphics_result_t bssh_parse_kitty(const QByteArray &payload,
                                         bssh_graphics_transmission_t &transmission,
                                         int cell_width, int cell_height);

bssh_graphics_result_t bssh_parse_iterm2(const QByteArray &payload,
                                          int cell_width, int cell_height);

bssh_graphics_result_t bssh_parse_sixel(const QByteArray &payload,
                                         int cell_width, int cell_height);

#endif /* BSSH_GRAPHICS_INTERNAL_H */
