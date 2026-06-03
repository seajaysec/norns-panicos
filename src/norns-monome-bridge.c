/*
 * norns-monome-bridge.c — bridge a real monome-protocol grid (serial / CDC-ACM,
 * e.g. OXI One mkII in grid mode → /dev/ttyACM0) into norns over its mext
 * serial protocol. No libmonome / libusb / usb-serial kernel driver needed.
 *
 * Reuses matron's existing FIFO "virtual grid" (built for the Push 2), so NO
 * matron rebuild is required:
 *   - grid key  → 4-byte type-3 frame [3, x, y, state] → NORNS_INPUT_FIFO
 *   - LED state ← NORNS_GRID_FIFO (16x8 levels 0-15) → mext 0x18 level-set cmds
 *
 * The grid FIFO must have exactly ONE reader, so on the handheld variant this
 * runs INSTEAD of the Push 2 norns-input-bridge (host gates on $NORNS_USE_MONOME).
 *
 * Usage: norns-monome-bridge [tty-device]
 *   tty-device  optional; else $NORNS_MONOME_DEV, else auto-detect the monome
 *               (USB VID 0x28e9) among /dev/ttyACM*, else the first /dev/ttyACM*.
 *   FIFOs from $NORNS_INPUT_FIFO / $NORNS_GRID_FIFO (sensible defaults).
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "norns-monome-mext.h"

#define DEFAULT_INPUT_FIFO "/tmp/norns-input-1"
#define DEFAULT_GRID_FIFO  "/tmp/norns-grid-1"
#define MONOME_USB_VID     "28e9"

static volatile sig_atomic_t running = 1;
static void on_signal(int sig) { (void)sig; running = 0; }

/* Read the USB idVendor backing /dev/ttyACM<idx>, lowercased, into vid. */
static int acm_vendor(int idx, char *vid, size_t n) {
    char path[96];
    snprintf(path, sizeof(path), "/sys/class/tty/ttyACM%d/device/../idVendor", idx);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t r = read(fd, vid, n - 1);
    close(fd);
    if (r <= 0) return -1;
    vid[r] = '\0';
    char *nl = strchr(vid, '\n');
    if (nl) *nl = '\0';
    return 0;
}

/* Pick the grid tty: prefer the monome VID; else the first present ttyACM. */
static int find_monome_tty(char *out, size_t n) {
    int first = -1;
    for (int i = 0; i < 8; i++) {
        char dev[32];
        snprintf(dev, sizeof(dev), "/dev/ttyACM%d", i);
        if (access(dev, F_OK) != 0) continue;
        if (first < 0) first = i;
        char vid[16];
        if (acm_vendor(i, vid, sizeof(vid)) == 0 && strcasecmp(vid, MONOME_USB_VID) == 0) {
            snprintf(out, n, "%s", dev);
            return 0;
        }
    }
    if (first >= 0) { snprintf(out, n, "/dev/ttyACM%d", first); return 0; }
    return -1;
}

