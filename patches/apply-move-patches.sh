#!/bin/sh
# apply-move-patches.sh — Patch norns source for Move (FIFO I/O, no GPIO/SPI)
# Run inside the chroot: cd /home/we/norns && sh /path/to/apply-move-patches.sh
set -e

NORNS_DIR="${1:-/home/we/norns}"
cd "$NORNS_DIR"

echo "=== Patching norns for Move ==="

# ── 1. Replace ssd1322.c with FIFO screen driver ──
cat > matron/src/hardware/screen/ssd1322.c << 'SCREENEOF'
/*
 * ssd1322.c — FIFO screen output for Move (replaces SPI/GPIO hardware driver)
 *
 * Reads NORNS_SCREEN_FIFO env var for FIFO path.
 * Converts Cairo ARGB32 surface to 4-bit packed grayscale (4096 bytes).
 * Writes to FIFO at ~60Hz for the DSP plugin to read.
 */
#include <cairo.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "event_types.h"
#include "events.h"

static int screen_fifo_fd = -1;
static uint32_t *surface_buffer = NULL;
static pthread_t screen_thread;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int display_dirty = 0;
static int screen_running = 0;

#define SURFACE_BUFFER_LEN (128 * 64 * sizeof(uint32_t))

static void *screen_thread_run(void *p) {
    (void)p;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 16666666 }; /* 60Hz */

    while (screen_running) {
        if (display_dirty) {
            /* Convert ARGB32 to 4-bit packed grayscale and write to FIFO */
            if (screen_fifo_fd >= 0 && surface_buffer) {
                uint8_t packed[4096];
                int bi = 0;

                pthread_mutex_lock(&lock);
                for (int y = 0; y < 64; y++) {
                    for (int x = 0; x < 128; x += 2) {
                        uint32_t p1 = surface_buffer[y * 128 + x];
                        uint32_t p2 = surface_buffer[y * 128 + x + 1];

                        /* ARGB32: B at byte 0, G at byte 1, R at byte 2, A at byte 3 */
                        uint8_t r1 = (p1 >> 16) & 0xFF;
                        uint8_t g1 = (p1 >> 8) & 0xFF;
                        uint8_t b1 = p1 & 0xFF;
                        uint8_t a1 = (p1 >> 24) & 0xFF;

                        uint8_t r2 = (p2 >> 16) & 0xFF;
                        uint8_t g2 = (p2 >> 8) & 0xFF;
                        uint8_t b2 = p2 & 0xFF;
                        uint8_t a2 = (p2 >> 24) & 0xFF;

                        /* Luminance-preserving grayscale with alpha */
                        float lum1 = (r1 * 0.299f + g1 * 0.587f + b1 * 0.114f) * a1 / 255.0f;
                        float lum2 = (r2 * 0.299f + g2 * 0.587f + b2 * 0.114f) * a2 / 255.0f;

                        /* Quantize to 4-bit (0-15) */
                        uint8_t l1 = (uint8_t)(lum1 / 17.0f);
                        uint8_t l2 = (uint8_t)(lum2 / 17.0f);
                        if (l1 > 15) l1 = 15;
                        if (l2 > 15) l2 = 15;

                        /* Pack: left pixel in high nybble, right in low */
                        packed[bi++] = (l1 << 4) | l2;
                    }
                }
                pthread_mutex_unlock(&lock);

                /* Non-blocking write — drop frame if FIFO full */
                write(screen_fifo_fd, packed, 4096);
            }
            display_dirty = 0;
        }

        event_post(event_data_new(EVENT_SCREEN_REFRESH));
        clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
    }
    return NULL;
}

void ssd1322_init(void) {
    surface_buffer = calloc(SURFACE_BUFFER_LEN, 1);
    if (!surface_buffer) {
        fprintf(stderr, "screen-fifo: couldn't allocate surface buffer\n");
        return;
    }

    const char *fifo_path = getenv("NORNS_SCREEN_FIFO");
    if (fifo_path && fifo_path[0]) {
        screen_fifo_fd = open(fifo_path, O_RDWR | O_NONBLOCK);
        if (screen_fifo_fd < 0) {
            fprintf(stderr, "screen-fifo: couldn't open %s: %s\n", fifo_path, strerror(errno));
        } else {
            fprintf(stderr, "screen-fifo: opened %s\n", fifo_path);
        }
    } else {
        fprintf(stderr, "screen-fifo: NORNS_SCREEN_FIFO not set, screen output disabled\n");
    }

    screen_running = 1;
    pthread_create(&screen_thread, NULL, screen_thread_run, NULL);
}

