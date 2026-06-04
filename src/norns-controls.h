/*
 * norns-controls.h — gamepad → norns control mapping, configurable at runtime.
 *
 * Header-only on purpose: the build compiles each binary from a single .c, and
 * the test harness is standalone native C. Keeping the parser + pure mapping
 * helpers here (no SDL) lets BOTH norns-panicos.c and tests/test_controls.c
 * include the *same* code, so the tests exercise the real logic, not a copy.
 *
 * Config format — named schemes selected by one line at the top:
 *
 *     scheme = sticks        # which layout below to use
 *
 *     # Global settings (apply to every scheme unless a scheme overrides them):
 *     k1 = y, b
 *     k2 = x
 *     k3 = a
 *     dpad_step      = 2
 *     stick_deadzone = 8192
 *
 *     [sticks]               # D-pad=E1, left stick=E2, right stick=E3
 *     e1 = dpad
 *     e2 = lstick
 *     e3 = rstick
 *
 *     [dpad]                 # up/down=E1, left/right=E2, L1/R1=E3 (no sticks)
 *     e1 = dpad-y
 *     e2 = dpad-x
 *     e3 = shoulders
 *
 * Lines before the first [section] are "global". The selected [scheme] section
 * is applied on top of globals. A flat file with no sections still works (every
 * line is global) — backward-compatible. Missing file / unknown scheme → the
 * built-in defaults (the `sticks` layout).
 *
 * Encoder sources: dpad | dpad-x | dpad-y | shoulders | none
 *                  lstick | lstick-y | lstick-y-inv | lstick-xy
 *                  rstick | rstick-y | rstick-y-inv | rstick-xy
 *   (…-y up = increment; …-y-inv down = increment; …-xy sums both axes so up OR
 *    right increments, down OR left decrements)
 * Key buttons:     a b x y l1 r1   (comma-separated, multiple per key)
 * Tunables:        dpad_step, stick_deadzone, stick_throttle, stick_invert
 */
#ifndef NORNS_CONTROLS_H
#define NORNS_CONTROLS_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>

/* Analog/discrete source feeding one norns encoder. The D-pad is NOT an encoder
 * source — it is an independent 2-axis input mapped via dpad_x/dpad_y (so a
 * D-pad axis and a stick can drive the same encoder). */
typedef enum {
    ENC_SRC_NONE = 0,
    ENC_SRC_LSTICK,      /* left stick X  (analog, velocity-scaled)           */
    ENC_SRC_LSTICK_Y,    /* left stick Y  (up = increment)                    */
    ENC_SRC_RSTICK,      /* right stick X                                     */
    ENC_SRC_RSTICK_Y,    /* right stick Y (up = increment)                    */
    ENC_SRC_LSTICK_XY,   /* left stick both axes summed (up+right = +)        */
    ENC_SRC_RSTICK_XY,   /* right stick both axes summed (up+right = +)       */
    ENC_SRC_LSTICK_Y_INV,/* left stick Y, inverted (down = increment)         */
    ENC_SRC_RSTICK_Y_INV,/* right stick Y, inverted (down = increment)        */
} enc_source_t;

/* Buttons bindable to keys (bitmask, uint16_t). L2/R2 are analog triggers, not
 * buttons. GUIDE (Menu/FN) is host-reserved and intentionally NOT bindable. */
enum {
    BTN_A      = 1 << 0,
    BTN_B      = 1 << 1,
    BTN_X      = 1 << 2,
    BTN_Y      = 1 << 3,
    BTN_L1     = 1 << 4,
    BTN_R1     = 1 << 5,
    BTN_SELECT = 1 << 6,
    BTN_START  = 1 << 7,
    BTN_L3     = 1 << 8,   /* left stick click  */
    BTN_R3     = 1 << 9,   /* right stick click */
};

