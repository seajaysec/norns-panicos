# Native Gamepad API (Phase 3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Expose the full handheld gamepad to norns Lua scripts via norns's native HID device API, with a per-script "native mode" that suppresses the K/E emulation and an ergonomic `pad` wrapper library on top.

**Architecture:** `norns-panicos` (host) gains a second FIFO (`/tmp/norns-hid-1`) carrying evdev-style frames. matron is patched to fabricate one stable **in-memory libevdev** HID device (no hardware fd) and stream those frames as `EVENT_HID_EVENT`, so stock `hid.connect()` sees a normal device. The host routes the pad to the HID FIFO (instead of the encoder/key FIFO) whenever the active context's overlay declares `mode = native`. Source-of-truth design doc: `docs/superpowers/specs/2026-06-03-phase3-native-gamepad-api-design.md`.

**Tech Stack:** C11 (host + matron patch, libevdev), SDL2 GameController (host input), Lua 5.3 (norns wrapper lib + mod-free packaging), pure-C unit tests (`cc` + `assert`, no framework).

---

## Prerequisites & build-order note

This phase was **specced first** (to surface what it imposes on the earlier phases) but is **built last**: the agreed order is Phase 1 → Phase 2 → Phase 3.

- **Phase 1 must be complete** before **Task 7** (escape interception). Task 7 relies on Phase 1 having: (a) the **Menu/FN** button liberated with `Menu short = home`, `Menu-hold / Select+Menu = quit`, and (b) a generalized **host-reserved-input** mechanism (an input the resolver can claim so it never reaches downstream). Until Phase 1 lands, Task 7 cannot be implemented as written.
- **Phase 2** adds editing `mode = native` from the on-device Norns Controls app. Phase 3 only needs the *parser* to understand `mode = native` (Task 2) — a hand-written conf line is enough to exercise everything here. The editor UI is Phase 2's job, not this plan's.
- **De-risk-early subset (safe to execute ahead of Phases 1–2):** Task 1 (spike), Task 2 (`native_mode` parse), Task 3 (`norns-hid.h`), Task 4 (matron patch), Task 8 (`pad.lua`). These have no Phase 1/2 dependency and prove the riskiest unknowns. If you want to validate Transport B before building Phases 1–2, do Task 1 now.

**Test convention (this repo):** pure-C tests include the header directly and have their own `main()`. Build & run a test with:
```bash
cc -Wall -Wextra tests/test_NAME.c -o /tmp/test_NAME && /tmp/test_NAME
```
Expected tail on success: `All tests passed.`

**matron note:** matron source is **not** in this repo — it is cloned and patched inside the Docker build (`scripts/build-panicos.sh` → `patches/apply-move-patches.sh`). Tasks 1 and 4 run in that build environment.

---

## File structure

**New files**
- `src/norns-hid.h` — header-only: evdev constants, `pad_input_t`, pure `pad_input_to_evdev()` and `hid_frame_encode()`. Shared by host + tests (mirrors how `norns-controls.h` is shared).
- `tests/test_hid.c` — unit tests for `norns-hid.h`.
- `patches/hid-virtual.patch` — prose description doc of the synthetic-HID matron patch (repo convention; the real application lives in `apply-move-patches.sh`).
- `lua/pad.lua` — the ergonomic wrapper library (shipped to the device's norns lua path by the build).
- `tests/test_pad.lua` — Lua unit test driving `pad.lua` against a stub `hid` table.

**Modified files**
- `src/norns-controls.h` — add `int native_mode` to `controls_t`, default it, parse `mode = native`.
- `tests/test_controls.c` — tests for `native_mode` parsing.
- `src/norns-panicos.c` — `FIFO_HID`, `hid_fd`, `send_hid()`, SDL→`pad_input_t` translation, native-mode routing, escape interception, `/tmp/norns-native` runtime override, `NORNS_HID_FIFO` env when spawning matron.
- `patches/apply-move-patches.sh` — new section registering the virtual HID device (modeled on the existing virtual-grid registration, §6/§8).
- `scripts/build-panicos.sh` — ship `lua/pad.lua` into the norns lua path; ensure `NORNS_HID_FIFO` reaches matron.
- `docs/controls.md` — document native mode + the `pad` API (or a new `docs/gamepad-native.md`).

---

## Task 1: Spike — prove an in-memory libevdev HID device in matron (throwaway)

**Purpose:** Confirm Transport B is viable before committing to the full patch. **This is exploratory, not TDD; the code is thrown away.** Findings feed Task 4.

**Files:**
- Scratch only: a temporary section appended to `patches/apply-move-patches.sh` and a scratch build. Revert before finishing.
- Notes: `docs/superpowers/plans/notes/2026-06-03-hid-spike-findings.md` (keep this).

- [ ] **Step 1: Read the real matron hid path.** In the cloned norns source (inside the build env), read:
  - `matron/src/device/device_hid.c`, `device_monitor.c`, `device_list.c`
  - `matron/src/weaver.c` — the `_norns.hid_add` / `_norns.hid_event` handlers
  - In this repo, read `patches/apply-move-patches.sh` §6 (device_monome virtual) and §8 (virtual grid registration in `main.c`/`device_list.c`) — this is the template for "fabricate a device at startup that reads a FIFO and posts events."
  Write down in the notes file: the exact `device_hid_t` struct shape, how `device_monitor` constructs one, and whether anything calls `libevdev_get_fd()` on it (the one real risk, per spec §6).

- [ ] **Step 2: Build a minimal synthetic device.** In a scratch section, create a `libevdev` object with no fd and a tiny capability set:

```c
#include <libevdev/libevdev.h>
struct libevdev *dev = libevdev_new();
libevdev_set_name(dev, "norns-panicos gamepad");
libevdev_enable_event_type(dev, EV_KEY);
libevdev_enable_event_code(dev, EV_KEY, BTN_SOUTH, NULL);   /* A */
libevdev_enable_event_type(dev, EV_ABS);
struct input_absinfo ai = { .minimum = -32768, .maximum = 32767 };
libevdev_enable_event_code(dev, EV_ABS, ABS_X, &ai);
```
  Register it through the same code path the virtual grid uses at startup, then post one event using matron's hid event post (the same call `device_hid`'s read loop uses), e.g. `BTN_SOUTH` down then up.

