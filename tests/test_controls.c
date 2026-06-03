/* tests/test_controls.c — unit tests for the configurable control mapping.
 * Pure C, no SDL: includes the real header so it tests the shipping logic. */
#include <assert.h>
#include <stdio.h>
#include "../src/norns-controls.h"

static void test_defaults(void) {
    controls_t c;
    controls_defaults(&c);
    assert(c.enc[0] == ENC_SRC_NONE);     /* E1 is D-pad-only */
    assert(c.enc[1] == ENC_SRC_LSTICK);   /* E2 = L stick (+ D-pad up/down) */
    assert(c.enc[2] == ENC_SRC_RSTICK);   /* E3 = R stick */
    assert(c.dpad_enc[0] == 0);           /* D-pad left/right → E1 */
    assert(c.dpad_enc[1] == 1);           /* D-pad up/down     → E2 */
    assert(c.shoulder_enc == 0);          /* L1/R1 → E1 */
    assert(c.trigger_enc == 2);           /* L2/R2 → E3 */
    assert(c.key_btn[0] == BTN_Y);        /* K1 = Y (B does nothing) */
    assert(c.key_btn[1] == BTN_X);
    assert(c.key_btn[2] == BTN_A);
    assert(c.dpad_step == 2);
    assert(c.accel == 1 && c.accel_delay == 18 && c.accel_ramp == 60 && c.accel_max == 5);
    assert(c.stick_accel_delay == 30 && c.stick_accel_ramp == 110);
    assert(c.dpad_y_invert == 0);         /* globally upright; flipped per-context */
    printf("  PASS test_defaults\n");
}

static void test_dpad_lookup_default(void) {
    controls_t c;
    controls_defaults(&c);
    assert(controls_enc_for_dpad(&c, 0) == 0);   /* left/right → E1 */
    assert(controls_enc_for_dpad(&c, 1) == 1);   /* up/down    → E2 */
    printf("  PASS test_dpad_lookup_default\n");
}

static void test_dpad_mapping(void) {
    /* D-pad axes are mapped independently of stick sources. */
    controls_t c;
    controls_defaults(&c);
    assert(controls_parse(&c, "dpad_x = 3\ndpad_y = none\n") == 2);
    assert(controls_enc_for_dpad(&c, 0) == 2);   /* left/right → E3 */
    assert(controls_enc_for_dpad(&c, 1) == -1);  /* up/down    → unbound */
    printf("  PASS test_dpad_mapping\n");
}

static void test_stick_classifiers(void) {
    controls_t c;
    controls_defaults(&c);
    assert(controls_enc_is_stick(&c, 0) == 0);        /* none   */
    assert(controls_enc_is_stick(&c, 1) == 1);        /* lstick */
    assert(controls_enc_is_left_stick(&c, 1) == 1);
    assert(controls_enc_is_left_stick(&c, 2) == 0);   /* rstick */
    assert(controls_enc_is_stick_y(&c, 1) == 0);
    c.enc[1] = ENC_SRC_LSTICK_Y;
    assert(controls_enc_is_stick_y(&c, 1) == 1);
    /* Combined-axis source: both-axes, left stick, not "y-only". */
    c.enc[0] = ENC_SRC_LSTICK_XY;
    assert(controls_enc_is_stick(&c, 0) == 1);
    assert(controls_enc_is_stick_xy(&c, 0) == 1);
    assert(controls_enc_is_left_stick(&c, 0) == 1);
    assert(controls_enc_is_stick_y(&c, 0) == 0);
    c.enc[2] = ENC_SRC_RSTICK_XY;
    assert(controls_enc_is_stick_xy(&c, 2) == 1);
    assert(controls_enc_is_left_stick(&c, 2) == 0);
    /* Inverted-Y: reads the Y axis but is NOT "up = increment" (no negate),
     * so down = increment. */
    controls_defaults(&c);
    c.enc[2] = ENC_SRC_RSTICK_Y_INV;
    assert(controls_enc_is_stick(&c, 2) == 1);
    assert(controls_enc_reads_y(&c, 2) == 1);     /* picks the Y axis */
    assert(controls_enc_is_stick_y(&c, 2) == 0);  /* but no up-is-increment negate */
    assert(controls_enc_is_left_stick(&c, 2) == 0);
    c.enc[0] = ENC_SRC_LSTICK_Y_INV;
    assert(controls_enc_is_left_stick(&c, 0) == 1);
    assert(controls_enc_reads_y(&c, 0) == 1);
    /* …and the parser accepts the -xy / -y-inv tokens (the [script:pixels] overlay). */
    controls_defaults(&c);
    assert(controls_parse(&c, "e1 = lstick-xy\ne2 = rstick\ne3 = rstick-y-inv\n") == 3);
    assert(c.enc[0] == ENC_SRC_LSTICK_XY);
    assert(c.enc[1] == ENC_SRC_RSTICK);
    assert(c.enc[2] == ENC_SRC_RSTICK_Y_INV);
    printf("  PASS test_stick_classifiers\n");
}