void ssd1322_deinit(void) {
    screen_running = 0;
    pthread_join(screen_thread, NULL);
    pthread_mutex_destroy(&lock);

    if (screen_fifo_fd >= 0) {
        close(screen_fifo_fd);
        screen_fifo_fd = -1;
    }
    free(surface_buffer);
    surface_buffer = NULL;
}

void ssd1322_update(cairo_surface_t *surface_pointer, bool surface_may_have_color) {
    (void)surface_may_have_color;
    pthread_mutex_lock(&lock);
    if (surface_buffer && surface_pointer) {
        const uint32_t w = cairo_image_surface_get_width(surface_pointer);
        const uint32_t h = cairo_image_surface_get_height(surface_pointer);
        if (w == 128 && h == 64) {
            memcpy(surface_buffer, cairo_image_surface_get_data(surface_pointer), SURFACE_BUFFER_LEN);
        }
    }
    display_dirty = 1;
    pthread_mutex_unlock(&lock);
}

void ssd1322_refresh(void) { /* no-op — FIFO write happens in thread */ }
void ssd1322_set_brightness(uint8_t b) { (void)b; }
void ssd1322_set_contrast(uint8_t c) { (void)c; }
void ssd1322_set_display_mode(int mode) { (void)mode; }
void ssd1322_set_gamma(double g) { (void)g; }
void ssd1322_set_refresh_rate(uint8_t hz) { (void)hz; }
uint8_t *ssd1322_resize_buffer(size_t size) { (void)size; return NULL; }
SCREENEOF

echo "  Replaced ssd1322.c with FIFO screen driver"

# ── 2. Replace ssd1322.h (remove GPIO/SPI/NEON includes) ──
cat > matron/src/hardware/screen/ssd1322.h << 'HDEOF'
#pragma once

#include <cairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

typedef enum {
    SSD1322_DISPLAY_MODE_NORMAL = 0,
    SSD1322_DISPLAY_MODE_INVERT = 1
} ssd1322_display_mode_t;

void ssd1322_init(void);
void ssd1322_deinit(void);
void ssd1322_refresh(void);
void ssd1322_update(cairo_surface_t *surface, bool should_translate_color);
void ssd1322_set_brightness(uint8_t b);
void ssd1322_set_contrast(uint8_t c);
void ssd1322_set_display_mode(ssd1322_display_mode_t mode);
void ssd1322_set_gamma(double g);
void ssd1322_set_refresh_rate(uint8_t hz);
uint8_t *ssd1322_resize_buffer(size_t size);
HDEOF

echo "  Replaced ssd1322.h (no NEON/GPIO/SPI includes, added display mode enum)"

# ── 3. Replace gpio.c with FIFO input driver ──
cat > matron/src/hardware/input/gpio.c << 'INPUTEOF'
/*
 * gpio.c — FIFO input driver for Move (replaces GPIO hardware driver)
 *
 * Reads NORNS_INPUT_FIFO env var for FIFO path.
 * Protocol: 4-byte frames [type][id][value_lo][value_hi]
 *   type 0 = encoder delta (id 0-2 = E1-E3, value = signed int16)
 *   type 1 = key state (id 0-2 = K1-K3, value 0=up 1=down)
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "events.h"
#include "hardware/input.h"
#include "hardware/io.h"

typedef struct {
    int fifo_fd;
} fifo_input_priv_t;

static int fifo_input_config(matron_io_t *io, lua_State *l);
static int fifo_input_setup(matron_io_t *io);
static void fifo_input_destroy(matron_io_t *io);
static void *fifo_input_poll(void *data);

/* Both key and encoder ops point to same FIFO driver */
input_ops_t key_gpio_ops = {
    .io_ops.name = "keys:fifo",
    .io_ops.type = IO_INPUT,
    .io_ops.data_size = sizeof(fifo_input_priv_t),
    .io_ops.config = fifo_input_config,
    .io_ops.setup = fifo_input_setup,
    .io_ops.destroy = fifo_input_destroy,
    .poll = fifo_input_poll,
};

input_ops_t enc_gpio_ops = {
    .io_ops.name = "enc:fifo",
    .io_ops.type = IO_INPUT,
    .io_ops.data_size = sizeof(fifo_input_priv_t),
    .io_ops.config = fifo_input_config,
    .io_ops.setup = fifo_input_setup,
    .io_ops.destroy = fifo_input_destroy,
    .poll = fifo_input_poll,
};

static int fifo_input_config(matron_io_t *io, lua_State *l) {
    (void)l;
    fifo_input_priv_t *priv = (fifo_input_priv_t *)io->data;
    priv->fifo_fd = -1;
    return 0;
}