- [ ] **Step 3: Verify in maiden.** Boot the patched matron. In the maiden REPL:
```lua
for i,d in ipairs(hid.vports) do print(i, d.name) end
hid.vports[1].event = function(t,c,v) print("hid", t, c, v) end
```
  Expected: the device list prints `norns-panicos gamepad`; triggering the posted event prints a `hid 1 304 1` / `hid 1 304 0` line (304 = `BTN_SOUTH`).

- [ ] **Step 4: Record the verdict.** In the notes file, record PASS/FAIL and the exact registration + event-post call signatures discovered. If `libevdev_get_fd()` coupling blocks registration, record it and mark **fallback to Transport C (uinput)** for Task 4 (same host code, device sourced from `/dev/uinput`).

- [ ] **Step 5: Revert the scratch.** Remove the temporary section from `apply-move-patches.sh`. Commit only the notes file:
```bash
git add docs/superpowers/plans/notes/2026-06-03-hid-spike-findings.md
git commit -m "spike: confirm in-memory libevdev HID device registers in matron"
```

**Gate:** Do not start Task 4 until this spike PASSES (or the notes record the Transport-C pivot).

---

## Task 2: `native_mode` overlay field (host, pure, TDD)

**Files:**
- Modify: `src/norns-controls.h` (struct field, default, parser)
- Test: `tests/test_controls.c`

- [ ] **Step 1: Write the failing test.** Add to `tests/test_controls.c` (before `main`), and add a call to it inside `main`:

```c
static void test_native_mode(void) {
    controls_t c;
    controls_defaults(&c);
    assert(c.native_mode == 0);                       /* off by default */

    const char *cfg =
        "scheme = sticks\n"
        "[sticks]\ne1 = none\ne2 = lstick\ne3 = rstick\n"
        "[script:gamepong]\nmode = native\n";
    char scheme[32] = "";
    controls_find_scheme(cfg, scheme, sizeof(scheme));
    controls_parse_scheme(&c, cfg, scheme);
    assert(c.native_mode == 0);                       /* scheme alone: not native */

    controls_parse_scheme(&c, cfg, "script:gamepong");
    assert(c.native_mode == 1);                       /* overlay turns it on */
    printf("  PASS test_native_mode\n");
}
```

- [ ] **Step 2: Run to verify it fails.**
```bash
cc -Wall -Wextra tests/test_controls.c -o /tmp/test_controls && /tmp/test_controls
```
Expected: compile error `'controls_t' has no member named 'native_mode'`.

- [ ] **Step 3: Add the field + default.** In `src/norns-controls.h`, add to `controls_t` (after `dpad_y_invert`):
```c
    /* Phase 3: when set by a [script:NAME] overlay, the host routes the pad to
     * the native HID FIFO and suppresses K/E emulation for that script. */
    int          native_mode;
```
In `controls_defaults()`, add:
```c
    c->native_mode = 0;              /* emulation; [script:*] overlay opts in */
```

- [ ] **Step 4: Parse the key.** In `controls__assign()`, before the final `fprintf(stderr, "controls: unknown key...`:
```c
    if (!strcmp(key, "mode")) {
        char v[16]; strncpy(v, val, sizeof(v) - 1); v[sizeof(v) - 1] = '\0';
        controls__canon(controls__trim(v));
        c->native_mode = !strcmp(v, "native") ? 1 : 0;   /* anything else = emulation */
        return 1;
    }
```

- [ ] **Step 5: Run to verify it passes.**
```bash
cc -Wall -Wextra tests/test_controls.c -o /tmp/test_controls && /tmp/test_controls
```
Expected: `PASS test_native_mode` then `All tests passed.`

- [ ] **Step 6: Commit.**
```bash
git add src/norns-controls.h tests/test_controls.c
git commit -m "feat(controls): parse [script:NAME] mode=native overlay flag"
```

---

## Task 3: evdev constants, pad-input mapping, frame encoder (host, pure, TDD)

**Files:**
- Create: `src/norns-hid.h`
- Create: `tests/test_hid.c`

- [ ] **Step 1: Write the failing test.** Create `tests/test_hid.c`:

```c
/* tests/test_hid.c — unit tests for the SDL-free evdev mapping + frame encoder. */
#include <assert.h>
#include <stdio.h>
#include "../src/norns-hid.h"

static void test_button_mapping(void) {
    uint16_t type, code;
    assert(pad_input_to_evdev(PAD_A, &type, &code) == 1);
    assert(type == EV_KEY && code == EVB_BTN_SOUTH);
    assert(pad_input_to_evdev(PAD_Y, &type, &code) == 1);
    assert(type == EV_KEY && code == EVB_BTN_NORTH);   /* Y = NORTH */
    assert(pad_input_to_evdev(PAD_X, &type, &code) == 1);
    assert(type == EV_KEY && code == EVB_BTN_WEST);    /* X = WEST */
    printf("  PASS test_button_mapping\n");
}

static void test_axis_mapping(void) {
    uint16_t type, code;
    assert(pad_input_to_evdev(PAD_LX, &type, &code) == 1);
    assert(type == EV_ABS && code == EVA_ABS_X);
    assert(pad_input_to_evdev(PAD_RY, &type, &code) == 1);
    assert(type == EV_ABS && code == EVA_ABS_RY);
    printf("  PASS test_axis_mapping\n");
}

static void test_frame_encode(void) {
    uint8_t f[HID_FRAME_SZ];
    hid_frame_encode(EV_KEY, EVB_BTN_SOUTH, 1, f);
    assert(f[0] == EV_KEY);
    assert((uint16_t)(f[1] | (f[2] << 8)) == EVB_BTN_SOUTH);
    assert((int16_t)(f[3] | (f[4] << 8)) == 1);

    hid_frame_encode(EV_ABS, EVA_ABS_X, -32768, f);
    assert((int16_t)(f[3] | (f[4] << 8)) == -32768);
    printf("  PASS test_frame_encode\n");
}

int main(void) {
    printf("test_hid:\n");
    test_button_mapping();
    test_axis_mapping();
    test_frame_encode();
    printf("All tests passed.\n");
    return 0;
}
```

- [ ] **Step 2: Run to verify it fails.**
```bash
cc -Wall -Wextra tests/test_hid.c -o /tmp/test_hid && /tmp/test_hid
```
Expected: fatal error `src/norns-hid.h: No such file or directory`.

- [ ] **Step 3: Implement `src/norns-hid.h`.**

```c
/*
 * norns-hid.h — SDL-free evdev mapping + frame encoder for the native HID path.
 *
 * Header-only (like norns-controls.h) so the host and the standalone tests share
 * the SAME logic. The host translates SDL events to pad_input_t (trivial switch,
 * SDL-coupled, lives in the .c); everything below is pure and unit-tested.
 *
 * Wire frame (host → matron NORNS_HID_FIFO): 5 bytes, little-endian
 *   [type:1][code_lo:1][code_hi:1][value_lo:1][value_hi:1]
 *   type  = EV_KEY (button) | EV_ABS (axis)
 *   value = 0/1 for buttons; -32768..32767 for axes
 */
#ifndef NORNS_HID_H
#define NORNS_HID_H

#include <stdint.h>

/* Linux evdev event types (subset) */
#define EV_KEY 0x01
#define EV_ABS 0x03

/* evdev button codes (linux/input-event-codes.h) */
#define EVB_BTN_SOUTH      0x130  /* A  */
#define EVB_BTN_EAST       0x131  /* B  */
#define EVB_BTN_NORTH      0x133  /* Y  */
#define EVB_BTN_WEST       0x134  /* X  */
#define EVB_BTN_TL         0x136  /* L1 */
#define EVB_BTN_TR         0x137  /* R1 */
#define EVB_BTN_TL2        0x138  /* L2 */
#define EVB_BTN_TR2        0x139  /* R2 */
#define EVB_BTN_SELECT     0x13a
#define EVB_BTN_START      0x13b
#define EVB_BTN_THUMBL     0x13d  /* L3 */
#define EVB_BTN_THUMBR     0x13e  /* R3 */
#define EVB_BTN_DPAD_UP    0x220
#define EVB_BTN_DPAD_DOWN  0x221
#define EVB_BTN_DPAD_LEFT  0x222
#define EVB_BTN_DPAD_RIGHT 0x223

/* evdev abs (axis) codes */
#define EVA_ABS_X   0x00
#define EVA_ABS_Y   0x01
#define EVA_ABS_RX  0x03
#define EVA_ABS_RY  0x04

#define HID_FRAME_SZ 5

/* Abstract pad inputs the host knows about (SDL → this, then this → evdev). */
typedef enum {
    PAD_A = 0, PAD_B, PAD_X, PAD_Y,
    PAD_L1, PAD_R1, PAD_L2, PAD_R2,
    PAD_SELECT, PAD_START, PAD_L3, PAD_R3,
    PAD_DUP, PAD_DDOWN, PAD_DLEFT, PAD_DRIGHT,
    PAD_LX, PAD_LY, PAD_RX, PAD_RY,
    PAD_COUNT
} pad_input_t;

/* Map a pad input to its evdev (type, code). Returns 1 if mapped, 0 otherwise. */
static inline int pad_input_to_evdev(pad_input_t in, uint16_t *type, uint16_t *code) {
    switch (in) {
    case PAD_A:      *type = EV_KEY; *code = EVB_BTN_SOUTH;      return 1;
    case PAD_B:      *type = EV_KEY; *code = EVB_BTN_EAST;       return 1;
    case PAD_X:      *type = EV_KEY; *code = EVB_BTN_WEST;       return 1;
    case PAD_Y:      *type = EV_KEY; *code = EVB_BTN_NORTH;      return 1;
    case PAD_L1:     *type = EV_KEY; *code = EVB_BTN_TL;         return 1;
    case PAD_R1:     *type = EV_KEY; *code = EVB_BTN_TR;         return 1;
    case PAD_L2:     *type = EV_KEY; *code = EVB_BTN_TL2;        return 1;
    case PAD_R2:     *type = EV_KEY; *code = EVB_BTN_TR2;        return 1;
    case PAD_SELECT: *type = EV_KEY; *code = EVB_BTN_SELECT;     return 1;
    case PAD_START:  *type = EV_KEY; *code = EVB_BTN_START;      return 1;
    case PAD_L3:     *type = EV_KEY; *code = EVB_BTN_THUMBL;     return 1;
    case PAD_R3:     *type = EV_KEY; *code = EVB_BTN_THUMBR;     return 1;
    case PAD_DUP:    *type = EV_KEY; *code = EVB_BTN_DPAD_UP;    return 1;
    case PAD_DDOWN:  *type = EV_KEY; *code = EVB_BTN_DPAD_DOWN;  return 1;
    case PAD_DLEFT:  *type = EV_KEY; *code = EVB_BTN_DPAD_LEFT;  return 1;
    case PAD_DRIGHT: *type = EV_KEY; *code = EVB_BTN_DPAD_RIGHT; return 1;
    case PAD_LX:     *type = EV_ABS; *code = EVA_ABS_X;          return 1;
    case PAD_LY:     *type = EV_ABS; *code = EVA_ABS_Y;          return 1;
    case PAD_RX:     *type = EV_ABS; *code = EVA_ABS_RX;         return 1;
    case PAD_RY:     *type = EV_ABS; *code = EVA_ABS_RY;         return 1;
    default:                                                    return 0;
    }
}

/* Encode one evdev event into a 5-byte little-endian wire frame. */
static inline void hid_frame_encode(uint16_t type, uint16_t code, int16_t value,
                                    uint8_t out[HID_FRAME_SZ]) {
    out[0] = (uint8_t)type;
    out[1] = (uint8_t)(code & 0xFF);
    out[2] = (uint8_t)((code >> 8) & 0xFF);
    out[3] = (uint8_t)((uint16_t)value & 0xFF);
    out[4] = (uint8_t)(((uint16_t)value >> 8) & 0xFF);
}

#endif /* NORNS_HID_H */
```

