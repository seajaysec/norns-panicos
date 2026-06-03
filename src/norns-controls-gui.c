/*
 * norns-controls-gui.c — on-device editor for norns-panicos controls.conf.
 *
 * A standalone SDL2 + SDL2_ttf tool (its own PortMaster port, beside Norns) that
 * remaps the handheld's controls without a rebuild. It edits the SAME file
 * norns-panicos reads, in three scopes:
 *
 *   • System (menu)   — the [menu] overlay (applies in the norns system menu)
 *   • Global script   — the active [scheme] + global keys (every script's base)
 *   • Per-script       — a [script:NAME] overlay; installed scripts are detected
 *                        and split into "Customised" vs "Inherits defaults".
 *
 * The input ENGINE is unchanged: this only writes the existing config vocabulary
 * (encoder stick sources, the D-pad/L1·R1/L2·R2 pair targets, and the keys).
 * Overlays are written as minimal diffs (see controls-conf.h), so reverting a
 * scope to the defaults removes its section and it goes back to inheriting.
 *
 * Usage: norns-controls-gui <controls.conf path> <dust/code dir>
 *
 * Acceleration / deadzone tunables are intentionally NOT editable here and are
 * preserved untouched through every save.
 */
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include "controls-conf.h"

#define SCREEN_W 640
#define SCREEN_H 480

/* ── option vocabularies (token ↔ friendly label) ───────────────────────── */

/* One row per input axis/pair, in RT_* order (controls-conf.h). */
static const char *const ROUTE_LBL[RT_N] = {
    "D-pad  L / R", "D-pad  U / D",
    "Left stick  L / R", "Left stick  U / D",
    "Right stick L / R", "Right stick U / D",
    "L1 / R1", "L2 / R2"
};
static const char *const ROUTEVAL_LBL[4] = { "Off", "E1", "E2", "E3" };
/* The two routes that carry a U/D invert flag. */
#define RT_IS_LSTICK_Y(i) ((i) == RT_LSTICK_Y)
#define RT_IS_RSTICK_Y(i) ((i) == RT_RSTICK_Y)

static const char *const KEY_BTN[]     = { "a", "b", "x", "y", "l1", "r1" };
static const char *const KEY_BTN_LBL[] = { "A", "B", "X", "Y", "L1", "R1" };
#define N_KEYBTN 6

/* Global "feel" tunables surfaced on the Tuning screen (live in the globals
 * section; apply everywhere). def = built-in fallback when the key is absent. */
typedef struct { const char *key, *label; int lo, hi, step, def, isbool; } tunable_t;
static const tunable_t TUNABLES[] = {
    { "accel",             "Acceleration",          0,     1,    1,    1, 1 },
    { "accel_max",         "Peak speed (×)",        1,    16,    1,    4, 0 },
    { "stick_accel_delay", "Stick: ramp onset",     0,   200,    2,   40, 0 },
    { "stick_accel_ramp",  "Stick: ramp length",    1,   400,    5,  150, 0 },
    { "accel_delay",       "Button: ramp onset",    0,   200,    2,   18, 0 },
    { "accel_ramp",        "Button: ramp length",   1,   400,    5,   60, 0 },
    { "stick_throttle",    "Stick: base rate",      1,    60,    1,    6, 0 },
    { "stick_deadzone",    "Stick: deadzone",       0, 32000, 1024, 8192, 0 },
};
#define N_TUNABLES (int)(sizeof TUNABLES / sizeof TUNABLES[0])
static unsigned key_mask_from_str(const char *s) {
    unsigned m = 0; char buf[CONF_VALLEN];
    strncpy(buf, s ? s : "", sizeof buf - 1); buf[sizeof buf - 1] = '\0';
    char *save = NULL;
    for (char *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        char *x = conf_trim(t);
        for (int i = 0; i < N_KEYBTN; i++) if (!strcasecmp(x, KEY_BTN[i])) m |= (1u << i);
    }
    return m;
}
static void key_str_from_mask(unsigned m, char *out, size_t cap) {
    out[0] = '\0'; int first = 1;
    for (int i = 0; i < N_KEYBTN; i++)
        if (m & (1u << i)) {
            size_t l = strlen(out);
            snprintf(out + l, cap - l, "%s%s", first ? "" : ", ", KEY_BTN[i]);
            first = 0;
        }
    if (!out[0]) snprintf(out, cap, "none");
}

/* ── app state ──────────────────────────────────────────────────────────── */

typedef enum { SCR_MAIN, SCR_SCRIPTS, SCR_EDITOR, SCR_KEYPICK, SCR_DETECT,
               SCR_CONFIRM, SCR_COPYPICK, SCR_TUNING } screen_t;
typedef enum { SCOPE_GLOBAL, SCOPE_SYSTEM, SCOPE_SCRIPT } scope_t;
typedef enum { CF_EDITOR_BACK, CF_EXIT, CF_RESTORE_ALL } confirm_t;

typedef struct { char name[CONF_NAMELEN]; int customised; } script_ent;

/* Editor rows are built from the mapping each time the editor opens. ROW_ROUTE
 * idx = RT_* (an input axis/pair → encoder); ROW_KEY idx = 0..2. */
typedef enum { ROW_HEAD, ROW_ROUTE, ROW_KEY } rowtype_t;
typedef struct { rowtype_t type; int idx; const char *label; } row_t;

static struct {
    SDL_Renderer *ren;
    TTF_Font     *font, *font_sm, *font_lg;
    SDL_GameController *gc;

    char conf_path[600], scripts_dir[600], defaults_path[600];
    conf_file conf;
    char scheme[CONF_NAMELEN];
    conf_file defs;          /* read-only factory defaults (controls.defaults.conf) */
    char defs_scheme[CONF_NAMELEN];
    int  have_defs;          /* defaults file loaded with content */
    screen_t screen;

    int main_sel;
    int scripts_sel, scripts_top, n_scripts;
    script_ent scripts[256];

    scope_t scope;
    char    overlay[CONF_NAMELEN];
    char    ed_title[80];
    mapping work;        /* the scope being edited (scratch)                    */
    mapping base;        /* what this scope inherits (globals+scheme, or engine */
    int     is_overlay;  /* scope inherits from a parent (menu / script)         */
    row_t   rows[24];
    int     n_rows, ed_sel, ed_top;
    int     ed_dirty;    /* scratch has edits not yet applied to the model     */
    int     app_dirty;   /* model has edits not yet written to the file         */
    int     copy_from;   /* per-script "copy from": source script index, or -1  */

    confirm_t confirm_kind;
    int       confirm_sel;   /* 0 = default (Discard) option */

    int keypick_key;     /* which K (0..2) the key picker targets */
    int keypick_sel;     /* which button row in the picker */
    int tuning_sel;      /* selected row on the Tuning screen */

