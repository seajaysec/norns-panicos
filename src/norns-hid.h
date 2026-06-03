/*
 * norns-hid.h — SDL-free evdev mapping + frame encoder for the native HID path.
 *
 * Header-only (like norns-controls.h) so the host and the standalone tests share
 * the SAME logic. The host translates SDL events to pad_input_t (trivial switch,
 * SDL-coupled, lives in the .c); everything below is pure and unit-tested.
 *
 * Wire frame (host → matron NORNS_HID_FIFO): 5 bytes, little-endian
 *   [type:1][code_lo:1][code_hi:1][value_lo:1][value_hi:1]
 *   type  = EV_KEY (button) | EV_ABS (axis)
 *   value = 0/1 for buttons; -32768..32767 for axes
 */
#ifndef NORNS_HID_H
#define NORNS_HID_H

#include <stdint.h>

/* Linux evdev event types (subset) */
#define EV_KEY 0x01
#define EV_ABS 0x03

/* evdev button codes (linux/input-event-codes.h) */
#define EVB_BTN_SOUTH      0x130  /* A  */
#define EVB_BTN_EAST       0x131  /* B  */
#define EVB_BTN_NORTH      0x133  /* Y  */
#define EVB_BTN_WEST       0x134  /* X  */
#define EVB_BTN_TL         0x136  /* L1 */
#define EVB_BTN_TR         0x137  /* R1 */
#define EVB_BTN_TL2        0x138  /* L2 */
#define EVB_BTN_TR2        0x139  /* R2 */
#define EVB_BTN_SELECT     0x13a
#define EVB_BTN_START      0x13b
#define EVB_BTN_THUMBL     0x13d  /* L3 */
#define EVB_BTN_THUMBR     0x13e  /* R3 */
#define EVB_BTN_DPAD_UP    0x220
#define EVB_BTN_DPAD_DOWN  0x221
#define EVB_BTN_DPAD_LEFT  0x222
#define EVB_BTN_DPAD_RIGHT 0x223

/* evdev abs (axis) codes */
#define EVA_ABS_X   0x00
#define EVA_ABS_Y   0x01
#define EVA_ABS_RX  0x03
#define EVA_ABS_RY  0x04

#define HID_FRAME_SZ 5

/* Abstract pad inputs the host knows about (SDL → this, then this → evdev). */
typedef enum {
    PAD_A = 0, PAD_B, PAD_X, PAD_Y,
    PAD_L1, PAD_R1, PAD_L2, PAD_R2,
    PAD_SELECT, PAD_START, PAD_L3, PAD_R3,
    PAD_DUP, PAD_DDOWN, PAD_DLEFT, PAD_DRIGHT,
    PAD_LX, PAD_LY, PAD_RX, PAD_RY,
    PAD_COUNT
} pad_input_t;

/* Map a pad input to its evdev (type, code). Returns 1 if mapped, 0 otherwise. */
static inline int pad_input_to_evdev(pad_input_t in, uint16_t *type, uint16_t *code) {
    switch (in) {
    case PAD_A:      *type = EV_KEY; *code = EVB_BTN_SOUTH;      return 1;
    case PAD_B:      *type = EV_KEY; *code = EVB_BTN_EAST;       return 1;
    case PAD_X:      *type = EV_KEY; *code = EVB_BTN_WEST;       return 1;
    case PAD_Y:      *type = EV_KEY; *code = EVB_BTN_NORTH;      return 1;
    case PAD_L1:     *type = EV_KEY; *code = EVB_BTN_TL;         return 1;
    case PAD_R1:     *type = EV_KEY; *code = EVB_BTN_TR;         return 1;
    case PAD_L2:     *type = EV_KEY; *code = EVB_BTN_TL2;        return 1;
    case PAD_R2:     *type = EV_KEY; *code = EVB_BTN_TR2;        return 1;
    case PAD_SELECT: *type = EV_KEY; *code = EVB_BTN_SELECT;     return 1;
    case PAD_START:  *type = EV_KEY; *code = EVB_BTN_START;      return 1;
    case PAD_L3:     *type = EV_KEY; *code = EVB_BTN_THUMBL;     return 1;
    case PAD_R3:     *type = EV_KEY; *code = EVB_BTN_THUMBR;     return 1;
    case PAD_DUP:    *type = EV_KEY; *code = EVB_BTN_DPAD_UP;    return 1;
    case PAD_DDOWN:  *type = EV_KEY; *code = EVB_BTN_DPAD_DOWN;  return 1;
    case PAD_DLEFT:  *type = EV_KEY; *code = EVB_BTN_DPAD_LEFT;  return 1;
    case PAD_DRIGHT: *type = EV_KEY; *code = EVB_BTN_DPAD_RIGHT; return 1;
    case PAD_LX:     *type = EV_ABS; *code = EVA_ABS_X;          return 1;
    case PAD_LY:     *type = EV_ABS; *code = EVA_ABS_Y;          return 1;
    case PAD_RX:     *type = EV_ABS; *code = EVA_ABS_RX;         return 1;
    case PAD_RY:     *type = EV_ABS; *code = EVA_ABS_RY;         return 1;
    default:                                                    return 0;
    }
}

/* Encode one evdev event into a 5-byte little-endian wire frame. */
static inline void hid_frame_encode(uint16_t type, uint16_t code, int16_t value,
                                    uint8_t out[HID_FRAME_SZ]) {
    out[0] = (uint8_t)type;
    out[1] = (uint8_t)(code & 0xFF);
    out[2] = (uint8_t)((code >> 8) & 0xFF);
    out[3] = (uint8_t)((uint16_t)value & 0xFF);
    out[4] = (uint8_t)(((uint16_t)value >> 8) & 0xFF);
}

#endif /* NORNS_HID_H */
