/* tests/test_monome_mext.c — unit tests for the mext grid parser + LED encoder. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/norns-monome-mext.h"

static void test_parse_down_up(void) {
    mext_parser_t p; mext_parser_init(&p);
    mext_key_t k;
    /* 0x21 03 05 = key down at (3,5) */
    assert(mext_parser_push(&p, 0x21, &k) == 0);
    assert(mext_parser_push(&p, 0x03, &k) == 0);
    assert(mext_parser_push(&p, 0x05, &k) == 1);
    assert(k.state == 1 && k.x == 3 && k.y == 5);
    /* 0x20 03 05 = key up */
    assert(mext_parser_push(&p, 0x20, &k) == 0);
    assert(mext_parser_push(&p, 0x03, &k) == 0);
    assert(mext_parser_push(&p, 0x05, &k) == 1);
    assert(k.state == 0 && k.x == 3 && k.y == 5);
    printf("  PASS test_parse_down_up\n");
}

static void test_parse_resync(void) {
    /* A stray coordinate byte before the first status byte must be skipped. */
    mext_parser_t p; mext_parser_init(&p);
    mext_key_t k;
    assert(mext_parser_push(&p, 0x05, &k) == 0);   /* stray → skipped */
    assert(mext_parser_push(&p, 0x21, &k) == 0);
    assert(mext_parser_push(&p, 0x02, &k) == 0);
    assert(mext_parser_push(&p, 0x04, &k) == 1);
    assert(k.state == 1 && k.x == 2 && k.y == 4);
    printf("  PASS test_parse_resync\n");
}

static void test_parse_stream(void) {
    /* Back-to-back frames (mirrors the live capture) decode in order. */
    const uint8_t s[] = { 0x21,0x02,0x04, 0x20,0x06,0x05, 0x21,0x00,0x07 };
    const mext_key_t want[] = { {2,4,1}, {6,5,0}, {0,7,1} };
    mext_parser_t p; mext_parser_init(&p);
    mext_key_t k; int got = 0;
    for (size_t i = 0; i < sizeof(s); i++)
        if (mext_parser_push(&p, s[i], &k)) {
            assert(k.x == want[got].x && k.y == want[got].y && k.state == want[got].state);
            got++;
        }
    assert(got == 3);
    printf("  PASS test_parse_stream\n");
}

static void test_bounds(void) {
    mext_key_t a = {15, 7, 1}, b = {16, 0, 1}, c = {0, 8, 0};
    assert(mext_key_in_bounds(&a) == 1);
    assert(mext_key_in_bounds(&b) == 0);   /* x out of range */
    assert(mext_key_in_bounds(&c) == 0);   /* y out of range */
    printf("  PASS test_bounds\n");
}

static void test_led_diff(void) {
    uint8_t last[MEXT_GRID_CELLS] = {0};
    uint8_t next[MEXT_GRID_CELLS] = {0};
    uint8_t out[MEXT_LED_DIFF_MAX];

    /* One lit cell (x=9,y=3,level=15) → exactly one 0x18 command. */
    next[3 * 16 + 9] = 15;
    int n = mext_led_diff(next, last, out);
    assert(n == 4);
    assert(out[0] == 0x18 && out[1] == 9 && out[2] == 3 && out[3] == 15);
    assert(last[3 * 16 + 9] == 15);          /* last[] updated */

    /* No change → no commands. */
    assert(mext_led_diff(next, last, out) == 0);

    /* Turn it off → one command with level 0. */
    next[3 * 16 + 9] = 0;
    n = mext_led_diff(next, last, out);
    assert(n == 4 && out[0] == 0x18 && out[3] == 0);

    /* Levels are masked to 0-15. */
    memset(last, 0, sizeof(last)); memset(next, 0, sizeof(next));
    next[0] = 0xFF;
    n = mext_led_diff(next, last, out);
    assert(n == 4 && out[3] == 0x0f);
    printf("  PASS test_led_diff\n");
}

int main(void) {
    printf("Running norns-monome-mext tests...\n");
    test_parse_down_up();
    test_parse_resync();
    test_parse_stream();
    test_bounds();
    test_led_diff();
    printf("All tests passed.\n");
    return 0;
}
