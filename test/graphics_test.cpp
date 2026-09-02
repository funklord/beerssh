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

void graphics_test::a_shrink_moves_an_image_with_its_row_rather_than_dropping_it() {
	/*
	 * This test used to assert the opposite, as
	 * images_are_dropped_when_the_grid_shrinks, and its reason was "the
	 * row it sits on no longer exists. Holding it would leak."
	 *
	 * The row does still exist; it MOVES. Measured on this fixture:
	 * shrinking 10 rows to 4 pushes lines into the scrollback and the
	 * text at row 7 arrives at row 2, still on the live screen. So the
	 * old behaviour dropped an image whose text was sitting three rows
	 * further up -- which is the failure the test below this one
	 * documents for the scrolling path, in its own words: "a plot
	 * vanished as soon as one more line of output arrived, while the text
	 * it was drawn beside was still there to scroll back to".
	 *
	 * It passed because the image genuinely was left behind: nothing
	 * moved placements on a resize, since libvterm reports a reflow
	 * through sb_pushline and only moverect reaches images.scrolled().
	 * Dropping the image was the only way to avoid a placement stranded
	 * on the wrong row, so the test pinned the workaround rather than the
	 * behaviour anybody wanted.
	 *
	 * Asserted against the TEXT, which is the thing the image is supposed
	 * to stay beside.
	 */
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	emulator.feed(QByteArrayLiteral("\x1b[8;1HMARKER"));
	emulator.feed(QByteArrayLiteral("\x1b[8;10H"));
	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100,c=2,r=1"),
	                            png_of(16, 16, qRgb(0, 255, 255))));

	QCOMPARE(emulator.images().count(), 1);
	QCOMPARE(emulator.images().placements().first().row, 7);

	emulator.resize(40, 4);

	/* Find where the text went. */
	int marker_row = -1;
	for (int r = 0; r < 4 && marker_row < 0; ++r) {
		if (emulator.screen().cell(r, 0).ch == U'M') marker_row = r;
	}
	QVERIFY2(marker_row >= 0, "the fixture lost its own marker, so it proves nothing");

	QCOMPARE(emulator.images().count(), 1);
	QCOMPARE(emulator.images().placements().first().row, marker_row);
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

void graphics_test::a_resize_moves_images_even_when_the_ring_is_full() {
	/*
	 * The same invariant as the test below, with the scrollback FULL --
	 * which is the normal state of a long session and the case the first
	 * version of the fix could not see.
	 *
	 * That fix measured how far the buffer moved as the change in
	 * scrollback COUNT. push() appends and then evicts from the front
	 * once the ring is at its cap, so a full ring answers a five-line
	 * push with count unchanged and base up by five: the delta is zero
	 * and the images do not move, while the text does.
	 *
	 * base() + count() is what actually advances, and it is what the
	 * absolute line numbering is built from.
	 */
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);
	emulator.set_scrollback_lines(8);

	QByteArray pixels;
	for (int i = 0; i < 4; ++i) {
		pixels.append(char(0xff));
		pixels.append(char(0x00));
		pixels.append(char(0x00));
	}

	/* Fill the ring well past its cap, so evictions are happening. */
	for (int i = 0; i < 40; ++i) {
		emulator.feed(QByteArrayLiteral("filler\r\n"));
	}
	QCOMPARE(emulator.scrollback().count(), 8);
	QVERIFY2(emulator.scrollback().base() > 0, "the ring must be evicting for this to mean anything");

	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=24,s=2,v=2"), pixels));
	emulator.feed(QByteArrayLiteral("\r\nAAA\r\nBBB\r\n"));

	QCOMPARE(emulator.images().count(), 1);

	const auto absolute_line = [&emulator]() {
		return emulator.scrollback().base() + emulator.scrollback().count()
		        + emulator.images().placements().first().row;
	};

	const qint64 before = absolute_line();
	const qint64 pushed_before = emulator.scrollback().base() + emulator.scrollback().count();

	emulator.resize(40, 5);

	QVERIFY2(emulator.scrollback().base() + emulator.scrollback().count() > pushed_before,
	          "the fixture did not actually push any lines, so this proves nothing");

	QCOMPARE(absolute_line(), before);
}

