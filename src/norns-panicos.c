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
