# Button Liberation (Phase 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Move quit/restart off Select/Start onto a host-reserved Menu/FN (SDL `GUIDE`) button — tap=home, hold/Select+Menu=quit — freeing Select, Start, L3, and R3 into the mappable pool, and deleting the Phase 3 interim carve-out.

**Architecture:** A pure, unit-tested system-button state machine (`sysbtn_*` in `norns-controls.h`) decides Home/Quit from GUIDE press timing + a Select+GUIDE chord; the host wires it into `handle_button`/`poll_encoders` using `SDL_GetTicks()` and live button state. The key-button bitmask widens `uint8_t`→`uint16_t` to add `select/start/l3/r3` as bindable inputs. Two on-device verification tasks (C1/C2) open the plan so we build on observed facts, not guesses.

**Tech Stack:** C11 (host + header-only shared logic), SDL2 GameController, pure-C `assert` tests (`cc`, no framework).

---

## Source spec

`docs/superpowers/specs/2026-06-03-phase1-button-liberation-design.md`. Read it.

## Prerequisites & notes

- **Touches `src/norns-controls.h`**, which the stashed `wip/control-remapping` branch also refactored — resuming that branch later needs a merge. Expected (the project pivoted).
- **Test convention:** `cc -Wall -Wextra tests/test_NAME.c -o /tmp/test_NAME && /tmp/test_NAME` → ends `All tests passed.`
- **Host compiles locally** (SDL2 present): `cc -O2 -Wall -Wextra src/norns-panicos.c -o /tmp/np $(pkg-config --cflags --libs sdl2) -lpthread -lm`.
- **Device:** `ssh panicos` (key auth). Gamepad = `/dev/input/event3` (`H700 Gamepad`). Menu/FN raw code already confirmed = `316 BTN_MODE`. `gptokeyb -1 norns-panicos` co-owns the pad.
- **Hardware facts already confirmed:** Menu/FN=`BTN_MODE`, Select=`BTN_SELECT`, Start=`BTN_START`, L3=`BTN_THUMBL`, R3=`BTN_THUMBR`.

## File structure

- **Modify `src/norns-controls.h`** — widen bitmask to `uint16_t`; add `BTN_SELECT/START/L3/R3`; extend `controls__parse_btn` (reject `guide`/`menu`); add the pure `sysbtn_*` state machine + `sys_action_t`.
- **Modify `src/norns-panicos.c`** — widen `key_btn` users; `sdl_btn_bit` maps the 4 new buttons; replace the `BACK`/`START` system-chord block + delete the native carve-out; add a `poll_native`/tick hook for GUIDE-hold; inject K1 for Home.
- **Modify `tests/test_controls.c`** — bitmask/parser + `sysbtn_*` tests.
- **Modify `docs/controls.md`** — document Menu/FN + freed buttons + new bindable names.
- **Create `tools/sdl_guide_probe.c`** (Task 1, throwaway-ish device probe).

---

## Task 1: C1 — confirm SDL delivers GUIDE + check gptokeyb (on-device verification)

**Not TDD — a device probe. Its result decides whether the system button is `GUIDE` (preferred) or the §8 fallback (`Start`).**

**Files:** Create `tools/sdl_guide_probe.c`.

- [ ] **Step 1: Write the probe.**

```c
/* tools/sdl_guide_probe.c — prints every SDL GameController button + name.
 * Build for device: aarch64 cross or on-device gcc; run, press Menu/FN. */
#include <SDL2/SDL.h>
#include <stdio.h>
int main(void) {
    if (SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1;
    }
    SDL_GameController *gc = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) { gc = SDL_GameControllerOpen(i); break; }
    printf("controller: %s\n", gc ? SDL_GameControllerName(gc) : "(none)");
    printf("press buttons (Menu/FN, A, Select, Start). Ctrl-C to quit.\n");
    SDL_Event e;
    while (1) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP)
                printf("cbutton=%d (%s) %s\n", e.cbutton.button,
                       SDL_GameControllerGetStringForButton((SDL_GameControllerButton)e.cbutton.button),
                       e.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up");
            if (e.type == SDL_CONTROLLERDEVICEADDED && !gc) gc = SDL_GameControllerOpen(e.cdevice.which);
        }
        SDL_Delay(16);
    }
}
```