typedef struct {
    enc_source_t enc[3];      /* analog (stick) source for E1, E2, E3          */
    /* Button-pair inputs → encoder index (0-2, -1=none), each a −/+ pair driven
     * with the same tap+accel logic. The D-pad is two pairs (x: left/right,
     * y: up/down); shoulders = L1/R1; triggers = L2/R2. */
    int8_t       dpad_enc[2]; /* [0]=left/right, [1]=up/down */
    int8_t       shoulder_enc;/* L1 = −, R1 = +              */
    int8_t       trigger_enc; /* L2 = −, R2 = +              */
    uint16_t     key_btn[3];  /* button bitmask mapped to K1, K2, K3           */
    int          dpad_step;   /* encoder delta per emitted detent              */
    int          stick_deadzone;
    int          stick_throttle;
    int          stick_invert;
    /* Hold-to-accelerate: rate ramps the longer an input is held in one
     * direction. The D-pad and sticks have separate delay/ramp so the analog
     * sticks can start later and ramp more gently. accel_max is shared. */
    int          accel;             /* 1 = enabled, 0 = constant rate          */
    int          accel_delay;       /* D-pad: frames at base before ramping    */
    int          accel_ramp;        /* D-pad: frames to ramp up to accel_max   */
    int          stick_accel_delay; /* sticks: frames before ramping           */
    int          stick_accel_ramp;  /* sticks: frames to ramp up               */
    int          accel_max;         /* peak rate multiplier (both)             */
    /* Flip the D-pad up/down direction. A plain flag in the active config; the
     * context harness turns it on for the menu (which scrolls down=+) and for
     * any [script:NAME] that asks, leaving other contexts unaffected. */
    int          dpad_y_invert;
    /* Phase 3: when set by a [script:NAME] overlay, the host routes the pad to
     * the native HID FIFO and suppresses K/E emulation for that script. */
    int          native_mode;
} controls_t;

/* Info about a load, for logging. */
typedef struct {
    int  file_found;       /* config file existed and was read                */
    int  scheme_found;     /* the selected [scheme] section existed           */
    int  applied;          /* assignments applied                             */
    char scheme[32];       /* active scheme name ("" if none requested)       */
} controls_load_info_t;

/* ── Host-reserved system button (Menu/FN = GUIDE) ───────────────────────────
 * Pure, SDL-free so the host AND tests share it. The host feeds events
 * (GUIDE down/up, Select-down while GUIDE held, per-frame tick) plus SDL_GetTicks()
 * as `now_ms`; this returns the system action to perform. */
typedef enum { SYS_NONE = 0, SYS_HOME, SYS_QUIT } sys_action_t;

typedef struct {
    int      guide_held;
    uint32_t guide_down_ms;
    int      consumed;       /* a QUIT already fired this hold → suppress HOME */
} sysbtn_t;

/* GUIDE pressed (down!=0) or released. select_held = is Select down right now. */
static inline sys_action_t sysbtn_guide(sysbtn_t *s, int down, int select_held,
                                        uint32_t now_ms, uint32_t hold_ms) {
    (void)hold_ms;   /* unused here; kept for a uniform timing-context signature */
    if (down) {
        s->guide_held = 1; s->guide_down_ms = now_ms; s->consumed = 0;
        if (select_held) { s->consumed = 1; return SYS_QUIT; }   /* Select+Menu chord */
        return SYS_NONE;
    }
    sys_action_t a = s->consumed ? SYS_NONE : SYS_HOME;          /* tap → home */
    s->guide_held = 0;
    return a;
}

/* Select pressed: a QUIT only if GUIDE is currently held (chord). Otherwise
 * NONE — Select is a normal freed button. */
static inline sys_action_t sysbtn_select_down(sysbtn_t *s) {
    if (s->guide_held && !s->consumed) { s->consumed = 1; return SYS_QUIT; }
    return SYS_NONE;
}

/* Per-frame: fire QUIT once GUIDE has been held past hold_ms. */
static inline sys_action_t sysbtn_tick(sysbtn_t *s, uint32_t now_ms, uint32_t hold_ms) {
    if (s->guide_held && !s->consumed && (now_ms - s->guide_down_ms) >= hold_ms) {
        s->consumed = 1; return SYS_QUIT;
    }
    return SYS_NONE;
}

/* ── Defaults: the `sticks` scheme ───────────────────────────────────────────
 * D-pad left/right → E1; D-pad up/down + left stick → E2; right stick → E3. */
