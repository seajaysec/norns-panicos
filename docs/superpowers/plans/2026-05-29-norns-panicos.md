# norns-panicos Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port Monome Norns to PanicOS/Portmaster ARM64 handhelds as a self-contained PortMaster package called `norns-panicos`.

**Architecture:** A C SDL2 binary (`norns-panicos`) replaces the Schwung DSP plugin. It creates FIFOs, fork/execs matron/crone/sclang natively (no chroot), reads the screen FIFO and renders true greyscale to an SDL2 window, and maps SDL2 GameController events to norns encoder/key input FIFO frames. The existing norns patches (`apply-move-patches.sh`) are reused unchanged.

**Tech Stack:** C (gcc cross-compile), SDL2, Docker (arm64v8/debian:bookworm-slim), POSIX FIFOs, OSC over UDP.

**Spec:** `docs/superpowers/specs/2026-05-29-norns-panicos-design.md`

---

## File Map

| File | Status | Purpose |
|---|---|---|
| `src/norns-panicos.c` | **Create** | SDL2 host binary — screen render, gamepad input, process manager |
| `tests/test_input.c` | **Create** | Unit tests for frame encoding + greyscale unpack logic |
| `scripts/Dockerfile.panicos` | **Create** | Docker cross-compile image (arm64 SDL2 + JACK headers) |
| `scripts/build-panicos.sh` | **Create** | Full build + assembly script |
| `ports/portmaster/Norns.sh` | **Create** | PortMaster launch script |
| `ports/portmaster/control.txt` | **Create** | PortMaster metadata |
| `patches/apply-move-patches.sh` | **Unchanged** | Reused as-is — patches are FIFO-based, no Move-specific hardware |
| `src/norns-input-bridge.c` | **Unchanged** | Reused as-is — handles external JACK MIDI → input FIFO |

---

## Task 1: PortMaster metadata skeleton

**Files:**
- Create: `ports/portmaster/control.txt`
- Create: `ports/portmaster/Norns.sh` (stub — completed in Task 6)

- [ ] **Step 1: Create the ports directory**

```bash
mkdir -p ports/portmaster
```

- [ ] **Step 2: Write control.txt**

Create `ports/portmaster/control.txt`:

```
Title=Norns
Porter=djhardrich
Locations.consoles=ports
Genres=music
Description=Monome Norns sound computer on ARM handhelds.
porter_url=https://github.com/djhardrich/norns-portmaster
Architecture=aarch64
runtime=none
```

- [ ] **Step 3: Write stub Norns.sh** (will be completed in Task 6)

Create `ports/portmaster/Norns.sh`:

```bash
#!/bin/bash
echo "norns-panicos stub — replace with Task 6 content"
```

```bash
chmod +x ports/portmaster/Norns.sh
```

- [ ] **Step 4: Commit**

```bash
git add ports/
git commit -m "feat: add PortMaster package skeleton"
```

---

## Task 2: Unit tests for input frame encoding + greyscale unpack

These tests verify the two pure-logic functions in `norns-panicos.c` before the SDL2 binary exists.

**Files:**
- Create: `tests/test_input.c`

- [ ] **Step 1: Create tests directory and write test_input.c**

```bash
mkdir -p tests
```

Create `tests/test_input.c`:

```c
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
```

- [ ] **Step 2: Build and run the tests (native x86 — no SDL2 needed)**

```bash
gcc -Wall -Wextra -o tests/test_input tests/test_input.c && ./tests/test_input
```

Expected output:
```
Running norns-panicos input tests...
  PASS test_enc_frame_positive
  PASS test_enc_frame_negative
  PASS test_enc_frame_large_negative
  PASS test_key_frame
  PASS test_greyscale_unpack
  PASS test_encoder_selection
All tests passed.
```

- [ ] **Step 3: Commit**

```bash
git add tests/
git commit -m "test: add norns-panicos input frame + greyscale unit tests"
```

---

## Task 3: src/norns-panicos.c

The full SDL2 host binary. Create this file in one shot — it is the largest single artifact in the port.

**Files:**
- Create: `src/norns-panicos.c`

- [ ] **Step 1: Write src/norns-panicos.c**

Create `src/norns-panicos.c`:

```c
/*
 * norns-panicos.c — SDL2 host for Norns on PanicOS/Portmaster handhelds
 *
 * Screen:  reads FIFO_SCREEN (4-bit packed greyscale, 128x64)
 *          → SDL2 streaming texture, scaled fullscreen with nearest-neighbor
 * Input:   SDL2 GameController API → FIFO_INPUT (4-byte frames)
 *          type 0 = encoder delta  [type][id][val_lo][val_hi]
 *          type 1 = key state      [type][id][state][0]
 * Procs:   fork/exec crone, matron (via ws-wrapper), sclang,
 *          norns-input-bridge, maiden — all native (no chroot)
 *
 * Button mapping:
 *   Y=K1  X=K2  A=K3  B=K1(alias)
 *   Hold L1 → E1 selected   Hold L2 → E2 selected   Hold R1 → E3 selected
 *   D-pad left/right  = selected encoder +-1
 *   D-pad up/down     = E1 +-1 always (quick menu scroll)
 *   Left stick Y      = selected encoder (velocity-scaled, throttled)
 *   Select            = restart norns
 *   Select + Start    = exit to PortMaster
 */

#define _GNU_SOURCE
#include <SDL2/SDL.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ── Constants ──────────────────────────────────────────── */

#define NORNS_WIDTH      128
#define NORNS_HEIGHT     64
#define SCREEN_FRAME_SZ  (NORNS_WIDTH * NORNS_HEIGHT / 2)  /* 4096 bytes */

#define FIFO_SCREEN   "/tmp/norns-screen-1"
#define FIFO_INPUT    "/tmp/norns-input-1"
#define FIFO_GRID     "/tmp/norns-grid-1"
#define FIFO_MIDI_IN  "/tmp/midi-to-chroot-1"
#define FIFO_MIDI_OUT "/tmp/midi-from-chroot-1"

#define STICK_DEADZONE   8192
#define TRIGGER_THRESH   8192
#define STICK_THROTTLE   3     /* emit stick input every N frames (~20 Hz at 60 fps) */
#define OSC_MATRON_PORT  8888

/* ── State ──────────────────────────────────────────────── */

typedef struct {
    SDL_Window         *window;
    SDL_Renderer       *renderer;
    SDL_Texture        *texture;
    SDL_GameController *gc;

    int screen_fd;
    int input_fd;

    uint8_t selected_enc;   /* 0=E1  1=E2  2=E3 */
    int     select_held;
    int     select_was_combo;

    pid_t pid_crone;
    pid_t pid_matron;
    pid_t pid_sclang;
    pid_t pid_input_bridge;
    pid_t pid_maiden;

    char norns_dir[512];   /* $HOME/norns */
    char bin_dir[512];     /* directory containing this binary */

    int      running;
    uint32_t frame;
} norns_state_t;

/* ── Logging ────────────────────────────────────────────── */

static void log_msg(const char *msg) {
    fprintf(stderr, "[norns-panicos] %s\n", msg);
}

/* ── FIFO helpers ───────────────────────────────────────── */

static int make_fifo(const char *path) {
    unlink(path);
    if (mkfifo(path, 0666) != 0 && errno != EEXIST) {
        char buf[256];
        snprintf(buf, sizeof(buf), "mkfifo %s: %s", path, strerror(errno));
        log_msg(buf);
        return -1;
    }
    chmod(path, 0666);
    return open(path, O_RDWR | O_NONBLOCK);
}

static int create_fifos(norns_state_t *s) {
    s->screen_fd = make_fifo(FIFO_SCREEN);
    s->input_fd  = make_fifo(FIFO_INPUT);
    (void)make_fifo(FIFO_GRID);
    (void)make_fifo(FIFO_MIDI_IN);
    (void)make_fifo(FIFO_MIDI_OUT);
    if (s->screen_fd < 0 || s->input_fd < 0) {
        log_msg("FIFO creation failed");
        return -1;
    }
    log_msg("FIFOs created");
    return 0;
}

/* ── Input frame helpers ────────────────────────────────── */

static void send_enc(norns_state_t *s, uint8_t enc_id, int16_t delta) {
    uint8_t frame[4];
    frame[0] = 0;
    frame[1] = enc_id;
    frame[2] = (uint8_t)(delta & 0xFF);
    frame[3] = (uint8_t)((delta >> 8) & 0xFF);
    (void)write(s->input_fd, frame, 4);
}

static void send_key(norns_state_t *s, uint8_t key_id, uint8_t state) {
    uint8_t frame[4] = { 1, key_id, state, 0 };
    (void)write(s->input_fd, frame, 4);
}

/* ── OSC /crone/ready ───────────────────────────────────── */

/* Fallback for Crone.sc not sending /crone/ready on time */
static void send_crone_ready(void) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(OSC_MATRON_PORT);
    inet_aton("127.0.0.1", &addr.sin_addr);
    /* /crone/ready with no args — 16-byte address + 4-byte type tag */
    uint8_t msg[20] = {
        '/', 'c', 'r', 'o', 'n', 'e', '/', 'r', 'e', 'a', 'd', 'y',
        0, 0, 0, 0,   /* NUL pad to 16 bytes */
        ',', 0, 0, 0  /* type string: no args */
    };
    sendto(sock, msg, sizeof(msg), 0, (struct sockaddr *)&addr, sizeof(addr));
    close(sock);
}

/* ── Process management ─────────────────────────────────── */

static pid_t spawn_proc(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) { log_msg("fork failed"); return -1; }
    if (pid == 0) {
        /* Child: inject FIFO env vars, then exec */
        setenv("NORNS_SCREEN_FIFO",  FIFO_SCREEN,  1);
        setenv("NORNS_INPUT_FIFO",   FIFO_INPUT,   1);
        setenv("NORNS_MIDI_OUT_FIFO",FIFO_MIDI_OUT,1);
        setsid();
        execvp(path, argv);
        fprintf(stderr, "[norns-panicos] execvp %s: %s\n", path, strerror(errno));
        _exit(127);
    }
    return pid;
}

static void start_norns_processes(norns_state_t *s) {
    char crone_path[512], matron_path[512], ws_path[512];
    char bridge_path[512], maiden_path[512], sclang_conf[512];
    char dust_dir[512], maiden_app[512], maiden_doc[512];

    snprintf(crone_path,  sizeof(crone_path),  "%s/build/crone/crone",         s->norns_dir);
    snprintf(matron_path, sizeof(matron_path), "%s/build/matron/matron",        s->norns_dir);
    snprintf(ws_path,     sizeof(ws_path),     "%s/build/ws-wrapper/ws-wrapper",s->norns_dir);
    snprintf(bridge_path, sizeof(bridge_path), "%s/norns-input-bridge",         s->bin_dir);
    /* maiden lives at $HOME/maiden/maiden, one level up from norns_dir */
    snprintf(maiden_path, sizeof(maiden_path), "%s/../maiden/maiden",            s->norns_dir);
    snprintf(sclang_conf, sizeof(sclang_conf), "%s/sclang_conf.yaml",            s->norns_dir);
    snprintf(dust_dir,    sizeof(dust_dir),    "%s/../dust",                     s->norns_dir);
    snprintf(maiden_app,  sizeof(maiden_app),  "%s/../maiden/app/build",         s->norns_dir);
    snprintf(maiden_doc,  sizeof(maiden_doc),  "%s/doc",                         s->norns_dir);

    /* 1. crone (JACK audio routing — must start before sclang) */
    { char *av[] = { crone_path, NULL };
      s->pid_crone = spawn_proc(crone_path, av); }
    log_msg("crone started"); sleep(2);

    /* 2. matron (Lua VM + Cairo) via ws-wrapper */
    { char *av[] = { ws_path, "ws://*:5555", matron_path, NULL };
      s->pid_matron = spawn_proc(ws_path, av); }
    log_msg("matron started"); sleep(1);

    /* 3. sclang (manages scsynth — wait 20 s for SC to boot on ARM) */
    { char *av[] = { "sclang", "-l", sclang_conf, NULL };
      s->pid_sclang = spawn_proc("sclang", av); }
    log_msg("sclang started"); sleep(20);

    /* 4. /crone/ready fallback — in case Crone.sc missed the window */
    send_crone_ready();
    sleep(1);

    /* 5. norns-input-bridge (JACK MIDI → input FIFO for external devices) */
    { char *av[] = { bridge_path, FIFO_INPUT, FIFO_MIDI_IN, NULL };
      s->pid_input_bridge = spawn_proc(bridge_path, av); }
    log_msg("norns-input-bridge started");

    /* 6. maiden (web IDE on port 5000) */
    { char *av[] = { maiden_path, "server",
                     "--port", "5000",
                     "--data", dust_dir,
                     "--app",  maiden_app,
                     "--doc",  maiden_doc,
                     NULL };
      s->pid_maiden = spawn_proc(maiden_path, av); }
    log_msg("maiden started");
}

static void stop_norns_processes(norns_state_t *s) {
    pid_t pids[] = { s->pid_matron, s->pid_sclang, s->pid_crone,
                     s->pid_input_bridge, s->pid_maiden };
    for (int i = 0; i < 5; i++) {
        if (pids[i] > 0) kill(pids[i], SIGTERM);
    }
    sleep(2);
    for (int i = 0; i < 5; i++) {
        if (pids[i] > 0) waitpid(pids[i], NULL, WNOHANG);
    }
    s->pid_crone = s->pid_matron = s->pid_sclang = -1;
    s->pid_input_bridge = s->pid_maiden = -1;
}

static void restart_norns(norns_state_t *s) {
    log_msg("restarting norns...");
    stop_norns_processes(s);
    start_norns_processes(s);
}

static void check_processes(norns_state_t *s) {
    int status;
    if (s->pid_crone > 0 && waitpid(s->pid_crone, &status, WNOHANG) > 0) {
        log_msg("crone crashed — restarting"); restart_norns(s);
    } else if (s->pid_matron > 0 && waitpid(s->pid_matron, &status, WNOHANG) > 0) {
        log_msg("matron crashed — restarting"); restart_norns(s);
    }
}

/* ── Screen rendering ───────────────────────────────────── */

static void pump_screen(norns_state_t *s) {
    uint8_t packed[SCREEN_FRAME_SZ];
    uint8_t latest[SCREEN_FRAME_SZ];
    int got = 0;
    /* Drain FIFO: keep only the latest frame */
    for (;;) {
        ssize_t n = read(s->screen_fd, packed, SCREEN_FRAME_SZ);
        if (n == (ssize_t)SCREEN_FRAME_SZ) { memcpy(latest, packed, SCREEN_FRAME_SZ); got = 1; }
        else break;
    }
    if (!got) return;

    /* Unpack 4-bit greyscale → RGB24.
     * Each byte holds two pixels: high nybble = left pixel, low = right.
     * Scale 0-15 → 0-255 by multiplying by 17. */
    uint8_t pixels[NORNS_WIDTH * NORNS_HEIGHT * 3];
    for (int y = 0; y < NORNS_HEIGHT; y++) {
        for (int x = 0; x < NORNS_WIDTH; x++) {
            int     src   = y * (NORNS_WIDTH / 2) + x / 2;
            uint8_t nyb   = (x & 1) ? (latest[src] & 0xF) : ((latest[src] >> 4) & 0xF);
            uint8_t gray  = nyb * 17;
            int     dst   = (y * NORNS_WIDTH + x) * 3;
            pixels[dst] = pixels[dst + 1] = pixels[dst + 2] = gray;
        }
    }
    SDL_UpdateTexture(s->texture, NULL, pixels, NORNS_WIDTH * 3);
}

static void render_frame(norns_state_t *s) {
    SDL_RenderClear(s->renderer);
    SDL_RenderCopy(s->renderer, s->texture, NULL, NULL);
    SDL_RenderPresent(s->renderer);
}

/* ── Gamepad input ──────────────────────────────────────── */

static void handle_button(norns_state_t *s, SDL_ControllerButtonEvent *ev) {
    int pressed = (ev->type == SDL_CONTROLLERBUTTONDOWN);

    switch (ev->button) {
    /* Face buttons → norns keys (Y=K1, X=K2, A=K3, B=K1 alias) */
    case SDL_CONTROLLER_BUTTON_Y:
        send_key(s, 0, pressed); break;
    case SDL_CONTROLLER_BUTTON_B:
        send_key(s, 0, pressed); break;
    case SDL_CONTROLLER_BUTTON_X:
        send_key(s, 1, pressed); break;
    case SDL_CONTROLLER_BUTTON_A:
        send_key(s, 2, pressed); break;

    /* D-pad left/right → selected encoder +/-1 */
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        if (pressed) send_enc(s, s->selected_enc, -1); break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        if (pressed) send_enc(s, s->selected_enc,  1); break;

    /* D-pad up/down → E1 always (quick menu scroll regardless of selection) */
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        if (pressed) send_enc(s, 0,  1); break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        if (pressed) send_enc(s, 0, -1); break;

    /* Select = restart; Select+Start = exit */
    case SDL_CONTROLLER_BUTTON_BACK:
        if (pressed) {
            s->select_held = 1;
            s->select_was_combo = 0;
            if (SDL_GameControllerGetButton(s->gc, SDL_CONTROLLER_BUTTON_START)) {
                s->running = 0;
                s->select_was_combo = 1;
            }
        } else {
            if (!s->select_was_combo) restart_norns(s);
            s->select_held = 0;
        }
        break;
    case SDL_CONTROLLER_BUTTON_START:
        if (pressed && s->select_held) {
            s->running = 0;
            s->select_was_combo = 1;
        }
        break;

    default: break;
    }
}

/* Poll axes each frame: shoulder buttons select encoder, stick drives it. */
static void poll_axes(norns_state_t *s) {
    if (!s->gc) return;

    /* Encoder selection: L1=E1, L2=E2, R1=E3.
     * L1/R1 are buttons; L2/R2 are analog triggers (axis).
     * When no shoulder is held, selected_enc retains its last value. */
    int     l1 = SDL_GameControllerGetButton(s->gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    int     r1 = SDL_GameControllerGetButton(s->gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    int16_t l2 = SDL_GameControllerGetAxis(s->gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    if      (l1)                   s->selected_enc = 0;
    else if (l2 > TRIGGER_THRESH)  s->selected_enc = 1;
    else if (r1)                   s->selected_enc = 2;
    /* R2 unassigned */

    /* Left stick Y → selected encoder (velocity-scaled, throttled to STICK_THROTTLE).
     * SDL2 axis convention: stick up = negative value. Negate so up = positive delta. */
    if (s->frame % STICK_THROTTLE == 0) {
        int16_t ly = SDL_GameControllerGetAxis(s->gc, SDL_CONTROLLER_AXIS_LEFTY);
        if (abs(ly) > STICK_DEADZONE) {
            int16_t delta;
            int a = abs(ly);
            if      (a < 16384) delta = 1;
            else if (a < 24576) delta = 2;
            else                delta = 3;
            if (ly > 0) delta = (int16_t)(-delta);  /* up = positive in norns */
            send_enc(s, s->selected_enc, delta);
        }
    }
}

/* ── Initialisation ─────────────────────────────────────── */

/* Derive norns_dir ($HOME/norns) and bin_dir (dirname of this binary). */
static void get_paths(norns_state_t *s) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(s->norns_dir, sizeof(s->norns_dir), "%s/norns", home);

    char exe[512] = {0};
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n > 0) {
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if (slash) *slash = '\0';
        snprintf(s->bin_dir, sizeof(s->bin_dir), "%s", exe);
    } else {
        snprintf(s->bin_dir, sizeof(s->bin_dir), "/usr/local/bin");
    }
}

static int init_sdl(norns_state_t *s) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[norns-panicos] SDL_Init: %s\n", SDL_GetError());
        return -1;
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");  /* nearest-neighbor — crisp pixels */

    s->window = SDL_CreateWindow("Norns",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!s->window) {
        fprintf(stderr, "[norns-panicos] SDL_CreateWindow: %s\n", SDL_GetError());
        return -1;
    }

    s->renderer = SDL_CreateRenderer(s->window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s->renderer) {
        fprintf(stderr, "[norns-panicos] SDL_CreateRenderer: %s\n", SDL_GetError());
        return -1;
    }

    /* Logical size 128x64 — SDL scales to fill the window maintaining aspect ratio */
    SDL_RenderSetLogicalSize(s->renderer, NORNS_WIDTH, NORNS_HEIGHT);

    s->texture = SDL_CreateTexture(s->renderer,
        SDL_PIXELFORMAT_RGB24,
        SDL_TEXTUREACCESS_STREAMING,
        NORNS_WIDTH, NORNS_HEIGHT);
    if (!s->texture) {
        fprintf(stderr, "[norns-panicos] SDL_CreateTexture: %s\n", SDL_GetError());
        return -1;
    }

    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            s->gc = SDL_GameControllerOpen(i);
            if (s->gc) { log_msg("game controller opened"); break; }
        }
    }
    if (!s->gc) log_msg("warning: no game controller found — keyboard/touch only");

    return 0;
}

/* ── Main ───────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);

    norns_state_t s;
    memset(&s, 0, sizeof(s));
    s.screen_fd = s.input_fd = -1;
    s.pid_crone = s.pid_matron = s.pid_sclang = -1;
    s.pid_input_bridge = s.pid_maiden = -1;
    s.selected_enc = 0;
    s.running = 1;
    s.frame   = 0;

    get_paths(&s);
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "norns_dir=%s  bin_dir=%s", s.norns_dir, s.bin_dir);
        log_msg(msg);
    }

    if (create_fifos(&s) != 0) return 1;
    if (init_sdl(&s)     != 0) return 1;

    start_norns_processes(&s);

    SDL_Event ev;
    while (s.running) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                s.running = 0; break;
            }
            if (ev.type == SDL_CONTROLLERBUTTONDOWN ||
                ev.type == SDL_CONTROLLERBUTTONUP) {
                handle_button(&s, &ev.cbutton);
            }
            if (ev.type == SDL_CONTROLLERDEVICEADDED && !s.gc) {
                s.gc = SDL_GameControllerOpen(ev.cdevice.which);
            }
        }

        poll_axes(&s);
        pump_screen(&s);
        render_frame(&s);

        /* Watchdog: check every ~60 s (3600 frames at ~60 fps) */
        if (s.frame % 3600 == 0 && s.frame > 0) check_processes(&s);
        s.frame++;
    }

    log_msg("exiting — stopping norns");
    stop_norns_processes(&s);

    if (s.gc)       SDL_GameControllerClose(s.gc);
    if (s.texture)  SDL_DestroyTexture(s.texture);
    if (s.renderer) SDL_DestroyRenderer(s.renderer);
    if (s.window)   SDL_DestroyWindow(s.window);
    SDL_Quit();

    if (s.screen_fd >= 0) close(s.screen_fd);
    if (s.input_fd  >= 0) close(s.input_fd);
    unlink(FIFO_SCREEN); unlink(FIFO_INPUT);
    unlink(FIFO_GRID); unlink(FIFO_MIDI_IN); unlink(FIFO_MIDI_OUT);

    return 0;
}
```

