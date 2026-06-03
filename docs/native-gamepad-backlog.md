# Native gamepad — backlog & design directions

Companion to the Phase 3 spec/plan (`docs/superpowers/specs/2026-06-03-…` and
`docs/superpowers/plans/2026-06-03-…`). Captures stretch items and the
control-mapping UX direction that don't belong in the Phase 3 critical path.

---

## Stretch: dynamic control-reference renaming in pre-launch text

**Idea (user, 2026-06-03):** When a script's pre-launch description / spiel text
mentions controls by their norns names — `"E1"`, `"e1 "`, `"first encoder"`,
`"K2"`, `"hold K1"`, etc. — detect those references and rewrite them in-place to
show the **actual mapped gamepad input(s)** for the current scheme. So a script
that says *"E1: tempo, K3: start"* would display *"L/R or D-pad: tempo, A: start"*
on this handheld.

**Why:** closes the gap between what a script's docs assume (norns hardware
vocabulary) and what the player actually presses. Especially valuable for the
huge library of existing emulation-mode scripts.

**Sketch:**
- A substitution pass over the description string: regex/token-match the control
  vocabulary (`E1-3`, `K1-3`, `enc 1`, `encoder 1`, `first/second/third
  encoder`, `key 1`, `button 1`, `PARAM`, etc.) → look up the resolved mapping
  (`controls_t` for that context) → render the human label(s) of the input(s)
  bound to it.
- Needs a host→label function: given an encoder/key, list the inputs that drive
  it ("L/R or D-pad", "A", "L2/R2"). The reverse of the resolver.
- Lives wherever the description text is shown (see UX direction below).
- Edge cases: ambiguous matches ("key" in prose), scripts that already use
  gamepad terms, native-mode scripts (no E/K mapping — skip or relabel to raw).

**Status:** backlog / stretch. Depends on the control-mapping UX deciding *where*
description text is surfaced.

---

## Direction: per-script & system-wide control mapping (native era)

This replaces the stashed standalone "Norns Controls" port app with an
**in-norns** experience. Two regimes coexist:

- **Emulation scripts** (most existing norns scripts): mapping = which pad inputs
  drive `K1-3` / `E1-3`. Edited at **global** (all scripts) and **per-script**
  levels — the existing overlay engine (`[menu]`, `[script:NAME]`).
- **Native scripts** (gamepad-aware, e.g. `padtest`): the pad is raw; "mapping"
  is mostly the **native toggle** + optional settings (deadzone, escape).

**Proposed UX (to be brainstormed/spec'd in Phase 2):**

1. **System Controls** — a SYSTEM-menu entry (added by the Phase 2 mod) to edit:
   (a) the system-menu mapping, (b) the global script-default mapping, (c) the
   reserved escape (Menu/FN). Integrated into norns rather than a separate port.
2. **Per-script at the description screen** — when a script is highlighted and
   its description shows, overlay the **resolved control scheme** (with the
   dynamic renaming above) and offer: **[Play] / [Edit controls] / [Native
   on·off]**. "Edit controls" writes the `[script:NAME]` overlay. This unifies
   the user's two ideas — "a button to remap at launch" and "accept/edit controls
   from the description text" — into one screen.

**Open questions for the Phase 2 brainstorm:**
- Entry point for per-script editing: dedicated launch-button vs. the
  description-screen prompt vs. always via the System Controls menu. (Lean:
  description-screen prompt — it's where the player already pauses, and it's the
  natural home for the dynamic renaming.)
- Does "Edit controls" reuse the stashed `norns-controls-gui` editor logic
  (`controls-conf.h`), ported into a norns Lua menu? (See lessons below.)
- Native scripts: what, if anything, is editable beyond the on/off toggle?

---

## Lessons carried from the stashed control-remapping effort

(The emulation remapping effort + on-device GUI app — preserved on branch
`wip/control-remapping`. What we keep from it.)

- **The three-level model** — system / global-default / per-script — is the right
  mental model and is already the backbone of Phase 3's per-script native mode
  (`[script:NAME]` overlays). Carry it directly into the native mapping UX above.
- **Acceleration / button-press feel** (`accel_*`, tap-then-ramp) is what made the
  emulation feel good and motivated going gamepad-native. It is **emulation-only**
  — native HID is raw, no acceleration needed — but the *tuning instinct* (deadzone,
  throttle) reappears as native stick settings.
- **The on-device config model** (`controls-conf.h`: read/edit/save with conflict
  detection, defaults file, per-script override create/remove) is a reusable
  pattern for the native mapping editor — port its logic into the in-norns menu
  rather than a separate SDL port app.
- **Conflict flagging** ("one input drives at most one encoder") generalizes to
  the host-reserved-input rule Phase 1 needs.

---

## Phase pointers (build order 1 → 2 → 3)

- **Phase 1** — liberate Menu/FN (Menu=home, hold/Select+Menu=quit); generalize a
  host-reserved-input mechanism. Phase 3 host code has an INTERIM carve-out
  (Select/Start kept as escape) that Phase 1 supersedes.
- **Phase 2** — the mapping UX above + the norns mod packaging + the dynamic
  renaming stretch item.
- **Phase 3** — native HID path. Host integration done; matron patch
  (`patches/hid-virtual.patch`) awaits a from-source build to verify.
