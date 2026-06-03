/*
 * controls-conf.h — structured read/edit/serialize of controls.conf.
 *
 * The Norns Controls GUI edits the SAME file norns-panicos reads, so it must
 * preserve everything it doesn't touch (the `scheme` selector, acceleration and
 * deadzone tunables, extra schemes) while changing only encoder/key mappings.
 *
 * This header is the pure, SDL-free core: parse the file into ordered sections
 * of key=value pairs, read/write a "mapping" view (the encoder sources, the
 * D-pad/shoulder/trigger pair targets, and the three keys) for a given scope,
 * and serialise back. Overlays ([menu], [script:NAME]) are written as MINIMAL
 * DIFFS against the base (globals + active scheme) so a script that matches the
 * defaults leaves no section at all — which is exactly what marks it as
 * "inherits defaults" in the UI.
 *
 * Kept header-only and allocation-free (fixed caps) so the same code runs on the
 * device and under the native unit tests (tests/test_controls_conf.c).
 */
#ifndef CONTROLS_CONF_H
#define CONTROLS_CONF_H

#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>

#define CONF_MAX_SECTIONS 96
#define CONF_MAX_KEYS     48
#define CONF_KEYLEN       24
#define CONF_VALLEN       64
#define CONF_NAMELEN      48

typedef struct { char key[CONF_KEYLEN]; char val[CONF_VALLEN]; } conf_kv;
typedef struct {
    char    name[CONF_NAMELEN];   /* "" = globals (lines before any [section]) */
    conf_kv kv[CONF_MAX_KEYS];
    int     nkv;
} conf_section;
typedef struct { conf_section sec[CONF_MAX_SECTIONS]; int nsec; } conf_file;

/* ── small string utils ─────────────────────────────────────────────────── */

/* Canonical key/section form for matching: lowercase, '-' → '_'. The engine
 * accepts both, so we normalise to underscores (matches the seeded template). */
static inline void conf_canon(char *s) {
    for (; *s; s++) { *s = (char)tolower((unsigned char)*s); if (*s == '-') *s = '_'; }
}
static inline char *conf_trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}
static inline int conf_canon_eq(const char *a, const char *b) {
    char ca[CONF_NAMELEN], cb[CONF_NAMELEN];
    strncpy(ca, a, sizeof ca - 1); ca[sizeof ca - 1] = '\0'; conf_canon(ca);
    strncpy(cb, b, sizeof cb - 1); cb[sizeof cb - 1] = '\0'; conf_canon(cb);
    return strcmp(ca, cb) == 0;
}

/* ── section + key access ───────────────────────────────────────────────── */

static inline conf_section *conf_section_by_name(conf_file *f, const char *name) {
    for (int i = 0; i < f->nsec; i++)
        if (conf_canon_eq(f->sec[i].name, name)) return &f->sec[i];
    return NULL;
}
static inline conf_section *conf_section_add(conf_file *f, const char *name) {
    conf_section *s = conf_section_by_name(f, name);
    if (s) return s;
    if (f->nsec >= CONF_MAX_SECTIONS) return NULL;
    s = &f->sec[f->nsec++];
    memset(s, 0, sizeof *s);
    strncpy(s->name, name, sizeof s->name - 1);
    return s;
}
static inline const char *conf_get(conf_section *s, const char *key) {
    if (!s) return NULL;
    for (int i = 0; i < s->nkv; i++)
        if (conf_canon_eq(s->kv[i].key, key)) return s->kv[i].val;
    return NULL;
}
static inline void conf_set(conf_section *s, const char *key, const char *val) {
    if (!s) return;
    for (int i = 0; i < s->nkv; i++) {
        if (conf_canon_eq(s->kv[i].key, key)) {
            strncpy(s->kv[i].val, val, CONF_VALLEN - 1); s->kv[i].val[CONF_VALLEN - 1] = '\0';
            return;
        }
    }
    if (s->nkv >= CONF_MAX_KEYS) return;
    conf_kv *kv = &s->kv[s->nkv++];
    strncpy(kv->key, key, CONF_KEYLEN - 1); kv->key[CONF_KEYLEN - 1] = '\0';
    strncpy(kv->val, val, CONF_VALLEN - 1); kv->val[CONF_VALLEN - 1] = '\0';
}
static inline void conf_unset(conf_section *s, const char *key) {
    if (!s) return;
    for (int i = 0; i < s->nkv; i++) {
        if (conf_canon_eq(s->kv[i].key, key)) {
            for (int j = i + 1; j < s->nkv; j++) s->kv[j - 1] = s->kv[j];
            s->nkv--;
            return;
        }
    }
}
/* A section "exists with content" → the script/menu is customised (vs inherits). */
static inline int conf_section_has_keys(conf_file *f, const char *name) {
    conf_section *s = conf_section_by_name(f, name);
    return s && s->nkv > 0;
}
/* Replace section `to`'s keys with a copy of section `from`'s (creating `to` if
 * needed). Used to copy one script's override set onto another. */
