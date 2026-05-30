/*
 * norns-input-bridge — Translates Push 2 / MIDI into Norns encoder/key/grid events
 *
 * Push 2 path (primary on PanicOS): the PipeWire "Midi-Bridge" silently drops
 * hardware MIDI before it reaches JACK clients, so we talk to the Push 2 over
 * libusb directly — claiming USB MIDI interface 2 (EP 0x82 in, 0x02 out), the
 * same way norns-push2-display drives the display on interface 0.
 *   - input:  EP 0x82 USB-MIDI packets → process_midi_msg() → input FIFO
 *   - LED:    /tmp/norns-grid-1 (128B, 16x8, level 0-15) → pad note-ons on EP 0x02
 *
 * JACK MIDI path (fallback for any device PipeWire does forward) is kept.
 *
 * Writes: /tmp/norns-input-<slot>    (4-byte frames: [type][id][val_lo][val_hi])
 *
 * Usage: norns-input-bridge <input_fifo> [midi_fifo]
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <jack/jack.h>
#include <jack/midiport.h>
#include <libusb-1.0/libusb.h>

static volatile int running = 1;
static jack_client_t *client = NULL;
static jack_port_t *midi_port = NULL;
static int input_fd = -1;

/* Push 2 state */
static volatile int push2_connected = 0;
static int push2_grid_mode = 1;  /* 1=Grid (pads→monome), 0=MIDI (pads→type-2) */
static int viewport_x = 0;       /* current grid viewport offset (multiples of 8) */
static int viewport_y = 0;

/* ── Push 2 USB MIDI ─────────────────────────────────────────────────────── */
#define PUSH2_VID         0x2982
#define PUSH2_PID         0x1967
#define PUSH2_MIDI_IFACE  2
#define PUSH2_EP_IN       0x82
#define PUSH2_EP_OUT      0x02
#define GRID_W            16     /* emulated monome grid is 16x8 */
#define GRID_H            8
#define GRID_FIFO         "/tmp/norns-grid-1"

static libusb_context       *g_usb    = NULL;
static libusb_device_handle *g_push2  = NULL;
static pthread_mutex_t       g_usb_lock = PTHREAD_MUTEX_INITIALIZER;
static int                   g_grid_fd = -1;
static uint8_t               g_last_pad[64];   /* last colour sent per pad (idx = rfb*8+col) */
static int                   g_dbg = 0;        /* set if /tmp/push2dbg exists at startup */

/* Display scaling mode — knob 8 cycles it; norns-push2-display reads the file. */
#define SCALE_FILE   "/tmp/norns-push2-scale"
#define NUM_SCALE_MODES 3
static int g_scale_fd   = -1;
static int g_scale_mode = 0;
static int g_scale_accum = 0;

static void write_scale_mode(void) {
    if (g_scale_fd < 0) return;
    uint8_t b = (uint8_t)g_scale_mode;
    pwrite(g_scale_fd, &b, 1, 0);
    if (g_dbg) fprintf(stderr, "push2: scale mode → %d\n", g_scale_mode);
}

/* Put Push 2 into User mode so we own the pad/button LEDs (spec §SysEx). */
static const uint8_t USER_MODE_SYSEX[] =
    {0xF0,0x00,0x21,0x1D,0x01,0x01,0x0A,0x01,0xF7};

static void handle_signal(int sig) { (void)sig; running = 0; }

/* Scan JACK output ports for Push 2 and connect to our input (fallback path) */
static void try_connect_push2(void) {
    if (!client) return;
    const char **ports = jack_get_ports(client, NULL,
                                        JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput);
    if (!ports) return;
    for (int i = 0; ports[i]; i++) {
        if (strstr(ports[i], "Push 2") || strstr(ports[i], "Push2") ||
                strstr(ports[i], "push2")) {
            int rc = jack_connect(client, ports[i], "norns-input:midi_in");
            if (rc == 0 || rc == EEXIST)
                fprintf(stderr, "norns-input-bridge: Push 2 JACK MIDI connected: %s\n",
                        ports[i]);
        }
    }
    jack_free(ports);
}

/* Called by JACK when a port is registered — auto-connect Push 2 on hotplug */
static void on_port_registered(jack_port_id_t id, int registered, void *arg) {
    (void)arg;
    if (!registered) return;
    jack_port_t *p = jack_port_by_id(client, id);
    if (!p) return;
    const char *name = jack_port_name(p);
    if (strstr(name, "Push 2") || strstr(name, "Push2") || strstr(name, "push2"))
        try_connect_push2();
}

