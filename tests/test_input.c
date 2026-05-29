/* tests/test_input.c — unit tests for norns-panicos input protocol */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

/* ── Frame encoding (mirrors norns-panicos.c logic) ── */

static void make_enc_frame(uint8_t *frame, uint8_t enc_id, int16_t delta) {
    frame[0] = 0;
    frame[1] = enc_id;
    frame[2] = (uint8_t)(delta & 0xFF);
    frame[3] = (uint8_t)((delta >> 8) & 0xFF);
}

static void make_key_frame(uint8_t *frame, uint8_t key_id, uint8_t state) {
    frame[0] = 1;
    frame[1] = key_id;
    frame[2] = state;
    frame[3] = 0;
}

/* ── Greyscale unpack (mirrors pump_screen logic) ── */

static uint8_t unpack_left(uint8_t packed)  { return ((packed >> 4) & 0xF) * 17; }
static uint8_t unpack_right(uint8_t packed) { return  (packed & 0xF)       * 17; }

/* ── Encoder selection state machine ── */

typedef struct { uint8_t selected_enc; } enc_state_t;

static void enc_select(enc_state_t *s, uint8_t enc) {
    if (enc < 3) s->selected_enc = enc;
}

/* ── Tests ── */

static void test_enc_frame_positive(void) {
    uint8_t f[4];
    make_enc_frame(f, 0, 3);
    assert(f[0] == 0);   /* type: encoder */
    assert(f[1] == 0);   /* id: E1 */
    assert(f[2] == 3);   /* val_lo */
    assert(f[3] == 0);   /* val_hi */
    printf("  PASS test_enc_frame_positive\n");
}

static void test_enc_frame_negative(void) {
    uint8_t f[4];
    make_enc_frame(f, 2, -1);
    assert(f[0] == 0);
    assert(f[1] == 2);     /* E3 */
    assert(f[2] == 0xFF);  /* -1 low byte */
    assert(f[3] == 0xFF);  /* -1 high byte */
    printf("  PASS test_enc_frame_negative\n");
}

static void test_enc_frame_large_negative(void) {
    uint8_t f[4];
    make_enc_frame(f, 1, -3);
    assert(f[0] == 0);
    assert(f[1] == 1);
    assert(f[2] == 0xFD);  /* -3 little-endian: 0xFFFD */
    assert(f[3] == 0xFF);
    printf("  PASS test_enc_frame_large_negative\n");
}

static void test_key_frame(void) {
    uint8_t f[4];
    make_key_frame(f, 0, 1);
    assert(f[0] == 1);  /* type: key */
    assert(f[1] == 0);  /* K1 */
    assert(f[2] == 1);  /* down */
    assert(f[3] == 0);
    make_key_frame(f, 2, 0);
    assert(f[0] == 1);
    assert(f[1] == 2);  /* K3 */
    assert(f[2] == 0);  /* up */
    printf("  PASS test_key_frame\n");
}

static void test_greyscale_unpack(void) {
    /* 0x0F: left=0 (black), right=15 (white) */
    assert(unpack_left(0x0F)  == 0);
    assert(unpack_right(0x0F) == 255);
    /* 0xFF: both white */
    assert(unpack_left(0xFF)  == 255);
    assert(unpack_right(0xFF) == 255);
    /* 0x00: both black */
    assert(unpack_left(0x00)  == 0);
    assert(unpack_right(0x00) == 0);
    /* 0xAF: left=10 (170), right=15 (255) */
    assert(unpack_left(0xAF)  == 170);
    assert(unpack_right(0xAF) == 255);
    /* 0x88: both 8 → 8*17=136 */
    assert(unpack_left(0x88)  == 136);
    assert(unpack_right(0x88) == 136);
    printf("  PASS test_greyscale_unpack\n");
}

static void test_encoder_selection(void) {
    enc_state_t s = { .selected_enc = 0 };
    assert(s.selected_enc == 0);
    enc_select(&s, 1);
    assert(s.selected_enc == 1);
    enc_select(&s, 2);
    assert(s.selected_enc == 2);
    enc_select(&s, 3);   /* invalid — ignored */
    assert(s.selected_enc == 2);
    enc_select(&s, 0);
    assert(s.selected_enc == 0);
    printf("  PASS test_encoder_selection\n");
}

int main(void) {
    printf("Running norns-panicos input tests...\n");
    test_enc_frame_positive();
    test_enc_frame_negative();
    test_enc_frame_large_negative();
    test_key_frame();
    test_greyscale_unpack();
    test_encoder_selection();
    printf("All tests passed.\n");
    return 0;
}