static int fifo_input_setup(matron_io_t *io) {
    fifo_input_priv_t *priv = (fifo_input_priv_t *)io->data;
    const char *fifo_path = getenv("NORNS_INPUT_FIFO");
    if (fifo_path && fifo_path[0]) {
        priv->fifo_fd = open(fifo_path, O_RDONLY | O_NONBLOCK);
        if (priv->fifo_fd < 0) {
            fprintf(stderr, "input-fifo: couldn't open %s: %s\n", fifo_path, strerror(errno));
        } else {
            fprintf(stderr, "input-fifo: opened %s\n", fifo_path);
        }
    } else {
        fprintf(stderr, "input-fifo: NORNS_INPUT_FIFO not set, input disabled\n");
    }
    /* Start the poll thread (io_setup_all only calls ops->setup,
     * it doesn't call input_setup which creates the thread) */
    return input_setup(io);
}

static void fifo_input_destroy(matron_io_t *io) {
    fifo_input_priv_t *priv = (fifo_input_priv_t *)io->data;
    if (priv->fifo_fd >= 0) {
        close(priv->fifo_fd);
        priv->fifo_fd = -1;
    }
}

static void *fifo_input_poll(void *data) {
    matron_input_t *input = (matron_input_t *)data;
    fifo_input_priv_t *priv = (fifo_input_priv_t *)input->io.data;

    if (priv->fifo_fd < 0) {
        /* No FIFO — sleep forever (thread will be cancelled on destroy) */
        while (1) usleep(100000);
        return NULL;
    }

    while (1) {
        uint8_t frame[4];
        ssize_t n = read(priv->fifo_fd, frame, 4);
        if (n == 4) {
            uint8_t type = frame[0];
            uint8_t id = frame[1];
            int16_t value = (int16_t)(frame[2] | (frame[3] << 8));

            if (type == 0 && id < 3) {
                /* Encoder delta */
                union event_data *ev = event_data_new(EVENT_ENC);
                ev->enc.n = id + 1;  /* norns encoders are 1-indexed */
                ev->enc.delta = value;
                event_post(ev);
            } else if (type == 1 && id < 3) {
                /* Key press/release */
                union event_data *ev = event_data_new(EVENT_KEY);
                ev->key.n = id + 1;  /* norns keys are 1-indexed */
                ev->key.val = value;
                event_post(ev);
            } else if (type == 2) {
                /* MIDI event — frame: [2, status, data1, data2]
                 * Post as EVENT_MIDI_EVENT on the virtual MIDI device
                 * so norns scripts receive it via midi.connect(). */
                extern uint32_t dev_midi_virtual_id(void);
                union event_data *ev = event_data_new(EVENT_MIDI_EVENT);
                ev->midi_event.id = dev_midi_virtual_id();
                ev->midi_event.data[0] = frame[1];  /* status */
                ev->midi_event.data[1] = frame[2];  /* data1 (note) */
                ev->midi_event.data[2] = frame[3];  /* data2 (velocity) */
                ev->midi_event.nbytes = 3;
                event_post(ev);
            } else if (type == 3) {
                /* Grid key — frame: [3, x, y, state]
                 * Post as EVENT_GRID_KEY on the virtual grid device. */
                extern uint32_t dev_monome_virtual_id(void);
                union event_data *ev = event_data_new(EVENT_GRID_KEY);
                ev->grid_key.id = (uint8_t)dev_monome_virtual_id();
                ev->grid_key.x = frame[1];
                ev->grid_key.y = frame[2];
                ev->grid_key.state = frame[3];
                event_post(ev);
            }
        } else if (n < 0 && errno == EAGAIN) {
            /* No data available — sleep briefly */
            usleep(1000); /* 1ms */
        } else if (n == 0) {
            usleep(1000);
        }
    }
    return NULL;
}
INPUTEOF

echo "  Replaced gpio.c with FIFO input driver"

# ── 4. Patch wscript to remove GPIO/SPI/NEON/monome dependencies ──
# Remove libgpiod check from configure
sed -i "/check_cfg.*libgpiod/d" wscript
# Remove libmonome check from configure
sed -i "/check_cc.*libmonome/,/uselib_store.*LIBMONOME/d" wscript
# Remove LIBGPIOD and LIBMONOME from matron wscript use list
sed -i "/'LIBGPIOD'/d" matron/wscript
sed -i "/'LIBMONOME'/d" matron/wscript
# Remove -mfpu=neon (ARM32 flag, invalid on aarch64)
sed -i "s/'-mfpu=neon'//" matron/wscript
# Remove ARM32-specific release flags
sed -i "/'-mcpu=cortex-a53'/d" matron/wscript
sed -i "/'-mtune=cortex-a53'/d" matron/wscript
sed -i "/'-mfpu=neon-fp-armv8'/d" matron/wscript
sed -i "/'-mfloat-abi=hard'/d" matron/wscript
# Remove -Werror (upstream code may have warnings on newer compilers)
sed -i "s/'-Werror'//" wscript