- [ ] **Step 4: Run to verify it passes.**
```bash
cc -Wall -Wextra tests/test_hid.c -o /tmp/test_hid && /tmp/test_hid
```
Expected: three `PASS` lines then `All tests passed.`

- [ ] **Step 5: Commit.**
```bash
git add src/norns-hid.h tests/test_hid.c
git commit -m "feat(hid): SDL-free evdev mapping + wire-frame encoder for native pad"
```

---

## Task 4: matron synthetic HID device patch (spike-confirmed)

**Depends on:** Task 1 PASS. Use the exact registration + event-post signatures the spike recorded.

**Files:**
- Modify: `patches/apply-move-patches.sh` (new section, modeled on §8 virtual-grid registration)
- Create: `patches/hid-virtual.patch` (prose description, repo convention)

- [ ] **Step 1: Write the prose patch doc.** Create `patches/hid-virtual.patch` describing: env var `NORNS_HID_FIFO`; the in-memory libevdev device (name `norns-panicos gamepad`, capabilities from spec §4.1); a FIFO-reader thread decoding 5-byte frames (`[type][code_lo][code_hi][val_lo][val_hi]`) and posting `EVENT_HID_EVENT`; registration at startup alongside the virtual grid; coexistence with the input FIFO. Mirror the style of `patches/input-virtual.patch`.

- [ ] **Step 2: Add the apply-move-patches.sh section.** After §8 (virtual grid registration), add a section that writes a `device_hid` virtual source and registers it. Skeleton (finalize struct/call names from the spike notes):