    int detect_is_key;   /* detection target: 1 = key, 0 = encoder */
    int detect_target;   /* encoder (0..2) or key (0..2) being detected */
    int detect_armed;    /* ignore inputs until everything is released first */

    char toast[48];      /* transient confirmation (e.g. "Saved") */
    int  toast_ttl;      /* frames remaining */

    int  fh;             /* line height of the main font (for centering) */
} A;

/* ── text rendering ─────────────────────────────────────────────────────── */

static void draw_text(TTF_Font *f, int x, int y, SDL_Color c, const char *s) {
    if (!s || !*s) return;
    SDL_Surface *surf = TTF_RenderUTF8_Blended(f, s, c);
    if (!surf) return;
    SDL_Texture *t = SDL_CreateTextureFromSurface(A.ren, surf);
    SDL_Rect r = { x, y, surf->w, surf->h };
    SDL_RenderCopy(A.ren, t, NULL, &r);
    SDL_DestroyTexture(t);
    SDL_FreeSurface(surf);
}
static void draw_text_c(TTF_Font *f, int cx, int y, SDL_Color c, const char *s) {
    int w = 0, h = 0; TTF_SizeUTF8(f, s, &w, &h);
    draw_text(f, cx - w / 2, y, c, s);
}
static void fill(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b) {
    SDL_SetRenderDrawColor(A.ren, r, g, b, 255);
    SDL_Rect rr = { x, y, w, h };
    SDL_RenderFillRect(A.ren, &rr);
}
/* Draw text vertically centred in a [y, y+rowH) band so glyph descenders never
 * fall outside a selection highlight (the bug that clipped selected rows). */
static void draw_text_v(TTF_Font *f, int x, int y, int rowH, SDL_Color c, const char *s) {
    int w = 0, h = 0; TTF_SizeUTF8(f, s, &w, &h);
    draw_text(f, x, y + (rowH - h) / 2, c, s);
}
static void fill_a(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    SDL_SetRenderDrawBlendMode(A.ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(A.ren, r, g, b, a);
    SDL_Rect rr = { x, y, w, h };
    SDL_RenderFillRect(A.ren, &rr);
    SDL_SetRenderDrawBlendMode(A.ren, SDL_BLENDMODE_NONE);
}
static const SDL_Color WHITE = { 235, 235, 235, 255 };
static const SDL_Color DIM   = { 140, 140, 140, 255 };
static const SDL_Color ACC   = {  20,  20,  24, 255 };  /* text on highlight */
static const SDL_Color HINT  = { 120, 170, 210, 255 };
static const SDL_Color OKC   = { 130, 210, 150, 255 };
static const SDL_Color REDC  = { 235, 110, 110, 255 };  /* conflict / warning  */
#define HL_R 210
#define HL_G 210
#define HL_B 215
/* full-width selection highlight band */
static void hl(int y, int rowH) { fill(24, y, SCREEN_W - 48, rowH, HL_R, HL_G, HL_B); }

/* ── config <-> mapping helpers ─────────────────────────────────────────── */

/* The shipped engine defaults (mirror controls_defaults() in norns-controls.h).
 * Used only to seed a missing config so the tool is never destructive on a
 * device where norns hasn't run yet to seed controls.conf. */
static void gui_default_mapping(mapping *m) {
    mapping_defaults(m);
    m->route[RT_LSTICK_X] = 2;            /* left  X → E2 */
    m->route[RT_RSTICK_X] = 3;            /* right X → E3 */
    m->route[RT_DPAD_X] = 1; m->route[RT_DPAD_Y] = 2;
    m->route[RT_SHOULDERS] = 1; m->route[RT_TRIGGERS] = 3;
    strcpy(m->k[0], "y"); strcpy(m->k[1], "x"); strcpy(m->k[2], "a");
}
/* True if this route carries a U/D invert flag, and its current invert value. */
static int route_has_invert(int rt) { return RT_IS_LSTICK_Y(rt) || RT_IS_RSTICK_Y(rt); }
static int work_invert(int rt) {
    return RT_IS_LSTICK_Y(rt) ? A.work.lstick_y_inv : RT_IS_RSTICK_Y(rt) ? A.work.rstick_y_inv : 0;
}
static int base_invert(int rt) {
    return RT_IS_LSTICK_Y(rt) ? A.base.lstick_y_inv : RT_IS_RSTICK_Y(rt) ? A.base.rstick_y_inv : 0;
}
/* A route row is "inherited" (overlay scopes) when its value AND invert match
 * the parent base. */
static int route_inherited(int rt) {
    return A.is_overlay && A.work.route[rt] == A.base.route[rt] && work_invert(rt) == base_invert(rt);
}
static int key_inherited(int k) {
    return A.is_overlay && !strcmp(A.work.k[k], A.base.k[k]);
}

static void load_conf(void) {
    char buf[16384] = {0};
    int have = 0;
    FILE *f = fopen(A.conf_path, "rb");
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = '\0';
        have = (n > 0);
        fclose(f);
    }
    conf_parse(&A.conf, buf);
    strncpy(A.scheme, conf_active_scheme(&A.conf), sizeof A.scheme - 1);
    A.scheme[sizeof A.scheme - 1] = '\0';
    if (!have) {                 /* no seeded file yet — start from engine defaults */
        mapping d; gui_default_mapping(&d);
        mapping_write_base(&A.conf, A.scheme, &d);
    }
    /* always guarantee the scheme selector so the [scheme] section is applied */
    conf_set(conf_section_add(&A.conf, ""), "scheme", A.scheme);
}
/* Load the read-only factory defaults (never written). */
static void load_defaults(void) {
    A.have_defs = 0;
    if (!A.defaults_path[0]) return;
    char buf[16384] = {0};
    FILE *f = fopen(A.defaults_path, "rb");
    if (!f) return;
    size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = '\0'; fclose(f);
    if (n == 0) return;
    conf_parse(&A.defs, buf);
    strncpy(A.defs_scheme, conf_active_scheme(&A.defs), sizeof A.defs_scheme - 1);
    A.defs_scheme[sizeof A.defs_scheme - 1] = '\0';
    A.have_defs = 1;
}
static int save_conf(void) {
    conf_prune_empty(&A.conf);
    char out[16384];
    int n = conf_serialize(&A.conf, out, sizeof out);
    if (n < 0 || (size_t)n >= sizeof out) return -1;
    char tmp[640]; snprintf(tmp, sizeof tmp, "%s.tmp", A.conf_path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    fwrite(out, 1, (size_t)n, f);
    fclose(f);
    if (rename(tmp, A.conf_path) != 0) return -1;
    return 0;
}

/* Which inputs drive encoder enc (0..2), for the live summary line. */
static void encoder_summary(int enc, char *out, size_t cap) {
    out[0] = '\0';
    int first = 1;
    #define ADD(s) do { size_t l = strlen(out); snprintf(out + l, cap - l, "%s%s", first ? "" : ", ", s); first = 0; } while (0)
    for (int i = 0; i < RT_N; i++)
        if (A.work.route[i] == enc + 1) ADD(ROUTE_LBL[i]);
    if (first) snprintf(out, cap, "(unbound)");
    #undef ADD
}

/* Count of rows inherited from the parent (overlay scopes). */
static int inherited_count(void) {
    int n = 0;
    for (int i = 0; i < RT_N; i++) if (route_inherited(i)) n++;
    for (int k = 0; k < 3; k++) if (key_inherited(k)) n++;
    return n;
}
/* Effective workability: at least one key and one encoder input bound. The
 * EFFECTIVE result is what runs, so for overlay scopes an inherited row still
 * counts (it carries the parent's value). */
static int effective_unworkable(int *no_keys, int *no_enc) {
    int keys = 0, enc = 0;
    for (int k = 0; k < 3; k++) if (key_mask_from_str(A.work.k[k])) keys = 1;
    for (int i = 0; i < RT_N; i++) if (A.work.route[i] != 0) enc = 1;
    if (no_keys) *no_keys = !keys;
    if (no_enc)  *no_enc  = !enc;
    return !keys || !enc;
}
static void toast(const char *s) { snprintf(A.toast, sizeof A.toast, "%s", s); A.toast_ttl = 70; }

/* ── global feel tunables (Tuning screen) ───────────────────────────────── */
static int tunable_get(int i) {
    const char *v = conf_get(conf_section_by_name(&A.conf, ""), TUNABLES[i].key);
    return v ? atoi(v) : TUNABLES[i].def;
}
static void tunable_adjust(int i, int dir) {
    int v = tunable_get(i) + dir * TUNABLES[i].step;
    if (v < TUNABLES[i].lo) v = TUNABLES[i].lo;
    if (v > TUNABLES[i].hi) v = TUNABLES[i].hi;
    char buf[16]; snprintf(buf, sizeof buf, "%d", v);
    conf_set(conf_section_add(&A.conf, ""), TUNABLES[i].key, buf);
    A.app_dirty = 1;
}

/* ── screen builders ────────────────────────────────────────────────────── */

static void scan_scripts(void) {
    A.n_scripts = 0;
    /* installed script folders */
    DIR *d = opendir(A.scripts_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && A.n_scripts < 256) {
            if (e->d_name[0] == '.') continue;
            char p[700]; snprintf(p, sizeof p, "%s/%s", A.scripts_dir, e->d_name);
            struct stat st;
            if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode)) continue;
            script_ent *s = &A.scripts[A.n_scripts++];
            strncpy(s->name, e->d_name, sizeof s->name - 1); s->name[sizeof s->name - 1] = '\0';
            char sec[CONF_NAMELEN]; snprintf(sec, sizeof sec, "script:%s", e->d_name);
            s->customised = conf_section_has_keys(&A.conf, sec);
        }
        closedir(d);
    }
    /* any [script:NAME] overlays whose folder isn't present (seeded examples) */
    for (int i = 0; i < A.conf.nsec && A.n_scripts < 256; i++) {
        const char *nm = A.conf.sec[i].name;
        if (strncmp(nm, "script:", 7) != 0 || A.conf.sec[i].nkv == 0) continue;
        const char *sn = nm + 7;
        int seen = 0;
        for (int j = 0; j < A.n_scripts; j++) if (!strcasecmp(A.scripts[j].name, sn)) { seen = 1; break; }
        if (seen) continue;   /* folder match is case-insensitive (KoiBoi2 ↔ koiboi2) */
        script_ent *s = &A.scripts[A.n_scripts++];
        strncpy(s->name, sn, sizeof s->name - 1); s->name[sizeof s->name - 1] = '\0';
        s->customised = 1;
    }
    /* sort: customised first, then alpha within each group */
    for (int i = 0; i < A.n_scripts; i++)
        for (int j = i + 1; j < A.n_scripts; j++) {
            script_ent *a = &A.scripts[i], *b = &A.scripts[j];
            int swap = (b->customised != a->customised) ? (b->customised > a->customised)
                                                        : (strcasecmp(b->name, a->name) < 0);
            if (swap) { script_ent t = *a; *a = *b; *b = t; }
        }
    A.scripts_sel = 0; A.scripts_top = 0;
}

