#include <QByteArray>
#include <QImage>
#include <QMap>
#include <QString>

#ifndef BSSH_NO_SIXEL
#include <sixel.h>
#endif

#include "term/graphics_internal.h"

/*
 * The three inline-image protocols (project.md sec 7.1, tier 4).
 *
 * They agree on nothing except the goal. Kitty sends APC-wrapped
 * key=value controls with a base64 payload; iTerm2 sends an OSC 1337 with
 * a different key=value syntax and a base64 payload; sixel sends a DCS
 * with a pixel format from 1974. Each parser below reduces its protocol
 * to one bssh_image_placement_t, which is the only thing the rest of the
 * project sees.
 *
 * Decoding is Qt's, not ours: QImage reads PNG and JPEG, and
 * QByteArray::fromBase64 does the transport decoding, so neither protocol
 * needs an image decoder written here. Sixel is the exception and is the
 * one place a real library is required -- see parse_sixel.
 */

namespace {

/*
 * The largest cell footprint an image may claim, in either direction.
 *
 * A ceiling rather than a clamp to the grid: the parsers are given a cell
 * size and nothing else, and an image placed off the right of a small
 * window is legitimate -- the renderer crops it. What is not legitimate is
 * a number this side of a `int` multiplication, and both protocols hand
 * one straight over. `c=300000000` on a 9-pixel cell overflows
 * `cols * cell_width` in the renderer's target rectangle, which is
 * undefined behaviour reached from bytes a remote sent; the row count has
 * the same shape and also drives the cursor advance the emulator emits
 * afterwards.
 *
 * 10000 is chosen to be far above any real terminal and far below the
 * point where the arithmetic downstream can wrap.
 */
const int BSSH_MAX_IMAGE_CELLS = 10000;

/*
 * The most pixels this will decode into an image, for any protocol.
 *
 * Tied to bssh_image_store's 32 MB per-session budget: at three bytes a
 * pixel this is 25 MB, so an image that passes fits the budget rather
 * than being evicted the moment it arrives. It is also larger than a 4K
 * screen (8,294,400), which is the biggest thing anybody sensibly sends
 * to a terminal.
 */
const qint64 BSSH_MAX_IMAGE_PIXELS = 8 * 1024 * 1024;


/* "a=T,f=100,c=2,r=1" -> {a: T, f: 100, ...} */
QMap<QString, QString> parse_kitty_controls(const QString &text) {
	QMap<QString, QString> controls;

	const QStringList parts = text.split(QLatin1Char(','), Qt::SkipEmptyParts);
	for (const QString &part : parts) {
		const int equals = part.indexOf(QLatin1Char('='));
		if (equals <= 0) continue;
		controls.insert(part.left(equals).trimmed(), part.mid(equals + 1).trimmed());
	}

	return controls;
}

} /* namespace */