/* Write a 4-byte input event to the norns input FIFO */
static void send_input(uint8_t type, uint8_t id, int16_t value) {
    uint8_t frame[4];
    frame[0] = type;
    frame[1] = id;
    frame[2] = (uint8_t)(value & 0xFF);
    frame[3] = (uint8_t)((value >> 8) & 0xFF);
    write(input_fd, frame, 4);  /* non-blocking, drop if full */
}

static void process_midi_msg(const uint8_t *msg, size_t len) {
    if (len < 1) return;

    /* ── CC messages (0xB0) ─────────────────────────────────────────────── */
    if (len >= 3 && (msg[0] & 0xF0) == 0xB0) {
        uint8_t cc  = msg[1];
        uint8_t val = msg[2];

        /* CC 71-73 → encoder delta E1-E3 (Move + Push 2 encoders 1-3).
         * Both use relative two's-complement: 1-63=CW, 65-127=CCW. */
        if (cc >= 71 && cc <= 73) {
            uint8_t enc_id = cc - 71;
            int delta = 0;
            if (val >= 1 && val <= 63)  delta =  (int)val;
            else if (val >= 65)         delta =  (int)val - 128;
            if (delta > 3)  delta =  3;
            if (delta < -3) delta = -3;
            if (delta != 0) send_input(0, enc_id, (int16_t)delta);
        }

        /* Push 2 encoder 8 (CC 78) → cycle display scaling mode (accumulate
         * detents so one notch ≈ one mode step). */
        if (cc == 78) {
            int d = 0;
            if (val >= 1 && val <= 63)  d =  (int)val;
            else if (val >= 65)         d =  (int)val - 128;
            g_scale_accum += d;
            if (g_scale_accum >= 8) {
                g_scale_accum = 0;
                g_scale_mode = (g_scale_mode + 1) % NUM_SCALE_MODES;
                write_scale_mode();
            } else if (g_scale_accum <= -8) {
                g_scale_accum = 0;
                g_scale_mode = (g_scale_mode + NUM_SCALE_MODES - 1) % NUM_SCALE_MODES;
                write_scale_mode();
            }
        }

        /* CC 43/42/41 → key K1/K2/K3 (Move track buttons) */
        if (cc >= 41 && cc <= 43) {
            uint8_t key_id = 43 - cc;
            send_input(1, key_id, val > 0 ? 1 : 0);
        }

        if (push2_connected) {
            /* Push 2 Shift (CC 49, press only) → toggle Grid/MIDI pad mode */
            if (cc == 49 && val > 0) {
                push2_grid_mode = !push2_grid_mode;
                fprintf(stderr, "norns-input-bridge: Push 2 mode → %s\n",
                        push2_grid_mode ? "Grid" : "MIDI");
            }

            /* norns three keys, mirrored on both display button rows:
             *   lower (under screen):    CC 20 → K2, CC 21 → K3, CC 22 → K1
             *   upper (under encoders):  CC 102→ K2, CC 103→ K3, CC 104→ K1 */
            if      (cc == 20 || cc == 102) send_input(1, 1, val > 0 ? 1 : 0);  /* K2 */
            else if (cc == 21 || cc == 103) send_input(1, 2, val > 0 ? 1 : 0);  /* K3 */
            else if (cc == 22 || cc == 104) send_input(1, 0, val > 0 ? 1 : 0);  /* K1 */

            /* Push 2 arrow buttons → monome grid viewport (8-key page on X).
             * Grid is 16 wide, so X offset is only 0 or 8; no Y paging. */
            if (val > 0) {
                if      (cc == 44) viewport_x = (viewport_x >= 8) ? viewport_x - 8 : 0;
                else if (cc == 45) viewport_x = (viewport_x < 8) ? viewport_x + 8 : 8;
                viewport_y = 0;

                if (cc >= 44 && cc <= 47)
                    fprintf(stderr, "norns-input-bridge: Push 2 viewport x=%d\n",
                            viewport_x);
            }
        }
    }

    /* ── Note on / off ──────────────────────────────────────────────────── */
    if (len >= 3 && ((msg[0] & 0xF0) == 0x90 || (msg[0] & 0xF0) == 0x80)) {
        uint8_t note = msg[1];
        uint8_t vel  = msg[2];
        int press = ((msg[0] & 0xF0) == 0x90 && vel > 0);

        /* Push 2 pads (notes 36-99) in Grid mode → type 3 grid key frames.
         * Physical layout: bottom-left pad = note 36, top-right = note 99.
         * Norns grid: (0,0)=top-left → invert rows so top row of Push 2 = y=0. */
        if (push2_connected && push2_grid_mode && note >= 36 && note <= 99) {
            int row_from_bottom = (note - 36) / 8;
            int col             = (note - 36) % 8;
            int gx = col + viewport_x;
            int gy = (7 - row_from_bottom) + viewport_y;
            if (gx <= 127 && gy <= 127) {
                uint8_t frame[4] = {3, (uint8_t)gx, (uint8_t)gy, press ? 1 : 0};
                write(input_fd, frame, 4);
            }
            return;  /* consumed as grid event; no MIDI pass-through */
        }

        /* All other notes (including Push 2 pads in MIDI mode) → type 2 */
        uint8_t frame[4] = {2, msg[0], msg[1], msg[2]};
        write(input_fd, frame, 4);
    }

    /* ── Grid key marker (0xF9 x y state) from DSP plugin ──────────────── */
    if (len >= 4 && msg[0] == 0xF9) {
        uint8_t frame[4] = {3, msg[1], msg[2], msg[3]};
        write(input_fd, frame, 4);
    }
}

