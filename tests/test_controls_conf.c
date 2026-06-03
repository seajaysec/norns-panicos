/* tests/test_controls_conf.c — unit tests for the GUI's config read/edit model.
 * Pure C, no SDL: exercises the real controls-conf.h the device tool uses. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/controls-conf.h"

/* A representative file: globals + active scheme + two overlays, like the one
 * Norns.sh seeds (acceleration tunables included to prove they survive edits). */
static const char *SAMPLE =
    "scheme = sticks\n"
    "k1 = y\n"
    "k2 = x\n"
    "k3 = a\n"
    "stick_deadzone = 8192\n"
    "accel_max = 5\n"
    "[sticks]\n"
    "lstick_x = 2\n"
    "rstick_x = 3\n"
    "dpad_x = 1\n"
    "dpad_y = 2\n"
    "shoulders = 1\n"
    "triggers = 3\n"
    "[menu]\n"
    "k2 = x, b\n"
    "dpad_y_invert = 1\n"
    "[script:pixels]\n"
    "lstick_x = 1\n"
    "lstick_y = 1\n"
    "rstick_x = 2\n"
    "rstick_y = 3\n"
    "rstick_y_invert = 1\n";

static void test_parse_sections(void) {
    conf_file f; conf_parse(&f, SAMPLE);
    assert(conf_section_by_name(&f, "") != NULL);        /* globals */
    assert(conf_section_by_name(&f, "sticks") != NULL);
    assert(conf_section_by_name(&f, "menu") != NULL);
    assert(conf_section_by_name(&f, "script:pixels") != NULL);
    assert(conf_section_by_name(&f, "ghost") == NULL);
    assert(!strcmp(conf_active_scheme(&f), "sticks"));
    /* underscore/dash canon: dpad-y-invert matches stored dpad_y_invert */
    assert(!strcmp(conf_get(conf_section_by_name(&f, "menu"), "dpad-y-invert"), "1"));
    printf("  PASS test_parse_sections\n");
}

static void test_get_set_unset(void) {
    conf_file f; conf_parse(&f, SAMPLE);
    conf_section *g = conf_section_by_name(&f, "");
    assert(!strcmp(conf_get(g, "k1"), "y"));
    conf_set(g, "k1", "y, l1");
    assert(!strcmp(conf_get(g, "k1"), "y, l1"));     /* update in place */
    conf_set(g, "newkey", "42");
    assert(!strcmp(conf_get(g, "newkey"), "42"));    /* append */
    conf_unset(g, "newkey");
    assert(conf_get(g, "newkey") == NULL);           /* removed */
    printf("  PASS test_get_set_unset\n");
}

static void test_roundtrip_preserves_tunables(void) {
    conf_file f; conf_parse(&f, SAMPLE);
    char out[4096];
    conf_serialize(&f, out, sizeof out);
    conf_file g; conf_parse(&g, out);                /* reparse the output */
    /* tunables we never touch must survive a parse→serialise→parse cycle */
    assert(!strcmp(conf_get(conf_section_by_name(&g, ""), "stick_deadzone"), "8192"));
    assert(!strcmp(conf_get(conf_section_by_name(&g, ""), "accel_max"), "5"));
    assert(!strcmp(conf_get(conf_section_by_name(&g, ""), "scheme"), "sticks"));
    assert(!strcmp(conf_get(conf_section_by_name(&g, "script:pixels"), "rstick_y_invert"), "1"));
    printf("  PASS test_roundtrip_preserves_tunables\n");
}

static void test_mapping_read_base(void) {
    conf_file f; conf_parse(&f, SAMPLE);
    mapping m; mapping_read_base(&f, conf_active_scheme(&f), &m);
    assert(m.route[RT_LSTICK_X] == 2);       /* left  X → E2 */
    assert(m.route[RT_LSTICK_Y] == 0);       /* left  Y unbound */
    assert(m.route[RT_RSTICK_X] == 3);       /* right X → E3 */
    assert(m.route[RT_DPAD_X] == 1);
    assert(m.route[RT_DPAD_Y] == 2);
    assert(m.route[RT_SHOULDERS] == 1);
    assert(m.route[RT_TRIGGERS] == 3);
    assert(m.lstick_y_inv == 0 && m.rstick_y_inv == 0);
    assert(!strcmp(m.k[0], "y") && !strcmp(m.k[1], "x") && !strcmp(m.k[2], "a"));
    printf("  PASS test_mapping_read_base\n");
}

static void test_mapping_read_overlay(void) {
    conf_file f; conf_parse(&f, SAMPLE);
    mapping m; mapping_read_overlay(&f, "sticks", "script:pixels", &m);
    assert(m.route[RT_LSTICK_X] == 1 && m.route[RT_LSTICK_Y] == 1);  /* both left → E1 */
    assert(m.route[RT_RSTICK_X] == 2);                                /* right X → E2 */
    assert(m.route[RT_RSTICK_Y] == 3 && m.rstick_y_inv == 1);         /* right Y inv → E3 */
    assert(m.route[RT_DPAD_X] == 1);         /* inherited from base */
    assert(!strcmp(m.k[0], "y"));            /* inherited */

    mapping mm; mapping_read_overlay(&f, "sticks", "menu", &mm);
    assert(!strcmp(mm.k[1], "x, b"));        /* menu overlay key */
    printf("  PASS test_mapping_read_overlay\n");
}