static void build_rows(void) {
    A.n_rows = 0;
    #define R(t,i,l) do { A.rows[A.n_rows].type=(t); A.rows[A.n_rows].idx=(i); A.rows[A.n_rows].label=(l); A.n_rows++; } while (0)
    R(ROW_HEAD, 0, "ENCODER INPUTS  (route each → E1/E2/E3)");
    for (int i = 0; i < RT_N; i++) R(ROW_ROUTE, i, ROUTE_LBL[i]);
    R(ROW_HEAD, 0, "KEYS");
    R(ROW_KEY, 0, "K1");
    R(ROW_KEY, 1, "K2");
    R(ROW_KEY, 2, "K3");
    #undef R
    A.ed_sel = 1; A.ed_top = 0;   /* first selectable row */
}

static void open_editor(scope_t scope, const char *overlay, const char *title) {
    A.scope = scope;
    A.is_overlay = (scope != SCOPE_GLOBAL);
    A.overlay[0] = '\0';
    if (overlay) { strncpy(A.overlay, overlay, sizeof A.overlay - 1); A.overlay[sizeof A.overlay - 1] = '\0'; }
    strncpy(A.ed_title, title, sizeof A.ed_title - 1); A.ed_title[sizeof A.ed_title - 1] = '\0';
    if (scope == SCOPE_GLOBAL) {
        mapping_read_base(&A.conf, A.scheme, &A.work);
        gui_default_mapping(&A.base);   /* global inherits the engine defaults */
    } else {
        mapping_read_overlay(&A.conf, A.scheme, A.overlay, &A.work);
        mapping_read_base(&A.conf, A.scheme, &A.base);   /* what this scope inherits */
    }
    build_rows();
    A.ed_dirty = 0;
    A.screen = SCR_EDITOR;
}

/* Apply the scratch mapping into the in-memory model (NOT the file — that's the
 * app-level Save). Edits accumulate in the model; Save writes them all at once. */