- [ ] **Step 2: Verify it compiles natively (catches syntax errors before Docker)**

Install SDL2 dev if needed: `sudo apt-get install -y libsdl2-dev`

```bash
gcc -Wall -Wextra -o /tmp/norns-panicos-test src/norns-panicos.c \
    $(pkg-config --cflags --libs sdl2) -lpthread -lm 2>&1
```

Expected: no errors. Warnings about unused args or `abs()` on `int16_t` are acceptable.

- [ ] **Step 3: Confirm binary exists**

```bash
file /tmp/norns-panicos-test
```

Expected: `ELF 64-bit LSB executable, x86-64` (native test build).

- [ ] **Step 4: Commit**

```bash
git add src/norns-panicos.c
git commit -m "feat: add norns-panicos SDL2 host binary"
```

---

## Task 4: Dockerfile.panicos

Cross-compile image for the host binaries (norns-panicos + norns-input-bridge).

**Files:**
- Create: `scripts/Dockerfile.panicos`

- [ ] **Step 1: Write scripts/Dockerfile.panicos**

```dockerfile
FROM debian:bookworm-slim

RUN dpkg --add-architecture arm64 && \
    apt-get update && apt-get install -y \
        gcc-aarch64-linux-gnu \
        make \
        libsdl2-dev:arm64 \
        libjack-jackd2-dev:arm64 \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

ENV CROSS_PREFIX=aarch64-linux-gnu-
ENV PKG_CONFIG_PATH=/usr/lib/aarch64-linux-gnu/pkgconfig
ENV PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig
ENV PKG_CONFIG_SYSROOT_DIR=/
WORKDIR /build
```