bssh_graphics_result_t bssh_parse_kitty(const QByteArray &payload,
                                         bssh_graphics_transmission_t &transmission,
                                         int cell_width, int cell_height) {
	bssh_graphics_result_t result;

	/* Payload is "G<controls>;<base64 data>", the leading G already known. */
	QByteArray body = payload;
	if (body.startsWith('G')) body.remove(0, 1);

	const int semicolon = body.indexOf(';');
	const QString control_text = QString::fromLatin1(semicolon >= 0 ? body.left(semicolon) : body);
	const QByteArray data = semicolon >= 0 ? body.mid(semicolon + 1) : QByteArray();

	const QMap<QString, QString> chunk_controls = parse_kitty_controls(control_text);

	/*
	 * Controls carry over from the first chunk of a transmission: the
	 * format, pixel dimensions and id are sent once, and a continuation
	 * chunk says little more than "m=0". Reading only the final chunk's
	 * controls decodes a PNG as raw RGBA and quietly produces nothing.
	 */
	QMap<QString, QString> controls = transmission.controls;
	for (auto it = chunk_controls.constBegin(); it != chunk_controls.constEnd(); ++it) {
		controls.insert(it.key(), it.value());
	}

	const QString action = controls.value(QStringLiteral("a"), QStringLiteral("t"));

	/*
	 * a=q is kitty's capability probe: a one-pixel image sent with the
	 * query action, asking only whether anything is listening. It is
	 * answered and NOT stored.
	 *
	 * This was missing, and the shape of the gap is the point. The query
	 * fell through to the transmit path, which decoded four bytes of
	 * base64 into a 1x1 image and placed it -- so the probe produced no
	 * reply at all, and a client that probes before using the protocol
	 * concluded the terminal had no kitty graphics while transmission
	 * worked perfectly. An implemented feature that answers "no" when
	 * asked is sec 7.1's own complaint about DEC 2026, arriving in the
	 * one protocol where the query is the ONLY way to find out: kitty
	 * graphics has no terminfo capability and no DA parameter.
	 *
	 * Found from the other end. The sibling qtty TUI probes for it in
	 * `src/backend/ansi/term_caps.cpp`, which sends
	 * `a=q,t=d,f=24,s=1,v=1` and looks for the reply in find_kitty() --
	 * so every Qt Widgets application it renders was falling back to
	 * half-blocks against a terminal that can draw pixels.
	 */
	if (action == QStringLiteral("q")) {
		result.action = bssh_graphics_result_t::ACKNOWLEDGE;
		result.id = controls.value(QStringLiteral("i")).toInt();
		return result;
	}

	if (action == QStringLiteral("d")) {
		result.action = bssh_graphics_result_t::DELETE_IMAGES;
		result.id = controls.value(QStringLiteral("i")).toInt();
		return result;
	}

	/*
	 * m=1 means another chunk follows. Terminals receive these split
	 * across reads anyway, so accumulating until m=0 is not an
	 * optimisation -- a large image simply does not arrive any other way.
	 */
	/*
	 * Bounded before it is appended to. A transmission over the cap is
	 * abandoned whole rather than truncated: half a base64 stream decodes
	 * to a different image, or to nothing, and neither is worth the
	 * memory it took to hold.
	 */
	if (qint64(transmission.data.size()) + data.size() > BSSH_MAX_TRANSMISSION_BYTES) {
		transmission.abandoned = true;
		transmission.data.clear();
		transmission.controls.clear();
	}

	/*
	 * An abandoned transmission stays abandoned until its last chunk.
	 * Clearing the buffer alone would let the REST of the same image
	 * accumulate again and sit there waiting for the next transmission to
	 * be appended to it -- which decodes to nothing, and reads as the
	 * terminal having stopped accepting images altogether.
	 */
	if (transmission.abandoned) {
		if (chunk_controls.value(QStringLiteral("m")) != QStringLiteral("1")) {
			transmission.abandoned = false;
			transmission.data.clear();
			transmission.controls.clear();
		}
		return result;
	}

	transmission.data.append(data);
	if (chunk_controls.value(QStringLiteral("m")) == QStringLiteral("1")) {
		transmission.controls = controls;
		result.action = bssh_graphics_result_t::CONTINUE;
		return result;
	}

	const QByteArray decoded = QByteArray::fromBase64(transmission.data);
	transmission.data.clear();
	transmission.controls.clear();

	if (decoded.isEmpty()) return result;

	const int format = controls.value(QStringLiteral("f"), QStringLiteral("32")).toInt();
	const int pixel_width = controls.value(QStringLiteral("s")).toInt();
	const int pixel_height = controls.value(QStringLiteral("v")).toInt();

	QImage image;
	if (format == 100) {
		/* PNG, which Qt decodes; kitty's "f=100" is exactly that. */
		image = QImage::fromData(decoded);

		/*
		 * The payload cap bounds the COMPRESSED size and says nothing
		 * about what it decodes to. A two-kilobyte solid-colour PNG is
		 * 5792x5792 once expanded -- 128 MB, which is Qt's own
		 * allocation limit and four times the image store's entire
		 * budget. The store cannot reclaim it either: enforce_budget
		 * refuses to drop anything still on screen.
		 *
		 * BSSH_MAX_IMAGE_PIXELS says it is "for any protocol" and was
		 * applied to sixel alone. This is the same guard on the branch
		 * next to it.
		 */
		if (qint64(image.width()) * qint64(image.height()) > BSSH_MAX_IMAGE_PIXELS) {
			return result;
		}
	} else if ((format == 24 || format == 32) && pixel_width > 0 && pixel_height > 0) {
		const QImage::Format qt_format = format == 24 ? QImage::Format_RGB888
		                                              : QImage::Format_RGBA8888;
		/*
		 * SIXTY-FOUR BIT, and that is the whole of this guard.
		 *
		 * Both of these were int, and both operands come from the
		 * remote's own s= and v= controls. `s=46341 v=46341 f=32` makes
		 * bytes_per_line 185364 and the product 8,589,953,124 -- which
		 * wraps to 18,532 in an int. The check then demanded 18 KB of
		 * payload and handed QImage a 46341x46341 image over it, and
		 * copy() reads rows until it has 8.59 GB: an out-of-bounds read
		 * of gigabytes, driven entirely by a remote, whose best outcome
		 * is a crash and whose worst is heap contents drawn on screen.
		 *
		 * In 64-bit arithmetic the product cannot wrap for any int pair,
		 * so the comparison means what it says -- and the payload cap in
		 * the emulator then bounds the image for free, since an image
		 * this can accept must fit in a payload that already has a
		 * ceiling.
		 */
		const qint64 bytes_per_line = qint64(pixel_width) * (format == 24 ? 3 : 4);
		const qint64 needed = bytes_per_line * qint64(pixel_height);

		/*
		 * Copied, not wrapped: QImage's borrowing constructor keeps a
		 * pointer into this buffer, which is a local about to die.
		 */
		if (needed > 0 && qint64(decoded.size()) >= needed) {
			image = QImage(reinterpret_cast<const uchar *>(decoded.constData()),
			                pixel_width, pixel_height, bytes_per_line, qt_format).copy();
		}
	}

	if (image.isNull()) return result;

	result.action = bssh_graphics_result_t::PLACE;
	result.placement.id = controls.value(QStringLiteral("i")).toInt();
	result.placement.image = image;
	result.placement.z = controls.value(QStringLiteral("z")).toInt();

	/*
	 * c/r are the footprint the remote asked for, in cells. Without them
	 * the footprint comes from the pixel size, rounded up so an image
	 * that is not an exact multiple of the cell size still gets the cell
	 * it partially occupies.
	 */
	const int cols = controls.value(QStringLiteral("c")).toInt();
	const int rows = controls.value(QStringLiteral("r")).toInt();
	result.placement.cols = qMin(cols > 0 ? cols
	                                       : (image.width() + cell_width - 1) / qMax(1, cell_width),
	                             BSSH_MAX_IMAGE_CELLS);
	result.placement.rows = qMin(rows > 0 ? rows
	                                       : (image.height() + cell_height - 1) / qMax(1, cell_height),
	                             BSSH_MAX_IMAGE_CELLS);

	return result;
}

