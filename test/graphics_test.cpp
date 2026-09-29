#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QSysInfo>
#include <QRandomGenerator>
#include <QTemporaryDir>
#include <QtTest>

#include "term/picture_spill.h"

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

void graphics_test::an_image_without_an_id_does_not_answer_to_a_remote_one() {
	/*
	 * sixel and iTerm2 carry no image id -- image_store.h says so, in the
	 * field's own comment: "0 means the protocol did not give one, which
	 * is the normal case for sixel and iTerm2".
	 *
	 * place() handed those an id anyway, out of a counter starting at 1,
	 * into the same namespace kitty's ids live in. kitty's numbering
	 * starts at 1 too, so in a session where both protocols are used --
	 * a plot from one tool, a picture from another -- the first
	 * `a=T,i=1` REPLACED the sixel frame, and `a=d,i=1` deleted it.
	 * Neither program had ever heard of the other.
	 *
	 * Driven through the emulator rather than the store, because what
	 * makes this reachable is the wire: nothing in either protocol lets
	 * a program discover the number the terminal invented for somebody
	 * else's image.
	 */
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	/* Two red columns, six pixels tall; one cell, at row 0. */
	emulator.feed(QByteArrayLiteral("\x1bPq#0;2;100;0;0#0~~\x1b\\"));
	QCOMPARE(emulator.images().count(), 1);

	/*
	 * A kitty image at the row below, so the covering test cannot be
	 * what decides this either way.
	 */
	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100,i=1"),
	                            png_of(8, 8, qRgb(0, 0, 255))));
	QCOMPARE(emulator.images().count(), 2);

	/* And deleting kitty's image 1 must not take the sixel with it. */
	emulator.feed(QByteArrayLiteral("\x1b_Ga=d,i=1\x1b\\"));
	QCOMPARE(emulator.images().count(), 1);
	QCOMPARE(emulator.images().placements().first().image.size(), QSize(2, 6));
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

	/*
	 * With nowhere to spill to, which is what the budget did before it
	 * could spill at all and still does when no cache directory exists:
	 * a promise about memory is kept by dropping rather than broken by
	 * keeping. The spilling half is the test below.
	 */
	store.set_spill_base(QString());

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

void graphics_test::a_small_payload_that_decodes_huge_is_refused() {
	/*
	 * BSSH_MAX_IMAGE_PIXELS says it is "for any protocol" and guarded
	 * ONE of the four decoders. For kitty f=100 and for iTerm2 the
	 * payload cap bounds the COMPRESSED bytes and says nothing about
	 * what they expand to: a solid-colour PNG of a couple of kilobytes
	 * is 5792x5792 decoded -- 128 MB, Qt's own allocation limit, and
	 * four times the image store's entire budget.
	 *
	 * The store cannot reclaim it either. Each lands on its own row, so
	 * drop_covered_by has nothing to drop, and enforce_budget refuses to
	 * touch anything still on screen -- which is the case it is
	 * deliberately written not to handle.
	 */
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	/* Comfortably past 8,388,608 pixels, and a tiny payload. */
	const QByteArray huge = png_of(4096, 4096, qRgb(0, 0, 255));
	QVERIFY2(huge.size() < 200000,
	          "the fixture needs a SMALL payload, or it proves nothing about decode size");

	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100,c=1,r=1"), huge));
	QCOMPARE(emulator.images().count(), 0);

	/* iTerm2 reaches the same guard by its own road. */
	const QByteArray osc = QByteArrayLiteral("\x1b]1337;File=inline=1:")
	        + huge.toBase64() + QByteArrayLiteral("\x07");
	emulator.feed(osc);
	QCOMPARE(emulator.images().count(), 0);

	/*
	 * And an ordinary image still arrives, so this is a bound on size
	 * rather than a decoder that refuses everything.
	 */
	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100,c=2,r=1"),
	                            png_of(32, 32, qRgb(255, 0, 0))));
	QCOMPARE(emulator.images().count(), 1);
}

