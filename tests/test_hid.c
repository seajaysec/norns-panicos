/* tests/test_hid.c — unit tests for the SDL-free evdev mapping + frame encoder. */
#include <assert.h>
#include <stdio.h>
#include "../src/norns-hid.h"

static void test_button_mapping(void) {
    uint16_t type, code;
    assert(pad_input_to_evdev(PAD_A, &type, &code) == 1);
    assert(type == EV_KEY && code == EVB_BTN_SOUTH);
    assert(pad_input_to_evdev(PAD_Y, &type, &code) == 1);
    assert(type == EV_KEY && code == EVB_BTN_NORTH);   /* Y = NORTH */
    assert(pad_input_to_evdev(PAD_X, &type, &code) == 1);
    assert(type == EV_KEY && code == EVB_BTN_WEST);    /* X = WEST */
    printf("  PASS test_button_mapping\n");
}

static void test_axis_mapping(void) {
    uint16_t type, code;
    assert(pad_input_to_evdev(PAD_LX, &type, &code) == 1);
    assert(type == EV_ABS && code == EVA_ABS_X);
    assert(pad_input_to_evdev(PAD_RY, &type, &code) == 1);
    assert(type == EV_ABS && code == EVA_ABS_RY);
    printf("  PASS test_axis_mapping\n");
}

static void test_frame_encode(void) {
    uint8_t f[HID_FRAME_SZ];
    hid_frame_encode(EV_KEY, EVB_BTN_SOUTH, 1, f);
    assert(f[0] == EV_KEY);
    assert((uint16_t)(f[1] | (f[2] << 8)) == EVB_BTN_SOUTH);
    assert((int16_t)(f[3] | (f[4] << 8)) == 1);

    hid_frame_encode(EV_ABS, EVA_ABS_X, -32768, f);
    assert((int16_t)(f[3] | (f[4] << 8)) == -32768);
    printf("  PASS test_frame_encode\n");
}

int main(void) {
    printf("test_hid:\n");
    test_button_mapping();
    test_axis_mapping();
    test_frame_encode();
    printf("All tests passed.\n");
    return 0;
}