/* ── Push 2 USB MIDI helpers ─────────────────────────────────────────────── */

/* Send raw MIDI bytes to the Push 2 as USB-MIDI event packets (EP 0x02) on the
 * given virtual cable. In User mode the controls/LEDs live on the User port
 * (cable 1); the mode-set SysEx goes on cable 0.
 * Caller must hold g_usb_lock and pass a live handle. */
static void push2_usb_send(libusb_device_handle *h, uint8_t cable,
                           const uint8_t *msg, int len) {
    uint8_t pkt[4];
    int xfr;
    uint8_t cn = (uint8_t)(cable << 4);
    /* Channel-voice message (status 0x80-0xEF): one packet, CIN = status nibble */
    if (len == 3 && msg[0] >= 0x80 && msg[0] < 0xF0) {
        pkt[0] = cn | ((msg[0] >> 4) & 0x0F);
        pkt[1] = msg[0]; pkt[2] = msg[1]; pkt[3] = msg[2];
        libusb_bulk_transfer(h, PUSH2_EP_OUT, pkt, 4, &xfr, 50);
        return;
    }
    /* SysEx / generic: 3-byte groups (CIN 4), final group CIN 5/6/7 by length */
    int i = 0;
    while (i < len) {
        int rem = len - i;
        if (rem > 3) {
            pkt[0] = cn | 0x04;
            pkt[1] = msg[i]; pkt[2] = msg[i+1]; pkt[3] = msg[i+2];
            i += 3;
        } else {
            pkt[0] = cn | ((rem == 1) ? 0x05 : (rem == 2) ? 0x06 : 0x07);
            pkt[1] = msg[i];
            pkt[2] = (rem > 1) ? msg[i+1] : 0;
            pkt[3] = (rem > 2) ? msg[i+2] : 0;
            i += rem;
        }
        libusb_bulk_transfer(h, PUSH2_EP_OUT, pkt, 4, &xfr, 50);
    }
}

#define PUSH2_CABLE_LIVE  0   /* Live port */
#define PUSH2_CABLE_USER  1   /* User port — controls + LEDs in User mode */

/* Set one RGB palette entry (spec §Set LED Color Palette Entry). Each 8-bit
 * component is split into 7 low bits + 1 high bit. */
static void push2_palette_entry(libusb_device_handle *h, uint8_t idx,
                                uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    uint8_t s[17] = {
        0xF0,0x00,0x21,0x1D,0x01,0x01,0x03, idx,
        (uint8_t)(r & 0x7F), (uint8_t)((r >> 7) & 1),
        (uint8_t)(g & 0x7F), (uint8_t)((g >> 7) & 1),
        (uint8_t)(b & 0x7F), (uint8_t)((b >> 7) & 1),
        (uint8_t)(w & 0x7F), (uint8_t)((w >> 7) & 1),
        0xF7
    };
    push2_usb_send(h, PUSH2_CABLE_LIVE, s, sizeof(s));
}