void graphics_test::a_chunked_transmission_cannot_grow_without_end() {
	/*
	 * bssh_accumulate bounds one APC payload at 8 MB and clears on the
	 * initial fragment. It does not bound a TRANSMISSION: m=1 stitches
	 * together any number of complete, in-budget payloads, and the only
	 * things that emptied it were m=0 -- which a remote need never send
	 * -- and a reset. The product of two bounds is not a bound.
	 *
	 * What this asserts is RECOVERY, and that is deliberate. Counting
	 * images after the oversized run proves nothing: unbounded, the
	 * transmission simply never completes and places nothing; bounded, it
	 * is thrown away and places nothing. Both look identical from
	 * outside. What differs is the state left behind -- and a later,
	 * perfectly ordinary image is what finds it.
	 */
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	const QByteArray chunk(2 * 1024 * 1024, 'A');   /* valid base64 alphabet */

	/* Well past BSSH_MAX_TRANSMISSION_BYTES, and never terminated. */
	for (int i = 0; i < 24; ++i) {
		emulator.feed(QByteArrayLiteral("\x1b_Ga=T,f=100,m=1;") + chunk
		               + QByteArrayLiteral("\x1b\\"));
	}

	QCOMPARE(emulator.images().count(), 0);

	/*
	 * The bound itself, asked of what is HELD.
	 *
	 * Counting images cannot see this: unbounded, the transmission never
	 * completes and places nothing; bounded, it is discarded and places
	 * nothing. Nor can a later image see it -- the terminating chunk
	 * clears the buffer either way. What differs is only the megabytes
	 * held in the meantime, so that is what this asks about.
	 */
	QVERIFY2(emulator.pending_transmission_bytes() <= BSSH_MAX_TRANSMISSION_BYTES,
	          qPrintable(QStringLiteral("the transmission buffer holds %1 bytes")
	                             .arg(emulator.pending_transmission_bytes())));

	/*
	 * The abandoned transmission is closed by its own last chunk, and
	 * then an ordinary chunked PNG has to arrive intact. Unbounded, the
	 * tens of megabytes still sitting in the buffer are prepended to it
	 * and it decodes to nothing.
	 */
	emulator.feed(QByteArrayLiteral("\x1b_Gm=0;") + chunk
	               + QByteArrayLiteral("\x1b\\"));

	const QByteArray b64 = png_of(16, 16, qRgb(0, 255, 0)).toBase64();
	const int half = int(b64.size() / 2);

	/*
	 * Built by hand rather than through kitty_escape(), which base64s
	 * what it is given -- handing it data that is already base64 encodes
	 * it twice. The chunked test at the top of this file does the same.
	 */
	emulator.feed(QByteArrayLiteral("\x1b_Ga=T,f=100,c=1,r=1,m=1;") + b64.left(half)
	               + QByteArrayLiteral("\x1b\\"));
	emulator.feed(QByteArrayLiteral("\x1b_Gm=0;") + b64.mid(half)
	               + QByteArrayLiteral("\x1b\\"));

	QVERIFY2(emulator.images().count() == 1,
	          "an ordinary image after an oversized transmission has to arrive: "
	          "the buffer was still holding the abandoned one");
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
	 * Sixel animation. Sixel carries no image id, so no frame can replace
	 * an earlier one and place() simply appends each -- and a program that
	 * redraws in place, which is what an animation IS, never scrolls.
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


void graphics_test::dropping_the_history_drops_the_pictures_in_it() {
	/*
	 * sec 6.4 records why somebody drops the scrollback: "before pasting
	 * a password". A screenshot that scrolled off the top is in the
	 * history as much as the text around it -- it is placed at a
	 * NEGATIVE row precisely so that scrolling back brings it with its
	 * output -- and clear_scrollback took the text and left the picture
	 * decoded in memory, unreachable and still charged to the budget.
	 *
	 * Unreachable is what makes it purely a cost: with the history gone
	 * there is nothing to scroll back to, so the placement can never be
	 * drawn again by any route.
	 */
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100"),
	                            png_of(64, 64, qRgb(0, 128, 255))));
	QCOMPARE(emulator.images().count(), 1);

	/* Push it off the top, so it lives in the history and nowhere else. */
	for (int i = 0; i < 40; ++i) emulator.feed(QByteArrayLiteral("filler\r\n"));

	const bssh_image_placement_t &above = emulator.images().placements().first();
	QVERIFY2(above.row + above.rows <= 0,
	          "the fixture left the image on the live screen, where dropping "
	          "it would be a terminal deleting what the user is reading");
	QVERIFY(emulator.images().memory_used() > 0);

	/*
	 * And one on the live screen, which must survive: dropping that
	 * would be a terminal deleting the picture being looked at, which is
	 * the thing enforce_budget already refuses to do. Placed after the
	 * assertion above so the fixture cannot confuse the two.
	 */
	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100"),
	                            png_of(32, 32, qRgb(255, 0, 0))));
	QCOMPARE(emulator.images().count(), 2);

	emulator.clear_scrollback();

	QCOMPARE(emulator.images().count(), 1);
	QVERIFY2(emulator.images().placements().first().row
	                 + emulator.images().placements().first().rows > 0,
	          "the one left is the one that was in the history");
	QVERIFY(emulator.images().memory_used() > 0);
}


