#ifndef BSSH_FONT_CATALOG_TEST_H
#define BSSH_FONT_CATALOG_TEST_H

#include <QObject>

/*
 * Tests for the curated font set (project.md sec 9).
 *
 * These have to hold on a machine with none of the bundled fonts present,
 * which is both the CI container and any checkout where the fetch script
 * has not been run -- so nothing here asserts that a particular family is
 * installed. What it asserts is the part that must be true regardless: the
 * picker is the same everywhere, ids are stable, and a font that is not
 * there resolves to one that is and says so.
 */
class font_catalog_test : public QObject {
	Q_OBJECT

private slots:
	/*
	 * Installed monospace fonts, offered after the set on a desktop
	 * (sec 9.4, the holder's answer of 2026-09-26).
	 */
	void an_installed_font_resolves_like_a_set_font();
	void an_installed_font_that_is_gone_falls_back_and_says_so();

	/*
	 * sec 9.3's whole promise: box-drawing characters meet at the cell
	 * edge. Measured rather than looked at -- see the test.
	 */
	void every_bundled_face_draws_a_box_the_width_of_its_cell();

	void ids_are_stable_and_unique();
	void the_picker_never_offers_fallback_only_families();
	void defaults_differ_by_form_factor_but_come_from_one_set();
	void names_resolve_to_ids_and_unknown_names_survive();
	void the_chain_always_ends_somewhere_drawable();
	void an_unavailable_font_is_reported_rather_than_swapped_silently();
	void glyph_lookup_answers_from_the_chain_and_repeats_itself();
	void fonts_are_built_with_merging_off();

	/*
	 * Ligatures off by default and on when asked (sec 9.2): the liga/clig/
	 * calt features are pinned to 0 for the grid, and left to the font's
	 * own defaults when the user opts in. Qt 6.7+ only, where per-font
	 * feature control exists.
	 */
	void ligatures_are_off_by_default_and_optional();
	void the_bundled_set_loads_and_covers_what_it_promises();
	void a_scheme_owns_the_background_and_the_text_not_the_palette();
	void an_unknown_scheme_is_kept_but_drawn_in_the_default();
	void every_bundled_font_names_a_licence_file_that_exists();
	void our_own_scheme_is_legible();
	void the_lift_keeps_a_colour_recognisable();
};

#endif /* BSSH_FONT_CATALOG_TEST_H */
