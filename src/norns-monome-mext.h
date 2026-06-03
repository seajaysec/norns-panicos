/*
 * norns-monome-mext.h — minimal monome "mext" serial protocol for a grid.
 *
 * Pure (no I/O): the parser + LED encoder are unit-tested on their own. We only
 * need the grid subset, and we never send queries — confirmed on the OXI One
 * mkII (it reports keys autonomously), so the device->host stream is just
 * 3-byte key frames and framing stays trivial.
 *
 * device -> host:  0x21 x y = key down,  0x20 x y = key up
 * host -> device:  0x18 x y level = set one LED's brightness (0-15, varibright)
 *                  0x12          = all LEDs off
 * (LED section verified live: 0x13 all-on / 0x17 intensity / 0x12 off lit the grid.)
 */
#ifndef NORNS_MONOME_MEXT_H
#define NORNS_MONOME_MEXT_H

#include <stdint.h>

#define MEXT_GRID_W       16
#define MEXT_GRID_H       8
#define MEXT_GRID_CELLS   (MEXT_GRID_W * MEXT_GRID_H)      /* 128 */
#define MEXT_LED_DIFF_MAX (MEXT_GRID_CELLS * 4)            /* worst-case diff bytes */

#define MEXT_CMD_LEVEL_SET 0x18
#define MEXT_CMD_ALL_OFF   0x12

/* A decoded grid key. state: 1 = down, 0 = up. */
typedef struct { uint8_t x, y, state; } mext_key_t;

/*
 * Incremental key-frame parser. Grid key messages are 3 bytes (0x21/0x20 x y).
 * Coordinates are always < 0x20 (grid is 16x8), so a status byte can never be
 * mistaken for a coordinate — once aligned we stay aligned, and a stray byte
 * before a frame is simply skipped (resync).
 */
typedef struct { uint8_t b[3]; int len; } mext_parser_t;

static inline void mext_parser_init(mext_parser_t *p) { p->len = 0; }

/* Feed one byte; returns 1 and fills *out when a key frame completes. */
static inline int mext_parser_push(mext_parser_t *p, uint8_t byte, mext_key_t *out) {
    if (p->len == 0) {
        if (byte == 0x21 || byte == 0x20) { p->b[0] = byte; p->len = 1; }
        return 0;                         /* stray byte: skip to resync */
    }
    p->b[p->len++] = byte;
    if (p->len < 3) return 0;
    p->len = 0;
    out->state = (p->b[0] == 0x21) ? 1 : 0;
    out->x = p->b[1];
    out->y = p->b[2];
    return 1;
}

/* True if a decoded key lies within the 16x8 grid. */
static inline int mext_key_in_bounds(const mext_key_t *k) {
    return k->x < MEXT_GRID_W && k->y < MEXT_GRID_H;
}

/*
 * Diff `next` against `last` (both 16x8 row-major level buffers, 0-15) and emit
 * one LED-level-set command (0x18 x y level) per changed cell into `out`
 * (caller supplies >= MEXT_LED_DIFF_MAX bytes). Updates `last`; returns the
 * number of bytes written. Sparse UIs cost only a few commands per refresh.
 */
static inline int mext_led_diff(const uint8_t *next, uint8_t *last, uint8_t *out) {
    int n = 0;
    for (int y = 0; y < MEXT_GRID_H; y++)
        for (int x = 0; x < MEXT_GRID_W; x++) {
            uint8_t lv = (uint8_t)(next[y * MEXT_GRID_W + x] & 0x0f);
            if (lv != last[y * MEXT_GRID_W + x]) {
                out[n++] = MEXT_CMD_LEVEL_SET;
                out[n++] = (uint8_t)x;
                out[n++] = (uint8_t)y;
                out[n++] = lv;
                last[y * MEXT_GRID_W + x] = lv;
            }
        }
    return n;
}

#endif /* NORNS_MONOME_MEXT_H */