static inline void conf_copy_section(conf_file *f, const char *from, const char *to) {
    conf_section *src = conf_section_by_name(f, from);
    conf_section *dst = conf_section_add(f, to);
    if (!src || !dst) return;
    dst->nkv = 0;
    for (int i = 0; i < src->nkv; i++) conf_set(dst, src->kv[i].key, src->kv[i].val);
}
/* Drop empty named sections (keep globals) so all-inherited scopes write nothing. */
static inline void conf_prune_empty(conf_file *f) {
    int w = 0;
    for (int i = 0; i < f->nsec; i++)
        if (f->sec[i].name[0] == '\0' || f->sec[i].nkv > 0) {
            if (w != i) f->sec[w] = f->sec[i];
            w++;
        }
    f->nsec = w;
}

/* ── parse / serialise ──────────────────────────────────────────────────── */

static inline void conf_parse(conf_file *f, const char *text) {
    memset(f, 0, sizeof *f);
    conf_section *cur = conf_section_add(f, "");   /* globals */
    char line[256];
    const char *p = text ? text : "";
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof line) len = sizeof line - 1;
        memcpy(line, p, len); line[len] = '\0';
        p = nl ? nl + 1 : p + strlen(p);
        char *h = strchr(line, '#'); if (h) *h = '\0';
        char *t = conf_trim(line);
        if (!*t) continue;
        if (*t == '[') {
            char *e = strchr(t, ']'); if (e) *e = '\0';
            cur = conf_section_add(f, conf_trim(t + 1));
            continue;
        }
        char *eq = strchr(t, '='); if (!eq) continue;
        *eq = '\0';
        char *k = conf_trim(t), *v = conf_trim(eq + 1);
        if (*k && cur) conf_set(cur, k, v);
    }
}

/* Serialise to `out` (NUL-terminated). Returns bytes that would be written
 * (may exceed cap, like snprintf). Globals first (no header), then each section. */
static inline int conf_serialize(conf_file *f, char *out, size_t cap) {
    size_t n = 0;
#define CONF_EMIT(...) do { \
        int w_ = snprintf(out + (n < cap ? n : cap), n < cap ? cap - n : 0, __VA_ARGS__); \
        if (w_ < 0) return -1; \
        n += (size_t)w_; \
    } while (0)
    CONF_EMIT("# norns-panicos controls — managed by the Norns Controls tool.\n");
    for (int i = 0; i < f->nsec; i++) {
        conf_section *s = &f->sec[i];
        if (s->name[0]) CONF_EMIT("\n[%s]\n", s->name);
        for (int j = 0; j < s->nkv; j++)
            CONF_EMIT("%s = %s\n", s->kv[j].key, s->kv[j].val);
    }
#undef CONF_EMIT
    if (cap) out[n < cap ? n : cap - 1] = '\0';
    return (int)n;
}

/* ── mapping view (the only thing the GUI edits) ────────────────────────── */