```sh
# ── 8b. Register virtual HID gamepad (in-memory libevdev, FIFO-fed) ──
echo "  Patching main.c / device_list.c (virtual HID gamepad)"
cat > matron/src/device/device_hid_virtual.c << 'HIDEOF'
/* device_hid_virtual.c — synthetic gamepad fed by NORNS_HID_FIFO.
 * Builds an in-memory libevdev device (no /dev/input fd) so matron's stock
 * hid add/serialize/event path runs unmodified, and streams 5-byte frames
 * from the FIFO as EVENT_HID_EVENT. See patches/hid-virtual.patch. */
#include <libevdev/libevdev.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "device_hid.h"
#include "device_list.h"
#include "events.h"

/* Capability table — keep in lockstep with src/norns-hid.h / spec §4.1. */
static const int hid_keys[] = {
    BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH, BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
    BTN_SELECT, BTN_START, BTN_THUMBL, BTN_THUMBR,
    BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT,
};
static const int hid_axes[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY };

static struct libevdev *build_synthetic(void) {
    struct libevdev *d = libevdev_new();
    libevdev_set_name(d, "norns-panicos gamepad");
    libevdev_enable_event_type(d, EV_KEY);
    for (unsigned i = 0; i < sizeof(hid_keys)/sizeof(hid_keys[0]); i++)
        libevdev_enable_event_code(d, EV_KEY, hid_keys[i], NULL);
    libevdev_enable_event_type(d, EV_ABS);
    struct input_absinfo ai = { .minimum = -32768, .maximum = 32767 };
    for (unsigned i = 0; i < sizeof(hid_axes)/sizeof(hid_axes[0]); i++)
        libevdev_enable_event_code(d, EV_ABS, hid_axes[i], &ai);
    return d;
}

/* Reader thread: decode 5-byte frames → post hid events for `id`.
 * <<spike>>: replace post_hid_event(...) with the EXACT call device_hid.c's
 * read loop uses (recorded in the spike notes). */
static void *hid_fifo_poll(void *arg) {
    int id = (int)(intptr_t)arg;
    const char *path = getenv("NORNS_HID_FIFO");
    int fd = open(path, O_RDONLY);          /* blocks until host opens write end */
    uint8_t f[5];
    while (read(fd, f, 5) == 5) {
        uint16_t type = f[0];
        uint16_t code = (uint16_t)(f[1] | (f[2] << 8));
        int16_t  val  = (int16_t)(f[3] | (f[4] << 8));
        post_hid_event(id, type, code, val);   /* <<spike: exact signature>> */
    }
    close(fd);
    return NULL;
}

void dev_hid_virtual_init(void) {
    if (!getenv("NORNS_HID_FIFO")) return;
    struct libevdev *d = build_synthetic();
    /* <<spike>>: register `d` exactly as device_monitor registers a hid device
     * (allocate device_hid_t, set ->dev = d, dev_list_add → EVENT_HID_ADD),
     * capturing the assigned device id. Then: */
    int id = /* <<spike: id from dev_list_add>> */ 0;
    pthread_t th;
    pthread_create(&th, NULL, hid_fifo_poll, (void *)(intptr_t)id);
}
HIDEOF
# Register call in main.c, next to the virtual-grid registration:
sed -i 's/\(.*dev_monome_virtual_init.*\)/\1\n    extern void dev_hid_virtual_init(void); dev_hid_virtual_init();/' \
    matron/src/main.c
# Add device_hid_virtual.c to the build (wscript sources list):
# <<spike>>: append 'matron/src/device/device_hid_virtual.c' to the matron
# sources glob in wscript exactly where device_hid.c is listed.
```

- [ ] **Step 3: Build matron with the patch.**
```bash
BUILD_FROM_SOURCE=1 ./scripts/build-panicos.sh
```
Expected: build completes; log shows `Patching main.c / device_list.c (virtual HID gamepad)`.

- [ ] **Step 4: Integration-verify on device/emulator.** Launch norns with `NORNS_HID_FIFO=/tmp/norns-hid-1` set, then from maiden:
```lua
for i,d in ipairs(hid.vports) do print(i, d.name) end   -- expect "norns-panicos gamepad"
hid.vports[1].event = function(t,c,v) print("hid",t,c,v) end
```
Then push one frame from a shell:
```bash
printf '\x01\x30\x01\x01\x00' > /tmp/norns-hid-1   # EV_KEY BTN_SOUTH down
```
Expected: maiden prints `hid 1 304 1`.

- [ ] **Step 5: Commit.**
```bash
git add patches/apply-move-patches.sh patches/hid-virtual.patch
git commit -m "feat(matron): synthetic in-memory libevdev HID gamepad fed by NORNS_HID_FIFO"
```

---

## Task 5: host HID FIFO plumbing (norns-panicos.c)

**Files:**
- Modify: `src/norns-panicos.c` (FIFO const, state field, creation, `send_hid()`, env var)

- [ ] **Step 1: Add the FIFO constant + state field.** Near the other `FIFO_*` defines (after `FIFO_INPUT`):
```c
#define FIFO_HID      "/tmp/norns-hid-1"
```
In `norns_state_t`, after `int input_fd;`:
```c
    int hid_fd;           /* native-mode evdev frames → matron NORNS_HID_FIFO */
```

- [ ] **Step 2: Create the FIFO.** In `create_fifos()`, after `s->input_fd = make_fifo(FIFO_INPUT);`:
```c
    s->hid_fd = make_fifo(FIFO_HID);
```
(`make_fifo` already opens `O_RDWR | O_NONBLOCK`, so writes never block even with no reader.)

- [ ] **Step 3: Add the include + writer.** At the top with the other includes:
```c
#include "norns-hid.h"
```
Next to `send_key()`:
```c
/* Emit one evdev event on the native HID FIFO (non-blocking; dropped if full). */
static void send_hid(norns_state_t *s, uint16_t type, uint16_t code, int16_t value) {
    if (s->hid_fd < 0) return;
    uint8_t frame[HID_FRAME_SZ];
    hid_frame_encode(type, code, value, frame);
    (void)write(s->hid_fd, frame, HID_FRAME_SZ);
}
```

- [ ] **Step 4: Pass `NORNS_HID_FIFO` to matron.** `NORNS_INPUT_FIFO` is set at **two** spawn sites — `src/norns-panicos.c:217` and `:280`. Add the HID env at **both**, immediately after each `setenv("NORNS_INPUT_FIFO", FIFO_INPUT, 1);` line:
```c
setenv("NORNS_HID_FIFO", FIFO_HID, 1);
```
(Miss either site and the FIFO works in only one of the two launch paths.)