# Add glib-2.0 dependency (needed by device_hid.c)
if ! grep -q "glib-2.0" wscript; then
    sed -i "/check_cfg.*package='alsa/a\\    conf.check_cfg(package='glib-2.0', args=['--cflags', '--libs'])" wscript
fi
if ! grep -q "'GLIB-2.0'" matron/wscript; then
    sed -i "s/'ALSA'/'ALSA',\n        'GLIB-2.0'/" matron/wscript
fi

echo "  Patched wscript (removed libgpiod, libmonome, ARM32 flags, -Werror; added glib-2.0)"

# ── 5. Stub out platform_factory() if needed ──
if grep -q "platform_factory" matron/src/hardware/platform.c 2>/dev/null; then
    echo "  platform.c has platform_factory — leaving as-is"
else
    echo "  Adding platform_factory stub"
    echo 'int platform_factory(void) { return 0; }' >> matron/src/hardware/platform.c
fi

# ── 6. Replace device_monome.h/c with FIFO-based grid emulator ──
echo "  Replacing device_monome.h (FIFO-based grid emulator)"
cat > matron/src/device/device_monome.h << 'MHSTUB'
#pragma once
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include "device_common.h"

typedef enum {
    DEVICE_MONOME_TYPE_GRID,
    DEVICE_MONOME_TYPE_ARC,
} device_monome_type_t;

struct dev_monome {
    struct dev_common dev;
    device_monome_type_t type;
    void *m;                    /* unused on Move (no libmonome) */
    uint8_t data[4][64];        /* unused on Move */
    uint8_t dirty[4];           /* unused on Move */
    bool connected;
    /* Move grid emulator state */
    uint8_t grid_leds[16 * 8];  /* 16x8 LED brightness buffer (0-15) */
    int grid_fifo_fd;           /* fd to write LED state to DSP plugin */
};

int dev_monome_init(void *dev);
void dev_monome_deinit(void *dev);
void dev_monome_grid_set_led(void *md, int x, int y, int z, int rel);
void dev_monome_arc_set_led(void *md, int n, int x, int val, int rel);
void dev_monome_all_led(void *md, int z, int rel);
void dev_monome_set_rotation(void *md, int z);
void dev_monome_tilt_enable(void *md, int id);
void dev_monome_tilt_disable(void *md, int id);
void dev_monome_refresh(void *md);
void dev_monome_intensity(void *md, int i);
int dev_monome_grid_rows(void *md);
int dev_monome_grid_cols(void *md);

/* Move-specific: virtual grid device ID tracking */
extern uint32_t dev_monome_virtual_id(void);
extern void dev_monome_virtual_set_id(uint32_t id);
MHSTUB

echo "  Replacing device_monome.c (FIFO-based grid emulator)"
cat > matron/src/device/device_monome.c << 'MCSTUB'
/* device_monome.c — FIFO-based grid emulator for Move
 *
 * Emulates a Monome 16x8 grid. LED state is stored in a 128-byte buffer
 * and flushed to a FIFO on refresh() for the DSP plugin to read.
 * Grid key input comes via the gpio FIFO driver (type 3 frames). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include "device/device_monome.h"

static uint32_t g_grid_virtual_id = 0;

uint32_t dev_monome_virtual_id(void) { return g_grid_virtual_id; }
void dev_monome_virtual_set_id(uint32_t id) {
    g_grid_virtual_id = id;
    fprintf(stderr, "virtual grid: assigned device ID %u\n", id);
}

int dev_monome_init(void *dev) {
    struct dev_monome *md = (struct dev_monome *)dev;
    md->type = DEVICE_MONOME_TYPE_GRID;
    md->connected = true;
    md->grid_fifo_fd = -1;
    memset(md->grid_leds, 0, sizeof(md->grid_leds));

    /* Open grid LED FIFO (created by DSP plugin) */
    const char *fifo = "/tmp/norns-grid-1";
    md->grid_fifo_fd = open(fifo, O_RDWR | O_NONBLOCK);
    if (md->grid_fifo_fd < 0) {
        fprintf(stderr, "grid: couldn't open %s: %s (will retry on refresh)\n",
                fifo, strerror(errno));
    } else {
        fprintf(stderr, "grid: opened %s for LED output\n", fifo);
    }

    /* No reader thread — grid keys come via gpio FIFO driver */
    md->dev.start = NULL;
    md->dev.deinit = &dev_monome_deinit;
    /* dev.name is set by dev_new to the caller's string literal —
     * do NOT free it (UB). Just overwrite with strdup. */
    md->dev.name = strdup("virtual grid");
    md->dev.serial = strdup("move-grid-01");

    fprintf(stderr, "virtual grid: initialized (16x8)\n");
    return 0;
}

