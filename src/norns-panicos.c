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
 * Button mapping is configurable — see src/norns-controls.h and the config
 * file (path from $NORNS_PANICOS_CONF, else $HOME/controls.conf). Defaults
 * (dual-stick devices such as the RG35XX Pro):
 *   Y=K1  X=K2  A=K3  B=K1(alias)
 *   D-pad        = E1 (quick menu scroll, both axes)
 *   Left stick   = E2 (velocity-scaled, throttled)
 *   Right stick  = E3 (velocity-scaled, throttled)
 *   Menu/FN (tap)               = home (return to the norns menu)
 *   Menu/FN (hold) / Select+Menu = quit to PortMaster
 *   Select, Start, L3, R3       = free, mappable (default unbound)
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

#include "norns-controls.h"
#include "norns-hid.h"

/* ── Constants ──────────────────────────────────────────── */

#define GUIDE_HOLD_MS 800     /* hold Menu/FN this long to quit */
#define SYS_BUTTON    SDL_CONTROLLER_BUTTON_GUIDE   /* C1: swap to _START if GUIDE is swallowed */

#define NORNS_WIDTH      128
#define NORNS_HEIGHT     64
#define SCREEN_FRAME_SZ  (NORNS_WIDTH * NORNS_HEIGHT / 2)  /* 4096 bytes */

#define FIFO_SCREEN   "/tmp/norns-screen-1"
#define FIFO_INPUT    "/tmp/norns-input-1"
#define FIFO_HID      "/tmp/norns-hid-1"   /* native-mode evdev frames → matron */
#define FIFO_GRID     "/tmp/norns-grid-1"
#define FIFO_MIDI_IN     "/tmp/midi-to-chroot-1"
#define FIFO_MIDI_OUT    "/tmp/midi-from-chroot-1"
#define FIFO_SCREEN_P2   "/tmp/norns-screen-push2"  /* tee for Push 2 display */

#define OSC_MATRON_PORT  8888

/* Base scroll rate (detents/frame) for a held D-pad direction, before
 * acceleration. ~0.12 @ ~60fps ≈ 7/s; a quick tap stays a single detent. */
#define ENC_DPAD_BASE_RATE  0.12f

/* ── State ──────────────────────────────────────────────── */

/* Per-encoder drive state for the hold-to-accelerate model (poll_encoders). */
typedef struct {
    int8_t dir;          /* current held direction: -1, 0, +1            */
    int    held_frames;  /* consecutive frames held in `dir`             */
    float  phase;        /* fractional detent accumulator                */
} enc_drive_t;

typedef struct {
    SDL_Window         *window;
    SDL_Renderer       *renderer;
    SDL_Texture        *texture;
    SDL_GameController *gc;

    int screen_fd;
    int input_fd;
    int hid_fd;           /* native-mode evdev frames → matron NORNS_HID_FIFO */
    int push2_screen_fd;  /* tee of screen FIFO → norns-push2-display */

    controls_t  controls;     /* active resolved mapping (per context)      */
    char       *cfg_text;     /* retained config text for context re-resolve */
    char        cfg_scheme[32];/* active [scheme] name                       */
    char        context[64];  /* norns context: "menu" or the script name   */
    uint8_t     dpad_btn;     /* held D-pad direction bitmask               */
    uint8_t     shoulder_btn; /* held L1/R1 bitmask (bit0=L1, bit1=R1)      */
    enc_drive_t enc_drive[3]; /* per-encoder accel state                    */
    sysbtn_t sysbtn;          /* Menu/FN reserved system-button state */
    int     native_grab_was;  /* last /tmp/norns-native state (runtime pad.grab) */

    pid_t pid_jackd;
    pid_t pid_crone;
    pid_t pid_matron;
    pid_t pid_sclang;
    int   sclang_stdin_wr;   /* write end of sclang's stdin pipe — never close while running */
    pid_t pid_input_bridge;
    pid_t pid_maiden;
    pid_t pid_push2_display;

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
    s->screen_fd       = make_fifo(FIFO_SCREEN);
    s->input_fd        = make_fifo(FIFO_INPUT);
    s->hid_fd          = make_fifo(FIFO_HID);
    s->push2_screen_fd = make_fifo(FIFO_SCREEN_P2);
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
    frame[3] = (uint8_t)(((uint16_t)delta >> 8) & 0xFF);
    (void)write(s->input_fd, frame, 4);
}

static void send_key(norns_state_t *s, uint8_t key_id, uint8_t state) {
    uint8_t frame[4] = { 1, key_id, state, 0 };
    (void)write(s->input_fd, frame, 4);
}