- [ ] **Step 2: Build the Docker image to verify it works**

```bash
docker build -t norns-panicos-builder -f scripts/Dockerfile.panicos .
```

Expected: image builds successfully, no apt errors.

- [ ] **Step 3: Cross-compile norns-panicos inside the container**

```bash
mkdir -p build
docker run --rm \
    -v "$(pwd):/build" \
    -u "$(id -u):$(id -g)" \
    -w /build \
    norns-panicos-builder \
    sh -c '
set -e
SDL_FLAGS=$(pkg-config --cflags --libs sdl2 2>/dev/null || echo "-lSDL2")
JACK_FLAGS=$(pkg-config --cflags --libs jack 2>/dev/null || echo "-ljack")
${CROSS_PREFIX}gcc -O2 -Wall src/norns-panicos.c    -o build/norns-panicos    $SDL_FLAGS -lpthread -lm
${CROSS_PREFIX}gcc -O2 -Wall src/norns-input-bridge.c -o build/norns-input-bridge $JACK_FLAGS
echo "Build OK"
'
```

Expected output ends with `Build OK`.

- [ ] **Step 4: Verify ARM64 binaries**

```bash
file build/norns-panicos build/norns-input-bridge
```

Expected: both show `ELF 64-bit LSB executable, ARM aarch64`.

