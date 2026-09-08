#ifndef BSSH_PHONE_METRICS_H
#define BSSH_PHONE_METRICS_H

/*
 * The screen the suite holds this app to, in one place.
 *
 * 320, not 360, and the number is a DEVICE rather than a round figure. A
 * Galaxy Z Fold 3's cover screen is 832x2268 at 420dpi, which is 320dp
 * wide, and folded is how it is carried.
 *
 * Every "fits a phone" assertion ran at 360 until 2026-09-07, so the
 * narrowest case the suite knew was 40dp wider than a screen this app is
 * used on daily -- a set of tabulated extremes that did not include the
 * extreme. Measured rather than assumed: at 320 three surfaces
 * overflowed, and the two causes were not the ones guessed at. The font
 * picker's colour strip was a fixed `16 * 18` pixmap and a QLabel demands
 * its pixmap's width, so one decorative strip set the floor for the
 * picker and for both editors that embed it -- 288px, to the pixel. And
 * every shortcut's description IS its form row's label, which QFormLayout
 * does not wrap, so the settings page's floor was its longest description
 * beside a QKeySequenceEdit. Neither was the combo boxes, which is what
 * the first two guesses said.
 *
 * A HEADER, and that is the part this file exists for. That change had to
 * be made by hand in every file with its own copy of the number, and it
 * reached two of six: layout_test.cpp and ui_shots.cpp, which also spelled
 * it differently from each other -- BSSH_PHONE_DP against BSSH_PHONE_W,
 * the same 320 under two names. secret_store_test.cpp kept 360 in a test
 * called `no_setting_is_cut_off_at_a_phone_width` and in a local `const
 * int phone = 360` whose comment cited a section that had moved;
 * platform_test.cpp and two more sites in layout_test.cpp kept it too.
 *
 * touch_metrics.h already states the argument for this file: the danger
 * is not the duplication, it is that a rule written in several places is
 * one that can be changed in all but one. The phone width is that rule,
 * and it has now been the exception twice.
 *
 * Lowering these is how a gate gets satisfied by deforming the thing it
 * measures. Lower them only for a device that is genuinely narrower.
 */
const int BSSH_PHONE_DP = 320;
const int BSSH_PHONE_TALL_DP = 640;

/*
 * The half-screen column sec 8.2 names when it talks about tiling -- a
 * small desktop window rather than a maximised one, because the tiled
 * case is where a desktop layout has to prove it still works.
 */
const int BSSH_SMALL_DESKTOP_W = 800;
const int BSSH_SMALL_DESKTOP_H = 600;

#endif /* BSSH_PHONE_METRICS_H */