/* Emit one evdev event on the native HID FIFO (non-blocking; dropped if full). */
static void send_hid(norns_state_t *s, uint16_t type, uint16_t code, int16_t value) {
    if (s->hid_fd < 0) return;
    uint8_t frame[HID_FRAME_SZ];
    hid_frame_encode(type, code, value, frame);
    (void)write(s->hid_fd, frame, HID_FRAME_SZ);
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

/* ── Forward declarations (needed by pump_for_ms before their definitions) ── */
static void pump_screen(norns_state_t *s);
static void render_frame(norns_state_t *s);
static void handle_button(norns_state_t *s, SDL_ControllerButtonEvent *ev);
static void poll_encoders(norns_state_t *s);
static void poll_native_axes(norns_state_t *s);

/* Run the display loop for `ms` milliseconds — keeps the screen live
 * during startup waits so the norns splash animation is visible while
 * SuperCollider boots.  Processes SDL_QUIT so the user can abort early. */
static void pump_for_ms(norns_state_t *s, int ms) {
    Uint32 deadline = SDL_GetTicks() + (Uint32)ms;
    SDL_Event ev;
    while (s->running && SDL_GetTicks() < deadline) {
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { s->running = 0; return; }
            if (ev.type == SDL_CONTROLLERBUTTONDOWN || ev.type == SDL_CONTROLLERBUTTONUP)
                handle_button(s, &ev.cbutton);
            if (ev.type == SDL_CONTROLLERDEVICEADDED && !s->gc)
                s->gc = SDL_GameControllerOpen(ev.cdevice.which);
        }
        poll_encoders(s);
        poll_native_axes(s);
        pump_screen(s);
        render_frame(s);
        SDL_Delay(16);  /* ~60fps during wait */
    }
}

/* ── Process management ─────────────────────────────────── */