static inline void controls_defaults(controls_t *c) {
    c->enc[0] = ENC_SRC_NONE;        /* E1 is D-pad-only (no analog source) */
    c->enc[1] = ENC_SRC_LSTICK;
    c->enc[2] = ENC_SRC_RSTICK;
    c->dpad_enc[0] = 0;              /* D-pad left/right → E1 */
    c->dpad_enc[1] = 1;              /* D-pad up/down     → E2 (mirrors the L stick) */
    c->shoulder_enc = 0;             /* L1/R1 → E1 (alt for the D-pad) */
    c->trigger_enc  = 2;             /* L2/R2 → E3 (alt for the R stick) */
    c->key_btn[0] = BTN_Y;           /* K1 = Y */
    c->key_btn[1] = BTN_X;           /* K2 = X */
    c->key_btn[2] = BTN_A;           /* K3 = A */
    c->dpad_step      = 2;           /* 2 = exactly one patched-matron detent */
    c->stick_deadzone = 8192;
    c->stick_throttle = 6;           /* emit ~every 6 frames; raise to slow sticks */
    c->stick_invert   = 0;
    c->accel       = 1;              /* hold to spin fast */
    c->accel_delay = 18;             /* D-pad: ~0.3s before ramping */
    c->accel_ramp  = 60;             /* D-pad: ~1s to reach peak */
    c->stick_accel_delay = 30;       /* sticks: start later (~0.5s) */
    c->stick_accel_ramp  = 110;      /* sticks: smoother, longer ramp (~1.8s) */
    c->accel_max   = 5;              /* up to 5x faster when held */
    c->dpad_y_invert = 0;            /* off by default; [menu]/[script:*] flip it */
    c->native_mode = 0;              /* emulation; [script:*] overlay opts in */
}

/* ── Pure mapping helpers (shared by the host and the tests) ──────────────── */

/* Encoder (0-2) driven by a D-pad axis, or -1 if unbound. vertical!=0 = up/down. */
static inline int controls_enc_for_dpad(const controls_t *c, int vertical) {
    return c->dpad_enc[vertical ? 1 : 0];
}

/* Encoder driven by the L1/R1 shoulder pair, or -1 if none. */
static inline int controls_enc_for_shoulder(const controls_t *c) {
    return c->shoulder_enc;
}

/* Encoder driven by the L2/R2 trigger pair, or -1 if none. */
static inline int controls_enc_for_trigger(const controls_t *c) {
    return c->trigger_enc;
}

/* True if encoder i is driven by an analog stick. */
static inline int controls_enc_is_stick(const controls_t *c, int i) {
    enc_source_t s = c->enc[i];
    return s == ENC_SRC_LSTICK    || s == ENC_SRC_LSTICK_Y     ||
           s == ENC_SRC_RSTICK    || s == ENC_SRC_RSTICK_Y     ||
           s == ENC_SRC_LSTICK_XY || s == ENC_SRC_RSTICK_XY    ||
           s == ENC_SRC_LSTICK_Y_INV || s == ENC_SRC_RSTICK_Y_INV;
}

/* True if encoder i sums BOTH axes of its stick (up+right increment). */
static inline int controls_enc_is_stick_xy(const controls_t *c, int i) {
    return c->enc[i] == ENC_SRC_LSTICK_XY || c->enc[i] == ENC_SRC_RSTICK_XY;
}

/* True if encoder i reads the vertical (Y) axis with up = increment (so the
 * host negates SDL's up-is-negative axis). Inverted-Y sources are NOT included —
 * they intentionally keep the raw axis so down = increment. */
static inline int controls_enc_is_stick_y(const controls_t *c, int i) {
    return c->enc[i] == ENC_SRC_LSTICK_Y || c->enc[i] == ENC_SRC_RSTICK_Y;
}

/* True if encoder i reads the vertical (Y) axis at all (normal or inverted) —
 * used only to pick the Y axis; direction is decided by is_stick_y. */
static inline int controls_enc_reads_y(const controls_t *c, int i) {
    return controls_enc_is_stick_y(c, i) ||
           c->enc[i] == ENC_SRC_LSTICK_Y_INV || c->enc[i] == ENC_SRC_RSTICK_Y_INV;
}

/* True if encoder i reads the LEFT stick (vs right). */
static inline int controls_enc_is_left_stick(const controls_t *c, int i) {
    return c->enc[i] == ENC_SRC_LSTICK || c->enc[i] == ENC_SRC_LSTICK_Y ||
           c->enc[i] == ENC_SRC_LSTICK_XY || c->enc[i] == ENC_SRC_LSTICK_Y_INV;
}