void graphics_test::resetting_the_terminal_drops_a_half_sent_image() {
	/*
	 * sec 6.4's Reset Terminal exists for "the modes a dead program left
	 * behind", and the classic cause it names is a binary landing on the
	 * terminal. A binary can open a chunked kitty transmission and never
	 * close it: m=1 says another chunk follows, so the bytes accumulate
	 * and wait, and the only things that ever emptied the buffer were the
	 * final chunk and the over-budget abandon.
	 *
	 * graphics_internal.h says a reset is one of the things that empties
	 * it. Nothing did -- the soft reset goes through libvterm, and this
	 * buffer is ours -- so the user's own cure left up to 32 MB held for
	 * a picture that is never going to arrive, and the next transmission
	 * was prepended to it.
	 */
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);

	const QByteArray chunk(64 * 1024, 'A');   /* valid base64 alphabet */
	emulator.feed(QByteArrayLiteral("\x1b_Ga=T,f=100,m=1;") + chunk
	               + QByteArrayLiteral("\x1b\\"));

	QVERIFY2(emulator.pending_transmission_bytes() > 0,
	          "the fixture left nothing pending, so the reset has nothing "
	          "to drop");

	emulator.reset_modes();

	QCOMPARE(emulator.pending_transmission_bytes(), qint64(0));

	/*
	 * And the terminal still takes an image afterwards, which is the
	 * control: a reset that cleared the bytes and left the transmission
	 * marked mid-stream would refuse the next one, and counting bytes
	 * cannot tell that from a clean buffer.
	 */
	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100"),
	                            png_of(16, 16, qRgb(0, 255, 0))));
	QCOMPARE(emulator.images().count(), 1);
}

/*
 * Noise, so neither compression nor a lucky fill can make two pictures
 * alike or one of them small: what comes back has to be this one.
 */
static QImage noise_image(int width, int height) {
	QImage image(width, height, QImage::Format_RGB32);
	for (int y = 0; y < height; ++y) {
		quint32 *line = reinterpret_cast<quint32 *>(image.scanLine(y));
		for (int x = 0; x < width; ++x)
			line[x] = 0xff000000u | (QRandomGenerator::global()->generate() & 0x00ffffffu);
	}
	return image;
}

/*
 * A spilled picture comes back from a worker: the first image() starts
 * the read and gives null. Asked again until it arrives, for at most five
 * seconds -- a read that never lands is a failure, not a hang.
 */
static QImage read_back(const bssh_image_store &store, int index) {
	QImage image = store.image(store.placements().at(index));
	for (int i = 0; i < 500 && image.isNull(); ++i) {
		QTest::qWait(10);
		image = store.image(store.placements().at(index));
	}
	return image;
}

static bssh_image_placement_t placement_at(const QImage &image, int row) {
	bssh_image_placement_t placement;
	placement.image = image;
	placement.row = row;
	placement.rows = 2;
	placement.cols = 4;
	return placement;
}

void graphics_test::a_picture_over_the_budget_is_spilled_and_comes_back_whole() {
	/*
	 * The holder's direction of 2026-09-26: bound what is resident, not
	 * what is remembered. The budget test above drops; with somewhere to
	 * put it, the same picture is written out and read back instead.
	 */
	QTemporaryDir base;
	QVERIFY(base.isValid());

	bssh_image_store store;
	store.set_spill_base(base.path());

	const QImage old_picture = noise_image(100, 100);
	const qint64 bytes = old_picture.sizeInBytes();
	store.set_memory_budget(bytes + bytes / 2);

	store.place(placement_at(old_picture, 1));
	store.scrolled(0, 24, 20);          /* into the scrollback */
	store.place(placement_at(noise_image(100, 100), 5));
	store.set_memory_budget(bytes + bytes / 2);
	QTRY_VERIFY(!store.spills_pending());

	/* Both still there; the one in history out of memory. */
	QCOMPARE(store.count(), 2);
	const bssh_image_placement_t &spilled = store.placements().first();
	QVERIFY(store.is_spilled(spilled));
	QVERIFY(spilled.image.isNull());
	QVERIFY(!store.is_spilled(store.placements().last()));
	QVERIFY(store.memory_used() <= bytes + bytes / 2);
	QVERIFY(store.disk_used() > 0);

	/* And scrolling to it gives back exactly what was drawn. */
	QCOMPARE(read_back(store, 0), old_picture);
}