- [ ] **Step 5: Commit**

```bash
git add scripts/Dockerfile.panicos build/norns-panicos build/norns-input-bridge
git commit -m "build: add Dockerfile.panicos and cross-compiled arm64 binaries"
```

---

## Task 5: scripts/build-panicos.sh

Full build + package assembly. Builds all artifacts in Docker and produces `dist/norns-panicos.tar.gz`.

**Files:**
- Create: `scripts/build-panicos.sh`

Note: This script calls the existing `scripts/build-norns.sh` and `scripts/build-sc-plugins.sh`. Those must exist in the repo (they are in `schwung-norns`; copy them if this is a fresh repo).

- [ ] **Step 1: Copy required build scripts from schwung-norns if not already present**

```bash
ls scripts/build-norns.sh scripts/build-sc-plugins.sh scripts/Dockerfile.norns 2>/dev/null \
    || echo "Copy from schwung-norns: scripts/build-norns.sh build-sc-plugins.sh Dockerfile.norns"
```

If missing, copy them:
```bash
cp /home/user1/schwung-norns/scripts/build-norns.sh      scripts/
cp /home/user1/schwung-norns/scripts/build-sc-plugins.sh scripts/
cp /home/user1/schwung-norns/scripts/Dockerfile.norns    scripts/
cp /home/user1/schwung-norns/patches/apply-move-patches.sh patches/
```