bssh_graphics_result_t bssh_parse_iterm2(const QByteArray &payload,
                                          int cell_width, int cell_height) {
	bssh_graphics_result_t result;

	/* "File=key=value;key=value:<base64>" */
	if (!payload.startsWith("File=")) return result;

	const int colon = payload.indexOf(':');
	if (colon < 0) return result;

	const QByteArray decoded = QByteArray::fromBase64(payload.mid(colon + 1));
	const QImage image = QImage::fromData(decoded);
	if (image.isNull()) return result;

	/* Decoded size, not payload size -- see the kitty f=100 branch. */
	if (qint64(image.width()) * qint64(image.height()) > BSSH_MAX_IMAGE_PIXELS) {
		return result;
	}

	result.action = bssh_graphics_result_t::PLACE;
	result.placement.image = image;
	result.placement.cols = (image.width() + cell_width - 1) / qMax(1, cell_width);
	result.placement.rows = (image.height() + cell_height - 1) / qMax(1, cell_height);

	/*
	 * iTerm2's width=/height= arguments accept cells, pixels or a
	 * percentage of the window, distinguished by a suffix. Only the
	 * unit-less cell form is honoured here; the others fall back to the
	 * pixel-derived footprint above rather than being silently
	 * misinterpreted as cells.
	 */
	const QList<QByteArray> arguments = payload.mid(5, colon - 5).split(';');
	for (const QByteArray &argument : arguments) {
		const int equals = argument.indexOf('=');
		if (equals <= 0) continue;

		const QByteArray key = argument.left(equals);
		const QByteArray value = argument.mid(equals + 1);
		bool numeric = false;
		const int cells = value.toInt(&numeric);
		if (!numeric || cells <= 0) continue;

		if (key == "width") result.placement.cols = qMin(cells, BSSH_MAX_IMAGE_CELLS);
		if (key == "height") result.placement.rows = qMin(cells, BSSH_MAX_IMAGE_CELLS);
	}

	return result;
}