/* Two colour ramps so each grid half navigates distinctly. The Push 2 hard
 * current-limits pads on USB bus power, so the ramp floor is high (~210/255). */
static const uint8_t COLOUR_A[3] = {  0, 200, 255};   /* cyan  — left half  (viewport_x=0) */
static const uint8_t COLOUR_B[3] = {255, 100,   0};   /* amber — right half (viewport_x=8) */
#define PAL_A_BASE  0     /* indices  1..15 = COLOUR_A ramp */
#define PAL_B_BASE  16    /* indices 17..31 = COLOUR_B ramp */
#define LED_FLOOR   235   /* brightness of norns level 1 (0-255); level 15 = full */

static void push2_program_ramp(libusb_device_handle *h, int base, const uint8_t *c) {
    for (int i = 1; i <= 15; i++) {
        int sc = LED_FLOOR + (i - 1) * (255 - LED_FLOOR) / 14;   /* LED_FLOOR..255 */
        push2_palette_entry(h, (uint8_t)(base + i),
                            (uint8_t)(c[0] * sc / 255),
                            (uint8_t)(c[1] * sc / 255),
                            (uint8_t)(c[2] * sc / 255), 0);
    }
}

/* Reprogram the palette: index 0 off, two bright colour ramps for the halves. */
static void push2_program_palette(libusb_device_handle *h) {
    static const uint8_t REAPPLY[] = {0xF0,0x00,0x21,0x1D,0x01,0x01,0x05,0xF7};
    push2_palette_entry(h, 0, 0, 0, 0, 0);
    push2_program_ramp(h, PAL_A_BASE, COLOUR_A);
    push2_program_ramp(h, PAL_B_BASE, COLOUR_B);
    push2_usb_send(h, PUSH2_CABLE_LIVE, REAPPLY, sizeof(REAPPLY));
}

static void push2_usb_close(void) {
    pthread_mutex_lock(&g_usb_lock);
    if (g_push2) {
        libusb_release_interface(g_push2, PUSH2_MIDI_IFACE);
        libusb_close(g_push2);
        g_push2 = NULL;
        push2_connected = 0;
        fprintf(stderr, "norns-input-bridge: Push 2 USB MIDI closed\n");
    }
    pthread_mutex_unlock(&g_usb_lock);
}

static void push2_usb_try_open(void) {
    pthread_mutex_lock(&g_usb_lock);
    if (g_push2) { pthread_mutex_unlock(&g_usb_lock); goto done; }
    libusb_device_handle *h =
        libusb_open_device_with_vid_pid(g_usb, PUSH2_VID, PUSH2_PID);
    if (h) {
        libusb_set_auto_detach_kernel_driver(h, 1);
        if (libusb_claim_interface(h, PUSH2_MIDI_IFACE) == 0) {
            g_push2 = h;
            push2_connected = 1;
            push2_usb_send(h, PUSH2_CABLE_LIVE, USER_MODE_SYSEX, sizeof(USER_MODE_SYSEX));
            push2_program_palette(h);
            memset(g_last_pad, 0xFF, sizeof(g_last_pad));  /* force full repaint */
            fprintf(stderr, "norns-input-bridge: Push 2 USB MIDI claimed (User mode)\n");
        } else {
            libusb_close(h);
        }
    }
    pthread_mutex_unlock(&g_usb_lock);
done:
    return;
}

/* MIDI message length for a USB-MIDI Code Index Number */
static int usbmidi_cin_len(uint8_t cin) {
    switch (cin) {
        case 0x5: case 0xF:                      return 1;
        case 0x2: case 0x6: case 0xC: case 0xD:  return 2;
        default:                                 return 3;
    }
}