/* Velocity-scaled encoder delta for a raw SDL axis value (-32768..32767),
 * already oriented so positive = increment. Returns 0 inside the deadzone,
 * else ±1/±2/±3 by magnitude. Applies stick_invert. */
static inline int controls_stick_delta(const controls_t *c, int axis) {
    if (c->stick_invert) axis = -axis;
    int a = axis < 0 ? -axis : axis;
    if (a <= c->stick_deadzone) return 0;
    int mag = (a < 24576) ? 1 : 2;   /* gentle: 1 step normally, 2 near full tilt */
    return axis < 0 ? -mag : mag;
}

/* Rate multiplier for an input held `held_frames` consecutive frames: 1.0 until
 * `delay`, then ramps linearly over `ramp` frames up to accel_max. With accel
 * disabled it is always 1.0. Caller passes the D-pad or stick delay/ramp so the
 * two can accelerate differently. */
static inline float controls_accel_factor(const controls_t *c, int held_frames,
                                          int delay, int ramp) {
    if (!c->accel || held_frames <= delay) return 1.0f;
    if (ramp <= 0) return (float)c->accel_max;
    int t = held_frames - delay;
    float f = 1.0f + (float)t * ((float)c->accel_max - 1.0f) / (float)ramp;
    return f > (float)c->accel_max ? (float)c->accel_max : f;
}

/* ── Token + line parsing ─────────────────────────────────────────────────── */

/* Trim leading/trailing whitespace in place; returns a pointer into `s`. */
static inline char *controls__trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

/* Lowercase + map '_' to '-' in place so "dpad_x"/"DPAD-X" both match. */
static inline void controls__canon(char *t) {
    for (; *t; t++) {
        *t = (char)tolower((unsigned char)*t);
        if (*t == '_') *t = '-';
    }
}

/* Copy the next '\n'-delimited line from *p into out (comment-stripped, not yet
 * trimmed); advance *p past it. Returns 0 when the buffer is exhausted. */
static inline int controls__nextline(const char **p, char *out, size_t n) {
    if (!**p) return 0;
    const char *s = *p;
    const char *nl = strchr(s, '\n');
    size_t len = nl ? (size_t)(nl - s) : strlen(s);
    if (len >= n) len = n - 1;
    memcpy(out, s, len);
    out[len] = '\0';
    *p = nl ? nl + 1 : s + strlen(s);
    char *hash = strchr(out, '#');
    if (hash) *hash = '\0';
    return 1;
}

/* If `line` is a "[section]" header, copy the canonicalised name into `name`
 * and return 1; else return 0. `line` should already be trimmed. */
static inline int controls__section(char *line, char *name, size_t n) {
    if (line[0] != '[') return 0;
    char *end = strchr(line, ']');
    if (end) *end = '\0';
    char *nm = controls__trim(line + 1);
    strncpy(name, nm, n - 1);
    name[n - 1] = '\0';
    controls__canon(name);
    return 1;
}

static inline enc_source_t controls__parse_enc(const char *tok) {
    if (!strcmp(tok, "none"))      return ENC_SRC_NONE;
    if (!strcmp(tok, "lstick") || !strcmp(tok, "lstick-x")) return ENC_SRC_LSTICK;
    if (!strcmp(tok, "lstick-y"))  return ENC_SRC_LSTICK_Y;
    if (!strcmp(tok, "lstick-y-inv")) return ENC_SRC_LSTICK_Y_INV;
    if (!strcmp(tok, "lstick-xy")) return ENC_SRC_LSTICK_XY;
    if (!strcmp(tok, "rstick") || !strcmp(tok, "rstick-x")) return ENC_SRC_RSTICK;
    if (!strcmp(tok, "rstick-y"))  return ENC_SRC_RSTICK_Y;
    if (!strcmp(tok, "rstick-y-inv")) return ENC_SRC_RSTICK_Y_INV;
    if (!strcmp(tok, "rstick-xy")) return ENC_SRC_RSTICK_XY;
    return (enc_source_t)-1;   /* unknown */
}