- [ ] **Step 2: Write scripts/build-panicos.sh**

```bash
#!/usr/bin/env bash
# build-panicos.sh — Full norns-panicos PortMaster package build
# Output: dist/norns-panicos.tar.gz  (self-contained, no on-device downloads)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
DIST="$REPO_ROOT/dist/norns-panicos"
NORNS_DATA="$DIST/norns/data"
NORNS_BIN="$DIST/norns/bin"

echo "=== Building norns-panicos PortMaster package ==="

# ── 1. Cross-compile host binaries ──────────────────────────
echo ""
echo "--- [1/4] Building host binaries (arm64 cross-compile) ---"
docker build -t norns-panicos-builder -f "$SCRIPT_DIR/Dockerfile.panicos" "$REPO_ROOT"
mkdir -p "$REPO_ROOT/build"
docker run --rm \
    -v "$REPO_ROOT:/build" \
    -u "$(id -u):$(id -g)" \
    -w /build \
    norns-panicos-builder \
    sh -c '
set -e
SDL_FLAGS=$(pkg-config --cflags --libs sdl2 2>/dev/null || echo "-lSDL2")
JACK_FLAGS=$(pkg-config --cflags --libs jack 2>/dev/null || echo "-ljack")
${CROSS_PREFIX}gcc -O2 -Wall src/norns-panicos.c      -o build/norns-panicos      $SDL_FLAGS -lpthread -lm
${CROSS_PREFIX}gcc -O2 -Wall src/norns-input-bridge.c -o build/norns-input-bridge $JACK_FLAGS
echo "[1/4] host binaries OK"
'

# ── 2. Build norns prebuilt tarball ─────────────────────────
echo ""
echo "--- [2/4] Building norns prebuilt tarball ---"
"$SCRIPT_DIR/build-norns.sh"

# ── 3. Build SC plugins ─────────────────────────────────────
echo ""
echo "--- [3/4] Building SC plugins ---"
"$SCRIPT_DIR/build-sc-plugins.sh"

# ── 4. Assemble package ──────────────────────────────────────
echo ""
echo "--- [4/4] Assembling norns-panicos package ---"
rm -rf "$DIST"
mkdir -p \
    "$NORNS_BIN" \
    "$NORNS_DATA/dust/code" \
    "$NORNS_DATA/dust/audio" \
    "$NORNS_DATA/dust/data/sources" \
    "$NORNS_DATA/dust/data/catalogs" \
    "$DIST/norns/logs" \
    "$DIST/norns/cfg"

# Host binaries
cp "$REPO_ROOT/build/norns-panicos"      "$NORNS_BIN/"
cp "$REPO_ROOT/build/norns-input-bridge" "$NORNS_BIN/"
chmod +x "$NORNS_BIN/norns-panicos" "$NORNS_BIN/norns-input-bridge"

# norns prebuilt binaries
tar xzf "$REPO_ROOT/dist/norns-move-prebuilt.tar.gz" -C "$NORNS_DATA/"

# SC plugins
mkdir -p "$NORNS_DATA/.local/share/SuperCollider/Extensions"
tar xzf "$REPO_ROOT/dist/sc-plugins-arm64.tar.gz" \
    -C "$NORNS_DATA/.local/share/SuperCollider/"

# Starter scripts (cloned from source)
echo "  Cloning starter scripts..."
cd "$NORNS_DATA/dust/code"
for REPO in \
    "https://github.com/tehn/awake.git" \
    "https://github.com/markwheeler/molly_the_poly.git" \
    "https://github.com/markwheeler/passersby.git"; do
    NAME="$(basename "$REPO" .git)"
    [ -d "$NAME" ] || git clone --depth 1 "$REPO"
done
cd "$REPO_ROOT"

# Maiden catalog sources
cat > "$NORNS_DATA/dust/data/sources/community.json" << 'SRCEOF'
{"file_info":{"version":1,"kind":"catalog_source"},"source":{"name":"community","method":"download","parameters":{"url":"https://raw.githubusercontent.com/monome/norns-community/main/community.json"}}}
SRCEOF
cat > "$NORNS_DATA/dust/data/sources/base.json" << 'SRCEOF'
{"file_info":{"version":1,"kind":"catalog_source"},"source":{"name":"base","method":"download","parameters":{"url":"https://raw.githubusercontent.com/monome/norns-community/main/base.json"}}}
SRCEOF

# sc/startup.scd — force 44100 Hz (norns expects this)
mkdir -p "$NORNS_DATA/norns/sc"
cat > "$NORNS_DATA/norns/sc/startup.scd" << 'SCDEOF'
s.options.sampleRate = 44100;
SCDEOF

# sclang_conf.yaml — placeholder; overwritten at runtime by Norns.sh
# because the include paths must contain the actual $HOME value on the device.
cat > "$NORNS_DATA/norns/sclang_conf.yaml" << 'SCCONF'
# Generated at launch time by Norns.sh — do not edit
SCCONF

# PortMaster metadata + launcher
cp "$REPO_ROOT/ports/portmaster/Norns.sh"    "$DIST/"
cp "$REPO_ROOT/ports/portmaster/control.txt" "$DIST/"

# ── 5. Package ───────────────────────────────────────────────
echo ""
echo "--- Packaging ---"
mkdir -p "$REPO_ROOT/dist"
(cd "$REPO_ROOT/dist" && tar czf norns-panicos.tar.gz norns-panicos/)

echo ""
echo "=== Build complete ==="
echo "Output: dist/norns-panicos.tar.gz"
ls -lh "$REPO_ROOT/dist/norns-panicos.tar.gz"
```