/* Background thread: read Push 2 input on EP 0x82, feed process_midi_msg(). */
static void *push2_reader(void *arg) {
    (void)arg;
    uint8_t buf[64];
    while (running) {
        pthread_mutex_lock(&g_usb_lock);
        libusb_device_handle *h = g_push2;
        pthread_mutex_unlock(&g_usb_lock);
        if (!h) {
            push2_usb_try_open();
            if (!g_push2) { usleep(500000); }
            continue;
        }
        int xfr = 0;
        int rc = libusb_bulk_transfer(h, PUSH2_EP_IN, buf, sizeof(buf), &xfr, 500);
        if (rc == LIBUSB_ERROR_TIMEOUT) continue;
        if (rc < 0) { push2_usb_close(); usleep(200000); continue; }
        for (int i = 0; i + 4 <= xfr; i += 4) {
            uint8_t cin = buf[i] & 0x0F;
            int mlen = usbmidi_cin_len(cin);
            if (g_dbg && buf[i+1] != 0xFE)   /* skip active-sensing spam */
                fprintf(stderr, "push2 IN: cable=%d %02X %02X %02X (grid_mode=%d)\n",
                        (buf[i] >> 4) & 0x0F, buf[i+1], buf[i+2], buf[i+3], push2_grid_mode);
            process_midi_msg(&buf[i+1], (size_t)mlen);
        }
    }
    return NULL;
}

/* norns grid level (0-15) + ramp base → Push 2 palette index (0 = off). */
static uint8_t led_colour(uint8_t level, int base) {
    if (level == 0) return 0;
    if (level > 15) level = 15;
    return (uint8_t)(base + level);
}

/* Read latest grid LED frame and paint the changed pads (rate-limited ~30Hz). */
static void push2_led_pump(void) {
    static int tick = 0;
    if (++tick < 33) return;      /* main loop runs at ~1ms → ~30Hz */
    tick = 0;

    if (g_grid_fd < 0) {
        g_grid_fd = open(GRID_FIFO, O_RDONLY | O_NONBLOCK);
        if (g_grid_fd < 0) return;
    }

    static uint8_t grid[GRID_W * GRID_H];
    static int have_grid = 0;
    uint8_t tmp[GRID_W * GRID_H];
    int new_frames = 0;
    while (read(g_grid_fd, tmp, sizeof(tmp)) == (ssize_t)sizeof(tmp)) {
        memcpy(grid, tmp, sizeof(tmp));
        have_grid = 1;
        new_frames++;
    }
    if (g_dbg && new_frames) {
        int nz = 0; for (int k = 0; k < GRID_W * GRID_H; k++) if (grid[k]) nz++;
        fprintf(stderr, "push2 GRID: %d frame(s), %d lit cells\n", new_frames, nz);
    }
    if (!have_grid) return;

    int vx = viewport_x; if (vx > GRID_W - 8) vx = GRID_W - 8; if (vx < 0) vx = 0;
    int base = (vx < 8) ? PAL_A_BASE : PAL_B_BASE;   /* colour by grid half */

    pthread_mutex_lock(&g_usb_lock);
    libusb_device_handle *h = g_push2;
    int painted = 0;
    if (h) {
        for (int ny = 0; ny < 8; ny++) {
            for (int nx = 0; nx < 8; nx++) {
                int gx = nx + vx;
                uint8_t col = led_colour(grid[ny * GRID_W + gx], base);
                int rfb = 7 - ny;                 /* Push 2 row from bottom */
                int idx = rfb * 8 + nx;
                if (g_last_pad[idx] != col) {
                    g_last_pad[idx] = col;
                    uint8_t note = (uint8_t)(36 + rfb * 8 + nx);
                    uint8_t m[3] = {0x90, note, col};
                    push2_usb_send(h, PUSH2_CABLE_USER, m, 3);
                    painted++;
                }
            }
        }
    }
    pthread_mutex_unlock(&g_usb_lock);
    if (g_dbg && painted)
        fprintf(stderr, "push2 LED: painted %d pads (have_handle=%d)\n", painted, h ? 1 : 0);
}

static int jack_process(jack_nframes_t nframes, void *arg) {
    (void)arg;
    void *buf = jack_port_get_buffer(midi_port, nframes);
    if (!buf) return 0;

    jack_nframes_t count = jack_midi_get_event_count(buf);
    for (jack_nframes_t i = 0; i < count; i++) {
        jack_midi_event_t ev;
        if (jack_midi_event_get(&ev, buf, i) == 0)
            process_midi_msg(ev.buffer, ev.size);
    }
    return 0;
}

