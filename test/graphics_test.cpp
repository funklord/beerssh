#include <QBuffer>
#include <QtTest>

#include "ui/terminal_view.h"
#include "term/screen.h"
#include <climits>

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

void graphics_test::an_absurd_cell_footprint_is_refused_by_both_protocols() {
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	/*
	 * `c` and `r` are the remote's claim about how many CELLS its image
	 * occupies, and both protocols used to hand them over with only a
	 * "greater than zero" check. The renderer then computes
	 * `cols * cell_width` into an int for its target rectangle, so
	 * 300000000 cells at 8 pixels is signed overflow -- undefined
	 * behaviour reached from bytes anything upstream can send, including
	 * a file somebody cats. The row count is worse than equal: it also
	 * drives the cursor advance the emulator writes back.
	 *
	 * Bounded rather than rejected, because an oversized footprint is not
	 * by itself malformed -- an image may legitimately hang off the right
	 * of a small window, and the renderer crops it.
	 */
	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100,c=300000000,r=300000000"),
	                            png_of(8, 16, qRgb(0, 255, 0))));

	QCOMPARE(emulator.images().count(), 1);

	const bssh_image_placement_t &kitty = emulator.images().placements().first();
	QVERIFY(kitty.cols > 0);
	QVERIFY(kitty.rows > 0);
	QVERIFY2(kitty.cols <= 10000, qPrintable(QString::number(kitty.cols)));
	QVERIFY2(kitty.rows <= 10000, qPrintable(QString::number(kitty.rows)));

	/*
	 * And the product a renderer forms from it still fits, which is the
	 * property that actually matters -- a bound that stopped short of
	 * this would look like a fix and not be one.
	 */
	QVERIFY(kitty.cols < INT_MAX / 64);
	QVERIFY(kitty.rows < INT_MAX / 64);

	/* iTerm2 says the same thing with different spelling. */
	bssh_emulator other(40, 10);
	other.set_cell_pixels(8, 16);
	other.feed(QByteArrayLiteral("\x1b]1337;File=inline=1;width=999999999;height=999999999:")
	            + png_of(8, 16, qRgb(0, 0, 255)).toBase64() + QByteArrayLiteral("\x07"));

	QCOMPARE(other.images().count(), 1);
	const bssh_image_placement_t &iterm = other.images().placements().first();
	QVERIFY2(iterm.cols <= 10000, qPrintable(QString::number(iterm.cols)));
	QVERIFY2(iterm.rows <= 10000, qPrintable(QString::number(iterm.rows)));
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

void graphics_test::an_image_scrolled_off_the_top_survives_in_the_scrollback() {
	bssh_image_store store;

	bssh_image_placement_t placement;
	placement.image = QImage(20, 20, QImage::Format_RGB32);
	placement.row = 2;
	placement.rows = 3;
	placement.cols = 4;
	store.place(placement);

	/* Ten lines of output push it well above the live screen. */
	store.scrolled(0, 24, 10);

	/*
	 * It used to be dropped the moment it left the screen, which meant a
	 * plot vanished as soon as one more line of output arrived -- while
	 * the text it was drawn beside was still there to scroll back to.
	 */
	QCOMPARE(store.count(), 1);
	QCOMPARE(store.placements().first().row, -8);
}

void graphics_test::the_memory_budget_drops_the_oldest_but_never_the_visible() {
	bssh_image_store store;

	/* Two images of a known size; room for one and a half. */
	const int bytes = QImage(100, 100, QImage::Format_RGB32).sizeInBytes();
	store.set_memory_budget(bytes + bytes / 2);

	bssh_image_placement_t old_one;
	old_one.image = QImage(100, 100, QImage::Format_RGB32);
	old_one.row = 1;
	old_one.rows = 2;
	store.place(old_one);

	store.scrolled(0, 24, 20);          /* pushes it into the scrollback */

	bssh_image_placement_t on_screen;
	on_screen.image = QImage(100, 100, QImage::Format_RGB32);
	on_screen.row = 5;
	on_screen.rows = 2;
	store.place(on_screen);

	store.scrolled(0, 24, 0);           /* no movement; budget still applies */
	store.set_memory_budget(bytes + bytes / 2);

	/* The one in history went; the one being looked at stayed. */
	QCOMPARE(store.count(), 1);
	QCOMPARE(store.placements().first().row, 5);

	/*
	 * And a budget too small for even the visible image does not blank
	 * it: over budget and unable to help it is the honest state, because
	 * dropping what the user is reading to satisfy a number is a terminal
	 * deleting its own output.
	 */
	store.set_memory_budget(1);
	QCOMPARE(store.count(), 1);
}

void graphics_test::a_background_image_is_drawn_under_the_text() {
	/*
	 * kitty's z < 0 -- how a program puts a background behind a TUI. It
	 * was stored and ignored: the renderer drew every image over the
	 * text, beneath a comment claiming it ran two passes. This asserts
	 * the pixels, because the previous evidence was a comment.
	 */
	bssh_terminal_view view;
	view.resize(200, 80);

	QJsonObject settings;
	settings.insert(QStringLiteral("colour_scheme"), QStringLiteral("default"));
	bssh_apply_appearance(&view, settings);

	bssh_screen screen(view.cols(), view.rows());
	for (int i = 0; i < 4; ++i) {
		bssh_cell_t cell;
		cell.ch = U'W';
		screen.set_cell(0, i, cell);
	}
	view.attach_screen(&screen);

	bssh_image_store store;
	bssh_image_placement_t background;
	background.image = QImage(40, 20, QImage::Format_RGB32);
	background.image.fill(QColor(Qt::red));
	background.row = 0;
	background.col = 0;
	background.cols = 4;
	background.rows = 1;
	background.z = -1;
	store.place(background);
	view.attach_images(&store);

	const QImage under = view.grab().toImage();

	/* The same image at z >= 0 must cover the text instead. */
	store.clear();
	background.z = 1;
	store.place(background);
	const QImage over = view.grab().toImage();

	QVERIFY2(under != over, "z < 0 must not render the same as z > 0");

	/*
	 * Under the text: some glyph pixels survive, so not every pixel in
	 * the covered band is the image's red. Over the text: they are.
	 */
	int text_pixels_under = 0;
	int text_pixels_over = 0;
	for (int x = 0; x < 40; ++x) {
		for (int y = 0; y < 16; ++y) {
			if (under.pixelColor(x, y) != QColor(Qt::red)) ++text_pixels_under;
			if (over.pixelColor(x, y) != QColor(Qt::red)) ++text_pixels_over;
		}
	}

	QVERIFY2(text_pixels_under > text_pixels_over,
	         "text must remain visible on top of a background image");
}