- [ ] **Step 2: Build it on the device** (device has gcc + SDL2; avoids cross-compile):

```bash
scp tools/sdl_guide_probe.c panicos:/tmp/
ssh panicos 'cc /tmp/sdl_guide_probe.c -o /tmp/guide_probe $(pkg-config --cflags --libs sdl2 2>/dev/null || echo -lSDL2) && echo BUILT'
```
Expected: `BUILT`.

- [ ] **Step 3: Run it (user presses buttons).** norns must NOT be running (frees the controller). Ask the user to run, in a Mac terminal, and press **Menu/FN, then A, then Select, then Start**, then Ctrl-C:

```bash
ssh -t panicos 'SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig" /tmp/guide_probe'
```
(If `$sdl_controllerconfig` isn't in the env, run without it — SDL falls back to its built-in db.)

- [ ] **Step 4: Record the verdict.**
  - Menu/FN prints `cbutton=5 (guide)` → **GUIDE confirmed**; Tasks 5 use `SDL_CONTROLLER_BUTTON_GUIDE`.
  - Menu/FN prints a *different* `cbutton=N` → use that constant in Task 5.
  - Menu/FN prints **nothing** (but A/Select do) → gptokeyb/SDL swallows it → **take the §8 fallback**: reserve `SDL_CONTROLLER_BUTTON_START` as the system button (Start-tap=Home, Start-hold=Quit), freeing Select+L3+R3. Note this in the commit and adjust Task 5's button constant accordingly.
  - Also note whether pressing Menu/FN made the app/PortMaster do anything (gptokeyb exit). If it fired a competing action, plan a `.gptk`/`Norns.sh` tweak (out of scope to fully solve here; record it).

- [ ] **Step 5: Commit the probe + findings.**
```bash
git add tools/sdl_guide_probe.c
git commit -m "tools: SDL GameController button probe (C1 verification)"
```
Record the verdict in the commit body.

**Gate:** Task 5 needs the confirmed button constant. Default assumption below is `GUIDE`; swap to the fallback if Step 4 says so.

---

## Task 2: C2 — confirm K1 injection returns to the menu (on-device verification)

**Files:** none (uses the deployed binary's FIFO).

- [ ] **Step 1: With norns running and a script loaded**, inject a K1 tap by writing the input FIFO frame directly:
```bash
ssh panicos 'printf "\x01\x00\x01\x00" > /tmp/norns-input-1; sleep 0.1; printf "\x01\x00\x00\x00" > /tmp/norns-input-1'
```
(Frame = `[type=1 key][id=0 K1][state][0]`; down then up.)

- [ ] **Step 2: Observe** whether norns returns to the system menu.
  - Returns to menu → **K1-home confirmed**; Task 5 uses K1 inject for Home.
  - Does nothing → try a longer hold (increase the sleep) or two taps; if still nothing, record that Home needs a different gesture and check `lua/core/menu.lua`'s key handling for the actual "to menu" key. Update Task 5's Home action accordingly.

- [ ] **Step 3:** No commit (observation only). Record the result for Task 5.

---

## Task 3: Widen the key-button bitmask + add Select/Start/L3/R3 (pure, TDD)

**Files:** Modify `src/norns-controls.h`; Test `tests/test_controls.c`.

- [ ] **Step 1: Write the failing test.** Add to `tests/test_controls.c` (and call from `main`):

```c
static void test_new_bindable_buttons(void) {
    controls_t c;
    controls_defaults(&c);
    /* defaults leave the freed buttons unbound */
    for (int k = 0; k < 3; k++) {
        assert(!(c.key_btn[k] & (BTN_SELECT | BTN_START | BTN_L3 | BTN_R3)));
    }
    /* they can be bound, including multiple per key */
    assert(controls_parse(&c, "k1 = select, start\nk2 = l3\nk3 = r3\n") == 3);
    assert(c.key_btn[0] == (uint16_t)(BTN_SELECT | BTN_START));
    assert(c.key_btn[1] == BTN_L3);
    assert(c.key_btn[2] == BTN_R3);
    /* "guide"/"menu" are host-reserved and never bind */
    controls_defaults(&c);
    assert(controls_parse(&c, "k1 = guide\nk2 = menu, a\n") == 2);
    assert(c.key_btn[0] == 0);            /* guide ignored → empty */
    assert(c.key_btn[1] == BTN_A);        /* menu ignored, a kept */
    printf("  PASS test_new_bindable_buttons\n");
}
```

- [ ] **Step 2: Run to verify it fails.**
```bash
cc -Wall -Wextra tests/test_controls.c -o /tmp/test_controls && /tmp/test_controls
```
Expected: compile error — `BTN_SELECT` undeclared.

- [ ] **Step 3: Widen the bitmask + add buttons.** In `src/norns-controls.h`, replace the button enum (lines ~69-76):
```c
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
```

- [ ] **Step 4: Widen the storage + parser types.** In `controls_t`, change `uint8_t key_btn[3];` to:
```c
    uint16_t     key_btn[3];  /* button bitmask mapped to K1, K2, K3           */
```
Change `controls__parse_btn` return type `uint8_t`→`uint16_t` and add the new tokens (it currently handles a/b/x/y/l1/r1):
```c
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
```
Change `controls__parse_btn_list` return type `uint8_t`→`uint16_t`:
```c
static inline uint16_t controls__parse_btn_list(char *val) {
    uint16_t mask = 0;
    for (char *save = NULL, *t = strtok_r(val, ",", &save); t;
              t = strtok_r(NULL, ",", &save)) {
        mask |= controls__parse_btn(controls__trim(t));
    }
    return mask;
}
```

- [ ] **Step 5: Run to verify it passes.**
```bash
cc -Wall -Wextra tests/test_controls.c -o /tmp/test_controls && /tmp/test_controls
```
Expected: `PASS test_new_bindable_buttons` then `All tests passed.`

- [ ] **Step 6: Commit.**
```bash
git add src/norns-controls.h tests/test_controls.c
git commit -m "feat(controls): widen key bitmask to u16; add select/start/l3/r3 bindable, reserve guide"
```

---

## Task 4: System-button state machine (pure, TDD)

**Files:** Modify `src/norns-controls.h`; Test `tests/test_controls.c`.

- [ ] **Step 1: Write the failing test.**

```c
static void test_sysbtn(void) {
    const uint32_t HOLD = 800;
    sysbtn_t s = {0};
    /* quick tap → HOME */
    assert(sysbtn_guide(&s, 1, /*select_held*/0, /*now*/1000, HOLD) == SYS_NONE);
    assert(sysbtn_guide(&s, 0, 0, 1200, HOLD) == SYS_HOME);     /* up after 200ms */
    /* hold past threshold → QUIT, release after is inert */
    s = (sysbtn_t){0};
    assert(sysbtn_guide(&s, 1, 0, 0, HOLD) == SYS_NONE);
    assert(sysbtn_tick(&s, 500, HOLD) == SYS_NONE);             /* not yet */
    assert(sysbtn_tick(&s, 900, HOLD) == SYS_QUIT);             /* held 900ms */
    assert(sysbtn_tick(&s, 1000, HOLD) == SYS_NONE);            /* already consumed */
    assert(sysbtn_guide(&s, 0, 0, 1100, HOLD) == SYS_NONE);     /* no phantom HOME */
    /* Select pressed while GUIDE held → QUIT chord */
    s = (sysbtn_t){0};
    sysbtn_guide(&s, 1, 0, 0, HOLD);
    assert(sysbtn_select_down(&s) == SYS_QUIT);
    assert(sysbtn_guide(&s, 0, 0, 100, HOLD) == SYS_NONE);
    /* GUIDE pressed while Select already held → immediate QUIT */
    s = (sysbtn_t){0};
    assert(sysbtn_guide(&s, 1, /*select_held*/1, 0, HOLD) == SYS_QUIT);
    /* Select down with no GUIDE → NONE (Select stays a free button) */
    s = (sysbtn_t){0};
    assert(sysbtn_select_down(&s) == SYS_NONE);
    printf("  PASS test_sysbtn\n");
}
```

- [ ] **Step 2: Run to verify it fails.**
```bash
cc -Wall -Wextra tests/test_controls.c -o /tmp/test_controls && /tmp/test_controls
```
Expected: compile error — `sysbtn_t` undeclared.

- [ ] **Step 3: Implement the state machine.** Add to `src/norns-controls.h` (after the `controls_t` struct, with the other pure helpers):

```c
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
```

- [ ] **Step 4: Run to verify it passes.**
```bash
cc -Wall -Wextra tests/test_controls.c -o /tmp/test_controls && /tmp/test_controls
```
Expected: `PASS test_sysbtn` then `All tests passed.`

- [ ] **Step 5: Commit.**
```bash
git add src/norns-controls.h tests/test_controls.c
git commit -m "feat(controls): pure system-button state machine (Menu tap=home, hold/Select+Menu=quit)"
```

---

## Task 5: Wire the state machine into the host; free Select/Start; drop the carve-out

**Files:** Modify `src/norns-panicos.c`.
**Uses:** the confirmed button constant from Task 1 (default `SDL_CONTROLLER_BUTTON_GUIDE`) and the Home action from Task 2 (default K1 inject).

- [ ] **Step 1: Add state + the hold threshold.** In `norns_state_t` (replace the `select_held`/`select_was_combo` fields, `:89-90`):
```c
    sysbtn_t sysbtn;          /* Menu/FN reserved system-button state */
```
Near the other constants (top of file):
```c
#define GUIDE_HOLD_MS 800     /* hold Menu/FN this long to quit */
#define SYS_BUTTON    SDL_CONTROLLER_BUTTON_GUIDE   /* C1: swap to _START if GUIDE is swallowed */
```

- [ ] **Step 2: Add a system-action applier + handler.** Above `handle_button`:
```c
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
        return 1;   /* Select consumed ONLY as the Select+Menu chord */
    }
    return 0;
}
```

- [ ] **Step 3: Intercept at the top of `handle_button`, delete the carve-out, delete the old chords.** Replace the native-carve-out block (`:598-612`) with a clean reserved-then-native flow:
```c
    /* Reserved system button (Menu/FN) — handled before everything else and
     * never reaches the script or the key map. */
    if (handle_system_button(s, ev, pressed)) return;

    /* Native mode: the running script owns the raw pad (incl. Select/Start now). */
    if (s->controls.native_mode) {
        int pad = sdl_btn_to_pad(ev->button);
        if (pad >= 0) send_pad_button(s, pad, pressed);
        return;
    }
```
Then delete the entire old `case SDL_CONTROLLER_BUTTON_BACK:` and `case SDL_CONTROLLER_BUTTON_START:` blocks (`:638-657`) — Select/Start are no longer system chords. (Their `default: break;` now lets them flow to the key map via `bit`.)

- [ ] **Step 4: Add the per-frame hold tick.** In `poll_encoders`, right after the existing `poll_context(s);`/`poll_native_override(s);` lines and BEFORE the `if (c->native_mode) return;` guard:
```c
    apply_sys_action(s, sysbtn_tick(&s->sysbtn, SDL_GetTicks(), GUIDE_HOLD_MS));
```
(Placing it before the native guard means Menu-hold-to-quit works in native mode too.)

- [ ] **Step 5: Build + verify it compiles.**
```bash
cc -O2 -Wall -Wextra src/norns-panicos.c -o /tmp/np $(pkg-config --cflags --libs sdl2) -lpthread -lm && echo BUILD OK
```
Expected: `BUILD OK` (no warnings about the removed `select_held`/`select_was_combo`).

- [ ] **Step 6: Commit.**
```bash
git add src/norns-panicos.c
git commit -m "feat(host): Menu/FN reserved system button; free Select/Start; drop Phase 3 carve-out"
```

---

## Task 6: Map the freed buttons in `sdl_btn_bit` (so they bind as keys in emulation)

**Files:** Modify `src/norns-panicos.c`.

- [ ] **Step 1: Widen `sdl_btn_bit` + add the four buttons.** Replace `sdl_btn_bit` (`:444-454`):
```c
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
```
Then update its caller in `handle_button`: change `uint8_t bit = sdl_btn_bit(ev->button);` to `uint16_t bit = sdl_btn_bit(ev->button);`.

- [ ] **Step 2: Build + verify.**
```bash
cc -O2 -Wall -Wextra src/norns-panicos.c -o /tmp/np $(pkg-config --cflags --libs sdl2) -lpthread -lm && echo BUILD OK
```
Expected: `BUILD OK`.

- [ ] **Step 3: Confirm the binding parses** via the existing test harness (do NOT launch the host binary — it forks the norns stack). Add a quick assertion to `tests/test_controls.c`'s `test_new_bindable_buttons` if not already covered, or rely on Task 3's test which already proves `k1 = select` → `BTN_SELECT`. Real key-*fire* is verified on device in Task 8.

- [ ] **Step 4: Commit.**
```bash
git add src/norns-panicos.c
git commit -m "feat(host): bind select/start/l3/r3 as keys via sdl_btn_bit"
```

---

## Task 7: Documentation

**Files:** Modify `docs/controls.md`.

- [ ] **Step 1: Update the controls doc.** Revise the "Select = restart / Select+Start = quit" references to the new scheme: **Menu/FN tap = home, Menu/FN hold (or Select+Menu) = quit**; Select/Start/L3/R3 are now mappable (`select`, `start`, `l3`, `r3` as key tokens), default unbound; `guide`/`menu` are reserved and cannot be bound. Note this is the starting point; richer per-script/native mapping UX is Phase 2.

- [ ] **Step 2: Run the full test sweep.**
```bash
for t in test_controls test_controls_conf test_hid test_input test_monome_mext; do
  cc -Wall -Wextra tests/$t.c -o /tmp/$t && /tmp/$t | tail -1 | sed "s/^/$t: /" || exit 1
done
```
Expected: every line `All tests passed.`

- [ ] **Step 3: Commit.**
```bash
git add docs/controls.md
git commit -m "docs(controls): Menu/FN system button + freed select/start/l3/r3"
```

---

## Task 8: On-device end-to-end verification

**Files:** none (deploy + observe). Needs the host binary built (Task 5/6) deployed.

- [ ] **Step 1: Deploy the new host binary.** Cross-build per `device-deploy-workflow` and scp to `/storage/roms/ports/norns/bin/norns-panicos`, then relaunch from the handheld.
- [ ] **Step 2: Verify each behavior:**
  - **Menu/FN tap** → returns to the norns system menu.
  - **Menu/FN hold (~1s)** → quits to PortMaster.
  - **Select+Menu/FN** → quits to PortMaster.
  - **Select alone, Start alone** → do nothing by default (freed, unbound); bind `k3 = start` in `controls.conf` and confirm Start now fires K3.
  - **L3/R3** → bindable (`k1 = l3`) and fire.
- [ ] **Step 3:** If C1 took the fallback (Start as system button), verify the Start-tap=home / Start-hold=quit variant instead. Record results; no code commit unless a fix is needed.

---

## Self-review notes (for the implementer)

- **Spec coverage:** D1 reserved GUIDE intercept (Task 5 Step 3) · D2 freed-default-unbound (Task 3 test + `controls_defaults` untouched) · D3 quit=hold AND chord (Task 4 `sysbtn_tick` + `sysbtn_select_down`) · D4 home=K1 inject (Task 2, Task 5 `apply_sys_action`) · D5 bindable select/start/l3/r3 (Tasks 3, 6) · D6 carve-out deleted (Task 5 Step 3). C1=Task 1, C2=Task 2. §8 fallback wired via the `SYS_BUTTON` constant (Task 5 Step 1) + Task 1 Step 4.
- **Type consistency:** bitmask is `uint16_t` everywhere (`key_btn`, `controls__parse_btn`, `controls__parse_btn_list`, `sdl_btn_bit`, the `bit` local). `sysbtn_t`/`sys_action_t`/`sysbtn_guide`/`sysbtn_select_down`/`sysbtn_tick` names match between Task 4 and Task 5. `SYS_BUTTON`/`GUIDE_HOLD_MS` defined Task 5 Step 1, used Steps 2/4.
- **Verify-first:** Tasks 1–2 are on-device fact-finding that gate the one real uncertainty (GUIDE delivery + Home gesture). Tasks 3–4 are pure and independent of them; only Task 5's button constant + Home action depend on the C1/C2 results.