/* Also read from MIDI input FIFO (secondary path for on_midi host events) */
static void poll_midi_fifo(int midi_fd) {
    static uint8_t buf[4096];
    static size_t buf_len = 0;

    uint8_t tmp[512];
    ssize_t n = read(midi_fd, tmp, sizeof(tmp));
    if (n > 0) {
        if (buf_len + n <= sizeof(buf)) {
            memcpy(buf + buf_len, tmp, n);
            buf_len += n;
        }
    }

    /* Parse complete MIDI frames (2-byte LE length prefix) */
    size_t pos = 0;
    while (pos + 2 <= buf_len) {
        uint16_t msg_len = buf[pos] | (buf[pos + 1] << 8);
        if (msg_len == 0) { pos += 2; continue; }
        if (pos + 2 + msg_len > buf_len) break;

        process_midi_msg(buf + pos + 2, msg_len);
        pos += 2 + msg_len;
    }

    if (pos > 0 && pos < buf_len) {
        memmove(buf, buf + pos, buf_len - pos);
        buf_len -= pos;
    } else if (pos >= buf_len) {
        buf_len = 0;
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input_fifo> [midi_fifo]\n", argv[0]);
        return 1;
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    g_dbg = (access("/tmp/push2dbg", F_OK) == 0);
    if (g_dbg) fprintf(stderr, "norns-input-bridge: Push 2 DEBUG logging on\n");

    /* Scaling-mode state file shared with norns-push2-display */
    g_scale_fd = open(SCALE_FILE, O_RDWR | O_CREAT, 0644);
    if (g_scale_fd >= 0) write_scale_mode();

    input_fd = open(argv[1], O_RDWR | O_NONBLOCK);
    if (input_fd < 0) { perror("open input fifo"); return 1; }

    /* Optional: also read from MIDI FIFO for on_midi host events */
    int midi_fifo_fd = -1;
    if (argc >= 3) {
        midi_fifo_fd = open(argv[2], O_RDWR | O_NONBLOCK);
        if (midi_fifo_fd < 0)
            fprintf(stderr, "norns-input-bridge: couldn't open MIDI FIFO %s (non-fatal)\n", argv[2]);
    }

    fprintf(stderr, "norns-input-bridge: INPUT=%s MIDI_FIFO=%s\n",
            argv[1], midi_fifo_fd >= 0 ? argv[2] : "none");

    /* Push 2 USB MIDI (primary): claim interface 2, reader thread + LED pump */
    pthread_t reader_thread;
    int have_reader = 0;
    if (libusb_init(&g_usb) == 0) {
        if (pthread_create(&reader_thread, NULL, push2_reader, NULL) == 0)
            have_reader = 1;
        else
            fprintf(stderr, "norns-input-bridge: failed to start Push 2 reader thread\n");
    } else {
        fprintf(stderr, "norns-input-bridge: libusb init failed (Push 2 USB disabled)\n");
        g_usb = NULL;
    }

    /* JACK client (fallback for MIDI that PipeWire does forward) */
    jack_status_t status;
    client = jack_client_open("norns-input", JackNoStartServer, &status);
    if (client) {
        midi_port = jack_port_register(client, "midi_in",
                                       JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
        if (midi_port) {
            jack_set_process_callback(client, jack_process, NULL);
            jack_set_port_registration_callback(client, on_port_registered, NULL);
            if (jack_activate(client) == 0) {
                jack_connect(client, "system:midi_capture_1", "norns-input:midi_in");
                try_connect_push2();
                fprintf(stderr, "norns-input-bridge: JACK MIDI active\n");
            } else {
                fprintf(stderr, "norns-input-bridge: can't activate JACK client\n");
                jack_client_close(client);
                client = NULL;
            }
        } else {
            jack_client_close(client);
            client = NULL;
        }
    } else {
        fprintf(stderr, "norns-input-bridge: JACK unavailable (status=%d), Push 2/FIFO only\n",
                status);
    }

    /* Unified service loop: poll host-MIDI FIFO + pump Push 2 LEDs */
    while (running) {
        if (midi_fifo_fd >= 0) poll_midi_fifo(midi_fifo_fd);
        push2_led_pump();
        usleep(1000);  /* 1ms */
    }

    if (client) { jack_deactivate(client); jack_client_close(client); }
    if (have_reader) pthread_join(reader_thread, NULL);
    push2_usb_close();
    if (g_usb) libusb_exit(g_usb);
    if (g_grid_fd >= 0) close(g_grid_fd);
    close(input_fd);
    if (midi_fifo_fd >= 0) close(midi_fifo_fd);
    return 0;
}