/* Every input axis/pair is a "route" to an encoder (0 = none/off, 1..3 = E1..E3),
 * mirroring the per-axis engine. Fixed order = editor row order. */
enum { RT_DPAD_X = 0, RT_DPAD_Y, RT_LSTICK_X, RT_LSTICK_Y,
       RT_RSTICK_X, RT_RSTICK_Y, RT_SHOULDERS, RT_TRIGGERS, RT_N };
static const char *const ROUTE_KEY[RT_N] = {
    "dpad_x", "dpad_y", "lstick_x", "lstick_y",
    "rstick_x", "rstick_y", "shoulders", "triggers"
};
/* The two U/D stick routes that carry an invert flag (key index → invert key). */
#define RT_LSTICK_Y_INVKEY "lstick_y_invert"
#define RT_RSTICK_Y_INVKEY "rstick_y_invert"

typedef struct {
    int  route[RT_N];         /* per-axis/pair → 0 none, 1..3 = E1..E3            */
    int  lstick_y_inv, rstick_y_inv;
    char k[3][CONF_VALLEN];   /* K1/K2/K3 button list ("y", "x, b", "a", "none")  */
} mapping;

static inline void mapping_defaults(mapping *m) {
    for (int i = 0; i < RT_N; i++) m->route[i] = 0;
    m->lstick_y_inv = m->rstick_y_inv = 0;
    for (int i = 0; i < 3; i++) strcpy(m->k[i], "none");
}
/* Legacy compat: translate an old `e<N> = <source>` line in section s into the
 * per-axis routes (mirrors controls__legacy_enc in norns-controls.h). */
static inline void mapping__apply_legacy(mapping *m, conf_section *s) {
    for (int e = 0; e < 3; e++) {
        char key[4] = { 'e', (char)('1' + e), 0, 0 };
        const char *v = conf_get(s, key);
        if (!v) continue;
        char tok[CONF_VALLEN]; strncpy(tok, v, sizeof tok - 1); tok[sizeof tok - 1] = '\0';
        conf_canon(tok);
        int E = e + 1;
        /* clear any stick axis already on this encoder */
        for (int r = RT_LSTICK_X; r <= RT_RSTICK_Y; r++) if (m->route[r] == E) m->route[r] = 0;
        if (!strcmp(tok, "lstick") || !strcmp(tok, "lstick_x")) m->route[RT_LSTICK_X] = E;
        else if (!strcmp(tok, "lstick_y"))     { m->route[RT_LSTICK_Y] = E; m->lstick_y_inv = 0; }
        else if (!strcmp(tok, "lstick_y_inv")) { m->route[RT_LSTICK_Y] = E; m->lstick_y_inv = 1; }
        else if (!strcmp(tok, "lstick_xy"))    { m->route[RT_LSTICK_X] = E; m->route[RT_LSTICK_Y] = E; }
        else if (!strcmp(tok, "rstick") || !strcmp(tok, "rstick_x")) m->route[RT_RSTICK_X] = E;
        else if (!strcmp(tok, "rstick_y"))     { m->route[RT_RSTICK_Y] = E; m->rstick_y_inv = 0; }
        else if (!strcmp(tok, "rstick_y_inv")) { m->route[RT_RSTICK_Y] = E; m->rstick_y_inv = 1; }
        else if (!strcmp(tok, "rstick_xy"))    { m->route[RT_RSTICK_X] = E; m->route[RT_RSTICK_Y] = E; }
    }
}
static inline void mapping_apply_section(mapping *m, conf_section *s) {
    if (!s) return;
    for (int i = 0; i < RT_N; i++) {
        const char *v = conf_get(s, ROUTE_KEY[i]);
        if (v) { int n = atoi(v); m->route[i] = (n >= 1 && n <= 3) ? n : 0; }
    }
    const char *li = conf_get(s, RT_LSTICK_Y_INVKEY); if (li) m->lstick_y_inv = atoi(li) ? 1 : 0;
    const char *ri = conf_get(s, RT_RSTICK_Y_INVKEY); if (ri) m->rstick_y_inv = atoi(ri) ? 1 : 0;
    for (int i = 0; i < 3; i++) {
        char key[4] = { 'k', (char)('1' + i), 0, 0 };
        const char *v = conf_get(s, key);
        if (v) { strncpy(m->k[i], v, CONF_VALLEN - 1); m->k[i][CONF_VALLEN - 1] = '\0'; }
    }
    mapping__apply_legacy(m, s);    /* legacy e1/e2/e3 last so it can refine */
}
/* Base = globals (keys live here) + the active scheme section. */
static inline void mapping_read_base(conf_file *f, const char *scheme, mapping *m) {
    mapping_defaults(m);
    mapping_apply_section(m, conf_section_by_name(f, ""));
    if (scheme && *scheme) mapping_apply_section(m, conf_section_by_name(f, scheme));
}
/* Effective mapping for an overlay scope = base then the overlay section. */
static inline void mapping_read_overlay(conf_file *f, const char *scheme,
                                        const char *overlay, mapping *m) {
    mapping_read_base(f, scheme, m);
    if (overlay && *overlay) mapping_apply_section(m, conf_section_by_name(f, overlay));
}