void graphics_test::a_spilled_picture_scrolled_back_to_reaches_the_screen() {
	/*
	 * The renderer must ask the store rather than read the field, or a
	 * spilled picture is a blank where it used to be -- remembered by
	 * the store and never drawn. Moved back onto the screen with
	 * shift_rows while still spilled, which is what scrolling the view
	 * back to it amounts to from the renderer's side.
	 */
	QTemporaryDir base;
	QVERIFY(base.isValid());

	bssh_terminal_view view;
	view.resize(200, 80);

	QJsonObject settings;
	settings.insert(QStringLiteral("colour_scheme"), QStringLiteral("default"));
	bssh_apply_appearance(&view, settings);

	bssh_screen screen(view.cols(), view.rows());
	view.attach_screen(&screen);

	bssh_image_store store;
	store.set_spill_base(base.path());
	store.set_memory_budget(1);

	QImage red(40, 20, QImage::Format_RGB32);
	red.fill(QColor(Qt::red));
	bssh_image_placement_t placement = placement_at(red, 0);
	placement.rows = 1;
	store.place(placement);
	view.attach_images(&store);

	const QPoint probe(view.cell_width() / 2, view.cell_height() / 2);
	QCOMPARE(view.grab().toImage().pixelColor(probe), QColor(Qt::red));

	store.shift_rows(10);
	QTRY_VERIFY(!store.spills_pending());
	QVERIFY(store.is_spilled(store.placements().first()));
	store.shift_rows(-10);
	QVERIFY2(store.is_spilled(store.placements().first()),
	          "the fixture needs the picture on screen and still out of memory");

	/* Read back on a worker, so it is drawn on a paint after it lands. */
	QTRY_COMPARE(view.grab().toImage().pixelColor(probe), QColor(Qt::red));
}

void graphics_test::a_spilled_picture_opens_only_under_its_own_name() {
	/*
	 * Sealed with the serial as nonce and additional data, so a file
	 * copied onto another picture's name does not open -- it would
	 * otherwise draw one picture where another belongs -- and a damaged
	 * one is refused rather than drawn as garbage.
	 */
	QTemporaryDir base;
	QVERIFY(base.isValid());

	bssh_picture_spill spill(base.path());
	const QImage first = noise_image(30, 30);
	const QImage second = noise_image(30, 30);
	QVERIFY(spill.write(1, first));
	QVERIFY(spill.write(2, second));

	QCOMPARE(spill.read(1), first);
	QCOMPARE(spill.read(2), second);

	const QString one = spill.directory() + QStringLiteral("/1");
	const QString two = spill.directory() + QStringLiteral("/2");

	QVERIFY(QFile::remove(two));
	QVERIFY(QFile::copy(one, two));
	QVERIFY2(spill.read(2).isNull(), "picture 1's file opened as picture 2");

	QFile file(one);
	QVERIFY(file.open(QIODevice::ReadWrite));
	QByteArray sealed = file.readAll();
	sealed[sealed.size() / 2] = char(sealed.at(sealed.size() / 2) ^ 0x01);
	file.seek(0);
	file.write(sealed);
	file.close();
	QVERIFY2(spill.read(1).isNull(), "a damaged spill was read back");
}

void graphics_test::a_spill_directory_goes_with_its_terminal() {
	QTemporaryDir base;
	QVERIFY(base.isValid());

	QString directory;
	{
		bssh_picture_spill spill(base.path());
		QVERIFY(spill.directory().isEmpty());     /* nothing until needed */
		QVERIFY(spill.write(1, noise_image(8, 8)));
		directory = spill.directory();
		QVERIFY(QDir(directory).exists());
	}

	QVERIFY2(!QDir(directory).exists(), "a closed terminal left its pictures on disk");
}