static void test_stick_delta(void) {
    controls_t c;
    controls_defaults(&c);
    assert(controls_stick_delta(&c, 0)      == 0);   /* centre → deadzone */
    assert(controls_stick_delta(&c, 4000)   == 0);   /* inside deadzone   */
    assert(controls_stick_delta(&c, 10000)  == 1);   /* normal            */
    assert(controls_stick_delta(&c, 20000)  == 1);   /* still 1 below ~75% */
    assert(controls_stick_delta(&c, 30000)  == 2);   /* near full tilt    */
    assert(controls_stick_delta(&c, -30000) == -2);  /* opposite direction */
    c.stick_invert = 1;
    assert(controls_stick_delta(&c, 30000)  == -2);  /* invert flips sign */
    printf("  PASS test_stick_delta\n");
}

static void test_accel_factor(void) {
    controls_t c;
    controls_defaults(&c);                /* dpad: delay 18 ramp 60; max 5 */
    int dd = c.accel_delay, dr = c.accel_ramp;
    assert(controls_accel_factor(&c, 0,  dd, dr) == 1.0f);   /* below delay → base */
    assert(controls_accel_factor(&c, 18, dd, dr) == 1.0f);   /* at delay    → base */
    assert(controls_accel_factor(&c, 78, dd, dr) == 5.0f);   /* delay+ramp  → max  */
    float mid = controls_accel_factor(&c, 48, dd, dr);
    assert(mid > 1.0f && mid < 5.0f);
    assert(controls_accel_factor(&c, 1000, dd, dr) == 5.0f); /* clamped at max */
    /* stick params ramp more gently: at the same held_frames, factor is lower */
    int sd = c.stick_accel_delay, sr = c.stick_accel_ramp;
    assert(controls_accel_factor(&c, 48, sd, sr) < mid);
    c.accel = 0;
    assert(controls_accel_factor(&c, 1000, dd, dr) == 1.0f); /* disabled → constant */
    printf("  PASS test_accel_factor\n");
}

static void test_parse_keys_and_tunables(void) {
    controls_t c;
    controls_defaults(&c);
    int n = controls_parse(&c,
        "# a comment\n"
        "k1 = a\n"
        "k2 = x, y\n"
        "dpad_step = 4\n"
        "stick_deadzone = 12000\n"
        "stick_throttle = 5\n"
        "stick_invert = 1\n"
        "stick_accel_delay = 40\n"
        "stick_accel_ramp = 120\n");
    assert(n == 8);
    assert(c.key_btn[0] == BTN_A);
    assert(c.key_btn[1] == (BTN_X | BTN_Y));
    assert(c.dpad_step == 4);
    assert(c.stick_deadzone == 12000);
    assert(c.stick_throttle == 5);
    assert(c.stick_invert == 1);
    assert(c.stick_accel_delay == 40 && c.stick_accel_ramp == 120);
    printf("  PASS test_parse_keys_and_tunables\n");
}

static void test_parse_clamps_and_canon(void) {
    controls_t c;
    controls_defaults(&c);
    /* underscore→dash canon + clamping; dpad_x value parsed to 0-based encoder */
    controls_parse(&c, "DPAD_X = 2\nDPAD_STEP = 999\nstick_throttle = 0\n");
    assert(c.dpad_enc[0] == 1);      /* dpad_x = 2 → encoder index 1 */
    assert(c.dpad_step == 16);       /* clamped to max */
    assert(c.stick_throttle == 1);   /* clamped to min */
    printf("  PASS test_parse_clamps_and_canon\n");
}

static void test_parse_ignores_unknown(void) {
    controls_t c;
    controls_defaults(&c);
    int n = controls_parse(&c, "bogus = 7\ne2 = lstick\ne3 = wat\n");
    assert(n == 1);                  /* only e2 applied */
    assert(c.enc[1] == ENC_SRC_LSTICK);
    assert(c.enc[2] == ENC_SRC_RSTICK);  /* unchanged from default */
    printf("  PASS test_parse_ignores_unknown\n");
}

static const char *TWO_SCHEME_CONF =
    "scheme = dpad\n"
    "k1 = y, b\n"
    "k2 = x\n"
    "k3 = a\n"
    "dpad_step = 3\n"
    "[sticks]\n"
    "e1 = none\n"
    "e2 = lstick\n"
    "e3 = rstick\n"
    "dpad_x = 1\n"
    "dpad_y = 2\n"
    "shoulders = none\n"
    "[dpad]\n"
    "dpad_x = 2\n"
    "dpad_y = 1\n"
    "shoulders = 3\n";

static void test_find_scheme(void) {
    char name[32] = "";
    assert(controls_find_scheme(TWO_SCHEME_CONF, name, sizeof(name)) == 1);
    assert(!strcmp(name, "dpad"));
    assert(controls_find_scheme("[x]\nscheme = nope\n", name, sizeof(name)) == 0);
    printf("  PASS test_find_scheme\n");
}