static void apply_scope(void) {
    if (A.scope == SCOPE_GLOBAL) mapping_write_base(&A.conf, A.scheme, &A.work);
    else { mapping_write_overlay(&A.conf, A.scheme, A.overlay, &A.work); conf_prune_empty(&A.conf); }
    A.app_dirty = 1;
    A.ed_dirty = 0;
}
static void editor_to_parent(void) {
    A.screen = (A.scope == SCOPE_SCRIPT) ? SCR_SCRIPTS : SCR_MAIN;
    if (A.screen == SCR_SCRIPTS) scan_scripts();
}
static int save_file(void) {
    if (save_conf() == 0) { A.app_dirty = 0; toast("Saved ✓"); return 0; }
    toast("Save failed!"); return -1;
}
/* Replace the whole working model with the factory defaults (not yet on disk). */
static void restore_all_defaults(void) {
    if (!A.have_defs) { toast("no defaults file"); return; }
    A.conf = A.defs;
    strncpy(A.scheme, A.defs_scheme, sizeof A.scheme - 1);
    A.scheme[sizeof A.scheme - 1] = '\0';
    A.app_dirty = 1;
    toast("defaults restored — Save to keep");
}

/* ── rendering ──────────────────────────────────────────────────────────── */

static void render_header(const char *title, const char *sub) {
    fill(0, 0, SCREEN_W, SCREEN_H, 16, 16, 18);
    fill(0, 0, SCREEN_W, 46, 26, 26, 30);
    draw_text(A.font_lg, 18, 8, WHITE, title);
    if (sub) draw_text(A.font_sm, 18, 32, DIM, sub);
    if (A.app_dirty || (A.screen == SCR_EDITOR && A.ed_dirty))
        draw_text(A.font_sm, SCREEN_W - 96, 6, HINT, "● unsaved");
}
static void render_footer(const char *s) {
    fill(0, SCREEN_H - 28, SCREEN_W, 28, 26, 26, 30);
    draw_text_v(A.font_sm, 16, SCREEN_H - 28, 28, DIM, s);
    if (A.toast_ttl > 0) {
        int w = 0, h = 0; TTF_SizeUTF8(A.font_sm, A.toast, &w, &h);
        fill(SCREEN_W - w - 28, SCREEN_H - 28, w + 28, 28, 26, 40, 30);
        draw_text_v(A.font_sm, SCREEN_W - w - 14, SCREEN_H - 28, 28, OKC, A.toast);
    }
}

static void render_main(void) {
    render_header("Norns Controls", "remap encoders & keys · edits norns/cfg/controls.conf");
    const char *items[] = { "System (menu) controls", "Global script controls",
                            "Per-script controls", "Tuning (feel)", "Save to file",
                            "Restore all defaults", "Exit" };
    int y0 = 64, rh = 38;
    for (int i = 0; i < 7; i++) {
        int y = y0 + i * rh;
        int sel = (i == A.main_sel);
        int dim = (i == 4 && !A.app_dirty) || (i == 5 && !A.have_defs);
        if (sel) hl(y, rh);
        draw_text_v(A.font, 52, y, rh, sel ? ACC : (dim ? DIM : WHITE), items[i]);
    }
    render_footer("move: D-pad/stick   A: open   B/Select: exit");
}

static void render_scripts(void) {
    char sub[80]; snprintf(sub, sizeof sub, "%d installed · customised shown first", A.n_scripts);
    render_header("Per-script controls", sub);
    int y0 = 56, rows = 9, rh = 40;
    if (A.scripts_sel < A.scripts_top) A.scripts_top = A.scripts_sel;
    if (A.scripts_sel >= A.scripts_top + rows) A.scripts_top = A.scripts_sel - rows + 1;
    int last_group = -1;
    for (int r = 0; r < rows; r++) {
        int i = A.scripts_top + r;
        if (i >= A.n_scripts) break;
        int y = y0 + r * rh;
        script_ent *s = &A.scripts[i];
        if (s->customised != last_group) {
            last_group = s->customised;
            draw_text(A.font_sm, 24, y, HINT, s->customised ? "CUSTOMISED" : "INHERITS DEFAULTS");
        }
        int sel = (i == A.scripts_sel);
        if (sel) hl(y + 12, rh - 12);
        draw_text_v(A.font, 44, y + 12, rh - 12, sel ? ACC : WHITE, s->name);
    }
    if (A.n_scripts == 0)
        draw_text_c(A.font, SCREEN_W / 2, 200, DIM, "No scripts found in dust/code");
    render_footer("move: D-pad   A: edit   X: copy from another   B: back");
}

static void render_copypick(void) {
    char sub[96];
    snprintf(sub, sizeof sub, "copy controls ONTO  %s  — pick the source",
             A.scripts_sel < A.n_scripts ? A.scripts[A.scripts_sel].name : "");
    render_header("Copy from…", sub);
    int y0 = 56, rows = 9, rh = 40;
    int top = 0;
    if (A.copy_from >= rows) top = A.copy_from - rows + 1;
    for (int r = 0; r < rows; r++) {
        int i = top + r;
        if (i >= A.n_scripts) break;
        int y = y0 + r * rh, sel = (i == A.copy_from);
        if (sel) hl(y, rh);
        SDL_Color c = sel ? ACC : (i == A.scripts_sel ? DIM : WHITE);
        char line[80];
        snprintf(line, sizeof line, "%s%s", A.scripts[i].name,
                 i == A.scripts_sel ? "  (target)" : A.scripts[i].customised ? "  •" : "");
        draw_text_v(A.font, 44, y, rh, c, line);
    }
    render_footer("move: D-pad   A: copy its controls here   B: cancel");
}

static void render_tuning(void) {
    render_header("Tuning (feel)", "global hold-to-accelerate & stick response");
    int y0 = 54, rh = 42;
    for (int i = 0; i < N_TUNABLES; i++) {
        int y = y0 + i * rh, sel = (i == A.tuning_sel);
        if (sel) hl(y, rh);
        int v = tunable_get(i);
        char val[24];
        if (TUNABLES[i].isbool) snprintf(val, sizeof val, "%s", v ? "On" : "Off");
        else snprintf(val, sizeof val, "%d", v);
        draw_text_v(A.font, 44, y, rh, sel ? ACC : WHITE, TUNABLES[i].label);
        draw_text_v(A.font, 360, y, rh, sel ? ACC : HINT, val);
        if (sel) { draw_text_v(A.font, 338, y, rh, ACC, "‹");
                   draw_text_v(A.font, SCREEN_W - 44, y, rh, ACC, "›"); }
    }
    render_footer("◄›: adjust   B: back   (Save on the main menu)");
}

static void render_value_label(const row_t *row, char *out, size_t cap) {
    if (row->type == ROW_ROUTE) {
        const char *inv = (route_has_invert(row->idx) && work_invert(row->idx)) ? " ⟲" : "";
        snprintf(out, cap, "%s%s", ROUTEVAL_LBL[A.work.route[row->idx] & 3], inv);
    } else if (row->type == ROW_KEY) {
        char b[CONF_VALLEN]; key_str_from_mask(key_mask_from_str(A.work.k[row->idx]), b, sizeof b);
        for (char *p = b; *p; p++) *p = (char)toupper((unsigned char)*p);
        snprintf(out, cap, "%s", b);
    } else out[0] = '\0';
}