void graphics_test::an_abandoned_spill_is_swept_and_a_live_one_is_not() {
	QTemporaryDir base;
	QVERIFY(base.isValid());

	bssh_picture_spill live(base.path());
	QVERIFY(live.write(1, noise_image(8, 8)));

	/*
	 * A crashed instance's: a lock naming a process that cannot exist,
	 * above any pid_max, on this host -- QLockFile's own test of stale.
	 */
	const QString dead = base.path() + QStringLiteral("/terminal-dead00");
	QVERIFY(QDir().mkpath(dead));
	QFile lock(dead + QStringLiteral("/owner.lock"));
	QVERIFY(lock.open(QIODevice::WriteOnly));
	lock.write(QByteArrayLiteral("999999999\n") + QCoreApplication::applicationName().toUtf8() +
	           QByteArrayLiteral("\n") + QSysInfo::machineHostName().toUtf8() + QByteArrayLiteral("\n"));
	lock.close();
	QFile picture(dead + QStringLiteral("/1"));
	QVERIFY(picture.open(QIODevice::WriteOnly));
	picture.write("sealed");
	picture.close();

	/* And one with no lock at all: a directory another instance is making. */
	const QString unlocked = base.path() + QStringLiteral("/terminal-new000");
	QVERIFY(QDir().mkpath(unlocked));

	QCOMPARE(bssh_picture_spill::sweep_abandoned(base.path()), 1);
	QVERIFY(!QDir(dead).exists());
	QVERIFY(QDir(unlocked).exists());
	QVERIFY(QDir(live.directory()).exists());
	QVERIFY(!live.read(1).isNull());
}

void graphics_test::the_disk_budget_forgets_the_oldest_spilled_picture() {
	QTemporaryDir base;
	QVERIFY(base.isValid());

	bssh_image_store store;
	store.set_spill_base(base.path());
	store.set_memory_budget(1);

	const QImage oldest = noise_image(40, 40);
	const QImage middle = noise_image(40, 40);
	const QImage newest = noise_image(40, 40);

	/* As the emulator scrolls: the push, then the move. */
	auto scroll = [&store](int lines) {
		for (int i = 0; i < lines; ++i) {
			store.pushed_into_history(1);
			store.scrolled(0, 24, 1);
		}
	};

	store.place(placement_at(oldest, 1));
	scroll(5);
	store.place(placement_at(middle, 1));
	scroll(5);
	store.place(placement_at(newest, 1));
	scroll(20);
	QTRY_VERIFY(!store.spills_pending());

	QCOMPARE(store.count(), 3);
	for (const bssh_image_placement_t &placement : store.placements())
		QVERIFY(store.is_spilled(placement));

	/* One byte short of all three: the one furthest back goes. */
	store.set_disk_budget(store.disk_used() - 1);

	QCOMPARE(store.count(), 2);
	QCOMPARE(read_back(store, 0), middle);
	QCOMPARE(read_back(store, 1), newest);
}

void graphics_test::a_picture_deep_in_the_history_stays_beside_its_text() {
	/*
	 * A picture wholly above the screen was moved by nothing: the scroll
	 * shifts what is inside the scrolling region, and the history is
	 * not. So it stopped at the row it left the screen from, and after
	 * forty more lines scrolling back found it forty lines from the text
	 * it was drawn beside. Measured: row -1 after a hundred lines.
	 *
	 * Asserted against the TEXT: the line above the picture, found in
	 * the scrollback, must stay exactly one row above it however much
	 * output follows.
	 */
	bssh_emulator emulator(40, 10);
	emulator.set_cell_pixels(8, 16);
	emulator.set_scrollback_lines(1000);

	emulator.feed(QByteArrayLiteral("MARKER\r\n"));
	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100"),
	                            png_of(16, 16, qRgb(0, 128, 255))));
	for (int i = 0; i < 60; ++i) emulator.feed(QByteArrayLiteral("filler\r\n"));

	const bssh_scrollback &history = emulator.scrollback();
	int marker = -1;
	for (int i = 0; i < history.count(); ++i) {
		const QList<bssh_cell_t> &line = history.line(i);
		if (line.size() >= 6 && line.at(0).ch == U'M' && line.at(5).ch == U'R') marker = i;
	}
	QVERIFY2(marker >= 0, "the fixture's marker line is not in the history");

	QCOMPARE(emulator.images().count(), 1);
	const qint64 marker_line = history.base() + marker;
	auto marker_row = [&history, marker_line]() {
		return int(marker_line - history.base()) - history.count();
	};
	QCOMPARE(emulator.images().placements().first().row, marker_row() + 1);

	/*
	 * And through a shrink and a grow, whose pushes and pops are paid
	 * for by one shift over every picture -- so the per-push move above
	 * must stay out of it, or the history's pictures go twice as far as
	 * their text.
	 */
	emulator.resize(40, 5);
	QCOMPARE(emulator.images().placements().first().row, marker_row() + 1);
	emulator.resize(40, 10);
	QCOMPARE(emulator.images().placements().first().row, marker_row() + 1);
}