static inline void mapping__set_route(conf_section *s, int i, int val) {
    char buf[8];
    if (val >= 1 && val <= 3) snprintf(buf, sizeof buf, "%d", val); else strcpy(buf, "none");
    conf_set(s, ROUTE_KEY[i], buf);
}
/* Write an edited BASE mapping: keys → globals, routes/inverts → [scheme]. */
static inline void mapping_write_base(conf_file *f, const char *scheme, const mapping *m) {
    conf_section *g = conf_section_add(f, "");
    conf_section *s = conf_section_add(f, (scheme && *scheme) ? scheme : "");
    for (int i = 0; i < RT_N; i++) mapping__set_route(s, i, m->route[i]);
    conf_set(s, RT_LSTICK_Y_INVKEY, m->lstick_y_inv ? "1" : "0");
    conf_set(s, RT_RSTICK_Y_INVKEY, m->rstick_y_inv ? "1" : "0");
    for (int i = 0; i < 3; i++) {
        char key[4] = { 'k', (char)('1' + i), 0, 0 };
        conf_set(g, key, m->k[i][0] ? m->k[i] : "none");
    }
}
/* Write an edited OVERLAY mapping as a minimal diff vs base into [overlay]:
 * each field that differs is set, each that matches is removed. */
static inline void mapping_write_overlay(conf_file *f, const char *scheme,
                                         const char *overlay, const mapping *edited) {
    mapping base; mapping_read_base(f, scheme, &base);
    conf_section *s = conf_section_add(f, overlay);
    for (int i = 0; i < RT_N; i++) {
        if (edited->route[i] != base.route[i]) mapping__set_route(s, i, edited->route[i]);
        else conf_unset(s, ROUTE_KEY[i]);
    }
    if (edited->lstick_y_inv != base.lstick_y_inv) conf_set(s, RT_LSTICK_Y_INVKEY, edited->lstick_y_inv ? "1" : "0");
    else conf_unset(s, RT_LSTICK_Y_INVKEY);
    if (edited->rstick_y_inv != base.rstick_y_inv) conf_set(s, RT_RSTICK_Y_INVKEY, edited->rstick_y_inv ? "1" : "0");
    else conf_unset(s, RT_RSTICK_Y_INVKEY);
    for (int i = 0; i < 3; i++) {
        char key[4] = { 'k', (char)('1' + i), 0, 0 };
        if (strcmp(edited->k[i], base.k[i]) != 0) conf_set(s, key, edited->k[i][0] ? edited->k[i] : "none");
        else conf_unset(s, key);
    }
}

/* Active scheme name from the `scheme = …` selector (defaults to "sticks"). */
static inline const char *conf_active_scheme(conf_file *f) {
    const char *v = conf_get(conf_section_by_name(f, ""), "scheme");
    return (v && *v) ? v : "sticks";
}

#endif /* CONTROLS_CONF_H */