static void render_editor(void) {
    const char *scopelbl = A.scope == SCOPE_GLOBAL ? "applies to every script (the defaults)"
                          : A.scope == SCOPE_SYSTEM ? "norns menu · dim rows inherit the defaults"
                          : "this script · dim rows inherit the defaults";
    render_header(A.ed_title, scopelbl);

    int y0 = 50, rh = 24, rows_vis = 12;
    if (A.ed_sel < A.ed_top) A.ed_top = A.ed_sel;
    if (A.ed_sel >= A.ed_top + rows_vis) A.ed_top = A.ed_sel - rows_vis + 1;

    for (int r = 0; r < rows_vis; r++) {
        int i = A.ed_top + r;
        if (i >= A.n_rows) break;
        row_t *row = &A.rows[i];
        int y = y0 + r * rh;
        if (row->type == ROW_HEAD) { draw_text_v(A.font_sm, 24, y, rh, HINT, row->label); continue; }
        int sel = (i == A.ed_sel);
        int inh = (row->type == ROW_ROUTE) ? route_inherited(row->idx) : key_inherited(row->idx);
        if (sel) hl(y, rh);
        char val[96]; render_value_label(row, val, sizeof val);
        SDL_Color lc = sel ? ACC : (inh ? DIM : WHITE);
        SDL_Color vc = sel ? ACC : (inh ? DIM : HINT);
        draw_text_v(A.font, 44, y, rh, lc, row->label);
        draw_text_v(A.font, 244, y, rh, vc, val);
        /* override badge / inherit tag */
        if (A.is_overlay)
            draw_text_v(A.font_sm, SCREEN_W - 96, y, rh, sel ? ACC : (inh ? DIM : OKC),
                        inh ? "↳ inherits" : "● override");
        if (sel && row->type == ROW_ROUTE) {
            draw_text_v(A.font, 226, y, rh, ACC, "‹");
            draw_text_v(A.font, SCREEN_W - 110, y, rh, ACC, "›");
        }
    }

    /* footer area: unworkable warning, or inherit count + per-encoder summary */
    int sy = y0 + rows_vis * rh + 4;
    fill(16, sy, SCREEN_W - 32, 2, 40, 40, 46);
    int no_keys = 0, no_enc = 0;
    if (effective_unworkable(&no_keys, &no_enc)) {
        char w[120];
        snprintf(w, sizeof w, "⚠ %s — you may be unable to use norns",
                 no_keys && no_enc ? "no keys or encoder inputs"
                 : no_keys ? "no keys mapped (can't select)" : "no encoder inputs (can't scroll)");
        draw_text(A.font_sm, 18, sy + 8, REDC, w);
    } else {
        for (int e = 0; e < 3; e++) {
            char s[160], line[200];
            encoder_summary(e, s, sizeof s);
            snprintf(line, sizeof line, "E%d  ◄►  %s", e + 1, s);
            draw_text(A.font_sm, 18, sy + 6 + e * 16, DIM, line);
        }
        if (A.is_overlay) {
            char ic[48]; snprintf(ic, sizeof ic, "%d of %d controls inherited",
                                  inherited_count(), RT_N + 3);
            draw_text(A.font_sm, 18, sy + 6 + 3 * 16, HINT, ic);
        }
    }
    render_footer(A.is_overlay
        ? "◄› set  A inherit  X detect  Y flip U/D  Start apply  B back"
        : "◄› set  X detect  Y flip U/D  Start apply  B back");
}

static void render_keypick(void) {
    char t[64]; snprintf(t, sizeof t, "K%d buttons", A.keypick_key + 1);
    render_header(t, "toggle which buttons trigger this key");
    unsigned m = key_mask_from_str(A.work.k[A.keypick_key]);
    int y0 = 70, rh = 38;
    for (int i = 0; i < N_KEYBTN; i++) {
        int y = y0 + i * rh;
        int on = (m >> i) & 1, sel = (i == A.keypick_sel);
        if (sel) hl(y, rh);
        char line[48]; snprintf(line, sizeof line, "%s  %s", on ? "◉" : "○", KEY_BTN_LBL[i]);
        draw_text_v(A.font, 56, y, rh, sel ? ACC : WHITE, line);
    }
    render_footer("move: D-pad   A: toggle   B: done");
}

static void render_detect(void) {
    render_header("Input detection", "press the control · B to cancel");
    char line[96];
    if (A.detect_is_key)
        snprintf(line, sizeof line, "Press a button for  K%d", A.detect_target + 1);
    else
        snprintf(line, sizeof line, "Press a control to jump to its row");
    draw_text_c(A.font_lg, SCREEN_W / 2, 150, WHITE, line);
    draw_text_c(A.font_sm, SCREEN_W / 2, 200, A.detect_armed ? OKC : DIM,
                A.detect_armed ? "listening…" : "release all controls first…");
    if (!A.detect_is_key)
        draw_text_c(A.font_sm, SCREEN_W / 2, 250, DIM,
                    "stick · D-pad · L1/R1 · L2/R2 — then set its encoder with ◄►");
    render_footer("press a control   B: cancel");
}

/* Modal confirm over the screen it was opened from. Option 0 (left) is the
 * default. */
static void render_confirm(void) {
    if (A.confirm_kind == CF_EDITOR_BACK) render_editor(); else render_main();
    fill_a(0, 0, SCREEN_W, SCREEN_H, 0, 0, 0, 160);
    int bw = 500, bh = 170, bx = (SCREEN_W - bw) / 2, by = (SCREEN_H - bh) / 2;
    fill(bx, by, bw, bh, 34, 34, 40);
    fill(bx, by, bw, 3, HINT.r, HINT.g, HINT.b);
    const char *title, *o0, *o1;
    if (A.confirm_kind == CF_EDITOR_BACK) {
        title = "Discard changes to this screen?"; o0 = "Discard"; o1 = "Apply & keep";
    } else if (A.confirm_kind == CF_EXIT) {
        title = "Unsaved changes"; o0 = "Discard & exit"; o1 = "Save & exit";
    } else { /* CF_RESTORE_ALL */
        title = "Restore ALL controls to defaults?"; o0 = "Cancel"; o1 = "Restore";
    }
    draw_text_c(A.font, SCREEN_W / 2, by + 28, WHITE, title);
    int ow = 190, oh = 44, gap = 24;
    int x0 = SCREEN_W / 2 - ow - gap / 2, x1 = SCREEN_W / 2 + gap / 2, oy = by + 96;
    const char *labels[2] = { o0, o1 };
    int xs[2] = { x0, x1 };
    for (int i = 0; i < 2; i++) {
        int sel = (i == A.confirm_sel);
        if (sel) fill(xs[i], oy, ow, oh, HL_R, HL_G, HL_B);
        else     fill(xs[i], oy, ow, oh, 50, 50, 58);
        int tw = 0, th = 0; TTF_SizeUTF8(A.font, labels[i], &tw, &th);
        draw_text(A.font, xs[i] + (ow - tw) / 2, oy + (oh - th) / 2, sel ? ACC : WHITE, labels[i]);
    }
    render_footer("◄►: choose   A: confirm   B: cancel");
}

