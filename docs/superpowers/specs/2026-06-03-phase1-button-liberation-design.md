# Button Liberation — Design Spec (Phase 1)

**Date:** 2026-06-03
**Status:** Approved
**Repo:** norns-panicos
**Part of:** the gamepad-native arc (Phase 1 of 3). Build order 1→2→3; this is the
first to *build*. See `docs/native-gamepad-backlog.md` and the Phase 3 spec
(`2026-06-03-phase3-native-gamepad-api-design.md`, §8) which imposed this phase's
requirements.

---

## 1. Goal

Move quit/restart off the **Select** and **Start** buttons onto a dedicated
**Menu/FN** button, so Select, Start, and the two stick-clicks (**L3/R3**) return
to the normal mappable pool — and so native scripts gain a host-reserved escape
that they can never swallow. This is the smallest, lowest-risk phase and it
removes the interim Select/Start carve-out currently in the Phase 3 host code.

### Non-goals (Phase 2, not here)
- Rich per-script / system mapping **UX** (menus, description-screen editor).
- Changing the **default** control feel — defaults stay as the current emulation
  baseline (see §6).
- The norns **mod** packaging.

---

## 2. Confirmed hardware fact (the load-bearing unknown, now resolved)

A raw evdev capture on the device (`H700 Gamepad`, `/dev/input/event3`) confirmed
the physical button codes:

| Physical button | evdev code | SDL button |
|---|---|---|
| **Menu/FN** | **316 `BTN_MODE`** | **`GUIDE`** |
| Select | 314 `BTN_SELECT` | `BACK` |
| Start | 315 `BTN_START` | `START` |
| L3 (left stick click) | 317 `BTN_THUMBL` | `LEFTSTICK` |
| R3 (right stick click) | 318 `BTN_THUMBR` | `RIGHTSTICK` |

So a distinct reserved button (**Menu/FN → SDL `GUIDE`**) exists, and L3/R3 are
available. Two things the raw test did NOT prove — they become Phase 1's opening
verification tasks (§7, C1/C2): that SDL *delivers* GUIDE to the app, and that
`gptokeyb` (a co-consumer of the pad) doesn't intercept Menu/FN.

---

## 3. New control scheme

| Input | Action |
|---|---|
| **Menu/FN (GUIDE) — short tap** | **Home** — host returns norns to the system menu. In native mode this is *the* escape. |
| **Menu/FN — hold (~800 ms)** *or* **Select + Menu/FN** | **Quit** to PortMaster. |
| **Select, Start, L3, R3** | **Freed** — ordinary mappable inputs; default unbound. |

Replaces today's hardcoded `Select`=restart / `Select+Start`=quit
(`src/norns-panicos.c:638-657`).

Note: **restart** norns is dropped as a dedicated chord — it was a workaround for
freezes, and Home (return to menu) + Quit cover the real needs. (If wanted later,
it can be a second hold tier or a `[menu]`-only chord — out of scope here.)

---

## 4. Key decisions (and what was rejected)

| # | Decision | Rejected | Why |
|---|---|---|---|
| D1 | **GUIDE is a host-reserved input**, intercepted at the very top of `handle_button` before any key/encoder/native routing. | Per-context handling. | This *is* the generalized reserved-input mechanism Phase 3 asked for; one button now, structured so more can be added. |
| D2 | **Freed buttons default unbound** (like `B` today). | Default Select→K1 alias. | They enter the mappable pool; config/scripts decide. Presuming intent is the wrong default. |
| D3 | **Quit = GUIDE-hold AND Select+GUIDE** (both). | Single gesture. | Redundancy is good for a destructive action; both are deliberate. Hold = one-handed; chord = explicit. |
| D4 | **Home = host injects a K1 tap** to the input FIFO → norns pops to menu. | A dedicated menu keystroke. | The port's own docs define K1 as back-to-home; reuses existing key injection. Verified in C2. |
| D5 | **Make Select/Start/L3/R3 bindable** (extend the key-button vocabulary), default unbound. | Leave them inert in emulation. | "Liberate" means usable, not just un-reserved. Native mode reaches them already (sdl_btn_to_pad); emulation needs the binding capability. |
| D6 | **Delete the Phase 3 interim carve-out** (Select/Start excluded from native routing). | Keep it. | With GUIDE as the escape, native scripts no longer need Select/Start reserved — they become full native buttons. |

---

## 5. Components & code touchpoints (all in `src/norns-panicos.c` + `norns-controls.h`)