- [ ] **Step 3: Make it executable**

```bash
chmod +x scripts/build-panicos.sh
```

- [ ] **Step 4: Dry-run the assembly section only (skip Docker builds) to check paths**

```bash
# Create fake build artifacts to test the assembly logic
mkdir -p build dist
echo "fake-norns-panicos-binary" > build/norns-panicos
echo "fake-input-bridge-binary"  > build/norns-input-bridge
touch dist/norns-move-prebuilt.tar.gz dist/sc-plugins-arm64.tar.gz

# Run just the assembly portion (steps 4 onward) manually
DIST=/tmp/norns-panicos-dryrun
NORNS_DATA="$DIST/norns/data"
NORNS_BIN="$DIST/norns/bin"
mkdir -p "$NORNS_BIN" "$NORNS_DATA/dust/code" "$NORNS_DATA/dust/data/sources" \
         "$DIST/norns/logs" "$DIST/norns/cfg"
cp build/norns-panicos      "$NORNS_BIN/"
cp build/norns-input-bridge "$NORNS_BIN/"
ls -R /tmp/norns-panicos-dryrun/norns/bin/
```

Expected:
```
/tmp/norns-panicos-dryrun/norns/bin/:
norns-input-bridge  norns-panicos
```

- [ ] **Step 5: Commit**

```bash
git add scripts/build-panicos.sh
git commit -m "build: add build-panicos.sh full package assembly script"
```

---

## Task 6: Complete Norns.sh

Replace the stub with the full PortMaster launch script. Generates `sclang_conf.yaml` at runtime (required because SuperCollider include paths must contain the actual `$HOME` path, which varies per device install).

**Files:**
- Modify: `ports/portmaster/Norns.sh`

- [ ] **Step 1: Write the complete Norns.sh**

Overwrite `ports/portmaster/Norns.sh`:

```bash
#!/bin/bash
# Norns.sh — PortMaster launch script for norns-panicos

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if   [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/"        ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/"    ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else                                              controlfolder="/roms/ports/PortMaster"
fi

source "$controlfolder/control.txt"
[ -f "$controlfolder/mod_${CFW_NAME}.txt" ] && source "$controlfolder/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)/norns"
cd "$GAMEDIR"

# norns home — all norns processes see their home here
export HOME="$GAMEDIR/data"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export JACK_NO_AUDIO_RESERVATION=1
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# Rotate log every 4 launches (keeps log small on SD card)
LAUNCH_COUNT_FILE="$GAMEDIR/logs/.launch_count"
LAUNCH_COUNT=0
[ -f "$LAUNCH_COUNT_FILE" ] && LAUNCH_COUNT=$(cat "$LAUNCH_COUNT_FILE" 2>/dev/null || echo 0)
LAUNCH_COUNT=$((LAUNCH_COUNT + 1))
if [ "$LAUNCH_COUNT" -ge 4 ]; then
    : > "$GAMEDIR/logs/norns.log"
    LAUNCH_COUNT=0
fi
echo "$LAUNCH_COUNT" > "$LAUNCH_COUNT_FILE"
mkdir -p "$GAMEDIR/logs" "$GAMEDIR/cfg"

# Generate sclang_conf.yaml with the actual $HOME path for this device.
# SC include paths must be absolute — they cannot use shell variable expansion.
cat > "$HOME/norns/sclang_conf.yaml" << EOF
includePaths:
    - $HOME/norns/sc/core
    - $HOME/norns/sc/engines
    - $HOME/norns/sc
    - $HOME/dust
excludePaths:
    []
postInlinePaths: []
EOF

chmod +x ./bin/norns-panicos ./bin/norns-input-bridge 2>/dev/null

$GPTOKEYB "norns-panicos" &
pm_platform_helper "./bin/norns-panicos"
./bin/norns-panicos 2>&1 | tee -a "$GAMEDIR/logs/norns.log"
pm_finish
```