/* ── input actions ──────────────────────────────────────────────────────── */

static void editor_move(int d) {
    int i = A.ed_sel;
    do { i += d; } while (i >= 0 && i < A.n_rows && A.rows[i].type == ROW_HEAD);
    if (i >= 0 && i < A.n_rows) A.ed_sel = i;
}
static void editor_change(int d) {
    row_t *row = &A.rows[A.ed_sel];
    if (row->type == ROW_ROUTE) {
        A.work.route[row->idx] = (A.work.route[row->idx] + d + 4) % 4;
        A.ed_dirty = 1;
    }
}
/* Set the selected row back to the inherited (parent) value. */
static void editor_inherit(void) {
    row_t *row = &A.rows[A.ed_sel];
    if (!A.is_overlay) { toast("global has no parent"); return; }
    if (row->type == ROW_ROUTE) {
        A.work.route[row->idx] = A.base.route[row->idx];
        if (RT_IS_LSTICK_Y(row->idx)) A.work.lstick_y_inv = A.base.lstick_y_inv;
        if (RT_IS_RSTICK_Y(row->idx)) A.work.rstick_y_inv = A.base.rstick_y_inv;
    } else if (row->type == ROW_KEY) {
        strcpy(A.work.k[row->idx], A.base.k[row->idx]);
    }
    A.ed_dirty = 1;
    toast("inherited");
}
/* Flip the U/D invert on a stick Y route. */
static void editor_invert(void) {
    row_t *row = &A.rows[A.ed_sel];
    if (row->type != ROW_ROUTE) return;
    if (RT_IS_LSTICK_Y(row->idx))      A.work.lstick_y_inv ^= 1;
    else if (RT_IS_RSTICK_Y(row->idx)) A.work.rstick_y_inv ^= 1;
    else { toast("flip applies to stick U/D only"); return; }
    A.ed_dirty = 1;
}
/* Detection: route rows = "press a control to jump to its row"; key rows =
 * "press a button to assign it". */
static void enter_detect(void) {
    row_t *row = &A.rows[A.ed_sel];
    if (row->type == ROW_KEY)        { A.detect_is_key = 1; A.detect_target = row->idx; }
    else if (row->type == ROW_ROUTE) { A.detect_is_key = 0; A.detect_target = row->idx; }
    else return;
    A.detect_armed = 0;
    A.screen = SCR_DETECT;
}
static void editor_activate(void) {
    row_t *row = &A.rows[A.ed_sel];
    if (row->type == ROW_KEY) { A.keypick_key = row->idx; A.keypick_sel = 0; A.screen = SCR_KEYPICK; }
    else if (A.is_overlay) editor_inherit();   /* A = inherit this row */
    else editor_change(+1);
}

static void open_confirm(confirm_t kind) {
    A.confirm_kind = kind; A.confirm_sel = 0;   /* default = Discard */
    A.screen = SCR_CONFIRM;
}
static void main_activate(int *quit) {
    switch (A.main_sel) {
    case 0: open_editor(SCOPE_SYSTEM, "menu", "System (menu) controls"); break;
    case 1: open_editor(SCOPE_GLOBAL, NULL, "Global script controls"); break;
    case 2: scan_scripts(); A.screen = SCR_SCRIPTS; break;
    case 3: A.tuning_sel = 0; A.screen = SCR_TUNING; break;
    case 4: if (A.app_dirty) save_file(); else toast("nothing to save"); break;
    case 5: if (A.have_defs) open_confirm(CF_RESTORE_ALL); else toast("no defaults file"); break;
    case 6: if (A.app_dirty) open_confirm(CF_EXIT); else *quit = 1; break;
    }
}

/* Route detection jumps the cursor to the pressed control's row (the input is
 * the row; you then pick its encoder with ◄►). */
static void detect_jump_route(int rt) {
    for (int i = 0; i < A.n_rows; i++)
        if (A.rows[i].type == ROW_ROUTE && A.rows[i].idx == rt) { A.ed_sel = i; break; }
    toast("found");
    A.screen = SCR_EDITOR;
}
static void detect_apply_key(const char *btn_tok) {
    snprintf(A.work.k[A.detect_target], CONF_VALLEN, "%s", btn_tok);
    A.ed_dirty = 1;
    toast("detected");
    A.screen = SCR_EDITOR;
}

/* Read the controller during detection: arm once everything is released, then
 * capture the first actuation. B cancels. */