- **Reserved-input state machine** (`handle_button`, replacing the `BACK`/`START`
  cases): tracks GUIDE press time for tap-vs-hold, watches Select+GUIDE, and
  fires Home (inject K1) / Quit (`s->running = 0`). GUIDE intercepted before the
  native block and the key/encoder routing.
- **Hold timing**: GUIDE-down records a timestamp; a check in `poll_encoders`
  (per-frame) fires Quit once held past the threshold; GUIDE-up before the
  threshold fires Home. (Mirrors the existing per-frame model.)
- **Free Select/Start**: remove them from the system-chord logic; remove the
  Phase 3 carve-out (`handle_button` native block) so they route to HID natively.
- **Bindable vocabulary** (`norns-controls.h`): extend the key-button bitmask +
  `controls__parse_btn` to recognize `select`, `start`, `l3`, `r3`; map them in
  `sdl_btn_bit`. Default mapping leaves them unbound.
- **Reserved-from-binding guard**: GUIDE can never be bound to a key (it's
  host-owned) — `controls__parse_btn` does not accept it.

**Heads-up (concurrency):** this edits `norns-controls.h`, which the stashed
`wip/control-remapping` branch also refactored. Resuming that branch later will
need a merge. Acceptable given the pivot.

---

## 6. Defaults & evolution

Per the approved direction: **Phase 1 changes nothing about the default control
feel.** The built-in defaults (`controls_defaults` / `controls.defaults.conf`) —
Y/X/A → K1/K2/K3, D-pad → E1, L-stick → E2, R-stick → E3, accel tunables — are
preserved exactly. Phase 1 only (a) moves quit/restart to Menu/FN and (b) makes
Select/Start/L3/R3 *available* (default unbound).

This is explicitly a starting point. The "mature state" — gamepad-native default
schemes, per-script control editing, the description-screen mapping prompt — is
Phase 2 (`docs/native-gamepad-backlog.md`).

---

## 7. Implementation checkpoints (Phase 1's FIRST tasks — verify, don't guess)

- **C1 — SDL delivers GUIDE + gptokeyb coexistence.** A ~20-line standalone SDL
  GameController probe (build, run on device, press Menu/FN) confirms
  `SDL_CONTROLLER_BUTTON_GUIDE` arrives. Simultaneously determine whether
  `gptokeyb` (running `gptokeyb -1 norns-panicos`) claims Menu/FN for PortMaster's
  own exit — if so, the fix is a `.gptk`/`Norns.sh` adjustment to release it.
  **This is the one real risk; resolve it before building the state machine.**
- **C2 — Home mechanism.** Confirm injecting a K1 tap returns to the menu from a
  running script. If K1 alone is insufficient, identify the correct gesture.

Both are cheap, both replace device-guessing with one observed fact each.

---

## 8. Error handling & edge cases

- **GUIDE not delivered (C1 fails):** without a reserved Menu/FN we can't free
  *both* Select and Start. Fallback: reserve **Start alone** for the system state
  machine (Start-tap = Home, Start-hold = Quit), which still frees **Select, L3,
  and R3**. Documented so the plan can branch without a redesign.
- **gptokeyb fires its own exit on Menu/FN:** acceptable as long as it means
  "quit" (aligns with our Quit). If it's a *different* action, suppress via
  `.gptk`. (C1.)
- **Held GUIDE + app restart:** Home/Quit are derived from live button state, not
  latched across a norns restart; no stuck state.
- **Accidental Quit:** the hold threshold (~800 ms) plus the explicit chord guard
  against fat-fingering; tune in testing.

---

## 9. Testing

- **Pure logic** (tap-vs-hold classifier, reserved-input decision, the extended
  `controls__parse_btn` for select/start/l3/r3) → unit tests in the standalone C
  harness (`tests/`, same pattern as `test_controls.c`).
- **C1/C2** → on-device observation via the SDL probe + a K1-injection check.
- **End-to-end** → on device: Menu-tap returns home, Menu-hold/Select+Menu quits,
  Select/Start bindable in a `[script:NAME]` overlay, native script gets
  Select/Start as HID.

---

## 10. What this unblocks

- Removes the Phase 3 host carve-out → native scripts get the full pad incl.
  Select/Start.
- Establishes the host-reserved-input mechanism Phases 2/3 rely on.
- Frees four inputs (Select, Start, L3, R3) into the mappable pool for the Phase 2
  mapping UX to expose.
