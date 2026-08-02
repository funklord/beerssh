#include <QBuffer>
#include <QtTest>

#include "graphics_test.h"
#include "term/emulator.h"
#include "term/graphics_internal.h"

/* A real PNG, encoded by Qt, so the decode path is the production one. */
static QByteArray png_of(int width, int height, QRgb colour) {
	QImage image(width, height, QImage::Format_RGBA8888);
	image.fill(colour);

	QByteArray bytes;
	QBuffer buffer(&bytes);
	buffer.open(QIODevice::WriteOnly);
	image.save(&buffer, "PNG");
	return bytes;
}

static QByteArray kitty_escape(const QByteArray &controls, const QByteArray &payload) {
	return QByteArrayLiteral("\x1b_") + controls + QByteArrayLiteral(";")
	        + payload.toBase64() + QByteArrayLiteral("\x1b\\");
}

void graphics_test::kitty_png_is_placed_at_the_cursor() {
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	emulator.feed(QByteArrayLiteral("hi\r\n"));
	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100"), png_of(32, 32, qRgb(255, 0, 0))));

	QCOMPARE(emulator.images().count(), 1);

	const bssh_image_placement_t &placement = emulator.images().placements().first();
	QCOMPARE(placement.image.size(), QSize(32, 32));

	/* Placed where the cursor was: row 1, after the newline. */
	QCOMPARE(placement.row, 1);

	/* 32x32 pixels over 8x16 cells is 4 columns by 2 rows. */
	QCOMPARE(placement.cols, 4);
	QCOMPARE(placement.rows, 2);
}

void graphics_test::kitty_raw_rgb_is_decoded() {
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	/* f=24 is raw RGB with the size given by s and v, not a container. */
	QByteArray pixels;
	for (int i = 0; i < 4; ++i) {
		pixels.append(char(0x10));
		pixels.append(char(0x20));
		pixels.append(char(0x30));
	}
	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=24,s=2,v=2"), pixels));

	QCOMPARE(emulator.images().count(), 1);
	QCOMPARE(emulator.images().placements().first().image.size(), QSize(2, 2));

	/*
	 * The pixel data must survive the parser: QImage's borrowing
	 * constructor would leave this pointing at a freed buffer, and the
	 * colour would be garbage rather than obviously absent.
	 */
	const QColor pixel = emulator.images().placements().first().image.pixelColor(0, 0);
	QCOMPARE(pixel.red(), 0x10);
	QCOMPARE(pixel.green(), 0x20);
	QCOMPARE(pixel.blue(), 0x30);
}

void graphics_test::kitty_chunked_transmission_reassembles() {
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	const QByteArray png = png_of(16, 16, qRgb(0, 255, 0));
	const QByteArray base64 = png.toBase64();

	/*
	 * m=1 means more follows. A large image never arrives in one APC, so
	 * this is the normal path rather than an edge case.
	 */
	const int split = base64.size() / 2;
	emulator.feed(QByteArrayLiteral("\x1b_Ga=T,f=100,m=1;") + base64.left(split)
	               + QByteArrayLiteral("\x1b\\"));
	QCOMPARE(emulator.images().count(), 0);

	emulator.feed(QByteArrayLiteral("\x1b_Gm=0;") + base64.mid(split)
	               + QByteArrayLiteral("\x1b\\"));

	QCOMPARE(emulator.images().count(), 1);
	QCOMPARE(emulator.images().placements().first().image.size(), QSize(16, 16));
}

void graphics_test::kitty_delete_removes_images() {
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100,i=7"), png_of(8, 8, qRgb(0, 0, 255))));
	QCOMPARE(emulator.images().count(), 1);

	emulator.feed(QByteArrayLiteral("\x1b_Ga=d,i=7\x1b\\"));
	QCOMPARE(emulator.images().count(), 0);
}

void graphics_test::iterm2_inline_image_is_placed() {
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	const QByteArray png = png_of(24, 16, qRgb(255, 255, 0));
	emulator.feed(QByteArrayLiteral("\x1b]1337;File=inline=1;size=")
	               + QByteArray::number(png.size()) + QByteArrayLiteral(":")
	               + png.toBase64() + QByteArrayLiteral("\x07"));

	QCOMPARE(emulator.images().count(), 1);

	const bssh_image_placement_t &placement = emulator.images().placements().first();
	QCOMPARE(placement.image.size(), QSize(24, 16));
	QCOMPARE(placement.cols, 3);
	QCOMPARE(placement.rows, 1);
}

void graphics_test::sixel_is_decoded_by_libsixel() {
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	/*
	 * "#0;2;100;0;0" defines colour 0 as pure red in libsixel's
	 * percentage form; "~~" is two columns with all six pixels of the
	 * band set. So: 2 wide, 6 tall, red.
	 */
	emulator.feed(QByteArrayLiteral("\x1bPq#0;2;100;0;0#0~~\x1b\\"));

	QCOMPARE(emulator.images().count(), 1);

	const bssh_image_placement_t &placement = emulator.images().placements().first();
	QCOMPARE(placement.image.size(), QSize(2, 6));
	QCOMPARE(placement.image.pixelColor(0, 0), QColor(255, 0, 0));

	/* 2x6 pixels in 8x16 cells rounds up to one cell each way. */
	QCOMPARE(placement.cols, 1);
	QCOMPARE(placement.rows, 1);
}

void graphics_test::malformed_sixel_is_declined_not_guessed() {
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	/*
	 * Nonsense where the palette should be. libsixel must not be handed
	 * this and have its answer trusted blindly -- and whatever it makes
	 * of it, the sequence must not leak into the text.
	 */
	emulator.feed(QByteArrayLiteral("\x1bPq#####\x1b\\"));
	QVERIFY(!emulator.screen().to_text().contains(QLatin1Char('#')));
}

void graphics_test::images_follow_the_text_when_the_screen_scrolls() {
	bssh_emulator emulator(40, 4);
	emulator.set_cell_pixels(8, 16);

	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100,c=2,r=1"),
	                            png_of(16, 16, qRgb(255, 0, 255))));
	const int placed_row = emulator.images().placements().first().row;

	/* Push the screen down by more lines than it has. */
	emulator.feed(QByteArrayLiteral("\r\na\r\nb\r\nc\r\nd\r\ne"));

	QVERIFY(emulator.images().count() <= 1);
	if (emulator.images().count() == 1) {
		QVERIFY(emulator.images().placements().first().row < placed_row);
	}
}

void graphics_test::images_are_dropped_when_the_grid_shrinks() {
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	emulator.feed(QByteArrayLiteral("\x1b[8;1H"));
	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100,c=2,r=1"),
	                            png_of(16, 16, qRgb(0, 255, 255))));
	QCOMPARE(emulator.images().count(), 1);

	/* The row it sits on no longer exists. Holding it would leak. */
	emulator.resize(40, 4);
	QCOMPARE(emulator.images().count(), 0);
}