/* Open a serial tty in raw mode at 115200 (baud is moot for CDC-ACM). */
static int open_tty_raw(const char *dev) {
    int fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return -1;
    struct termios t;
    if (tcgetattr(fd, &t) != 0) { close(fd); return -1; }
    cfmakeraw(&t);
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    t.c_cflag |= (CLOCAL | CREAD);
    t.c_cflag &= ~HUPCL;   /* don't drop DTR on close — avoids device auto-reset */
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(fd, TCSANOW, &t);
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int write_all(int fd, const uint8_t *buf, int len) {
    int off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, (size_t)(len - off));
        if (w > 0) { off += (int)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    const char *fixed_dev = (argc > 1) ? argv[1] : getenv("NORNS_MONOME_DEV");
    const char *input_path = getenv("NORNS_INPUT_FIFO");
    const char *grid_path  = getenv("NORNS_GRID_FIFO");
    if (!input_path || !*input_path) input_path = DEFAULT_INPUT_FIFO;
    if (!grid_path  || !*grid_path)  grid_path  = DEFAULT_GRID_FIFO;

    /* O_RDWR so opening never blocks waiting for a peer; 4-byte writes are
     * atomic (< PIPE_BUF), so sharing the input FIFO with other writers is safe. */
    int input_fd = open(input_path, O_RDWR | O_NONBLOCK);
    int grid_fd  = open(grid_path,  O_RDONLY | O_NONBLOCK);

    fprintf(stderr, "norns-monome-bridge: input=%s grid=%s device=%s\n",
            input_path, grid_path, fixed_dev ? fixed_dev : "(auto)");

    int tty_fd = -1;
    mext_parser_t parser;
    mext_parser_init(&parser);
    uint8_t led_last[MEXT_GRID_CELLS];
    uint8_t gridbuf[MEXT_GRID_CELLS] = {0};

    while (running) {
        if (input_fd < 0) input_fd = open(input_path, O_RDWR | O_NONBLOCK);
        if (grid_fd  < 0) grid_fd  = open(grid_path,  O_RDONLY | O_NONBLOCK);

        if (tty_fd < 0) {
            char dev[64];
            const char *target = fixed_dev;
            if (!target) { if (find_monome_tty(dev, sizeof(dev)) == 0) target = dev; }
            if (target) tty_fd = open_tty_raw(target);
            if (tty_fd < 0) { usleep(500000); continue; }   /* no grid yet — retry */
            uint8_t off = MEXT_CMD_ALL_OFF;
            write_all(tty_fd, &off, 1);
            memset(led_last, 0, sizeof(led_last));
            mext_parser_init(&parser);
            fprintf(stderr, "norns-monome-bridge: grid ready on %s\n", target);
        }

        int maxfd = tty_fd;
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(tty_fd, &rfds);
        if (grid_fd >= 0) { FD_SET(grid_fd, &rfds); if (grid_fd > maxfd) maxfd = grid_fd; }
        struct timeval tv = { 0, 16000 };   /* 16ms → ~60Hz LED ceiling */
        int rc = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) { if (errno == EINTR) continue; usleep(10000); continue; }

        /* Grid keys → norns input FIFO. */
        if (rc > 0 && FD_ISSET(tty_fd, &rfds)) {
            uint8_t rb[256];
            ssize_t r = read(tty_fd, rb, sizeof(rb));
            if (r == 0 || (r < 0 && errno != EAGAIN && errno != EINTR)) {
                fprintf(stderr, "norns-monome-bridge: grid disconnected, reopening\n");
                close(tty_fd); tty_fd = -1;
                continue;
            }
            for (ssize_t i = 0; i < r; i++) {
                mext_key_t k;
                if (mext_parser_push(&parser, rb[i], &k) && mext_key_in_bounds(&k)
                        && input_fd >= 0) {
                    uint8_t frame[4] = { 3, k.x, k.y, k.state };
                    (void)write(input_fd, frame, sizeof(frame));  /* atomic, may drop */
                }
            }
        }

        /* norns LED buffer → grid (drain to newest frame, then diff). */
        if (grid_fd >= 0) {
            uint8_t tmp[MEXT_GRID_CELLS];
            int got = 0;
            while (read(grid_fd, tmp, sizeof(tmp)) == (ssize_t)sizeof(tmp)) {
                memcpy(gridbuf, tmp, sizeof(tmp));
                got = 1;
            }
            if (got) {
                uint8_t cmds[MEXT_LED_DIFF_MAX];
                int n = mext_led_diff(gridbuf, led_last, cmds);
                if (n > 0 && write_all(tty_fd, cmds, n) < 0) {
                    fprintf(stderr, "norns-monome-bridge: LED write failed, reopening\n");
                    close(tty_fd); tty_fd = -1;
                }
            }
        }
    }

    if (tty_fd >= 0) {
        uint8_t off = MEXT_CMD_ALL_OFF;
        write_all(tty_fd, &off, 1);
        close(tty_fd);
    }
    if (input_fd >= 0) close(input_fd);
    if (grid_fd >= 0) close(grid_fd);
    fprintf(stderr, "norns-monome-bridge: exiting\n");
    return 0;
}