/* Single button token → bit, or 0 if unknown. */
static inline uint16_t controls__parse_btn(const char *tok) {
    if (!strcmp(tok, "a"))      return BTN_A;
    if (!strcmp(tok, "b"))      return BTN_B;
    if (!strcmp(tok, "x"))      return BTN_X;
    if (!strcmp(tok, "y"))      return BTN_Y;
    if (!strcmp(tok, "l1"))     return BTN_L1;
    if (!strcmp(tok, "r1"))     return BTN_R1;
    if (!strcmp(tok, "select")) return BTN_SELECT;
    if (!strcmp(tok, "start"))  return BTN_START;
    if (!strcmp(tok, "l3"))     return BTN_L3;
    if (!strcmp(tok, "r3"))     return BTN_R3;
    /* "guide"/"menu" deliberately unmapped — host-reserved (Phase 1). */
    return 0;
}

/* Parse a comma-separated button list ("y, b") into a bitmask. */
static inline uint16_t controls__parse_btn_list(char *val) {
    uint16_t mask = 0;
    for (char *save = NULL, *t = strtok_r(val, ",", &save); t;
              t = strtok_r(NULL, ",", &save)) {
        mask |= controls__parse_btn(controls__trim(t));
    }
    return mask;
}

static inline int controls__clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

/* Parse a D-pad axis target: "1"/"2"/"3" → encoder 0-2; anything else → -1
 * (none). atoi("none") == 0 → -1, so "none" works without a string compare. */
static inline int8_t controls__parse_dpad_target(const char *val) {
    int n = atoi(val);
    return (n >= 1 && n <= 3) ? (int8_t)(n - 1) : -1;
}

/* Apply one "key = val" assignment to c. Returns 1 if recognised, 0 otherwise. */
static inline int controls__assign(controls_t *c, const char *key, char *val) {
    if (key[0] == 'e' && key[1] >= '1' && key[1] <= '3' && key[2] == '\0') {
        char tok[64];
        strncpy(tok, val, sizeof(tok) - 1); tok[sizeof(tok) - 1] = '\0';
        controls__canon(controls__trim(tok));
        enc_source_t src = controls__parse_enc(tok);
        if (src == (enc_source_t)-1) {
            fprintf(stderr, "controls: unknown encoder source '%s' for %s\n", tok, key);
            return 0;
        }
        c->enc[key[1] - '1'] = src;
        return 1;
    }
    if (key[0] == 'k' && key[1] >= '1' && key[1] <= '3' && key[2] == '\0') {
        char buf[128];
        strncpy(buf, val, sizeof(buf) - 1); buf[sizeof(buf) - 1] = '\0';
        controls__canon(buf);
        c->key_btn[key[1] - '1'] = controls__parse_btn_list(buf);
        return 1;
    }
    if (!strcmp(key, "dpad-step"))      { c->dpad_step      = controls__clamp(atoi(val), 1, 16);    return 1; }
    if (!strcmp(key, "stick-deadzone")) { c->stick_deadzone = controls__clamp(atoi(val), 0, 32000); return 1; }
    if (!strcmp(key, "stick-throttle")) { c->stick_throttle = controls__clamp(atoi(val), 1, 60);    return 1; }
    if (!strcmp(key, "stick-invert"))   { c->stick_invert   = atoi(val) ? 1 : 0;                     return 1; }
    if (!strcmp(key, "accel"))          { c->accel       = atoi(val) ? 1 : 0;                      return 1; }
    if (!strcmp(key, "accel-delay"))    { c->accel_delay = controls__clamp(atoi(val), 0, 600);     return 1; }
    if (!strcmp(key, "accel-ramp"))     { c->accel_ramp  = controls__clamp(atoi(val), 1, 600);     return 1; }
    if (!strcmp(key, "accel-max"))      { c->accel_max   = controls__clamp(atoi(val), 1, 64);      return 1; }
    if (!strcmp(key, "dpad-x"))         { c->dpad_enc[0]  = controls__parse_dpad_target(val);       return 1; }
    if (!strcmp(key, "dpad-y"))         { c->dpad_enc[1]  = controls__parse_dpad_target(val);       return 1; }
    if (!strcmp(key, "shoulders"))      { c->shoulder_enc = controls__parse_dpad_target(val);       return 1; }
    if (!strcmp(key, "triggers"))       { c->trigger_enc  = controls__parse_dpad_target(val);       return 1; }
    if (!strcmp(key, "stick-accel-delay")) { c->stick_accel_delay = controls__clamp(atoi(val), 0, 600); return 1; }
    if (!strcmp(key, "stick-accel-ramp"))  { c->stick_accel_ramp  = controls__clamp(atoi(val), 1, 600); return 1; }
    if (!strcmp(key, "dpad-y-invert"))  { c->dpad_y_invert = atoi(val) ? 1 : 0;                       return 1; }
    if (!strcmp(key, "mode")) {
        char v[16]; strncpy(v, val, sizeof(v) - 1); v[sizeof(v) - 1] = '\0';
        controls__canon(controls__trim(v));
        c->native_mode = !strcmp(v, "native") ? 1 : 0;   /* anything else = emulation */
        return 1;
    }
    fprintf(stderr, "controls: unknown key '%s'\n", key);
    return 0;
}