void graphics_test::a_picture_whose_text_left_the_scrollback_is_forgotten() {
	/*
	 * Kept only while the text it was drawn beside can still be scrolled
	 * to. With spilling, the budget no longer throws old pictures away,
	 * so without this one the text had left would sit on disk for the
	 * life of the session, drawn by nothing. Asserted against a longer
	 * history keeping the same picture, so the pair says the reach is
	 * what decided it.
	 */
	for (const int history : {20, 1000}) {
		bssh_emulator emulator(40, 10);
		emulator.set_cell_pixels(8, 16);
		emulator.set_scrollback_lines(history);

		emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100"),
		                            png_of(16, 16, qRgb(0, 128, 255))));
		QCOMPARE(emulator.images().count(), 1);

		for (int i = 0; i < 100; ++i) emulator.feed(QByteArrayLiteral("filler\r\n"));

		QCOMPARE(emulator.images().count(), history == 20 ? 0 : 1);
	}
}

void graphics_test::text_after_an_image_in_the_same_read_is_not_hidden_under_it() {
	/*
	 * The cursor owes an image its rows, and paid them after the whole
	 * read -- so text that followed the image in the same read was
	 * printed first, onto the rows the image covers, and drawn under it.
	 * A `cat` of a file does exactly this; one image per read, the way
	 * every other test here feeds, never does.
	 */
	bssh_emulator emulator(40, 12);
	emulator.set_cell_pixels(8, 16);

	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100"), png_of(16, 48, qRgb(255, 0, 0)))
	              + QByteArrayLiteral("\r\nMARKER"));

	QCOMPARE(emulator.images().count(), 1);
	const bssh_image_placement_t &image = emulator.images().placements().first();

	int marker_row = -1;
	for (int r = 0; r < emulator.screen().rows(); ++r)
		if (emulator.screen().cell(r, 0).ch == U'M') marker_row = r;

	QVERIFY2(marker_row >= 0, "the fixture's text is not on the screen");
	QVERIFY2(marker_row >= image.row + image.rows,
	          qPrintable(QStringLiteral("text at row %1 is under the image on rows %2-%3")
	                             .arg(marker_row).arg(image.row).arg(image.row + image.rows - 1)));
}

void graphics_test::the_next_image_begun_in_the_same_read_is_not_cut_off() {
	/*
	 * The owed cursor-down was fed after the whole read, and when the next
	 * image's APC had begun in that read, its ESC landed inside the open
	 * string and ended it. The rest of that chunk was lost and the image
	 * never appeared. Measured: two of five images missing from one
	 * `cat` on an emulator.
	 */
	bssh_emulator emulator(40, 12);
	emulator.set_cell_pixels(8, 16);

	const QByteArray second = png_of(16, 16, qRgb(0, 0, 255)).toBase64();
	const QByteArray first_chunk = QByteArrayLiteral("\x1b_Ga=T,f=100,m=1;")
	                               + second.left(second.size() / 2) + QByteArrayLiteral("\x1b\\");
	const QByteArray last_chunk = QByteArrayLiteral("\x1b_Gm=0;")
	                              + second.mid(second.size() / 2) + QByteArrayLiteral("\x1b\\");

	/* The first image whole, and the second's first chunk begun but not ended. */
	const int split = first_chunk.size() / 2;
	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100"), png_of(16, 16, qRgb(255, 0, 0)))
	              + first_chunk.left(split));
	emulator.feed(first_chunk.mid(split) + last_chunk);

	/* Against the decoded fixture: png_of's fill takes a pixel, not a QRgb. */
	QCOMPARE(emulator.images().count(), 2);
	QCOMPARE(emulator.images().placements().last().image.pixelColor(0, 0),
	         QImage::fromData(png_of(16, 16, qRgb(0, 0, 255))).pixelColor(0, 0));
	QVERIFY(emulator.images().placements().last().image.pixelColor(0, 0)
	        != emulator.images().placements().first().image.pixelColor(0, 0));
}

void graphics_test::an_image_whose_terminator_is_split_across_reads_still_moves_the_text() {
	/*
	 * The ST's ESC at the end of one read and its backslash at the start
	 * of the next. The write is split at terminators it can see whole, so
	 * the trailing ESC is held back to arrive with its other half -- or
	 * the image is placed partway into the second read, and the text
	 * after it in that read lands under it.
	 */
	bssh_emulator emulator(40, 12);
	emulator.set_cell_pixels(8, 16);

	const QByteArray image = kitty_escape(QByteArrayLiteral("Ga=T,f=100"),
	                                      png_of(16, 48, qRgb(255, 0, 0)));
	emulator.feed(image.left(image.size() - 1));
	emulator.feed(image.right(1) + QByteArrayLiteral("\r\nMARKER"));

	QCOMPARE(emulator.images().count(), 1);
	const bssh_image_placement_t &placed = emulator.images().placements().first();

	int marker_row = -1;
	for (int r = 0; r < emulator.screen().rows(); ++r)
		if (emulator.screen().cell(r, 0).ch == U'M') marker_row = r;

	QVERIFY2(marker_row >= placed.row + placed.rows,
	          qPrintable(QStringLiteral("text at row %1 is under the image on rows %2-%3")
	                             .arg(marker_row).arg(placed.row).arg(placed.row + placed.rows - 1)));
}

