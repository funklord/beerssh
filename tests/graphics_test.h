#ifndef BSSH_GRAPHICS_TEST_H
#define BSSH_GRAPHICS_TEST_H

#include <QObject>

/*
 * Inline images (project.md sec 7.1, tier 4).
 *
 * The protocol parsers are pure functions from bytes to a placement, so
 * they are tested directly on byte strings -- an image protocol is
 * exactly the kind of thing that looks right on screen while being wrong
 * about its footprint, and a screenshot cannot tell you which.
 */
class graphics_test : public QObject {
	Q_OBJECT

private slots:
	void kitty_png_is_placed_at_the_cursor();
	void kitty_raw_rgb_is_decoded();
	void kitty_chunked_transmission_reassembles();
	void kitty_delete_removes_images();
	void iterm2_inline_image_is_placed();
	void sixel_is_decoded_by_libsixel();
	void malformed_sixel_is_declined_not_guessed();
	void images_follow_the_text_when_the_screen_scrolls();
	void images_are_dropped_when_the_grid_shrinks();
};

#endif /* BSSH_GRAPHICS_TEST_H */
