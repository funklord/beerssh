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
	void a_picture_over_the_budget_is_spilled_and_comes_back_whole();
	void a_spilled_picture_scrolled_back_to_reaches_the_screen();
	void a_spilled_picture_opens_only_under_its_own_name();
	void a_spill_directory_goes_with_its_terminal();
	void an_abandoned_spill_is_swept_and_a_live_one_is_not();
	void the_disk_budget_forgets_the_oldest_spilled_picture();
	void a_picture_deep_in_the_history_stays_beside_its_text();
	void text_after_an_image_in_the_same_read_is_not_hidden_under_it();
	void the_next_image_begun_in_the_same_read_is_not_cut_off();
	void an_image_whose_terminator_is_split_across_reads_still_moves_the_text();
	void a_link_before_an_image_that_scrolls_stays_on_its_text();
	void text_after_an_image_at_the_bottom_is_not_hidden_under_it();
	void a_picture_whose_text_left_the_scrollback_is_forgotten();
	void a_background_image_is_drawn_under_the_text();
	void an_image_that_arrived_as_bytes_reaches_the_screen();
	void dropping_the_history_drops_the_pictures_in_it();
	void resetting_the_terminal_drops_a_half_sent_image();
};

#endif /* BSSH_GRAPHICS_TEST_H */