static pid_t spawn_proc(const char *path, char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) { log_msg("fork failed"); return -1; }
    if (pid == 0) {
        /* Child: inject FIFO env vars, then exec.
         * Redirect stdin from /dev/null — none of our child processes need it,
         * and sclang's REPL thread quits immediately if stdin is a broken pipe
         * (ENOTSUP), which PortMaster's pm_platform_helper provides. */
        int dn = open("/dev/null", O_RDONLY);
        if (dn >= 0) { dup2(dn, STDIN_FILENO); close(dn); }
        setenv("NORNS_SCREEN_FIFO",  FIFO_SCREEN,  1);
        setenv("NORNS_INPUT_FIFO",   FIFO_INPUT,   1);
        setenv("NORNS_HID_FIFO",     FIFO_HID,     1);
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

    /* 0. jackd — start before crone so we own its lifecycle.
     * Playback-only on hw:0 (H616 codec); ALSA falls back gracefully when
     * no capture device is available.  scsynth connects to this jackd for
     * audio output even when crone crashes on ADC port connection. */
    { char *av[] = { "jackd", "-T", "-d", "alsa", "-r", "48000", "-p", "1024", "-n", "2", NULL };
      s->pid_jackd = spawn_proc("jackd", av); }
    log_msg("jackd started");
    pump_for_ms(s, 2000);  /* wait for JACK to initialise before crone connects */

    /* 1. crone (JACK audio routing — must start before sclang) */
    { char *av[] = { crone_path, NULL };
      s->pid_crone = spawn_proc(crone_path, av); }
    log_msg("crone started");
    pump_for_ms(s, 1000);

    /* 2. matron (Lua VM + Cairo) via ws-wrapper */
    { char *av[] = { ws_path, "ws://*:5555", matron_path, NULL };
      s->pid_matron = spawn_proc(ws_path, av); }
    log_msg("matron started");
    pump_for_ms(s, 1000);

    /* 3. sclang — give it a persistent pipe as stdin so the REPL thread
     * never gets EOF/ENOTSUP (both cause an immediate quit).  The parent
     * holds the write end open for the lifetime of the norns session. */
    { int pp[2] = {-1, -1};
      if (pipe(pp) != 0) { pp[0] = pp[1] = -1; }
      pid_t pid = fork();
      if (pid < 0) { log_msg("fork failed"); }
      if (pid == 0) {
          /* child: read end → stdin, then exec sclang */
          if (pp[0] >= 0) { dup2(pp[0], STDIN_FILENO); close(pp[0]); }
          if (pp[1] >= 0) close(pp[1]);
          int dn = open("/dev/null", O_RDONLY);
          /* only use /dev/null if pipe creation failed */
          if (pp[0] < 0 && dn >= 0) dup2(dn, STDIN_FILENO);
          if (dn >= 0) close(dn);
          setenv("NORNS_SCREEN_FIFO",  FIFO_SCREEN,  1);
          setenv("NORNS_INPUT_FIFO",   FIFO_INPUT,   1);
          setenv("NORNS_HID_FIFO",     FIFO_HID,     1);
          setenv("NORNS_MIDI_OUT_FIFO",FIFO_MIDI_OUT,1);
          setsid();
          char *av[] = { "sclang", "-l", sclang_conf, NULL };
          execvp("sclang", av);
          fprintf(stderr, "[norns-panicos] execvp sclang: %s\n", strerror(errno));
          _exit(127);
      }
      /* parent: close read end, keep write end so sclang never sees EOF */
      if (pp[0] >= 0) close(pp[0]);
      if (s->sclang_stdin_wr >= 0) close(s->sclang_stdin_wr);
      s->sclang_stdin_wr = pp[1];
      s->pid_sclang = pid; }
    log_msg("sclang started");
    pump_for_ms(s, 20000);

    /* 4. /crone/ready fallback — in case Crone.sc missed the window */
    send_crone_ready();
    pump_for_ms(s, 1000);

    /* 5/6. External-device bridge.
     * Handheld variant ($NORNS_USE_MONOME=1): a real monome-protocol grid is
     * driven over its serial port by norns-monome-bridge, which OWNS the grid
     * FIFO — so the Push 2 input-bridge/display must NOT also run (two readers
     * would corrupt grid frames). Otherwise (Move): Push 2 bridge + display. */
    const char *use_monome = getenv("NORNS_USE_MONOME");
    if (use_monome && use_monome[0] == '1') {
        char mb_path[512];
        snprintf(mb_path, sizeof(mb_path), "%s/norns-monome-bridge", s->bin_dir);
        char *av[] = { mb_path, NULL };   /* auto-detects the grid; FIFOs via env */
        s->pid_input_bridge = spawn_proc(mb_path, av);
        log_msg("norns-monome-bridge started");
    } else {
        { char *av[] = { bridge_path, FIFO_INPUT, FIFO_MIDI_IN, NULL };
          s->pid_input_bridge = spawn_proc(bridge_path, av); }
        log_msg("norns-input-bridge started");

        { char p2_path[512];
          snprintf(p2_path, sizeof(p2_path), "%s/norns-push2-display", s->bin_dir);
          char *av[] = { p2_path, FIFO_SCREEN_P2, NULL };
          s->pid_push2_display = spawn_proc(p2_path, av); }
        log_msg("norns-push2-display started");
    }

    /* 7. maiden (web IDE on port 5000) */
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
                     s->pid_input_bridge, s->pid_maiden, s->pid_push2_display,
                     s->pid_jackd };
    for (int i = 0; i < 7; i++) {
        if (pids[i] > 0) kill(-pids[i], SIGTERM);   /* SIGTERM to process group */
    }
    sleep(2);
    for (int i = 0; i < 6; i++) {
        if (pids[i] > 0) {
            if (waitpid(pids[i], NULL, WNOHANG) == 0) {
                kill(-pids[i], SIGKILL);              /* SIGKILL to process group */
                waitpid(pids[i], NULL, 0);
            }
        }
    }
    /* Close the sclang stdin pipe so sclang gets EOF and exits cleanly */
    if (s->sclang_stdin_wr >= 0) { close(s->sclang_stdin_wr); s->sclang_stdin_wr = -1; }
    s->pid_jackd = s->pid_crone = s->pid_matron = s->pid_sclang = -1;
    s->pid_input_bridge = s->pid_maiden = s->pid_push2_display = -1;
}

static void restart_norns(norns_state_t *s) {
    log_msg("restarting norns...");
    stop_norns_processes(s);
    start_norns_processes(s);
}

