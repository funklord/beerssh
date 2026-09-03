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
	void a_declared_size_that_overflows_is_refused();
	void a_small_payload_that_decodes_huge_is_refused();
	void a_chunked_transmission_cannot_grow_without_end();
	void a_resize_keeps_an_image_with_the_text_it_was_drawn_beside();
	void a_resize_moves_images_even_when_the_ring_is_full();
	void a_frame_drawn_over_an_earlier_one_does_not_keep_it_alive();
	void kitty_png_is_placed_at_the_cursor();
	void kitty_raw_rgb_is_decoded();
	void kitty_chunked_transmission_reassembles();
	void kitty_delete_removes_images();
	void an_image_without_an_id_does_not_answer_to_a_remote_one();
	void iterm2_inline_image_is_placed();
	void sixel_is_decoded_by_libsixel();
	void malformed_sixel_is_declined_not_guessed();
	void images_follow_the_text_when_the_screen_scrolls();
	void a_shrink_moves_an_image_with_its_row_rather_than_dropping_it();
	void an_absurd_cell_footprint_is_refused_by_both_protocols();
	void an_image_scrolled_off_the_top_survives_in_the_scrollback();
	void the_memory_budget_drops_the_oldest_but_never_the_visible();
	void a_background_image_is_drawn_under_the_text();
	void an_image_that_arrived_as_bytes_reaches_the_screen();
};

#endif /* BSSH_GRAPHICS_TEST_H */