void graphics_test::a_resize_keeps_an_image_with_the_text_it_was_drawn_beside() {
	/*
	 * A shrink pushes lines into the scrollback exactly as a scroll
	 * does -- but libvterm reports it through sb_pushline, and only
	 * moverect reaches on_moverect and so images.scrolled(). So the text
	 * moved and the images did not.
	 *
	 * Measured before the fix, on this fixture: shrinking 10 rows to 5
	 * pushed five lines, the text moved up five, and the image stayed at
	 * row 0 -- sitting on top of output from five rows later, five rows
	 * from what it belonged to. That is the "terminal has come apart"
	 * the scroll handling exists to prevent, arriving by the one route
	 * that skipped it.
	 *
	 * Asserted in ABSOLUTE lines, which is the numbering the view scrolls
	 * and selects in: a resize reflows the screen and must not move an
	 * image relative to the CONTENT. Comparing rows instead would just
	 * restate the arithmetic the fix performs.
	 */
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	QByteArray pixels;
	for (int i = 0; i < 4; ++i) {
		pixels.append(char(0xff));
		pixels.append(char(0x00));
		pixels.append(char(0x00));
	}

	emulator.feed(QByteArrayLiteral("TOP\r\n"));
	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=24,s=2,v=2"), pixels));
	emulator.feed(QByteArrayLiteral("\r\nAAA\r\nBBB\r\nCCC\r\nDDD\r\nEEE\r\nFFF\r\nGGG\r\n"));

	QCOMPARE(emulator.images().count(), 1);

	/*
	 * base + count + row: how far down the whole session's output this
	 * image sits. bssh_terminal_view names lines exactly this way.
	 */
	const auto absolute_line = [&emulator]() {
		return emulator.scrollback().base() + emulator.scrollback().count()
		        + emulator.images().placements().first().row;
	};

	const qint64 before = absolute_line();

	const int lines_before = emulator.scrollback().count();

	emulator.resize(40, 5);

	/*
	 * Checked on the SCROLLBACK rather than on the image: a guard that
	 * asks whether the placement moved cannot tell "the fixture did not
	 * reflow" from "the bug is present", and would report the wrong one.
	 */
	QVERIFY2(emulator.scrollback().count() > lines_before,
	          "the fixture did not actually reflow, so this proves nothing");

	QVERIFY2(emulator.images().count() == 1, "the resize dropped the image entirely");

	QCOMPARE(absolute_line(), before);

	/*
	 * And back the other way: growing pops lines off the scrollback, so
	 * the images come down with the text. The same invariant, run
	 * backwards, which is what stops the fix being a one-directional
	 * subtraction that happens to suit a shrink.
	 */
	emulator.resize(40, 12);

	QCOMPARE(emulator.images().count(), 1);
	QCOMPARE(absolute_line(), before);
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

void graphics_test::a_frame_drawn_over_an_earlier_one_does_not_keep_it_alive() {
	/*
	 * Sixel animation. Sixel carries no image id, so place() hands every
	 * frame a fresh one and appends it -- and a program that redraws in
	 * place, which is what an animation IS, never scrolls.
	 *
	 * That put the whole run in exactly the case the budget declines to
	 * touch: enforce_budget drops only what has gone above the fold, and
	 * every frame is on screen by construction. So the store grew one
	 * decoded frame at a time for as long as the animation ran, past a
	 * budget that exists, in its own words, so "a session left running all
	 * day cannot quietly become the reason the app is killed".
	 *
	 * A covered frame is not an image anybody is reading: the renderer
	 * walks the list in order and paints the later one over it, so not one
	 * of its pixels can reach the screen again.
	 */
	bssh_image_store store;

	const qint64 frame = QImage(200, 200, QImage::Format_RGB32).sizeInBytes();

	for (int i = 0; i < 60; ++i) {
		bssh_image_placement_t frame_i;
		frame_i.image = QImage(200, 200, QImage::Format_RGB32);
		frame_i.row = 3;
		frame_i.col = 0;
		frame_i.cols = 20;
		frame_i.rows = 10;
		store.place(frame_i);
	}

	/* One frame, not sixty. */
	QCOMPARE(store.count(), 1);
	QCOMPARE(store.memory_used(), frame);

	/*
	 * A frame that does NOT cover the last one is a second image rather
	 * than a replacement -- so this is a check on the covering test and
	 * not simply on "the newest wins".
	 */
	bssh_image_placement_t elsewhere;
	elsewhere.image = QImage(200, 200, QImage::Format_RGB32);
	elsewhere.row = 30;
	elsewhere.col = 0;
	elsewhere.cols = 20;
	elsewhere.rows = 10;
	store.place(elsewhere);
	QCOMPARE(store.count(), 2);

	/* And one that covers only PART of an image leaves it, because the
	 * rest of it is still on the screen. */
	bssh_image_placement_t partial;
	partial.image = QImage(200, 200, QImage::Format_RGB32);
	partial.row = 30;
	partial.col = 0;
	partial.cols = 10;
	partial.rows = 10;
	store.place(partial);
	QCOMPARE(store.count(), 3);

	/*
	 * A below-text image is painted in its own pass, so an image above the
	 * text does not cover it however exactly the cells line up.
	 */
	bssh_image_store layered;

	bssh_image_placement_t background;
	background.image = QImage(200, 200, QImage::Format_RGB32);
	background.row = 0;
	background.col = 0;
	background.cols = 20;
	background.rows = 10;
	background.z = -1;
	layered.place(background);

	bssh_image_placement_t foreground = background;
	foreground.z = 0;
	layered.place(foreground);

	QCOMPARE(layered.count(), 2);
}