void graphics_test::a_link_before_an_image_that_scrolls_stays_on_its_text() {
	/*
	 * A link's cells are marked after the write, by row -- so a
	 * cursor-down that scrolls in between would mark the row that has
	 * moved into its place. The image's cursor-down now goes in partway
	 * through a read, so the marks are settled before it as they are at
	 * the end of one.
	 */
	bssh_emulator emulator(40, 6);
	emulator.set_cell_pixels(8, 16);

	emulator.feed(QByteArrayLiteral("\r\n\r\n\r\n\r\n\x1b]8;;https://example.com/\x1b\\LINK"
	                                "\x1b]8;;\x1b\\\r\n")
	              + kitty_escape(QByteArrayLiteral("Ga=T,f=100"), png_of(16, 48, qRgb(255, 0, 0)))
	              + QByteArrayLiteral("after"));

	int link_row = -1;
	for (int r = 0; r < emulator.screen().rows(); ++r)
		if (emulator.screen().cell(r, 0).ch == U'L') link_row = r;
	QVERIFY2(link_row >= 0, "the fixture scrolled the link off the screen");

	QVERIFY2(emulator.screen().cell(link_row, 0).link != 0, "the link's text lost its link");
	for (int r = 0; r < emulator.screen().rows(); ++r) {
		if (r == link_row) continue;
		QVERIFY2(emulator.screen().cell(r, 0).link == 0,
		          qPrintable(QStringLiteral("row %1 was marked instead of row %2").arg(r).arg(link_row)));
	}
}

void graphics_test::text_after_an_image_at_the_bottom_is_not_hidden_under_it() {
	/*
	 * Where output puts nearly every image: at the bottom. The cursor's
	 * advance past the image was a cursor-down, which stops at the bottom
	 * margin, so it stayed inside the image's rows and the text after it
	 * was hidden. Measured on an emulator: the lines after each tall image
	 * in a `cat` were under the image, not below it.
	 */
	bssh_emulator emulator(40, 6);
	emulator.set_cell_pixels(8, 16);

	emulator.feed(QByteArrayLiteral("\r\n\r\n\r\n\r\n"));
	emulator.feed(kitty_escape(QByteArrayLiteral("Ga=T,f=100"), png_of(16, 48, qRgb(255, 0, 0))));
	emulator.feed(QByteArrayLiteral("MARKER"));

	QCOMPARE(emulator.images().count(), 1);
	const bssh_image_placement_t &image = emulator.images().placements().first();

	int marker_row = -1;
	for (int r = 0; r < emulator.screen().rows(); ++r)
		if (emulator.screen().cell(r, 0).ch == U'M') marker_row = r;

	QVERIFY2(marker_row >= 0, "the fixture's text is not on the screen");
	QVERIFY2(marker_row >= image.row + image.rows,
	          qPrintable(QStringLiteral("text at row %1 is under the image on rows %2-%3")
	                             .arg(marker_row).arg(image.row).arg(image.row + image.rows - 1)));
}

void graphics_test::a_spill_is_sealed_off_the_thread_and_the_picture_stays_meanwhile() {
	/*
	 * Sealing a large picture took 160 ms on the emulator, on the thread
	 * the output arrives on. It is sealed on a worker now: when the budget
	 * is applied the spill has only begun, the picture is still in memory
	 * and still drawable, and it leaves memory when the seal comes back.
	 */
	QTemporaryDir base;
	QVERIFY(base.isValid());

	bssh_image_store store;
	store.set_spill_base(base.path());

	const QImage picture = noise_image(100, 100);
	store.place(placement_at(picture, 1));
	store.scrolled(0, 24, 20);
	store.set_memory_budget(1);

	QVERIFY2(store.spills_pending(), "the spill was sealed on the calling thread");
	QVERIFY(!store.is_spilled(store.placements().first()));
	QCOMPARE(store.image(store.placements().first()), picture);

	QTRY_VERIFY(!store.spills_pending());
	QVERIFY(store.is_spilled(store.placements().first()));
	QCOMPARE(read_back(store, 0), picture);
}