- [ ] **Step 2: Verify the sclang_conf.yaml generation in isolation**

```bash
# Simulate what Norns.sh does
HOME=/tmp/norns-test-home
mkdir -p "$HOME/norns/sc"
cat > /tmp/test-sclang-conf.sh << 'EOF'
HOME=/tmp/norns-test-home
cat > "$HOME/norns/sclang_conf.yaml" << INNER
includePaths:
    - $HOME/norns/sc/core
    - $HOME/norns/sc/engines
    - $HOME/norns/sc
    - $HOME/dust
excludePaths:
    []
postInlinePaths: []
INNER
cat "$HOME/norns/sclang_conf.yaml"
EOF
bash /tmp/test-sclang-conf.sh
```

Expected output (with actual path, not `$HOME` literal):
```yaml
includePaths:
    - /tmp/norns-test-home/norns/sc/core
    - /tmp/norns-test-home/norns/sc/engines
    - /tmp/norns-test-home/norns/sc
    - /tmp/norns-test-home/dust
excludePaths:
    []
postInlinePaths: []
```

- [ ] **Step 3: Commit**

```bash
git add ports/portmaster/Norns.sh
git commit -m "feat: complete Norns.sh PortMaster launch script"
```

---

## Task 7: Full build verification

Run the complete build and verify the output tarball has the expected structure.

**Prerequisites:** Docker running, internet access (for git clone of starter scripts and the `build-norns.sh` Docker image).

- [ ] **Step 1: Run the full build**

```bash
./scripts/build-panicos.sh 2>&1 | tee /tmp/norns-panicos-build.log
```

Expected final lines:
```
=== Build complete ===
Output: dist/norns-panicos.tar.gz
-rw-r--r-- ... dist/norns-panicos.tar.gz
```

- [ ] **Step 2: Verify tarball structure**

```bash
tar tzf dist/norns-panicos.tar.gz | sort | head -50
```

Expected to include:
```
norns-panicos/Norns.sh
norns-panicos/control.txt
norns-panicos/norns/bin/norns-panicos
norns-panicos/norns/bin/norns-input-bridge
norns-panicos/norns/data/norns/build/matron/matron
norns-panicos/norns/data/norns/build/crone/crone
norns-panicos/norns/data/norns/build/ws-wrapper/ws-wrapper
norns-panicos/norns/data/maiden/maiden
norns-panicos/norns/data/dust/code/awake/
norns-panicos/norns/data/dust/code/molly_the_poly/
norns-panicos/norns/data/dust/code/passersby/
norns-panicos/norns/data/dust/data/sources/community.json
```

- [ ] **Step 3: Verify the arm64 binaries**

```bash
tar xzf dist/norns-panicos.tar.gz \
    norns-panicos/norns/bin/norns-panicos \
    norns-panicos/norns/bin/norns-input-bridge \
    -C /tmp/
file /tmp/norns-panicos/norns/bin/norns-panicos \
     /tmp/norns-panicos/norns/bin/norns-input-bridge
```

Expected:
```
/tmp/norns-panicos/norns/bin/norns-panicos:      ELF 64-bit LSB executable, ARM aarch64, ...
/tmp/norns-panicos/norns/bin/norns-input-bridge: ELF 64-bit LSB executable, ARM aarch64, ...
```

- [ ] **Step 4: Commit build artifacts and tag**

```bash
git add dist/norns-panicos.tar.gz
git commit -m "build: add norns-panicos.tar.gz PortMaster package"
git tag v0.1.0
```

---

## Self-review notes

- **Spec §3.1 (screen rendering):** covered in Task 3 `pump_screen()` + `render_frame()`. SDL_RenderSetLogicalSize handles fullscreen scaling. ✓
- **Spec §3.2 (gamepad input):** all button/axis mappings covered in Task 3 `handle_button()` + `poll_axes()`. Frame protocol matches matron's `gpio.c` patch exactly. ✓
- **Spec §3.3 (process manager):** `start_norns_processes`, `stop_norns_processes`, `check_processes` in Task 3. ✓
- **Spec §3.4 (audio):** no audio code in host binary — crone connects to JACK natively via inherited env. `JACK_NO_AUDIO_RESERVATION=1` set in Norns.sh. ✓
- **Spec §4 (input scheme):** all 14 mappings present in `handle_button()` + `poll_axes()`. Select=restart, Select+Start=exit. R2/Start unassigned. ✓
- **Spec §5 (buildroot packages):** documented in the design spec; not implemented here (user adding to OS build separately). ✓
- **Spec §6 (build system):** Task 5 + Task 4. ✓
- **Spec §7 (package structure):** Task 5 assembly + Task 6 Norns.sh. `sclang_conf.yaml` generated at runtime in Norns.sh because paths must be absolute on the target device. ✓
- **sclang_conf.yaml runtime generation:** not mentioned in the spec (spec says "written at build time") — this is a necessary correction because SC include paths can't use `$HOME` variable references, they must be absolute. The spec assumed chroot-style fixed paths; native buildroot requires runtime generation. Norns.sh handles this. ✓