void dev_monome_deinit(void *dev) {
    struct dev_monome *md = (struct dev_monome *)dev;
    if (md->grid_fifo_fd >= 0) {
        close(md->grid_fifo_fd);
        md->grid_fifo_fd = -1;
    }
}

void dev_monome_grid_set_led(void *md, int x, int y, int z, int rel) {
    struct dev_monome *m = (struct dev_monome *)md;
    (void)rel;
    if (x < 0 || x >= 16 || y < 0 || y >= 8) return;
    if (z < 0) z = 0;
    if (z > 15) z = 15;
    m->grid_leds[y * 16 + x] = (uint8_t)z;
}

void dev_monome_all_led(void *md, int z, int rel) {
    struct dev_monome *m = (struct dev_monome *)md;
    (void)rel;
    uint8_t val = (z < 0) ? 0 : (z > 15) ? 15 : (uint8_t)z;
    memset(m->grid_leds, val, sizeof(m->grid_leds));
}

void dev_monome_refresh(void *md) {
    struct dev_monome *m = (struct dev_monome *)md;
    /* Retry open if FIFO wasn't ready at init time */
    if (m->grid_fifo_fd < 0) {
        m->grid_fifo_fd = open("/tmp/norns-grid-1", O_RDWR | O_NONBLOCK);
        if (m->grid_fifo_fd < 0) return;
        fprintf(stderr, "grid: late-opened FIFO for LED output\n");
    }
    /* Write entire 128-byte LED buffer to FIFO for DSP plugin */
    write(m->grid_fifo_fd, m->grid_leds, 128);
}

int dev_monome_grid_rows(void *md) { (void)md; return 8; }
int dev_monome_grid_cols(void *md) { (void)md; return 16; }

/* Unused stubs */
void dev_monome_arc_set_led(void *md, int n, int x, int val, int rel) {
    (void)md; (void)n; (void)x; (void)val; (void)rel;
}
void dev_monome_set_rotation(void *md, int z) { (void)md; (void)z; }
void dev_monome_tilt_enable(void *md, int id) { (void)md; (void)id; }
void dev_monome_tilt_disable(void *md, int id) { (void)md; (void)id; }
void dev_monome_intensity(void *md, int i) { (void)md; (void)i; }
MCSTUB

# ── 7. Replace dev_midi_virtual_init with FIFO-based version (no ALSA) ──
# On Move there is no ALSA sequencer (/dev/snd/seq), so the stock virtual
# MIDI init fails.  Replace it with a stub that succeeds without ALSA,
# allowing the virtual MIDI device to register in norns DEVICES > MIDI.
# MIDI data is injected via EVENT_MIDI_EVENT from the gpio FIFO driver.
echo "  Patching device_midi.c (FIFO-based virtual MIDI, no ALSA)"

# Add virtual MIDI ID tracking (before the function replacement)
if ! grep -q "g_virtual_midi_id" matron/src/device/device_midi.c; then
    # Insert global vars and accessor functions right before dev_midi_virtual_init
    sed -i '/^int dev_midi_virtual_init/i\
/* ── Move: FIFO-based virtual MIDI ID tracking ────── */\
static uint32_t g_virtual_midi_id = 0;\
\
uint32_t dev_midi_virtual_id(void) {\
    return g_virtual_midi_id;\
}\
\
void dev_midi_virtual_set_id(uint32_t id) {\
    g_virtual_midi_id = id;\
    fprintf(stderr, "virtual MIDI: assigned device ID %u\\n", id);\
}\
' matron/src/device/device_midi.c
fi

# Replace the existing dev_midi_virtual_init function body
python3 -c "
import re
with open('matron/src/device/device_midi.c', 'r') as f:
    src = f.read()

old = re.search(r'int dev_midi_virtual_init\(void \*self\) \{.*?\n\}', src, re.DOTALL)
if old:
    new_fn = '''int dev_midi_virtual_init(void *self) {
    struct dev_midi *midi = (struct dev_midi *)self;
    struct dev_common *base = (struct dev_common *)self;

    /* Move: no ALSA sequencer available — register as a FIFO-based
     * virtual device.  MIDI input comes from the gpio FIFO driver
     * (type 2 frames) which posts EVENT_MIDI_EVENT with our device ID. */
    midi->handle_in = NULL;
    midi->handle_out = NULL;
    midi->clock_enabled = false;

    base->start = NULL;   /* no reader thread — gpio FIFO driver handles input */
    base->deinit = &dev_midi_deinit;  /* safe with NULL handles */

    /* Override name — do NOT free the old pointer (it's a string literal from dev_new) */
    base->name = strdup(\"Move Pads\");

    fprintf(stderr, \"virtual MIDI: registered as Move Pads (FIFO-based)\\\\n\");
    return 0;
}'''
    src = src[:old.start()] + new_fn + src[old.end():]
    with open('matron/src/device/device_midi.c', 'w') as f:
        f.write(src)
    print('  Replaced dev_midi_virtual_init')