void graphics_test::an_image_that_arrived_as_bytes_reaches_the_screen() {
	/*
	 * The JOIN, which nothing tested. Every other test in this file
	 * stops at the image store -- bytes in, placement out -- and the one
	 * paint test builds its placement by hand and never goes near a
	 * parser. Both halves were covered and the chain between them was
	 * not.
	 *
	 * The lens is the sibling qtty's, offered after it found three
	 * input faults of exactly this shape in one function: **the parse
	 * being right is what hides the delivery being wrong**, because the
	 * parse is the half that is easy to assert. A test proving the
	 * decoder decodes is what makes the area look covered.
	 *
	 * It is not hypothetical here. The kitty a=q defect fixed earlier
	 * lived in this exact gap: a capability QUERY fell through to the
	 * transmit path, was decoded as a 1x1 image and PLACED, and every
	 * test in this file went on passing because each was asking the
	 * store what it held rather than asking the screen what it showed.
	 */
	bssh_emulator emulator(40, 10);

	bssh_terminal_view view;
	view.resize(320, 160);

	QJsonObject settings;
	settings.insert(QStringLiteral("colour_scheme"), QStringLiteral("default"));
	bssh_apply_appearance(&view, settings);

	view.attach_screen(&emulator.screen());
	view.attach_images(&emulator.images());
	emulator.set_cell_pixels(view.cell_width(), view.cell_height());

	/*
	 * Sampled BEFORE, so the assertion is that this image arrived rather
	 * than that the terminal is red -- a scheme whose background happened
	 * to be red would satisfy an "is red" test with no image at all.
	 */
	const QPoint probe(view.cell_width() / 2, view.cell_height() / 2);
	const QColor before = view.grab().toImage().pixelColor(probe);
	QVERIFY2(before != QColor(Qt::red), "the fixture needs a background that is not the image");

	/* Four red pixels, raw RGB, two by two -- the same shape the parse
	 * test next door uses, so a failure here is about delivery. */
	QByteArray pixels;
	for (int i = 0; i < 4; ++i) {
		pixels.append(char(0xff));
		pixels.append(char(0x00));
		pixels.append(char(0x00));
	}
	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=24,s=2,v=2"), pixels));

	/* The parse half, so a failure below is unambiguous about which
	 * half broke. */
	QCOMPARE(emulator.images().count(), 1);

	const QColor after = view.grab().toImage().pixelColor(probe);

	QVERIFY2(after == QColor(Qt::red),
	          qPrintable(QStringLiteral("an image that parsed was never drawn: the cell "
	                                     "under the cursor is %1").arg(after.name())));
}

void graphics_test::a_declared_size_that_overflows_is_refused() {
	/*
	 * The guard on kitty's raw formats multiplied two remote-supplied
	 * ints. `s=46341 v=46341 f=32` gives bytes_per_line 185364 and a
	 * product of 8,589,953,124 -- which wraps to 18,532 in an int. The
	 * check then asked for 18 KB of payload and handed QImage a
	 * 46341x46341 image over it, and copy() reads until it has 8.59 GB.
	 *
	 * An out-of-bounds read of gigabytes, chosen by whatever is on the
	 * other end of the connection. Best case a crash; worst case heap
	 * contents decoded into an image and drawn on the screen.
	 *
	 * 46341 is not arbitrary: it is the smallest square whose 32-bit
	 * product wraps to a value small enough for an ordinary payload to
	 * satisfy, which is what makes this reachable rather than theoretical.
	 */
	const int side = 46341;
	const QByteArray payload(20000, '\x01');

	const QByteArray apc = QByteArrayLiteral("\x1b_Ga=T,f=32,s=")
	        + QByteArray::number(side)
	        + QByteArrayLiteral(",v=")
	        + QByteArray::number(side)
	        + QByteArrayLiteral(";")
	        + payload.toBase64()
	        + QByteArrayLiteral("\x1b\\");

	bssh_emulator emulator(80, 24);
	emulator.feed(apc);

	QVERIFY2(emulator.images().count() == 0,
	          "a declared size the payload cannot cover must not become an image");

	/*
	 * The control, and it is the one that matters: an image whose declared
	 * size the payload really does cover is still accepted, so this cannot
	 * pass against a parser that refuses everything.
	 */
	const int small = 4;
	const QByteArray real(small * small * 4, '\x7f');
	const QByteArray good = QByteArrayLiteral("\x1b_Ga=T,f=32,s=")
	        + QByteArray::number(small)
	        + QByteArrayLiteral(",v=")
	        + QByteArray::number(small)
	        + QByteArrayLiteral(";")
	        + real.toBase64()
	        + QByteArrayLiteral("\x1b\\");

	emulator.feed(good);
	QVERIFY2(emulator.images().count() == 1,
	          "an image whose payload covers its declared size must still arrive");
}