static void test_overlay_written_as_diff(void) {
    conf_file f; conf_parse(&f, SAMPLE);
    /* Edit a NEW script overlay: route left stick U/D to E1 (base has it off). */
    mapping m; mapping_read_base(&f, "sticks", &m);
    m.route[RT_LSTICK_Y] = 1;
    mapping_write_overlay(&f, "sticks", "script:awake", &m);
    conf_prune_empty(&f);
    conf_section *s = conf_section_by_name(&f, "script:awake");
    assert(s != NULL && s->nkv == 1);                 /* ONLY the changed field */
    assert(!strcmp(conf_get(s, "lstick_y"), "1"));
    assert(conf_get(s, "lstick_x") == NULL);          /* matching fields omitted */
    assert(conf_get(s, "dpad_x") == NULL);
    printf("  PASS test_overlay_written_as_diff\n");
}

static void test_inherit_detection(void) {
    /* The GUI marks a row inherited when its overlay value == base value. */
    conf_file f; conf_parse(&f, SAMPLE);
    mapping base; mapping_read_base(&f, "sticks", &base);
    mapping eff;  mapping_read_overlay(&f, "sticks", "script:pixels", &eff);
    /* pixels overrides the stick routes but inherits D-pad + keys */
    assert(eff.route[RT_LSTICK_X] != base.route[RT_LSTICK_X]);   /* overridden */
    assert(eff.route[RT_RSTICK_Y] != base.route[RT_RSTICK_Y]);   /* overridden */
    assert(eff.route[RT_DPAD_X] == base.route[RT_DPAD_X]);       /* inherited */
    assert(eff.route[RT_SHOULDERS] == base.route[RT_SHOULDERS]); /* inherited */
    assert(!strcmp(eff.k[1], base.k[1]));                        /* inherited */
    printf("  PASS test_inherit_detection\n");
}

static void test_copy_section(void) {
    /* Copy one script's overrides onto another (preserve inherit elsewhere). */
    conf_file f; conf_parse(&f, SAMPLE);
    conf_copy_section(&f, "script:pixels", "script:awake");
    conf_section *s = conf_section_by_name(&f, "script:awake");
    assert(s != NULL);
    assert(!strcmp(conf_get(s, "lstick_x"), "1"));     /* got pixels' overrides */
    assert(!strcmp(conf_get(s, "rstick_y_invert"), "1"));
    assert(conf_get(s, "dpad_x") == NULL);             /* and nothing pixels inherits */
    printf("  PASS test_copy_section\n");
}

static void test_revert_to_default_removes_section(void) {
    conf_file f; conf_parse(&f, SAMPLE);
    assert(conf_section_has_keys(&f, "script:pixels") == 1);   /* customised */
    /* Set pixels back to exactly the base → its overlay should vanish. */
    mapping base; mapping_read_base(&f, "sticks", &base);
    mapping_write_overlay(&f, "sticks", "script:pixels", &base);
    conf_prune_empty(&f);
    assert(conf_section_by_name(&f, "script:pixels") == NULL); /* now inherits */
    assert(conf_section_has_keys(&f, "script:pixels") == 0);
    printf("  PASS test_revert_to_default_removes_section\n");
}

static void test_write_base_updates_scheme_and_globals(void) {
    conf_file f; conf_parse(&f, SAMPLE);
    mapping m; mapping_read_base(&f, "sticks", &m);
    m.route[RT_LSTICK_Y] = 2;            /* route left U/D → E2 */
    strcpy(m.k[0], "y, b");              /* change a global key  */
    m.route[RT_TRIGGERS] = 0;            /* unbind L2/R2         */
    mapping_write_base(&f, "sticks", &m);
    assert(!strcmp(conf_get(conf_section_by_name(&f, "sticks"), "lstick_y"), "2"));
    assert(!strcmp(conf_get(conf_section_by_name(&f, ""), "k1"), "y, b"));
    assert(!strcmp(conf_get(conf_section_by_name(&f, "sticks"), "triggers"), "none"));
    /* untouched tunable still present */
    assert(!strcmp(conf_get(conf_section_by_name(&f, ""), "accel_max"), "5"));
    printf("  PASS test_write_base_updates_scheme_and_globals\n");
}

int main(void) {
    printf("Running controls-conf tests...\n");
    test_parse_sections();
    test_get_set_unset();
    test_roundtrip_preserves_tunables();
    test_mapping_read_base();
    test_mapping_read_overlay();
    test_overlay_written_as_diff();
    test_inherit_detection();
    test_copy_section();
    test_revert_to_default_removes_section();
    test_write_base_updates_scheme_and_globals();
    printf("All tests passed.\n");
    return 0;
}
