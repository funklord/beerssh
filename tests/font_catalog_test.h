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
	void ids_are_stable_and_unique();
	void the_picker_never_offers_fallback_only_families();
	void defaults_differ_by_form_factor_but_come_from_one_set();
	void names_resolve_to_ids_and_unknown_names_survive();
	void the_chain_always_ends_somewhere_drawable();
	void an_unavailable_font_is_reported_rather_than_swapped_silently();
	void glyph_lookup_answers_from_the_chain_and_repeats_itself();
	void fonts_are_built_with_merging_off();
	void the_bundled_set_loads_and_covers_what_it_promises();
	void a_colour_scheme_repaints_only_the_sixteen_it_owns();
	void an_unknown_scheme_is_kept_but_drawn_in_the_default();
};

#endif /* BSSH_FONT_CATALOG_TEST_H */