/*
 * Apply global lines plus the lines of the [want] section to `c` (which should
 * already hold defaults). want==NULL applies only global (sectionless) lines.
 * The `scheme = ...` selector line is skipped (handled by controls_find_scheme).
 * Returns the count of recognised assignments. Input buffer is not modified.
 */
static inline int controls_parse_scheme(controls_t *c, const char *text, const char *want) {
    int applied = 0;
    const char *p = text;
    char line[256];
    char cur[32] = "";   /* current section, "" = global */

    while (controls__nextline(&p, line, sizeof(line))) {
        char *t = controls__trim(line);
        if (*t == '\0') continue;
        if (controls__section(t, cur, sizeof(cur))) continue;

        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key_s = controls__trim(t);
        char *val   = controls__trim(eq + 1);
        if (*key_s == '\0') continue;

        char key[64];
        strncpy(key, key_s, sizeof(key) - 1); key[sizeof(key) - 1] = '\0';
        controls__canon(key);
        if (!strcmp(key, "scheme")) continue;   /* selector, not an assignment */

        int is_global = (cur[0] == '\0');
        if (!is_global && !(want && !strcmp(cur, want))) continue;

        if (controls__assign(c, key, val)) applied++;
    }
    return applied;
}

/* Backward-compatible flat parse: applies global (sectionless) lines only. */
static inline int controls_parse(controls_t *c, const char *text) {
    return controls_parse_scheme(c, text, NULL);
}

/* Find the `scheme = X` selector (a global line). Returns 1 and fills `out`
 * (canonicalised) if present, else 0. */
static inline int controls_find_scheme(const char *text, char *out, size_t outsz) {
    const char *p = text;
    char line[256];
    char cur[32] = "";
    while (controls__nextline(&p, line, sizeof(line))) {
        char *t = controls__trim(line);
        if (*t == '\0') continue;
        if (controls__section(t, cur, sizeof(cur))) continue;
        if (cur[0] != '\0') continue;            /* selector must be global */
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        char key[64];
        strncpy(key, controls__trim(t), sizeof(key) - 1); key[sizeof(key) - 1] = '\0';
        controls__canon(key);
        if (strcmp(key, "scheme")) continue;
        strncpy(out, controls__trim(eq + 1), outsz - 1); out[outsz - 1] = '\0';
        controls__canon(out);
        return 1;
    }
    return 0;
}

/* True if a [want] section header exists in text. */
static inline int controls_has_section(const char *text, const char *want) {
    const char *p = text;
    char line[256], name[32];
    while (controls__nextline(&p, line, sizeof(line))) {
        char *t = controls__trim(line);
        if (controls__section(t, name, sizeof(name)) && !strcmp(name, want)) return 1;
    }
    return 0;
}

/* Load config from a file into `c` (already holding defaults), honouring the
 * `scheme` selector. `info` may be NULL. Returns 1 if the file was read. */
static inline int controls_load_file(controls_t *c, const char *path,
                                     controls_load_info_t *info) {
    if (info) { info->file_found = 0; info->scheme_found = 0;
                info->applied = 0; info->scheme[0] = '\0'; }
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    char scheme[32] = "";
    int have = controls_find_scheme(buf, scheme, sizeof(scheme));
    int applied = controls_parse_scheme(c, buf, have ? scheme : NULL);

    if (info) {
        info->file_found   = 1;
        info->applied      = applied;
        info->scheme_found = have ? controls_has_section(buf, scheme) : 0;
        strncpy(info->scheme, scheme, sizeof(info->scheme) - 1);
        info->scheme[sizeof(info->scheme) - 1] = '\0';
    }
    return 1;
}

#endif /* NORNS_CONTROLS_H */
