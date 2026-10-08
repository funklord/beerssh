#ifndef SK_METRICS_H
#define SK_METRICS_H

/*
 * The smallest a key may be drawn by default, in dp: a thumb's reliable
 * target. Material's 48dp, the floor BeerSSH's sec 6.2 holds every control
 * to. A user may drag a keyboard's keys below it (sk_keyboard's resize
 * bars), never the default.
 */
const int SK_TOUCH_TARGET_DP = 48;

#endif /* SK_METRICS_H */
