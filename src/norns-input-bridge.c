/*
 * norns-input-bridge — Translates MIDI from Move into Norns encoder/key events
 *
 * JACK MIDI client: reads from system:midi_capture via JACK
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
#include <jack/jack.h>
#include <jack/midiport.h>

static volatile int running = 1;
static jack_client_t *client = NULL;
static jack_port_t *midi_port = NULL;
static int input_fd = -1;

/* Push 2 state */
static volatile int push2_connected = 0;
static int push2_grid_mode = 1;  /* 1=Grid (pads→monome), 0=MIDI (pads→type-2) */
static int viewport_x = 0;       /* current grid viewport offset (multiples of 8) */
static int viewport_y = 0;

static void handle_signal(int sig) { (void)sig; running = 0; }

/* Scan JACK output ports for Push 2 and connect to our input */
static void try_connect_push2(void) {
    if (!client) return;
    const char **ports = jack_get_ports(client, NULL,
                                        JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput);
    if (!ports) return;
    for (int i = 0; ports[i]; i++) {
        if (strstr(ports[i], "Push 2") || strstr(ports[i], "Push2") ||
                strstr(ports[i], "push2")) {
            int rc = jack_connect(client, ports[i], "norns-input:midi_in");
            if (rc == 0 || rc == EEXIST) {
                fprintf(stderr, "norns-input-bridge: Push 2 MIDI connected: %s\n",
                        ports[i]);
                push2_connected = 1;
            }
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

            /* Push 2 arrow buttons → monome grid viewport (8-key pages).
             * Arrow Up/Down and Octave Up/Down navigate the Y axis;
             * Arrow Left/Right navigate the X axis. */
            if (val > 0) {
                if      (cc == 44) viewport_x = (viewport_x >= 8)  ? viewport_x - 8 : 0;
                else if (cc == 45) viewport_x = (viewport_x < 120) ? viewport_x + 8 : 120;
                else if (cc == 46) viewport_y = (viewport_y >= 8)  ? viewport_y - 8 : 0;
                else if (cc == 47) viewport_y = (viewport_y < 120) ? viewport_y + 8 : 120;
                /* Octave Down (CC54) = lower rows of grid; Octave Up (CC55) = higher */
                else if (cc == 54) viewport_y = (viewport_y < 120) ? viewport_y + 8 : 120;
                else if (cc == 55) viewport_y = (viewport_y >= 8)  ? viewport_y - 8 : 0;

                if (cc >= 44 && cc <= 47) {
                    fprintf(stderr,
                            "norns-input-bridge: Push 2 viewport x=%d y=%d\n",
                            viewport_x, viewport_y);
                }
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

static int jack_process(jack_nframes_t nframes, void *arg) {
    (void)arg;
    void *buf = jack_port_get_buffer(midi_port, nframes);
    if (!buf) return 0;

    jack_nframes_t count = jack_midi_get_event_count(buf);
    for (jack_nframes_t i = 0; i < count; i++) {
        jack_midi_event_t ev;
        if (jack_midi_event_get(&ev, buf, i) == 0) {
            process_midi_msg(ev.buffer, ev.size);
        }
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

    /* Open JACK client */
    jack_status_t status;
    client = jack_client_open("norns-input", JackNoStartServer, &status);
    if (!client) {
        fprintf(stderr, "norns-input-bridge: JACK client open failed (status=%d)\n", status);
        /* Fall back to FIFO-only mode if JACK isn't available */
        if (midi_fifo_fd < 0) {
            fprintf(stderr, "norns-input-bridge: no MIDI source available, exiting\n");
            close(input_fd);
            return 1;
        }
        fprintf(stderr, "norns-input-bridge: running in FIFO-only mode\n");
        while (running) {
            poll_midi_fifo(midi_fifo_fd);
            usleep(1000);
        }
        close(input_fd);
        close(midi_fifo_fd);
        return 0;
    }

    midi_port = jack_port_register(client, "midi_in",
                                   JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    if (!midi_port) {
        fprintf(stderr, "norns-input-bridge: can't register MIDI port\n");
        jack_client_close(client);
        close(input_fd);
        return 1;
    }

    jack_set_process_callback(client, jack_process, NULL);
    jack_set_port_registration_callback(client, on_port_registered, NULL);

    if (jack_activate(client)) {
        fprintf(stderr, "norns-input-bridge: can't activate JACK client\n");
        jack_client_close(client);
        close(input_fd);
        return 1;
    }

    /* Connect to system MIDI capture and any already-connected Push 2 */
    jack_connect(client, "system:midi_capture_1", "norns-input:midi_in");
    try_connect_push2();
    try_connect_sc_output();

    fprintf(stderr, "norns-input-bridge: JACK MIDI active\n");

    while (running) {
        /* Also poll FIFO for on_midi host events (secondary path) */
        if (midi_fifo_fd >= 0) {
            poll_midi_fifo(midi_fifo_fd);
        }
        usleep(1000);  /* 1ms */
    }

    jack_deactivate(client);
    jack_client_close(client);
    close(input_fd);
    if (midi_fifo_fd >= 0) close(midi_fifo_fd);
    return 0;
}
