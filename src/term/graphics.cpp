#include <QByteArray>
#include <QImage>
#include <QMap>
#include <QString>

#include <sixel.h>

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
	} else if ((format == 24 || format == 32) && pixel_width > 0 && pixel_height > 0) {
		const QImage::Format qt_format = format == 24 ? QImage::Format_RGB888
		                                              : QImage::Format_RGBA8888;
		const int bytes_per_line = pixel_width * (format == 24 ? 3 : 4);

		/*
		 * Copied, not wrapped: QImage's borrowing constructor keeps a
		 * pointer into this buffer, which is a local about to die.
		 */
		if (decoded.size() >= bytes_per_line * pixel_height) {
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
	result.placement.cols = cols > 0 ? cols
	                                 : (image.width() + cell_width - 1) / qMax(1, cell_width);
	result.placement.rows = rows > 0 ? rows
	                                 : (image.height() + cell_height - 1) / qMax(1, cell_height);

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

		if (key == "width") result.placement.cols = cells;
		if (key == "height") result.placement.rows = cells;
	}

	return result;
}

bssh_graphics_result_t bssh_parse_sixel(const QByteArray &payload,
                                         int cell_width, int cell_height) {
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

	QImage image(width, height, QImage::Format_RGB888);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			/*
			 * Clamped: the index comes from the remote, and a sixel
			 * naming a colour outside the palette it declared must not
			 * read past the end of it.
			 */
			const int index = qBound(0, int(pixels[y * width + x]), ncolors - 1);
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
}
