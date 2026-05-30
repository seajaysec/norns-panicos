/*
 * norns-push2-display — Renders norns 128×64 display on Ableton Push 2 at 30fps
 *
 * Screen source: <screen_fifo>  (4096 bytes, 4-bit packed greyscale, hi-nybble=left px)
 * Display target: USB bulk to Push 2 (VID=0x2982 PID=0x1967)
 *                 960×160 BGR565, XOR-masked per spec §Display
 *
 * Keeps the display alive by resending the last known frame even when norns
 * is not redrawing (Push 2 blanks after 2s without a frame).
 *
 * Usage: norns-push2-display <screen_fifo>
 */
#include <errno.h>
#include <fcntl.h>
#include <libusb-1.0/libusb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PUSH2_VID          0x2982
#define PUSH2_PID          0x1967
#define PUSH2_BULK_EP      0x01
#define PUSH2_IFACE        0
#define PUSH2_W            960
#define PUSH2_H            160
#define PUSH2_LINE_SZ      2048    /* 1920 pixel bytes + 128 filler (spec §Display) */
#define PUSH2_RETRY_FRAMES 150     /* ~5s at 30fps before retrying USB open */

#define NORNS_W            128
#define NORNS_H            64
#define SCREEN_FRAME_SZ    (NORNS_W * NORNS_H / 2)   /* 4096 bytes */

static const uint8_t FRAME_HEADER[16] = {
    0xFF, 0xCC, 0xAA, 0x88,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00
};

/* Repeating XOR mask applied to every line buffer byte before transmission.
 * Pattern: 0xE7 0xF3 0xE7 0xFF (spec §Display "signal shaping pattern 0xFFE7F3E7") */
static const uint8_t XOR_MASK[4] = {0xE7, 0xF3, 0xE7, 0xFF};

static volatile int g_running = 1;
static void handle_sig(int s) { (void)s; g_running = 0; }

/* 8-bit grey → 16-bit BGR565 (Push 2 format: b[4:0] g[5:0] r[4:0], LE) */
static inline uint16_t gray_to_bgr565(uint8_t g) {
    return (uint16_t)(((uint16_t)(g >> 3) << 11) |
                      ((uint16_t)(g >> 2) <<  5) |
                       (uint16_t)(g >> 3));
}

/* Extract 4-bit pixel from packed norns frame (hi-nybble = left / even x) */
static inline uint8_t norns_px(const uint8_t *fr, int x, int y) {
    uint8_t b = fr[y * (NORNS_W / 2) + x / 2];
    return (x & 1) ? (b & 0xF) : (b >> 4);
}

static libusb_device_handle *push2_open(libusb_context *ctx) {
    libusb_device_handle *dev =
        libusb_open_device_with_vid_pid(ctx, PUSH2_VID, PUSH2_PID);
    if (!dev) return NULL;
    libusb_set_auto_detach_kernel_driver(dev, 1);
    if (libusb_claim_interface(dev, PUSH2_IFACE) != 0) {
        libusb_close(dev);
        return NULL;
    }
    fprintf(stderr, "norns-push2-display: Push 2 display opened\n");
    return dev;
}

static void push2_close(libusb_device_handle *dev) {
    libusb_release_interface(dev, PUSH2_IFACE);
    libusb_close(dev);
    fprintf(stderr, "norns-push2-display: Push 2 display closed\n");
}

/* Scale norns 128×64 4-bit greyscale → 960×160 BGR565 and send one display frame. */
static int push2_send_frame(libusb_device_handle *dev, const uint8_t *norns_frame) {
    static uint8_t line[PUSH2_LINE_SZ];
    int xfr, rc;

    rc = libusb_bulk_transfer(dev, PUSH2_BULK_EP,
                              (uint8_t *)FRAME_HEADER, 16, &xfr, 100);
    if (rc) return rc;

    for (int py = 0; py < PUSH2_H; py++) {
        /* Nearest-neighbour row mapping: full-width stretch */
        int ny = (py * NORNS_H) / PUSH2_H;

        for (int px = 0; px < PUSH2_W; px++) {
            int nx = (px * NORNS_W) / PUSH2_W;
            uint8_t gray = (uint8_t)(norns_px(norns_frame, nx, ny) * 17);
            uint16_t pix = gray_to_bgr565(gray);
            line[px * 2]     = (uint8_t)(pix & 0xFF);
            line[px * 2 + 1] = (uint8_t)(pix >> 8);
        }

        /* Filler bytes: zero before XOR, XOR_MASK produces non-zero — spec compliant */
        memset(line + PUSH2_W * 2, 0, PUSH2_LINE_SZ - PUSH2_W * 2);

        for (int i = 0; i < PUSH2_LINE_SZ; i++)
            line[i] ^= XOR_MASK[i & 3];

        rc = libusb_bulk_transfer(dev, PUSH2_BULK_EP, line,
                                  PUSH2_LINE_SZ, &xfr, 200);
        if (rc) return rc;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <screen_fifo>\n", argv[0]);
        return 1;
    }

    signal(SIGINT,  handle_sig);
    signal(SIGTERM, handle_sig);
    signal(SIGPIPE, SIG_IGN);

    /* Wait up to 60s for norns-panicos to create the screen FIFO */
    int fifo_fd = -1;
    for (int tries = 0; fifo_fd < 0 && tries < 60 && g_running; tries++) {
        fifo_fd = open(argv[1], O_RDONLY | O_NONBLOCK);
        if (fifo_fd < 0) {
            struct timespec t = {1, 0};
            nanosleep(&t, NULL);
        }
    }
    if (fifo_fd < 0) {
        fprintf(stderr, "norns-push2-display: cannot open %s: %s\n",
                argv[1], strerror(errno));
        return 1;
    }

    libusb_context *ctx = NULL;
    libusb_init(&ctx);

    uint8_t cur[SCREEN_FRAME_SZ];
    memset(cur, 0, sizeof(cur));
    int has_frame = 0;

    libusb_device_handle *dev = NULL;
    int retry_cd = 0;  /* cooldown counter before next USB open attempt */

    /* 30fps: 33.3ms per frame */
    struct timespec frame_interval = {0, 33333333};

    while (g_running) {
        /* Re-open Push 2 after cooldown (reduces USB enumeration overhead) */
        if (!dev) {
            if (retry_cd > 0) {
                retry_cd--;
                nanosleep(&frame_interval, NULL);
                continue;
            }
            dev = push2_open(ctx);
            if (!dev) {
                retry_cd = PUSH2_RETRY_FRAMES;
                nanosleep(&frame_interval, NULL);
                continue;
            }
        }

        /* Drain screen FIFO, keep newest frame */
        {
            uint8_t buf[SCREEN_FRAME_SZ];
            ssize_t n;
            while ((n = read(fifo_fd, buf, SCREEN_FRAME_SZ)) ==
                   (ssize_t)SCREEN_FRAME_SZ) {
                memcpy(cur, buf, SCREEN_FRAME_SZ);
                has_frame = 1;
            }
        }

        /* Send to Push 2 — even if screen unchanged to keep display alive */
        if (has_frame) {
            if (push2_send_frame(dev, cur) != 0) {
                push2_close(dev);
                dev = NULL;
                retry_cd = PUSH2_RETRY_FRAMES;
            }
        }

        nanosleep(&frame_interval, NULL);
    }

    if (dev) push2_close(dev);
    libusb_exit(ctx);
    close(fifo_fd);
    return 0;
}