else:
    print('  WARN: dev_midi_virtual_init not found')
"

# Add declarations to device_midi.h
if ! grep -q "dev_midi_virtual_id" matron/src/device/device_midi.h; then
    sed -i '/extern int dev_midi_virtual_init/a\extern uint32_t dev_midi_virtual_id(void);\nextern void dev_midi_virtual_set_id(uint32_t id);' matron/src/device/device_midi.h
fi

# Hook dev_midi_virtual_set_id into post_add_event for MIDI_VIRTUAL
# After the device is created and gets its ID, store it
if ! grep -q "dev_midi_virtual_set_id" matron/src/device/device_list.c; then
    sed -i '/case DEV_TYPE_MIDI_VIRTUAL:/,/return;/{
        /event_post(ev);/i\            dev_midi_virtual_set_id(d->base.id);
    }' matron/src/device/device_list.c
    echo "  Hooked dev_midi_virtual_set_id into device_list.c"
fi

# ── 8. Register virtual grid device at startup ──
# Add a virtual grid device alongside the virtual MIDI device.
# Patch main.c to create the grid, and device_list.c to handle
# DEV_TYPE_MONOME with path==NULL as a virtual (FIFO-based) grid.
echo "  Patching main.c (add virtual grid device)"
if ! grep -q "virtual grid" matron/src/main.c; then
    sed -i '/dev_list_add(DEV_TYPE_MIDI_VIRTUAL/a\    dev_list_add(DEV_TYPE_MONOME, NULL, "virtual grid", NULL);' matron/src/main.c
fi

echo "  Patching device_list.c (virtual grid registration)"
# Add include for device_monome.h
if ! grep -q "device_monome.h" matron/src/device/device_list.c; then
    sed -i '/#include "device_midi.h"/a\#include "device_monome.h"' matron/src/device/device_list.c
fi

# Add virtual grid path in DEV_TYPE_MONOME case inside dev_list_add (path==NULL means virtual)
# Use python3 for precise insertion — sed can't distinguish between
# DEV_TYPE_MONOME in dev_list_add vs dev_list_remove.
if ! grep -q "dev_monome_virtual_set_id" matron/src/device/device_list.c; then
    python3 -c "
with open('matron/src/device/device_list.c', 'r') as f:
    src = f.read()

# Find DEV_TYPE_MONOME case inside dev_list_add (appears before dev_list_remove)
marker = 'case DEV_TYPE_MONOME:'
first_idx = src.find(marker)
if first_idx >= 0:
    # Insert virtual grid handling right after the case line
    nl = src.find('\n', first_idx)
    insert = '''
        if (path == NULL) {
            /* Virtual grid (Move) — FIFO-based, no libmonome */
            d = dev_new(type, NULL, name, false, 0, NULL);
            ev = post_add_event(d, EVENT_MONOME_ADD);
            if (ev != NULL) {
                dev_monome_virtual_set_id(d->base.id);
                ev->monome_add.dev = d;
                event_post(ev);
            }
            return;
        }'''
    src = src[:nl+1] + insert + src[nl:]
    with open('matron/src/device/device_list.c', 'w') as f:
        f.write(src)
    print('  Hooked dev_monome_virtual_set_id into device_list.c')
else:
    print('  WARN: DEV_TYPE_MONOME case not found')
"
fi

# ── 8c. Register virtual HID gamepad (norns-panicos native pad) ──
# In-memory libevdev device fed by NORNS_HID_FIFO; matron's stock hid add/event
# path + core/hid.lua are reused unchanged. See patches/hid-virtual.patch.
echo "  Patching device_hid.c + main.c (virtual HID gamepad)"

# (a) Append the virtual init + FIFO-reader start to device_hid.c.
if ! grep -q "dev_hid_make_virtual" matron/src/device/device_hid.c; then
cat >> matron/src/device/device_hid.c << 'HIDEOF'

/* ── norns-panicos: virtual gamepad (in-memory libevdev, fed by NORNS_HID_FIFO) ──
 * Registered from main.c via dev_list_add(DEV_TYPE_HID, NULL, ...). A NULL path
 * means "virtual": build the device in memory and stream evdev frames from the
 * FIFO instead of reading /dev/input. Reuses this file's static add_types/
 * add_codes/handle_event, so the stock hid_add/hid_event path is unchanged. */
#include <unistd.h>