- [ ] **Step 5: Verify it still builds.** Build the host binary (per `scripts/build-panicos.sh`'s host compile line — the `gcc ... norns-panicos.c`):
```bash
gcc -O2 -Wall -Wextra src/norns-panicos.c -o /tmp/norns-panicos \
    $(pkg-config --cflags --libs sdl2) 2>&1 | head
```
Expected: compiles with no errors (warnings about unused `send_hid` are fine until Task 6 wires it).

- [ ] **Step 6: Commit.**
```bash
git add src/norns-panicos.c
git commit -m "feat(host): native HID FIFO + send_hid writer + NORNS_HID_FIFO env"
```

---

## Task 6: native-mode routing in the host (norns-panicos.c)

**Files:**
- Modify: `src/norns-panicos.c` (SDL→pad_input translation; route to `send_hid` in native mode; suppress emulation; stream sticks/triggers as axes)

- [ ] **Step 1: Add SDL→pad_input translation.** Near `sdl_btn_bit()` (`:429`):
```c
/* SDL button → abstract pad input, or -1 if it isn't a pad button we expose. */
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
    default:                                  return -1;
    }
}

/* Emit a pad button as a native HID key event. */
static void send_pad_button(norns_state_t *s, int pad, int pressed) {
    uint16_t type, code;
    if (pad >= 0 && pad_input_to_evdev((pad_input_t)pad, &type, &code))
        send_hid(s, type, code, (int16_t)(pressed ? 1 : 0));
}
```

- [ ] **Step 2: Route buttons in native mode.** At the very top of `handle_button()` (`:507`), after `int pressed = ...`, before the L1/R1 block:
```c
    /* Native mode: the script owns the raw pad. Route every pad button to HID
     * and suppress all K/E emulation. (Reserved system inputs are intercepted
     * in Task 7, which runs BEFORE this — they never reach here.) */
    if (s->controls.native_mode) {
        int pad = sdl_btn_to_pad(ev->button);
        if (pad >= 0) send_pad_button(s, pad, pressed);
        return;
    }
```

- [ ] **Step 3: Stream sticks + triggers as axes in native mode.** Add a per-frame poller and call it from the main loop next to `poll_encoders(s)`. Add near `poll_encoders`:
```c
/* In native mode, mirror the analog sticks and L2/R2 triggers to HID axes.
 * Only emits on change to avoid flooding the FIFO. */
static void poll_native_axes(norns_state_t *s) {
    if (!s->controls.native_mode || !s->gc) return;
    static int last[6];               /* LX,LY,RX,RY,L2,R2 */
    struct { SDL_GameControllerAxis ax; uint16_t code; int analog; } m[] = {
        { SDL_CONTROLLER_AXIS_LEFTX,  EVA_ABS_X,  1 },
        { SDL_CONTROLLER_AXIS_LEFTY,  EVA_ABS_Y,  1 },
        { SDL_CONTROLLER_AXIS_RIGHTX, EVA_ABS_RX, 1 },
        { SDL_CONTROLLER_AXIS_RIGHTY, EVA_ABS_RY, 1 },
    };
    for (int i = 0; i < 4; i++) {
        int v = SDL_GameControllerGetAxis(s->gc, m[i].ax);
        if (v != last[i]) { last[i] = v; send_hid(s, EV_ABS, m[i].code, (int16_t)v); }
    }
    /* L2/R2 as buttons (digital threshold), matching spec §4.1 default. */
    int l2 = SDL_GameControllerGetAxis(s->gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > TRIGGER_THRESH;
    int r2 = SDL_GameControllerGetAxis(s->gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > TRIGGER_THRESH;
    if (l2 != last[4]) { last[4] = l2; send_hid(s, EV_KEY, EVB_BTN_TL2, (int16_t)l2); }
    if (r2 != last[5]) { last[5] = r2; send_hid(s, EV_KEY, EVB_BTN_TR2, (int16_t)r2); }
}
```
Then in the main loop, next to the existing `poll_encoders(s);` call, add:
```c
    poll_native_axes(s);
```
And guard `poll_encoders` so it does nothing in native mode. `poll_encoders()` already calls `poll_context(s);` on its second line (`:578`). Insert the guard **immediately after** that existing call (do not add a second `poll_context`):
```c
    const controls_t *c = &s->controls;
    poll_context(s);
    if (c->native_mode) return;     /* native mode: no encoder emulation */
```

- [ ] **Step 4: Build + manual verify.** Build the host (Task 5 Step 5 command). With a conf containing a `[script:NAME] mode = native` overlay and that script running, confirm `/tmp/norns-context` flips and the maiden `hid.vports[1].event` handler prints button/axis events while K/E events stop. Expected: pressing A prints `hid 1 304 1`; `key()`/`enc()` no longer fire.

- [ ] **Step 5: Commit.**
```bash
git add src/norns-panicos.c
git commit -m "feat(host): route pad to native HID + suppress K/E emulation per-script"
```

---

## Task 7: escape interception — Menu/FN reserved (DEPENDS ON PHASE 1)

**Blocked until Phase 1 ships** the liberated Menu/FN button + the host-reserved-input mechanism. Do not start until `handle_button` has a Menu/FN case and a generalized reserved-input path.

**Files:**
- Modify: `src/norns-panicos.c` (intercept Menu/FN before native routing; synthesize K1 home tap)

- [ ] **Step 1: Reserve Menu/FN ahead of native routing.** In `handle_button()`, the Phase-1 Menu/FN handling must run **before** the Task 6 Step 2 native block. Short Menu/FN while in native mode = pop home:
```c
    /* Phase 1 provides the Menu/FN button + hold/quit chord. Here we add the
     * native-mode meaning of a SHORT press: synthesize a K1 tap so matron's
     * menu system returns home; context then flips out of native on its own. */
    if (ev->button == SDL_BUTTON_MENU_FN /* Phase 1's constant */) {
        /* Phase 1 already handles hold/Select+Menu = quit. Short press: */
        if (!pressed /* on release, not a hold/quit */ && s->controls.native_mode) {
            send_key(s, 0, 1);   /* K1 down  (= "home" in this port) */
            send_key(s, 0, 0);   /* K1 up    */
        }
        return;   /* reserved: never reaches the script */
    }
```
(Exact button constant + hold/quit gating come from Phase 1; wire to its API rather than re-implementing quit here.)

- [ ] **Step 2: Verify the escape.** With a native script running, press Menu/FN: norns returns to the system menu, `/tmp/norns-context` becomes `menu`, native mode clears, K/E emulation resumes. Hold Menu/FN (or Select+Menu): norns quits (Phase 1 behavior, unchanged).

- [ ] **Step 3: Commit.**
```bash
git add src/norns-panicos.c
git commit -m "feat(host): reserve Menu/FN as native-mode escape to norns menu"
```

---

## Task 8: `pad` wrapper library (Lua)

**Files:**
- Create: `lua/pad.lua`
- Create: `tests/test_pad.lua`

- [ ] **Step 1: Write the failing test.** Create `tests/test_pad.lua`:
```lua
-- tests/test_pad.lua — drives pad.lua against a stub norns `hid` global.
-- Run: lua tests/test_pad.lua   (expects "All tests passed.")
package.path = "lua/?.lua;" .. package.path

-- Minimal stub of the norns hid API surface pad.lua uses.
local stub_dev = { name = "norns-panicos gamepad" }
hid = { vports = { stub_dev } }
hid.connect = function(n) return hid.vports[n or 1] end

local pad = require("pad")
assert(pad.available == true, "pad.available should detect the synthetic device")

local got = {}
pad.on("a", function(v) got.a = v end)
pad.event = function(name, v) got.last = name end

-- Simulate the device firing BTN_SOUTH (304) down, then ABS_X (0) to +16384.
stub_dev.event(0x01, 304, 1)
assert(got.a == 1, "pad.on('a') should fire on BTN_SOUTH down")
assert(pad.a == true, "pad.a polled state should be true while held")
assert(got.last == "a", "pad.event should receive the semantic name")

stub_dev.event(0x03, 0, 16384)
assert(math.abs(pad.lstick.x - 0.5) < 0.05, "ABS_X should normalise to ~0.5")

stub_dev.event(0x01, 304, 0)
assert(pad.a == false, "pad.a should clear on release")

print("All tests passed.")
```

- [ ] **Step 2: Run to verify it fails.**
```bash
lua tests/test_pad.lua
```
Expected: error `module 'pad' not found`.

- [ ] **Step 3: Implement `lua/pad.lua`.**
```lua
-- pad.lua — ergonomic wrapper over the norns-panicos synthetic HID gamepad.
-- Sugar only: it binds the stock hid device, exposes semantic names, polled
-- state, an on()/event() callback surface, and pad.available detection.
local pad = { available = false, a=false, b=false, x=false, y=false,
  l1=false, r1=false, l2=false, r2=false, select=false, start=false,
  l3=false, r3=false, up=false, down=false, left=false, right=false,
  lstick = { x=0, y=0 }, rstick = { x=0, y=0 } }

local SYNTH_NAME = "norns-panicos gamepad"

-- evdev code → semantic button name
local BTN = {
  [0x130]="a",[0x131]="b",[0x134]="x",[0x133]="y",
  [0x136]="l1",[0x137]="r1",[0x138]="l2",[0x139]="r2",
  [0x13a]="select",[0x13b]="start",[0x13d]="l3",[0x13e]="r3",
  [0x220]="up",[0x221]="down",[0x222]="left",[0x223]="right",
}
-- evdev abs code → {stick, axis}
local AXIS = { [0]={"lstick","x"},[1]={"lstick","y"},[3]={"rstick","x"},[4]={"rstick","y"} }

local handlers = {}                 -- name → fn(value)
function pad.on(name, fn) handlers[name] = fn end
pad.event = nil                     -- optional: fn(name, value)

local function fire(name, value)
  if handlers[name] then handlers[name](value) end
  if pad.event then pad.event(name, value) end
end

local function on_hid(typ, code, value)
  if typ == 0x01 then                       -- EV_KEY
    local name = BTN[code]; if not name then return end
    pad[name] = (value ~= 0)
    fire(name, value)
  elseif typ == 0x03 then                   -- EV_ABS
    local a = AXIS[code]; if not a then return end
    local norm = value / 32767.0
    if norm < -1 then norm = -1 elseif norm > 1 then norm = 1 end
    pad[a[1]][a[2]] = norm
    fire(a[1].."."..a[2], norm)
  end
end

-- Bind to the synthetic device if present.
local function bind()
  if not hid or not hid.vports then return end
  for i, d in ipairs(hid.vports) do
    if d.name == SYNTH_NAME then
      pad.available = true
      d.event = on_hid
      return
    end
  end
end
bind()

-- Optional runtime opt-in (Task 9): request native mode without editing config.
function pad.grab()    local f=io.open("/tmp/norns-native","w") if f then f:write("1") f:close() end end
function pad.release()  local f=io.open("/tmp/norns-native","w") if f then f:write("")  f:close() end end

return pad
```

- [ ] **Step 4: Run to verify it passes.**
```bash
lua tests/test_pad.lua
```
Expected: `All tests passed.`

- [ ] **Step 5: Ship it via the build.** In `scripts/build-panicos.sh`, alongside the other lua-path setup, copy the lib into norns's lua path:
```sh
cp "$REPO_ROOT/lua/pad.lua" "$NORNS_DATA/norns/lua/lib/pad.lua"
echo "  Installed pad.lua (native gamepad wrapper) into norns lua/lib"
```

- [ ] **Step 6: Commit.**
```bash
git add lua/pad.lua tests/test_pad.lua scripts/build-panicos.sh
git commit -m "feat(lua): pad wrapper library over the native HID gamepad"
```

---

## Task 9: runtime `pad.grab()` override (host reads /tmp/norns-native)

**Files:**
- Modify: `src/norns-panicos.c` (poll `/tmp/norns-native` as a native override on top of config)

- [ ] **Step 1: Add the override poll.** Next to `poll_context()` (`:485`):
```c
/* Runtime native opt-in: pad.grab() writes "1" to /tmp/norns-native, pad.release()
 * clears it. This overrides the config-derived native_mode for the running script
 * (spec D8). Polled on the same cadence as context. */
#define NATIVE_FILE "/tmp/norns-native"
static void poll_native_override(norns_state_t *s) {
    if (s->frame % 6 != 0) return;
    int fd = open(NATIVE_FILE, O_RDONLY);
    if (fd < 0) return;
    char buf[8] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    int want = (n > 0 && buf[0] == '1');
    if (want && !s->controls.native_mode) s->controls.native_mode = 1;
    else if (!want && s->native_grab_was) s->controls.native_mode = 0; /* released */
    s->native_grab_was = want;
}
```
Add `int native_grab_was;` to `norns_state_t`. In `poll_encoders()`, call `poll_native_override(s);` **between** `poll_context(s);` and the Task 6 `if (c->native_mode) return;` guard — it must run *before* the guard, since it is what flips `native_mode` on when a script calls `pad.grab()`. Final order:
```c
    const controls_t *c = &s->controls;
    poll_context(s);
    poll_native_override(s);
    if (c->native_mode) return;
```

- [ ] **Step 2: Reset on context change.** In `poll_context()`, when the context changes, clear a stale grab so a script that exits without `pad.release()` can't strand native mode — after `resolve_controls(s);`:
```c
        unlink(NATIVE_FILE);                 /* drop any stale grab */
        s->native_grab_was = 0;
```

- [ ] **Step 3: Build + verify.** Build the host. With a non-native script that calls `pad.grab()` in init, confirm native mode engages at runtime and clears on script exit / `pad.release()`.

- [ ] **Step 4: Commit.**
```bash
git add src/norns-panicos.c
git commit -m "feat(host): runtime pad.grab() native override via /tmp/norns-native"
```

---

## Task 10: documentation + example overlay

**Files:**
- Modify: `docs/controls.md` (new "Native gamepad mode" section) and `ports/portmaster/controls.defaults.conf` (commented example)

- [ ] **Step 1: Document native mode + the `pad` API.** Add a section to `docs/controls.md` covering: what `mode = native` does (suppresses K/E for that script, routes raw HID), the reserved Menu/FN escape, the stock `hid` path, and the `pad` wrapper (`require 'pad'`, `pad.available`, `pad.on`, `pad.event`, polled state, `pad.grab()/release()`). State the cross-device stable identity (`norns-panicos gamepad`).

- [ ] **Step 2: Add a commented example overlay.** In `ports/portmaster/controls.defaults.conf`, add (commented, so it ships inert):
```ini
# Native gamepad mode — give a script the raw pad (all buttons + both sticks)
# instead of the K/E emulation. Menu/FN still returns you to the norns menu.
# [script:mygamepadscript]
# mode = native
```

- [ ] **Step 3: Run all tests as a final gate.**
```bash
for t in test_controls test_controls_conf test_hid test_input test_monome_mext; do
  cc -Wall -Wextra tests/$t.c -o /tmp/$t && /tmp/$t || exit 1
done
lua tests/test_pad.lua
```
Expected: every binary prints `All tests passed.` and the Lua test does too.

- [ ] **Step 4: Commit.**
```bash
git add docs/controls.md ports/portmaster/controls.defaults.conf
git commit -m "docs(controls): document native gamepad mode + pad API"
```

---

## Self-review notes (for the implementer)

- **Spec coverage:** D1 hybrid (Tasks 3,8) · D2 per-script native + escape (Tasks 2,6,7) · D3 Transport B (Tasks 1,4) · D4 in-memory libevdev (Tasks 1,4) · D5 Menu=home escape (Task 7, gated on Phase 1) · D6 no mod (none added) · D7 device always advertised / events gated (Task 4 always-registers; Task 6 gates emission via `native_mode`) · D8 dual entry: config (Task 2) + runtime grab (Task 9). §4.1 capabilities mirrored in `hid_keys[]`/`hid_axes[]` (Task 4) and `src/norns-hid.h` (Task 3) — **keep these in lockstep**. §6 error handling: non-blocking FIFO (Task 5 Step 2), grab reset on context change (Task 9 Step 2).
- **Phase-1 dependency is real:** Task 7 cannot complete before Phase 1. All other tasks are independent and form a working, testable HID core on their own (a native script reached via a hand-written `mode = native` overlay works end-to-end after Task 6; Task 7 only adds the *reserved* escape — until then, the existing Select=restart still gets you out).
- **Throwaway spike:** Task 1's code is reverted (Step 5); only the in-repo patch (Task 4) is kept.