static void check_processes(norns_state_t *s) {
    int status;
    /* Reap crone if it exited (ADC port failure on playback-only devices) but
     * don't restart — crone crashing doesn't take down scsynth or the Lua VM.
     * The crone-adc-optional patch in apply-move-patches.sh fixes this properly
     * once the norns prebuilt is rebuilt from source. */
    if (s->pid_crone > 0 && waitpid(s->pid_crone, &status, WNOHANG) > 0) {
        log_msg("crone exited (ADC port unavailable) — continuing without audio input");
        s->pid_crone = -1;
    }
    if (s->pid_matron > 0 && waitpid(s->pid_matron, &status, WNOHANG) > 0) {
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
        if (n == (ssize_t)SCREEN_FRAME_SZ) {
            memcpy(latest, packed, SCREEN_FRAME_SZ);
            got = 1;
        } else if (n > 0) {
            log_msg("screen: partial frame — skipping");
            break;
        } else {
            break;  /* EAGAIN or error */
        }
    }
    if (!got) return;

    /* Tee raw frame to Push 2 display process (non-blocking, drop if full) */
    if (s->push2_screen_fd >= 0)
        (void)write(s->push2_screen_fd, latest, SCREEN_FRAME_SZ);

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

/* Map an SDL button to our key-binding bit, or 0 if it isn't key-bindable.
 * (D-pad = encoders; L2/R2 = analog triggers; GUIDE = host-reserved.) */
static uint16_t sdl_btn_bit(Uint8 button) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:             return BTN_A;
    case SDL_CONTROLLER_BUTTON_B:             return BTN_B;
    case SDL_CONTROLLER_BUTTON_X:             return BTN_X;
    case SDL_CONTROLLER_BUTTON_Y:             return BTN_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return BTN_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return BTN_R1;
    case SDL_CONTROLLER_BUTTON_BACK:          return BTN_SELECT;
    case SDL_CONTROLLER_BUTTON_START:         return BTN_START;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return BTN_L3;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return BTN_R3;
    default:                                  return 0;
    }
}

/* SDL button → abstract pad input, or -1 if it isn't a pad button we expose to
 * native scripts. (L2/R2 are analog triggers, handled in poll_native_axes.) */
static int sdl_btn_to_pad(Uint8 button) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:             return PAD_A;
    case SDL_CONTROLLER_BUTTON_B:             return PAD_B;
    case SDL_CONTROLLER_BUTTON_X:             return PAD_X;
    case SDL_CONTROLLER_BUTTON_Y:             return PAD_Y;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  return PAD_L1;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return PAD_R1;
    case SDL_CONTROLLER_BUTTON_LEFTSTICK:     return PAD_L3;
    case SDL_CONTROLLER_BUTTON_RIGHTSTICK:    return PAD_R3;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:       return PAD_DUP;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     return PAD_DDOWN;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     return PAD_DLEFT;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    return PAD_DRIGHT;
    case SDL_CONTROLLER_BUTTON_BACK:          return PAD_SELECT;
    case SDL_CONTROLLER_BUTTON_START:         return PAD_START;
    default:                                  return -1;
    }
}

/* Emit a pad button as a native HID key event. */
static void send_pad_button(norns_state_t *s, int pad, int pressed) {
    uint16_t type, code;
    if (pad >= 0 && pad_input_to_evdev((pad_input_t)pad, &type, &code))
        send_hid(s, type, code, (int16_t)(pressed ? 1 : 0));
}

/* Held button-pair tracking. handle_button only updates these bitmasks (and emits
 * the immediate tap detent, so a quick press isn't missed by the per-frame poll);
 * poll_encoders turns held directions into accelerating encoder motion. */
#define DPADBIT_UP    0x1
#define DPADBIT_DOWN  0x2
#define DPADBIT_LEFT  0x4
#define DPADBIT_RIGHT 0x8
#define SHBIT_L1      0x1
#define SHBIT_R1      0x2
#define TRIGGER_THRESH 8192   /* L2/R2 analog-trigger press threshold (0..32767) */

/* Held direction of each button-pair: +1 / -1 / 0 (right/up/R = +). */
static int dpad_dir_x(uint8_t b)     { return (b & DPADBIT_RIGHT) ? 1 : (b & DPADBIT_LEFT) ? -1 : 0; }
static int dpad_dir_y(uint8_t b)     { return (b & DPADBIT_UP)    ? 1 : (b & DPADBIT_DOWN) ? -1 : 0; }
static int shoulder_dir(uint8_t b)   { return (b & SHBIT_R1)      ? 1 : (b & SHBIT_L1)     ? -1 : 0; }

/* Emit one detent for a button-pair press on the EVENT, so a quick tap is never
 * missed by the per-frame poll. poll_encoders adds the hold-acceleration. */
static void enc_tap(norns_state_t *s, int e, int sign) {
    if (e >= 0) send_enc(s, (uint8_t)e, (int16_t)(sign * s->controls.dpad_step));
}

/* Resolve the active mapping for the current context: built-in defaults, then
 * the global config + the selected [scheme], then the context overlay — [menu]
 * when in the menu, or [script:NAME] for the running script. The overlay only
 * needs to list what differs. */