static void *dev_hid_virtual_start(void *self) {
    struct dev_hid *di = (struct dev_hid *)self;
    const char *path = getenv("NORNS_HID_FIFO");
    if (path == NULL) { return NULL; }
    int fd = open(path, O_RDONLY);          /* blocks until the host opens write end */
    if (fd < 0) { return NULL; }
    uint8_t f[5];
    for (;;) {
        ssize_t got = 0;                    /* read exactly 5 bytes (robust to splits) */
        while (got < 5) {
            ssize_t r = read(fd, f + got, 5 - got);
            if (r <= 0) { close(fd); return NULL; }
            got += r;
        }
        struct input_event ie;
        memset(&ie, 0, sizeof(ie));
        ie.type  = (uint16_t)f[0];
        ie.code  = (uint16_t)(f[1] | (f[2] << 8));
        ie.value = (int16_t)(f[3] | (f[4] << 8));
        handle_event(di, &ie);              /* posts EVENT_HID_EVENT */
    }
}

/* Keep in lockstep with src/norns-hid.h + spec §4.1. */
static const int k_hid_virtual_keys[] = {
    BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
    BTN_SELECT, BTN_START, BTN_THUMBL, BTN_THUMBR,
    BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
};
static const int k_hid_virtual_axes[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };

int dev_hid_make_virtual(struct dev_hid *d) {
    struct dev_common *base = (struct dev_common *)d;
    struct libevdev *dev = libevdev_new();
    if (dev == NULL) { return -1; }
    libevdev_set_name(dev, "norns-panicos gamepad");
    libevdev_set_id_bustype(dev, BUS_VIRTUAL);
    libevdev_set_id_vendor(dev, 0x1209);
    libevdev_set_id_product(dev, 0x6e70);
    libevdev_enable_event_type(dev, EV_KEY);
    for (unsigned i = 0; i < sizeof(k_hid_virtual_keys) / sizeof(k_hid_virtual_keys[0]); i++) {
        libevdev_enable_event_code(dev, EV_KEY, k_hid_virtual_keys[i], NULL);
    }
    libevdev_enable_event_type(dev, EV_ABS);
    struct input_absinfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.minimum = -32768; ai.maximum = 32767;
    for (unsigned i = 0; i < sizeof(k_hid_virtual_axes) / sizeof(k_hid_virtual_axes[0]); i++) {
        libevdev_enable_event_code(dev, EV_ABS, k_hid_virtual_axes[i], &ai);
    }
    d->dev = dev;
    add_types(d);
    add_codes(d);
    d->vid = libevdev_get_id_vendor(dev);
    d->pid = libevdev_get_id_product(dev);
    { guint16 raw_guid[16]; get_guid(dev, raw_guid); guid_to_string(raw_guid, d->guid); }
    base->start  = &dev_hid_virtual_start;
    base->deinit = &dev_hid_deinit;
    return 0;
}
HIDEOF
fi

# (b) Dispatch to the virtual path at the very top of dev_hid_init.
if ! grep -q "dev_hid_make_virtual((struct dev_hid" matron/src/device/device_hid.c; then
    python3 -c "
p='matron/src/device/device_hid.c'; s=open(p).read()
a='int dev_hid_init(void *self) {'; i=s.find(a)
if i>=0:
    j=s.find(chr(10), i)+1
    ins='    if (((struct dev_common *)self)->path == NULL) { return dev_hid_make_virtual((struct dev_hid *)self); }'+chr(10)
    open(p,'w').write(s[:j]+ins+s[j:]); print('  dev_hid_init: NULL-path virtual dispatch added')
else: print('  WARN: dev_hid_init not found')
"
fi

# (c) Forward-declare dev_hid_make_virtual (defined at end of file, used in dev_hid_init).
if ! grep -q "dev_hid_make_virtual" matron/src/device/device_hid.h; then
    sed -i '/extern int dev_hid_init/i\extern int dev_hid_make_virtual(struct dev_hid *d);' matron/src/device/device_hid.h
fi

# (d) Register the virtual gamepad at startup, right after the virtual grid.
if ! grep -q "norns-panicos gamepad" matron/src/main.c; then
    sed -i '/dev_list_add(DEV_TYPE_MONOME, NULL, "virtual grid"/a\    dev_list_add(DEV_TYPE_HID, NULL, "norns-panicos gamepad", NULL);' matron/src/main.c
fi

# ── 9. Fix lo_message typedef conflict with lo/lo_types.h ──
if grep -q 'typedef void \*lo_message' matron/src/event_types.h 2>/dev/null; then
    echo "  Fixing lo_message typedef conflict in event_types.h"
    sed -i 's|// lo_message.*|#include <lo/lo_types.h>|' matron/src/event_types.h
    sed -i '/^typedef void \*lo_message/d' matron/src/event_types.h