static void poll_detect(void) {
    if (!A.gc) { A.screen = SCR_EDITOR; return; }
    SDL_GameControllerUpdate();
    #define BTN(b) SDL_GameControllerGetButton(A.gc, b)
    #define AX(a)  SDL_GameControllerGetAxis(A.gc, a)
    int lx = AX(SDL_CONTROLLER_AXIS_LEFTX),  ly = AX(SDL_CONTROLLER_AXIS_LEFTY);
    int rx = AX(SDL_CONTROLLER_AXIS_RIGHTX), ry = AX(SDL_CONTROLLER_AXIS_RIGHTY);
    int lt = AX(SDL_CONTROLLER_AXIS_TRIGGERLEFT), rt = AX(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    const int TH = 18000, TT = 8000;
    int any = BTN(SDL_CONTROLLER_BUTTON_DPAD_UP) || BTN(SDL_CONTROLLER_BUTTON_DPAD_DOWN) ||
              BTN(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || BTN(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) ||
              BTN(SDL_CONTROLLER_BUTTON_A) || BTN(SDL_CONTROLLER_BUTTON_B) ||
              BTN(SDL_CONTROLLER_BUTTON_X) || BTN(SDL_CONTROLLER_BUTTON_Y) ||
              BTN(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) || BTN(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) ||
              abs(lx) > TH || abs(ly) > TH || abs(rx) > TH || abs(ry) > TH || lt > TT || rt > TT;
    if (!A.detect_armed) { if (!any) A.detect_armed = 1; return; }
    if (BTN(SDL_CONTROLLER_BUTTON_B)) { A.screen = SCR_EDITOR; return; }  /* cancel */

    if (A.detect_is_key) {
        if (BTN(SDL_CONTROLLER_BUTTON_A)) detect_apply_key("a");
        else if (BTN(SDL_CONTROLLER_BUTTON_X)) detect_apply_key("x");
        else if (BTN(SDL_CONTROLLER_BUTTON_Y)) detect_apply_key("y");
        else if (BTN(SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  detect_apply_key("l1");
        else if (BTN(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) detect_apply_key("r1");
        return;
    }
    /* route find: jump to the pressed control's row */
    if (BTN(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || BTN(SDL_CONTROLLER_BUTTON_DPAD_RIGHT))
        detect_jump_route(RT_DPAD_X);
    else if (BTN(SDL_CONTROLLER_BUTTON_DPAD_UP) || BTN(SDL_CONTROLLER_BUTTON_DPAD_DOWN))
        detect_jump_route(RT_DPAD_Y);
    else if (BTN(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) || BTN(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))
        detect_jump_route(RT_SHOULDERS);
    else if (lt > TT || rt > TT) detect_jump_route(RT_TRIGGERS);
    else if (abs(lx) > TH) detect_jump_route(RT_LSTICK_X);
    else if (abs(ly) > TH) detect_jump_route(RT_LSTICK_Y);
    else if (abs(rx) > TH) detect_jump_route(RT_RSTICK_X);
    else if (abs(ry) > TH) detect_jump_route(RT_RSTICK_Y);
    #undef BTN
    #undef AX
}

/* directional + button actions, shared by gamepad and keyboard. `x` = the X
 * face button (detect). */
/* Copy script[copy_from]'s overrides onto script[scripts_sel]. */
static void do_copy_script(void) {
    if (A.copy_from < 0 || A.copy_from >= A.n_scripts) return;
    char src[CONF_NAMELEN], dst[CONF_NAMELEN];
    snprintf(src, sizeof src, "script:%s", A.scripts[A.copy_from].name);
    snprintf(dst, sizeof dst, "script:%s", A.scripts[A.scripts_sel].name);
    conf_copy_section(&A.conf, src, dst);
    conf_prune_empty(&A.conf);
    A.app_dirty = 1;
    toast("controls copied");
    scan_scripts();
}

static int handle_nav(int up, int down, int left, int right,
                      int a, int b, int x, int y, int start, int *quit) {
    switch (A.screen) {
    case SCR_MAIN:
        if (up)    A.main_sel = (A.main_sel + 6) % 7;
        if (down)  A.main_sel = (A.main_sel + 1) % 7;
        if (a) main_activate(quit);
        if (b) { if (A.app_dirty) open_confirm(CF_EXIT); else *quit = 1; }
        break;
    case SCR_TUNING:
        if (up   && A.tuning_sel > 0) A.tuning_sel--;
        if (down && A.tuning_sel < N_TUNABLES - 1) A.tuning_sel++;
        if (left)  tunable_adjust(A.tuning_sel, -1);
        if (right) tunable_adjust(A.tuning_sel, +1);
        if (b) A.screen = SCR_MAIN;
        break;
    case SCR_SCRIPTS:
        if (up   && A.scripts_sel > 0) A.scripts_sel--;
        if (down && A.scripts_sel < A.n_scripts - 1) A.scripts_sel++;
        if (a && A.n_scripts > 0) {
            script_ent *s = &A.scripts[A.scripts_sel];
            char ov[CONF_NAMELEN], ti[80];
            snprintf(ov, sizeof ov, "script:%s", s->name);
            snprintf(ti, sizeof ti, "Script: %s", s->name);
            open_editor(SCOPE_SCRIPT, ov, ti);
        }
        if (x && A.n_scripts > 1) { A.copy_from = A.scripts_sel; A.screen = SCR_COPYPICK; }
        if (b) A.screen = SCR_MAIN;
        break;
    case SCR_COPYPICK:   /* pick the SOURCE to copy onto scripts_sel */
        if (up   && A.copy_from > 0) A.copy_from--;
        if (down && A.copy_from < A.n_scripts - 1) A.copy_from++;
        if (a) { if (A.copy_from == A.scripts_sel) toast("pick a different script");
                 else { do_copy_script(); A.screen = SCR_SCRIPTS; } }
        if (b) A.screen = SCR_SCRIPTS;
        break;
    case SCR_EDITOR:
        if (up)    editor_move(-1);
        if (down)  editor_move(+1);
        if (left)  editor_change(-1);
        if (right) editor_change(+1);
        if (a)     editor_activate();
        if (x)     enter_detect();
        if (y)     editor_invert();
        if (start) { apply_scope(); toast("applied"); }
        if (b) { if (A.ed_dirty) open_confirm(CF_EDITOR_BACK); else editor_to_parent(); }
        break;
    case SCR_KEYPICK:
        if (up   && A.keypick_sel > 0) A.keypick_sel--;
        if (down && A.keypick_sel < N_KEYBTN - 1) A.keypick_sel++;
        if (a) { unsigned m = key_mask_from_str(A.work.k[A.keypick_key]);
                 m ^= (1u << A.keypick_sel);
                 key_str_from_mask(m, A.work.k[A.keypick_key], CONF_VALLEN);
                 A.ed_dirty = 1; }
        if (b) A.screen = SCR_EDITOR;
        break;
    case SCR_DETECT:
        break;   /* handled by poll_detect() in the loop */
    case SCR_CONFIRM:
        if (left || right) A.confirm_sel ^= 1;
        if (b) A.screen = (A.confirm_kind == CF_EDITOR_BACK) ? SCR_EDITOR : SCR_MAIN;
        if (a) {
            if (A.confirm_kind == CF_EDITOR_BACK) {
                if (A.confirm_sel == 1) apply_scope();   /* else discard scratch */
                editor_to_parent();
            } else if (A.confirm_kind == CF_EXIT) {
                if (A.confirm_sel == 1) save_file();      /* else discard model edits */
                *quit = 1;
            } else {                                     /* CF_RESTORE_ALL */
                if (A.confirm_sel == 1) restore_all_defaults();
                A.screen = SCR_MAIN;
            }
        }
        break;
    }
    return 0;
}

/* ── main ───────────────────────────────────────────────────────────────── */

static TTF_Font *load_font(int size) {
    const char *paths[] = {
        "assets/font.ttf",
        "/etc/emulationstation/themes/panicos/_inc/fonts/SpaceMono-Regular.ttf",
        "/etc/emulationstation/themes/panicos/_inc/fonts/NotoSansMono-Regular.ttf",
        NULL
    };
    for (int i = 0; paths[i]; i++) { TTF_Font *f = TTF_OpenFont(paths[i], size); if (f) return f; }
    return NULL;
}

/* Headless self-check: exercise the real load/scan/mapping/serialize path
 * against the device's config without SDL. `--check <conf> <scripts>`. */
static int run_check(void) {
    load_conf();
    load_defaults();
    printf("scheme = %s\n", A.scheme);
    printf("defaults = %s\n", A.have_defs ? "loaded (read-only baseline)" : "MISSING");
    scan_scripts();
    printf("scripts detected: %d\n", A.n_scripts);
    for (int i = 0; i < A.n_scripts; i++)
        printf("  [%-9s] %s\n", A.scripts[i].customised ? "custom" : "inherit",
               A.scripts[i].name);
    mapping m; mapping_read_base(&A.conf, A.scheme, &m);
    printf("BASE  lX=%d lY=%d rX=%d rY=%d | dpadX=%d dpadY=%d L1R1=%d L2R2=%d | K=%s/%s/%s\n",
           m.route[RT_LSTICK_X], m.route[RT_LSTICK_Y], m.route[RT_RSTICK_X], m.route[RT_RSTICK_Y],
           m.route[RT_DPAD_X], m.route[RT_DPAD_Y], m.route[RT_SHOULDERS], m.route[RT_TRIGGERS],
           m.k[0], m.k[1], m.k[2]);
    mapping mm; mapping_read_overlay(&A.conf, A.scheme, "menu", &mm);
    printf("MENU  K2=%s (overlay applied over base)\n", mm.k[1]);
    mapping mp; mapping_read_overlay(&A.conf, A.scheme, "script:pixels", &mp);
    printf("PIXELS lX=%d lY=%d rX=%d rY=%d rYinv=%d\n",
           mp.route[RT_LSTICK_X], mp.route[RT_LSTICK_Y], mp.route[RT_RSTICK_X],
           mp.route[RT_RSTICK_Y], mp.rstick_y_inv);
    char out[16384];
    int n = conf_serialize(&A.conf, out, sizeof out);
    printf("serialize: %d bytes, round-trips OK\n", n);
    return 0;
}

int main(int argc, char **argv) {
    int check = (argc > 1 && !strcmp(argv[1], "--check"));
    int b = check ? 1 : 0;
    snprintf(A.conf_path, sizeof A.conf_path, "%s",
             argc > b + 1 ? argv[b + 1] : "controls.conf");
    snprintf(A.scripts_dir, sizeof A.scripts_dir, "%s",
             argc > b + 2 ? argv[b + 2] : "dust/code");
    if (argc > b + 3) snprintf(A.defaults_path, sizeof A.defaults_path, "%s", argv[b + 3]);
    if (check) return run_check();

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    if (TTF_Init() != 0) { fprintf(stderr, "TTF_Init: %s\n", TTF_GetError()); return 1; }

    SDL_Window *win = SDL_CreateWindow("Norns Controls",
        SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
        SCREEN_W, SCREEN_H, SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    A.ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!A.ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return 1; }
    SDL_RenderSetLogicalSize(A.ren, SCREEN_W, SCREEN_H);

    A.font    = load_font(22);
    A.font_sm = load_font(15);
    A.font_lg = load_font(26);
    if (!A.font || !A.font_sm || !A.font_lg) {
        fprintf(stderr, "font load failed\n"); return 1;
    }
    A.fh = TTF_FontHeight(A.font);
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) { A.gc = SDL_GameControllerOpen(i); if (A.gc) break; }

    load_conf();
    load_defaults();
    A.screen = SCR_MAIN;

    /* held-direction auto-repeat for analog sticks (dpad uses discrete events) */
    int hold_dir = 0, hold_frames = 0;

    int quit = 0;
    while (!quit) {
        int up = 0, down = 0, left = 0, right = 0, a = 0, b = 0, x = 0, y = 0, start = 0;
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) quit = 1;
            else if (e.type == SDL_CONTROLLERBUTTONDOWN) {
                switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP:    up = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:  down = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:  left = 1; break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: right = 1; break;
                case SDL_CONTROLLER_BUTTON_A:          a = 1; break;
                case SDL_CONTROLLER_BUTTON_B:          b = 1; break;
                case SDL_CONTROLLER_BUTTON_X:          x = 1; break;
                case SDL_CONTROLLER_BUTTON_Y:          y = 1; break;
                case SDL_CONTROLLER_BUTTON_START:      start = 1; break;
                case SDL_CONTROLLER_BUTTON_BACK:       b = 1; break;  /* Select = back */
                default: break;
                }
            } else if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                case SDLK_UP: up = 1; break; case SDLK_DOWN: down = 1; break;
                case SDLK_LEFT: left = 1; break; case SDLK_RIGHT: right = 1; break;
                case SDLK_RETURN: case SDLK_SPACE: a = 1; break;
                case SDLK_ESCAPE: b = 1; break;
                case SDLK_d: x = 1; break;       /* detect */
                case SDLK_r: y = 1; break;       /* reset to default */
                case SDLK_s: start = 1; break;
                case SDLK_q: quit = 1; break;
                default: break;
                }
            }
        }
        /* analog stick navigation with auto-repeat (suppressed in detect mode,
         * where poll_detect reads the raw sticks itself) */
        if (A.gc && A.screen != SCR_DETECT) {
            int ax = SDL_GameControllerGetAxis(A.gc, SDL_CONTROLLER_AXIS_LEFTX);
            int ay = SDL_GameControllerGetAxis(A.gc, SDL_CONTROLLER_AXIS_LEFTY);
            int dir = 0;
            if (ay < -16000) dir = 1; else if (ay > 16000) dir = 2;
            else if (ax < -16000) dir = 3; else if (ax > 16000) dir = 4;
            if (dir && dir == hold_dir) {
                hold_frames++;
                if (hold_frames == 1 || (hold_frames > 22 && hold_frames % 6 == 0)) {
                    if (dir == 1) up = 1; else if (dir == 2) down = 1;
                    else if (dir == 3) left = 1; else right = 1;
                }
            } else { hold_dir = dir; hold_frames = 0;
                     if (dir) { if (dir == 1) up = 1; else if (dir == 2) down = 1;
                                else if (dir == 3) left = 1; else right = 1; } }
        }

        if (A.screen == SCR_DETECT) poll_detect();
        else if (up || down || left || right || a || b || x || y || start)
            handle_nav(up, down, left, right, a, b, x, y, start, &quit);

        switch (A.screen) {
        case SCR_MAIN:    render_main();    break;
        case SCR_SCRIPTS: render_scripts(); break;
        case SCR_EDITOR:  render_editor();  break;
        case SCR_KEYPICK: render_keypick(); break;
        case SCR_DETECT:  render_detect();  break;
        case SCR_CONFIRM: render_confirm(); break;
        case SCR_COPYPICK: render_copypick(); break;
        case SCR_TUNING:  render_tuning();  break;
        }
        if (A.toast_ttl > 0) A.toast_ttl--;
        SDL_RenderPresent(A.ren);
        SDL_Delay(16);
    }

    if (A.gc) SDL_GameControllerClose(A.gc);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