static void resolve_controls(norns_state_t *s) {
    controls_defaults(&s->controls);
    if (!s->cfg_text) return;
    controls_parse_scheme(&s->controls, s->cfg_text,
                          s->cfg_scheme[0] ? s->cfg_scheme : NULL);
    if (s->context[0]) {
        char sec[80];
        if (strcmp(s->context, "menu") == 0) snprintf(sec, sizeof(sec), "menu");
        else snprintf(sec, sizeof(sec), "script:%s", s->context);
        controls__canon(sec);   /* match the canonicalised section header */
        controls_parse_scheme(&s->controls, s->cfg_text, sec);
    }
}

/* norns' context ("menu" or the script name) is written on each transition by
 * the patched menu.lua. Poll it cheaply (~every 6 frames); on change, re-resolve
 * the mapping so per-context overlays take effect. */
#define CONTEXT_FILE "/tmp/norns-context"
#define NATIVE_FILE  "/tmp/norns-native"   /* pad.grab()/release() runtime opt-in */
static void poll_context(norns_state_t *s) {
    if (s->frame % 6 != 0) return;
    int fd = open(CONTEXT_FILE, O_RDONLY);
    if (fd < 0) return;
    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' ')) buf[--n] = '\0';
    if (strcmp(buf, s->context) != 0) {
        snprintf(s->context, sizeof(s->context), "%s", buf);
        resolve_controls(s);
        unlink(NATIVE_FILE);          /* a script that exited can't strand native */
        s->native_grab_was = 0;
        fprintf(stderr, "[norns-panicos] context: %s\n", s->context);
    }
}

/* Runtime native opt-in: pad.grab() writes "1" to /tmp/norns-native, pad.release()
 * clears it. This overrides the config-derived native_mode for the running script
 * (so a script you author can go native with no config edit). Polled with context. */
static void poll_native_override(norns_state_t *s) {
    if (s->frame % 6 != 0) return;
    int fd = open(NATIVE_FILE, O_RDONLY);
    if (fd < 0) return;
    char buf[8] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    int want = (n > 0 && buf[0] == '1');
    if (want)                       s->controls.native_mode = 1;   /* grab forces on */
    else if (s->native_grab_was)    s->controls.native_mode = 0;   /* explicit release */
    s->native_grab_was = want;
}

/* In native mode, mirror the analog sticks (as ABS axes) and the L2/R2 triggers
 * (as buttons) to HID. Emits only on change so the FIFO isn't flooded. */