fi

# ── 8. Fix select.lua: replace GNU find -printf with BusyBox-compatible equivalent ──
# BusyBox find does not support -printf "%P\n".  Replace with -print piped
# through sed to strip the leading path, producing the same relative output.
echo "  Patching lua/core/menu/select.lua (BusyBox-compatible find)"
python3 << 'PYEOF'
path = 'lua/core/menu/select.lua'
with open(path) as f:
    src = f.read()
src = src.replace(
    '-name "*.lua" -printf "%P\\n"',
    '-name "*.lua" -print'
)
src = src.replace(
    "'grep -Ev \"/(lib|data|crow|test|docs)/\" | ' ..\n                   'sort',",
    "'sed \"s@.*/dust/code/@@\" | grep -Ev \"/(lib|data|crow|test|docs)/\" | ' ..\n                   'sort',"
)
# Also fix hardcoded /home/we path — use paths.code (set correctly at runtime)
src = src.replace(
    "table.insert(t,'/home/we/dust/code/' .. filename)",
    "table.insert(t,paths.code .. filename)"
)
with open(path, 'w') as f:
    f.write(src)
print('    done')
PYEOF

# ── 9. Make crone ADC port connection non-fatal ──────────────────────
# connectAdcPorts() throws when no physical capture ports exist (e.g. a
# playback-only device with no USB audio interface attached).  Make it warn
# and continue instead — crone still works for output; input just returns
# silence.  Works correctly when a USB interface IS present.
echo "  Patching crone/src/Client.h (ADC connection optional)"
# Robust single-line replacements. The previous multi-line exact-block python
# .replace() silently no-op'd in the build container (newline/whitespace
# sensitivity), shipping an unpatched crone that crashes on capture-less boards.
# These sed edits match just the unique throw statements and then we VERIFY the
# result, aborting loudly if it didn't take.
sed -i \
  -e 's@throw std::runtime_error("no ADC ports found");@{ std::cerr << "crone: no ADC ports, continuing without audio input" << std::endl; return; }@' \
  -e 's@throw std::runtime_error("connectAdcPorts() failed");@std::cerr << "crone: ADC port " << i << " connect failed, skipping" << std::endl;@' \
  crone/src/Client.h
if grep -q "continuing without audio input" crone/src/Client.h; then
    echo "    done (verified)"
else
    echo "ERROR: crone ADC patch did not apply to crone/src/Client.h" >&2
    exit 1
fi

# ── 9. Fix missing #include <string> in BufDiskWorker.h (GCC 15+) ──
if ! grep -q '#include <string>' crone/src/BufDiskWorker.h 2>/dev/null; then
    echo "  Adding #include <string> to BufDiskWorker.h (GCC 15 fix)"
    sed -i '1s/^/#include <string>\n/' crone/src/BufDiskWorker.h
fi

# ── 9. Upgrade waf for Python 3.12+ compatibility ──
# waf 2.0.14 uses the 'imp' module removed in Python 3.12.
# If waf --version succeeds and reports 2.1+, skip. Otherwise upgrade.
WAF_VERSION=$(python3 waf --version 2>/dev/null | grep -oP '\d+\.\d+\.\d+' || echo "")
case "$WAF_VERSION" in
    2.1.*|2.2.*|2.3.*|2.4.*|2.5.*)
        echo "  waf $WAF_VERSION — no upgrade needed"
        ;;
    *)
        echo "  Upgrading waf to 2.1.5 (current: ${WAF_VERSION:-crashed})"
        curl -fsSL https://waf.io/waf-2.1.5 -o waf
        chmod +x waf
        # Also replace extracted waflib/ if it exists (stale cache)
        if [ -d waflib ]; then
            rm -rf waflib
            python3 waf --version >/dev/null 2>&1 || true
        fi
        ;;
esac

# ── 10. Create Move-specific matronrc.lua ──
echo "  Creating Move-specific matronrc.lua"
cat > matronrc.lua << 'MATRONRC'
-- matronrc.lua — Move-specific norns configuration
-- FIFO-based I/O: screen, input, and grid are handled by
-- the FIFO drivers compiled into matron (no GPIO/evdev needed).

function init_norns()
  _boot.add_io("keys:fifo", {})
  _boot.add_io("enc:fifo",  {index=1})
  _boot.add_io("enc:fifo",  {index=2})
  _boot.add_io("enc:fifo",  {index=3})
end

init_norns()
MATRONRC

echo ""
echo "=== Patches applied ==="
echo "Now rebuild:"
echo "  cd $NORNS_DIR"
echo "  python3 waf configure"
echo "  python3 waf build"