bssh_graphics_result_t bssh_parse_sixel(const QByteArray &payload,
                                         int cell_width, int cell_height) {
#ifdef BSSH_NO_SIXEL
	/*
	 * Built without libsixel -- currently the Android build, where the
	 * library has no cross-compiled copy yet. Declining is the same
	 * honest path this took before libsixel existed at all: no image, no
	 * crash, and the sequence swallowed rather than leaking into the text
	 * as garbage.
	 */
	Q_UNUSED(payload);
	Q_UNUSED(cell_width);
	Q_UNUSED(cell_height);
	return bssh_graphics_result_t();
#else
	bssh_graphics_result_t result;
	if (payload.isEmpty()) return result;

	/*
	 * Decoding is libsixel's. Sixel is the one of the three protocols
	 * whose payload is not a container Qt already reads -- it is a 1974
	 * encoding with its own palette handling -- and reimplementing that
	 * decoder is exactly the work libsixel has already done well.
	 *
	 * Two things about its contract are established by experiment rather
	 * than by its header, and both are silent when wrong:
	 *
	 *   - The DCS introducer must be included. Passing only the body
	 *     after the "q" returns SIXEL_OK and a 1x1 image, so a caller
	 *     that trimmed the introducer would see success and one stray
	 *     pixel.
	 *   - The palette is RGB triplets, three bytes per colour, despite
	 *     the header comment calling it ARGB. Reading it as four-byte
	 *     entries gives colours that are wrong but plausible.
	 */
	unsigned char *pixels = nullptr;
	unsigned char *palette = nullptr;
	int width = 0;
	int height = 0;
	int ncolors = 0;

	const SIXELSTATUS status = sixel_decode_raw(
	        reinterpret_cast<unsigned char *>(const_cast<char *>(payload.constData())),
	        payload.size(), &pixels, &width, &height, &palette, &ncolors, nullptr);

	if (!SIXEL_SUCCEEDED(status) || !pixels || !palette
	    || width <= 0 || height <= 0 || ncolors <= 0) {
		free(pixels);
		free(palette);
		return result;
	}

	/*
	 * Bounded BEFORE anything is allocated, which nothing did: the
	 * dimensions come back from libsixel and were used as they arrived.
	 * BSSH_MAX_IMAGE_CELLS caps the PLACEMENT, not the decode, so it
	 * never reached this.
	 *
	 * Three faults fall out of the one guard. The allocation below could
	 * fail and return a null image, and the loop then called setPixel on
	 * it. `y * width + x` is int arithmetic, so a decode wider than
	 * INT_MAX / height wrapped the index and read outside the buffer --
	 * the same overflow as the kitty guard above, a hundred lines apart.
	 * And a large image is minutes of per-pixel work on the thread that
	 * runs the terminal.
	 *
	 * The ceiling is argued rather than picked: bssh_image_store's budget
	 * is 32 MB per session, and at three bytes a pixel this is 25 MB --
	 * an image that fits the budget, and still larger than a 4K screen's
	 * 8,294,400 pixels. Anything bigger would be evicted on arrival, so
	 * decoding it was work with no possible outcome.
	 */
	const qint64 total_pixels = qint64(width) * qint64(height);
	if (total_pixels > BSSH_MAX_IMAGE_PIXELS) {
		free(pixels);
		free(palette);
		return result;
	}

	QImage image(width, height, QImage::Format_RGB888);
	if (image.isNull()) {
		free(pixels);
		free(palette);
		return result;
	}

	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			/*
			 * Clamped: the index comes from the remote, and a sixel
			 * naming a colour outside the palette it declared must not
			 * read past the end of it.
			 */
			/*
			 * Widened as well as bounded above. The cap makes this
			 * unable to overflow today; saying so in the type means a
			 * larger cap later cannot quietly reintroduce it.
			 */
			const qint64 at = qint64(y) * qint64(width) + qint64(x);
			const int index = qBound(0, int(pixels[at]), ncolors - 1);
			const unsigned char *entry = palette + index * 3;
			image.setPixel(x, y, qRgb(entry[0], entry[1], entry[2]));
		}
	}

	free(pixels);
	free(palette);

	result.action = bssh_graphics_result_t::PLACE;
	result.placement.image = image;
	result.placement.cols = (width + cell_width - 1) / qMax(1, cell_width);
	result.placement.rows = (height + cell_height - 1) / qMax(1, cell_height);

	return result;
#endif
}