static void test_scheme_selection(void) {
    /* Selecting [dpad] applies globals + only the dpad section. */
    controls_t c;
    controls_defaults(&c);
    controls_parse_scheme(&c, TWO_SCHEME_CONF, "dpad");
    assert(c.dpad_step == 3);               /* global applied */
    assert(c.key_btn[0] == (BTN_Y | BTN_B));/* global applied */
    assert(controls_enc_for_dpad(&c, 0) == 1);  /* left/right → E2 */
    assert(controls_enc_for_dpad(&c, 1) == 0);  /* up/down    → E1 */
    assert(controls_enc_for_shoulder(&c) == 2); /* L1/R1 → E3 */
    printf("  PASS test_scheme_selection\n");
}

static void test_scheme_isolation(void) {
    /* Selecting [sticks] from the same file yields the stick layout. */
    controls_t c;
    controls_defaults(&c);
    controls_parse_scheme(&c, TWO_SCHEME_CONF, "sticks");
    assert(c.enc[1] == ENC_SRC_LSTICK);
    assert(c.enc[2] == ENC_SRC_RSTICK);
    assert(controls_enc_for_dpad(&c, 0) == 0);  /* left/right → E1 */
    assert(controls_enc_for_dpad(&c, 1) == 1);  /* up/down    → E2 */
    assert(controls_enc_for_shoulder(&c) == -1);
    printf("  PASS test_scheme_isolation\n");
}

static void test_has_section(void) {
    assert(controls_has_section(TWO_SCHEME_CONF, "dpad")   == 1);
    assert(controls_has_section(TWO_SCHEME_CONF, "sticks") == 1);
    assert(controls_has_section(TWO_SCHEME_CONF, "ghost")  == 0);
    printf("  PASS test_has_section\n");
}

static void test_unknown_scheme_keeps_globals(void) {
    controls_t c;
    controls_defaults(&c);
    controls_parse_scheme(&c, TWO_SCHEME_CONF, "bogus");
    assert(c.dpad_step == 3);             /* global applied */
    assert(c.enc[0] == ENC_SRC_NONE);     /* default layout intact */
    assert(c.enc[1] == ENC_SRC_LSTICK);
    assert(c.dpad_enc[0] == 0 && c.dpad_enc[1] == 1);
    printf("  PASS test_unknown_scheme_keeps_globals\n");
}

static const char *CONTEXT_CONF =
    "k1 = y\n"
    "k2 = x\n"
    "k3 = a\n"
    "[menu]\n"
    "k2 = x, b\n"           /* B mirrors K2 only in the menu */
    "dpad_y_invert = 1\n"   /* menu list scrolls down=+, flip D-pad up/down */
    "[script:koiboi2]\n"
    "dpad_y_invert = 1\n";

static void test_context_overlay(void) {
    /* Mirrors resolve_controls(): base parse, then overlay one named section. */
    controls_t base;
    controls_defaults(&base);
    controls_parse_scheme(&base, CONTEXT_CONF, NULL);   /* globals only */
    assert(base.key_btn[1] == BTN_X);
    assert(base.dpad_y_invert == 0);

    /* Menu overlay: B mirrors K2, D-pad up/down inverted. */
    controls_t menu = base;
    controls_parse_scheme(&menu, CONTEXT_CONF, "menu");
    assert(menu.key_btn[1] == (BTN_X | BTN_B));
    assert(menu.dpad_y_invert == 1);
    assert(menu.key_btn[0] == BTN_Y);   /* untouched keys stay */

    /* Per-script overlay: only dpad_y_invert differs; K2 stays bare X. */
    controls_t koi = base;
    controls_parse_scheme(&koi, CONTEXT_CONF, "script:koiboi2");
    assert(koi.dpad_y_invert == 1);
    assert(koi.key_btn[1] == BTN_X);    /* no B mirror outside the menu */
    printf("  PASS test_context_overlay\n");
}

static void test_native_mode(void) {
    controls_t c;
    controls_defaults(&c);
    assert(c.native_mode == 0);                       /* off by default */

    const char *cfg =
        "scheme = sticks\n"
        "[sticks]\ne1 = none\ne2 = lstick\ne3 = rstick\n"
        "[script:gamepong]\nmode = native\n";
    char scheme[32] = "";
    controls_find_scheme(cfg, scheme, sizeof(scheme));
    controls_parse_scheme(&c, cfg, scheme);
    assert(c.native_mode == 0);                       /* scheme alone: not native */

    controls_parse_scheme(&c, cfg, "script:gamepong");
    assert(c.native_mode == 1);                       /* overlay turns it on */

    /* Any non-"native" value parses back to emulation (0). */
    const char *off = "[script:plain]\nmode = emulation\n";
    controls_parse_scheme(&c, off, "script:plain");
    assert(c.native_mode == 0);
    printf("  PASS test_native_mode\n");
}

int main(void) {
    printf("Running norns-controls tests...\n");
    test_defaults();
    test_dpad_lookup_default();
    test_dpad_mapping();
    test_stick_classifiers();
    test_stick_delta();
    test_accel_factor();
    test_parse_keys_and_tunables();
    test_parse_clamps_and_canon();
    test_parse_ignores_unknown();
    test_find_scheme();
    test_scheme_selection();
    test_scheme_isolation();
    test_has_section();
    test_unknown_scheme_keeps_globals();
    test_context_overlay();
    test_native_mode();
    printf("All tests passed.\n");
    return 0;
}