static void poll_native_axes(norns_state_t *s) {
    if (!s->controls.native_mode || !s->gc) return;
    static int last[6];   /* LX, LY, RX, RY, L2, R2 */
    const struct { SDL_GameControllerAxis ax; uint16_t code; } sticks[4] = {
        { SDL_CONTROLLER_AXIS_LEFTX,  EVA_ABS_X  },
        { SDL_CONTROLLER_AXIS_LEFTY,  EVA_ABS_Y  },
        { SDL_CONTROLLER_AXIS_RIGHTX, EVA_ABS_RX },
        { SDL_CONTROLLER_AXIS_RIGHTY, EVA_ABS_RY },
    };
    for (int i = 0; i < 4; i++) {
        int v = SDL_GameControllerGetAxis(s->gc, sticks[i].ax);
        if (v != last[i]) { last[i] = v; send_hid(s, EV_ABS, sticks[i].code, (int16_t)v); }
    }
    int l2 = SDL_GameControllerGetAxis(s->gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > TRIGGER_THRESH;
    int r2 = SDL_GameControllerGetAxis(s->gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > TRIGGER_THRESH;
    if (l2 != last[4]) { last[4] = l2; send_hid(s, EV_KEY, EVB_BTN_TL2, (int16_t)l2); }
    if (r2 != last[5]) { last[5] = r2; send_hid(s, EV_KEY, EVB_BTN_TR2, (int16_t)r2); }
}

/* +1 normally; -1 flips D-pad up/down when the active config asks (the menu and
 * any opted-in script set dpad_y_invert). */
static int dpad_y_flip(const norns_state_t *s) {
    return s->controls.dpad_y_invert ? -1 : 1;
}

static void apply_sys_action(norns_state_t *s, sys_action_t a) {
    if (a == SYS_QUIT) {
        s->running = 0;
    } else if (a == SYS_HOME) {
        send_key(s, 0, 1); send_key(s, 0, 0);   /* inject a K1 tap → norns home (C2) */
    }
}

/* Returns 1 if the event was consumed as a reserved system action. */
static int handle_system_button(norns_state_t *s, SDL_ControllerButtonEvent *ev, int pressed) {
    if (ev->button == SYS_BUTTON) {
        int sel = SDL_GameControllerGetButton(s->gc, SDL_CONTROLLER_BUTTON_BACK);
        apply_sys_action(s, sysbtn_guide(&s->sysbtn, pressed, sel, SDL_GetTicks(), GUIDE_HOLD_MS));
        return 1;   /* GUIDE is always host-reserved */
    }
    if (ev->button == SDL_CONTROLLER_BUTTON_BACK && pressed && s->sysbtn.guide_held) {
        apply_sys_action(s, sysbtn_select_down(&s->sysbtn));
        return 1;   /* Select consumed ONLY as the Select+Menu chord (down only).
                     * The matching Select-up isn't consumed, but every chord path
                     * sets running=0 (quit), so the app exits before that up is
                     * processed — no spurious key-up reaches a bound Select. */
    }
    return 0;
}

static void handle_button(norns_state_t *s, SDL_ControllerButtonEvent *ev) {
    if (!s->gc) return;
    int pressed = (ev->type == SDL_CONTROLLERBUTTONDOWN);
    uint16_t bit = sdl_btn_bit(ev->button);

    /* Reserved system button (Menu/FN) — handled before everything else and
     * never reaches the script or the key map. */
    if (handle_system_button(s, ev, pressed)) return;

    /* Native mode: the running script owns the raw pad (incl. Select/Start now). */
    if (s->controls.native_mode) {
        int pad = sdl_btn_to_pad(ev->button);
        if (pad >= 0) send_pad_button(s, pad, pressed);
        return;
    }

    /* L1/R1 as an encoder pair take precedence over any key binding on them. */
    if ((bit == BTN_L1 || bit == BTN_R1) && s->controls.shoulder_enc >= 0) {
        int sign = (bit == BTN_R1) ? 1 : -1;
        if (pressed) { s->shoulder_btn |= (bit == BTN_R1 ? SHBIT_R1 : SHBIT_L1);
                       enc_tap(s, s->controls.shoulder_enc, sign); }
        else           s->shoulder_btn &= ~(bit == BTN_R1 ? SHBIT_R1 : SHBIT_L1);
        return;
    }

    /* Face/shoulder buttons → norns keys (every key bound to this button fires). */
    if (bit) {
        for (int k = 0; k < 3; k++)
            if (s->controls.key_btn[k] & bit) send_key(s, (uint8_t)k, (uint8_t)pressed);
        return;
    }

    switch (ev->button) {
    /* D-pad: emit the tap detent now (never missed), and track the held
     * direction so poll_encoders can add hold-acceleration. */
    case SDL_CONTROLLER_BUTTON_DPAD_UP:    if (pressed) { s->dpad_btn |= DPADBIT_UP;    enc_tap(s, s->controls.dpad_enc[1],  dpad_y_flip(s)); } else s->dpad_btn &= ~DPADBIT_UP;    break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  if (pressed) { s->dpad_btn |= DPADBIT_DOWN;  enc_tap(s, s->controls.dpad_enc[1], -dpad_y_flip(s)); } else s->dpad_btn &= ~DPADBIT_DOWN;  break;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  if (pressed) { s->dpad_btn |= DPADBIT_LEFT;  enc_tap(s, s->controls.dpad_enc[0], -1); } else s->dpad_btn &= ~DPADBIT_LEFT;  break;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: if (pressed) { s->dpad_btn |= DPADBIT_RIGHT; enc_tap(s, s->controls.dpad_enc[0],  1); } else s->dpad_btn &= ~DPADBIT_RIGHT; break;

    default: break;
    }
}

/* SDL axis for the stick driving encoder e. */
static SDL_GameControllerAxis stick_axis_for_enc(const controls_t *c, int e) {
    int left = controls_enc_is_left_stick(c, e);
    int y    = controls_enc_reads_y(c, e);   /* Y or inverted-Y both read the Y axis */
    if (left) return y ? SDL_CONTROLLER_AXIS_LEFTY  : SDL_CONTROLLER_AXIS_LEFTX;
    else      return y ? SDL_CONTROLLER_AXIS_RIGHTY : SDL_CONTROLLER_AXIS_RIGHTX;
}

/* Unified per-encoder drive, evaluated every frame. Each encoder is driven by a
 * button-pair (D-pad axis, L1/R1, or L2/R2) and/or an analog stick, all feeding
 * one acceleration model: hold longer → spin faster. Button-pairs share the
 * D-pad's (snappier) accel curve and emit their first detent on the button event
 * (handle_button) so quick taps never miss; sticks and the L2/R2 triggers emit
 * their first detent here and use the gentler stick accel curve. A button-pair
 * targeting an encoder takes precedence over a stick on the same encoder. */
static void poll_encoders(norns_state_t *s) {
    const controls_t *c = &s->controls;
    poll_context(s);
    poll_native_override(s);        /* may flip native_mode (pad.grab) — before guard */
    apply_sys_action(s, sysbtn_tick(&s->sysbtn, SDL_GetTicks(), GUIDE_HOLD_MS));
    if (c->native_mode) return;     /* native mode: no encoder/key emulation */

    /* Resolve every button-pair to a per-encoder held direction. pair_event[e]=1
     * means the tap was already emitted on the event (D-pad / shoulders). */
    int pair_dir[3]   = { 0, 0, 0 };
    int pair_event[3] = { 0, 0, 0 };
    int dx = dpad_dir_x(s->dpad_btn);
    int dy = dpad_dir_y(s->dpad_btn) * dpad_y_flip(s);  /* flipped in the menu */
    if (c->dpad_enc[0] >= 0 && dx) { pair_dir[c->dpad_enc[0]] = dx; pair_event[c->dpad_enc[0]] = 1; }
    if (c->dpad_enc[1] >= 0 && dy && !pair_dir[c->dpad_enc[1]]) { pair_dir[c->dpad_enc[1]] = dy; pair_event[c->dpad_enc[1]] = 1; }
    int sh = shoulder_dir(s->shoulder_btn);
    if (c->shoulder_enc >= 0 && sh && !pair_dir[c->shoulder_enc]) { pair_dir[c->shoulder_enc] = sh; pair_event[c->shoulder_enc] = 1; }
    if (c->trigger_enc >= 0 && !pair_dir[c->trigger_enc] && s->gc) {
        int l2 = SDL_GameControllerGetAxis(s->gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int r2 = SDL_GameControllerGetAxis(s->gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        int tg = (r2 > TRIGGER_THRESH) ? 1 : (l2 > TRIGGER_THRESH) ? -1 : 0;
        if (tg) pair_dir[c->trigger_enc] = tg;   /* poll-tapped (analog axis) */
    }

    for (int e = 0; e < 3; e++) {
        int   dir   = 0;
        float base  = 0.0f;   /* detents/frame at the un-accelerated rate */
        int   delay = c->stick_accel_delay, ramp = c->stick_accel_ramp;
        int   from_event = 0;

        if (pair_dir[e] != 0) {                 /* a button-pair drives this encoder */
            dir = pair_dir[e];
            base = ENC_DPAD_BASE_RATE;
            delay = c->accel_delay; ramp = c->accel_ramp;
            from_event = pair_event[e];
        } else if (controls_enc_is_stick(c, e) && s->gc) {
            int m;
            if (controls_enc_is_stick_xy(c, e)) {
                /* Sum both axes independently (no angle): up OR right = +. */
                int left = controls_enc_is_left_stick(c, e);
                int vx = SDL_GameControllerGetAxis(s->gc,
                    left ? SDL_CONTROLLER_AXIS_LEFTX : SDL_CONTROLLER_AXIS_RIGHTX);
                int vy = -SDL_GameControllerGetAxis(s->gc,
                    left ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_RIGHTY);
                m = controls_stick_delta(c, vx) + controls_stick_delta(c, vy);
            } else {
                int v = SDL_GameControllerGetAxis(s->gc, stick_axis_for_enc(c, e));
                if (controls_enc_is_stick_y(c, e)) v = -v;  /* push up = increment */
                m = controls_stick_delta(c, v);              /* -2..2, deadzone-aware */
            }
            dir  = (m > 0) - (m < 0);
            base = (float)(m < 0 ? -m : m) / (float)c->stick_throttle;
        }

        enc_drive_t *d = &s->enc_drive[e];
        if (dir == 0) { d->dir = 0; d->held_frames = 0; d->phase = 0.0f; continue; }
        if (dir != d->dir) {                 /* new press / direction reversal */
            d->dir = (int8_t)dir; d->held_frames = 0; d->phase = 0.0f;
            if (!from_event)   /* sticks + L2/R2 triggers emit their first detent here */
                send_enc(s, (uint8_t)e, (int16_t)(dir * c->dpad_step));
            continue;
        }
        d->held_frames++;
        d->phase += base * controls_accel_factor(c, d->held_frames, delay, ramp);
        int steps = (int)d->phase;
        if (steps > 0) {
            d->phase -= (float)steps;
            send_enc(s, (uint8_t)e, (int16_t)(dir * steps * c->dpad_step));
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

/* Debug: when /tmp/norns-input-debug exists, log raw SDL input events so we can
 * see exactly what a control (e.g. the D-pad) emits — controller button, axis,
 * or underlying joystick hat/button. Cheap: gated, and most events are sparse. */
static void debug_log_event(const SDL_Event *ev) {
    static int dbg = -1;
    if (dbg < 0) dbg = (access("/tmp/norns-input-debug", F_OK) == 0) ? 1 : 0;
    if (!dbg) return;
    switch (ev->type) {
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        fprintf(stderr, "[indbg] cbutton=%d %s\n", ev->cbutton.button,
                ev->type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up"); break;
    case SDL_CONTROLLERAXISMOTION:
        if (abs(ev->caxis.value) > 8000)
            fprintf(stderr, "[indbg] caxis=%d val=%d\n", ev->caxis.axis, ev->caxis.value);
        break;
    case SDL_JOYHATMOTION:
        fprintf(stderr, "[indbg] jhat=%d val=%d\n", ev->jhat.hat, ev->jhat.value); break;
    case SDL_JOYBUTTONDOWN:
        fprintf(stderr, "[indbg] jbutton=%d\n", ev->jbutton.button); break;
    case SDL_JOYAXISMOTION:
        if (abs(ev->jaxis.value) > 12000)
            fprintf(stderr, "[indbg] jaxis=%d val=%d\n", ev->jaxis.axis, ev->jaxis.value);
        break;
    }
}

/* ── Main ───────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_IGN);

    norns_state_t s;
    memset(&s, 0, sizeof(s));
    s.screen_fd = s.input_fd = s.push2_screen_fd = -1;
    s.sclang_stdin_wr = -1;
    s.pid_jackd = s.pid_crone = s.pid_matron = s.pid_sclang = -1;
    s.pid_input_bridge = s.pid_maiden = s.pid_push2_display = -1;
    s.running = 1;
    s.frame   = 0;

    get_paths(&s);
    {
        char msg[512];
        snprintf(msg, sizeof(msg), "norns_dir=%s  bin_dir=%s", s.norns_dir, s.bin_dir);
        log_msg(msg);
    }

    /* Control mapping: read the config text once and keep it, so the active
     * mapping can be re-resolved per context (menu / per-script overlays) at
     * runtime. The `scheme = <name>` line picks the base layout. */
    {
        const char *cfg = getenv("NORNS_PANICOS_CONF");
        char fallback[600];
        if (!cfg || !*cfg) {
            const char *home = getenv("HOME");
            snprintf(fallback, sizeof(fallback), "%s/controls.conf", home ? home : "/tmp");
            cfg = fallback;
        }
        FILE *f = fopen(cfg, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0 && sz < 65536 && (s.cfg_text = malloc((size_t)sz + 1))) {
                size_t rd = fread(s.cfg_text, 1, (size_t)sz, f);
                s.cfg_text[rd] = '\0';
                controls_find_scheme(s.cfg_text, s.cfg_scheme, sizeof(s.cfg_scheme));
            }
            fclose(f);
        }
        resolve_controls(&s);   /* base mapping; context overlays applied at runtime */
        char msg[800];
        snprintf(msg, sizeof(msg), "controls: %s (scheme: %s)",
                 s.cfg_text ? cfg : "built-in defaults",
                 s.cfg_scheme[0] ? s.cfg_scheme : "default");
        log_msg(msg);
    }

    if (create_fifos(&s) != 0) return 1;
    if (init_sdl(&s)     != 0) return 1;

    start_norns_processes(&s);

    SDL_Event ev;
    while (s.running) {
        while (SDL_PollEvent(&ev)) {
            debug_log_event(&ev);
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
            if (ev.type == SDL_CONTROLLERDEVICEREMOVED && s.gc) {
                SDL_GameControllerClose(s.gc);
                s.gc = NULL;
                log_msg("game controller disconnected");
            }
        }

        poll_encoders(&s);
        poll_native_axes(&s);
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

    if (s.screen_fd       >= 0) close(s.screen_fd);
    if (s.input_fd        >= 0) close(s.input_fd);
    if (s.push2_screen_fd >= 0) close(s.push2_screen_fd);
    unlink(FIFO_SCREEN); unlink(FIFO_INPUT); unlink(FIFO_SCREEN_P2);
    unlink(FIFO_GRID); unlink(FIFO_MIDI_IN); unlink(FIFO_MIDI_OUT);

    return 0;
}