void graphics_test::a_picture_back_on_screen_before_its_spill_lands_stays_in_memory() {
	/*
	 * The picture was chosen because it was above the screen; if it is
	 * back on screen when its seal returns, spilling it now would take
	 * what is being looked at out of memory -- the one thing the budget
	 * refuses. It stays, and nothing is written.
	 */
	QTemporaryDir base;
	QVERIFY(base.isValid());

	bssh_image_store store;
	store.set_spill_base(base.path());
	store.set_memory_budget(1);

	store.place(placement_at(noise_image(100, 100), 1));
	store.shift_rows(10);
	QVERIFY(store.spills_pending());
	store.shift_rows(-10);

	QTRY_VERIFY(!store.spills_pending());
	QVERIFY(!store.is_spilled(store.placements().first()));
	QVERIFY(!store.placements().first().image.isNull());
	QCOMPARE(store.disk_used(), qint64(0));
}

void graphics_test::a_spill_in_flight_is_not_paid_for_twice() {
	/*
	 * A picture being sealed is still in memory until it lands, and every
	 * scroll applies the budget again. Counting it as staying would spill
	 * the next picture for bytes that are already on their way out.
	 */
	QTemporaryDir base;
	QVERIFY(base.isValid());

	bssh_image_store store;
	store.set_spill_base(base.path());

	const qint64 bytes = noise_image(100, 100).sizeInBytes();
	store.set_memory_budget(bytes + bytes / 2);

	store.place(placement_at(noise_image(100, 100), 1));
	store.place(placement_at(noise_image(100, 100), 4));
	store.scrolled(0, 24, 20);          /* both above the screen; room for one */
	QVERIFY(store.spills_pending());
	store.scrolled(0, 24, 1);           /* the budget again, before it lands */

	QTRY_VERIFY(!store.spills_pending());
	int spilled = 0;
	for (const bssh_image_placement_t &placement : store.placements())
		if (store.is_spilled(placement)) ++spilled;
	QCOMPARE(spilled, 1);
}

void graphics_test::a_spilled_picture_is_read_back_off_the_thread() {
	/*
	 * Reading a spilled picture back was 49-55 ms on the emulator, on the
	 * thread that was scrolling. It is read on a worker now: the first
	 * image() starts it and returns null, which the renderer skips, and
	 * the store says when it has landed so the view can draw it.
	 */
	QTemporaryDir base;
	QVERIFY(base.isValid());

	bssh_image_store store;
	store.set_spill_base(base.path());
	int restored = 0;
	store.set_restored_callback([&restored] { ++restored; });

	const QImage picture = noise_image(100, 100);
	store.place(placement_at(picture, 1));
	store.scrolled(0, 24, 20);
	store.set_memory_budget(1);
	QTRY_VERIFY(!store.spills_pending());
	QVERIFY(store.is_spilled(store.placements().first()));

	QVERIFY2(store.image(store.placements().first()).isNull(),
	          "the read-back ran on the calling thread");
	QTRY_COMPARE(restored, 1);
	QCOMPARE(store.image(store.placements().first()), picture);

	/*
	 * And a file that does not verify is read once, not on every paint
	 * that asks for it: the second damaged request starts nothing.
	 */
	store.place(placement_at(noise_image(100, 100), 1));
	store.scrolled(0, 24, 20);
	QTRY_VERIFY(!store.spills_pending());

	const QStringList files = QDir(base.path()).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
	QCOMPARE(files.size(), 1);
	const QString dir = base.path() + QLatin1Char('/') + files.first();
	const QString damaged = dir + QLatin1Char('/') + QString::number(store.placements().last().serial);
	QFile file(damaged);
	QVERIFY(file.open(QIODevice::ReadWrite));
	const QByteArray good = file.readAll();
	QByteArray broken = good;
	broken[broken.size() / 2] = char(broken.at(broken.size() / 2) ^ 0x01);
	file.seek(0);
	file.write(broken);
	file.close();

	QVERIFY(store.image(store.placements().last()).isNull());
	QTest::qWait(300);

	/*
	 * The good bytes back: a store that asked again would now succeed,
	 * so still null is what says it did not ask.
	 */
	QVERIFY(file.open(QIODevice::WriteOnly));
	file.write(good);
	file.close();

	QVERIFY(store.image(store.placements().last()).isNull());
	QTest::qWait(300);
	QVERIFY(store.image(store.placements().last()).isNull());
	QCOMPARE(restored, 1);
}
