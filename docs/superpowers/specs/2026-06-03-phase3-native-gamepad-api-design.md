# Native Gamepad API — Design Spec (Phase 3)

**Date:** 2026-06-03
**Status:** Approved
**Repo:** norns-panicos
**Part of:** the "gamepad-native handheld" arc — 3 sequenced sub-projects (this is Phase 3, specced first to shape the earlier two).

---

## 0. The arc this belongs to

The broader ambition: turn the RG35XX-class handheld into a gamepad-native norns,
exposing the full pad (A/B/X/Y, L1/L2/R1/R2, D-pad, two clickable analog sticks,
plus freed Select/Start) to scripts — beyond the stock K1/K2/K3 + E1/E2/E3.

Decomposed into three independently-shippable sub-projects:

- **Phase 1 — Liberate the system buttons.** Move quit/restart off Select/Start onto
  a dedicated Menu/FN button so Select, Start, and the stick-clicks (L3/R3) return to
  the mappable pool. Config/C only, smallest blast radius.
- **Phase 2 — Enrich the remap engine + menu gamepad mode.** Grow the config/overlay
  engine to cover every input, richer per-`[script:NAME]` schemes, a gamepad-first
  system menu, and the norns **mod** that packages the "enable → whole system goes
  gamepad" UX. Zero matron changes.
- **Phase 3 — Native gamepad API (THIS SPEC).** Deliver the raw pad to Lua via norns's
  native **HID** path, gated by a per-script native mode. The platform leap; the only
  phase requiring matron patches.

**Build order:** 1 → 2 → 3 (each de-risks the next). **Spec order:** 3 first, so the
requirements Phase 3 imposes on 1 and 2 are discovered by design, not guessed
(see §8).

---

## 1. Goal

Let a norns script consume the full gamepad natively — all buttons and both analog
sticks — through norns's **existing HID device API** (`hid.connect()` →
`device.event(type, code, value)`), with an optional ergonomic wrapper library on top.
Existing community gamepad scripts that already speak `hid` work unmodified. New scripts
get a friendly surface. The system stays usable (menu, escape, quit) the whole time.

### Non-goals (explicitly out of scope for Phase 3)

- The gamepad-first **system menu** layout (Phase 2).
- The norns **mod** packaging / mods-menu entry (Phase 2 — see §8; no mod in Phase 3).
- Editing `mode = native` from the on-device Norns Controls app (Phase 2).
- Phase 1's button-liberation itself (Phase 3 only *imposes requirements* on it).

---

## 2. Key decisions (and what was rejected)

| # | Decision | Rejected alternatives | Why |
|---|---|---|---|
| D1 | **Hybrid API:** stock `hid` as substrate + optional ergonomic `pad` wrapper lib. | Stock-hid-only (ergonomics); bespoke-module-only (no ecosystem compat). | Legacy gamepad scripts "just work"; new scripts get sugar; wrapper is pure Lua over the real device. |
| D2 | **Per-script native mode** suppresses K/E emulation for that script; one host-reserved escape guarantees return to menu. | Additive (both streams always → phantom input, no pure game button); global mode switch (couples to Phase 2 menu, most invasive); runtime-only opt-in. | Reuses the existing per-`[script:NAME]` overlay engine; no double-meaning on buttons; clean escape. |
| D3 | **Transport B — HID-over-FIFO + synthetic device.** New FIFO `/tmp/norns-hid-1`; matron patch fabricates one stable in-memory `libevdev` device and streams `EVENT_HID_EVENT`. | A: matron reads real `/dev/input/eventN` (breaks no-hardware invariant, per-device identity varies, risks resurrecting removed init). C: uinput mirror + re-enabled scanner (two fragile parts; scanner may grab the real device). | Keeps matron hardware-free, mode logic in C, and gives every handheld an **identical synthetic pad** → script portability. C is the documented fallback if the spike fails. |
| D4 | **In-memory libevdev trick:** `libevdev_new()` + `libevdev_enable_event_code()` (no backing fd); register via matron's stock add path; replace only the read source. | Hand-fake the capability serialization. | matron's weaver/Lua hid path runs 100% unmodified against a genuine (hardware-less) libevdev object — keeps the patch ~150–200 lines, no weaver/Lua changes. |
| D5 | **Menu/FN = home (short); quit = Menu-hold or Select+Menu.** | Keep Phase 1's Menu=quit + reserve a chord for home; context-sensitive Menu; reserve Select alone for home. | "Menu gets me out of whatever I'm in" is one learnable, context-free rule. Revises Phase 1 (see §8). |
| D6 | **No mod in Phase 3.** Host always exposes the synthetic device (harmless to legacy scripts); availability is detected by device name (`pad.available`); native mode is overlay-driven. | Mod as master gate (host↔mod coupling, dead path until enabled); mod owns menu-shift (that's Phase 2). | The mod has no real job in Phase 3; it earns its keep in Phase 2 (menu gamepad-shift). Avoid coupling for its own sake. |
| D7 | **Device always advertised, events gated.** Synthetic pad is `hid_add`-ed at matron startup and stays present; host emits HID events only in native context. | Dynamic add/remove per native script. | Simpler patch (no live-remove), and a non-native script calling `hid.connect()` just sees a silent pad — harmless. *Ramification:* legacy hid script outside native mode sees a connected-but-quiet device. |
| D8 | **Two ways to enter native mode:** config overlay `mode = native` **and** optional runtime `pad.grab()` (wrapper signals host via `/tmp/norns-native`). | Config-only (frictionless authoring lost); runtime-only (can't mark someone else's existing script). | Config marks existing scripts once; `pad.grab()` makes scripts *you author* native with zero config. *Ramification:* two sources of truth — host resolves runtime-grab as an override on top of config. |

---

## 3. Architecture

One new data path, parallel to the existing e/k FIFO. The control resolver already
knows the running context (`/tmp/norns-context`) and the per-`[script:NAME]` overlay;
Phase 3 adds a single branch: native context routes the pad to the HID FIFO.

```
                 ┌─────────────── norns-panicos (host, C) ───────────────┐
  SDL gamepad ──▶│ read pad ──▶ control resolver (context + overlays)     │
                 │                 │                                       │
                 │      ┌──────────┴───────────┐                          │
                 │  emulation?              native?                        │
                 │  type0/1 frames        evdev frames                     │
                 └──────┼───────────────────────┼──────────────────────────┘
                        ▼                        ▼
                 /tmp/norns-input-1       /tmp/norns-hid-1   (NEW FIFO)
                        │                        │
                 ┌──────┴────────────────────────┴──────── matron (patched) ┐
                 │  EVENT_KEY/ENC          synthetic in-memory libevdev dev   │
                 │                         → EVENT_HID_ADD + EVENT_HID_EVENT  │
                 └──────┬────────────────────────┬───────────────────────────┘
                        ▼                         ▼
                  key()/enc()              hid.connect() → device.event
                                                  ▲
                                           pad.lua wrapper (sugar)
```

---

## 4. Components

| Component | Lang | Responsibility | Risk |
|---|---|---|---|
| **HID-FIFO writer** | C (host) | Map SDL button/axis → evdev `(type, code, value)` frames on `/tmp/norns-hid-1`. Fixed lookup table. `O_NONBLOCK`, drop on `EAGAIN`. | Low |
| **Native-mode router** | C (host) | Read overlay `mode` (+ `/tmp/norns-native` runtime override); in native context suppress e/k emit, enable HID emit, reserve Menu/FN. | Low (extends resolver) |
| **Synthetic HID patch** | C (matron) | `libevdev_new()` + enable buttons/axes (no fd); register via stock add path → `EVENT_HID_ADD`; FIFO-reader thread → `EVENT_HID_EVENT`. Gated on `NORNS_HID_FIFO`. | **Medium — spike target** |
| **`pad` wrapper lib** | Lua | `require`-able. Semantic names (`pad.a`, `pad.lstick.x`), `pad.on('a', fn)`, `pad.event`, `pad.available`, `pad.grab()/release()`. Sugar over the stock hid device, detected by name. | Low |
| **Escape interception** | C (host) | Menu short → synthesize K1-home tap on the e/k FIFO; Menu-hold / Select+Menu → quit. Reserved input never reaches the script. | Low |

### 4.1 Synthetic device capabilities (fixed table)

- **`EV_KEY`:** `BTN_SOUTH` (A), `BTN_EAST` (B), `BTN_WEST` (X), `BTN_NORTH` (Y),
  `BTN_TL` (L1), `BTN_TR` (R1), `BTN_TL2` (L2), `BTN_TR2` (R2), `BTN_SELECT`, `BTN_START`,
  `BTN_THUMBL` (L3), `BTN_THUMBR` (R3), `BTN_DPAD_UP/DOWN/LEFT/RIGHT`.
- **`EV_ABS`:** `ABS_X/ABS_Y` (left stick), `ABS_RX/ABS_RY` (right stick),
  `ABS_HAT0X/ABS_HAT0Y` (D-pad as hat, optional — D-pad may be reported as buttons).
- L2/R2 reported as buttons by default; analog `ABS_Z/ABS_RZ` exposed only if the SDL
  device reports analog triggers.
- The synthetic device advertises a **stable name** (e.g. `"norns-panicos gamepad"`) and
  fixed vid/pid so `pad.available` detection is identical across handhelds.

---

## 5. Native-mode lifecycle (data flow)

1. A script whose overlay has `mode = native` (or which calls `pad.grab()`) loads.
   `/tmp/norns-context` → that script's shortname; resolver enters native mode.
2. Host **stops** emitting e/k frames for pad buttons and **starts** emitting evdev
   frames on the HID FIFO. The always-advertised synthetic matron device now streams.
3. The script's `hid.connect()` / `pad` wrapper receives raw buttons + analog sticks.
   Y is now `BTN_NORTH`, not K1.
4. **Menu/FN never reaches the script.** Host intercepts: short = synthesize a K1 tap on
   the e/k FIFO → matron pops to the system menu; context flips to `menu`; native mode
   exits; e/k emulation resumes. Hold or Select+Menu = quit norns.

Native mode is **derived from context, not latched** — so a crash/restart that returns to
the menu cannot leave a stuck-native state.

---

## 6. Error handling & edge cases

- **HID FIFO has no reader** (matron unpatched/old): writer is `O_NONBLOCK`, drops on
  `EAGAIN`, never blocks the host loop. Pad falls back to active emulation.
- **Spike fails / `libevdev_get_fd` coupling:** if the in-memory device can't register
  cleanly, fall back to Transport C (uinput) — same host code, different device source.
- **Script crash in native mode:** existing matron-restart path returns to menu; context
  → `menu`; emulation resumes; no stuck state (native is derived, §5).
- **Unknown/extra SDL buttons:** map to nothing (dropped) — never to a wrong evdev code.
- **Runtime/​config conflict:** `pad.grab()` overrides config; `pad.release()` (or script
  exit / context change) reverts to config-derived state.

---

## 7. Testing

- **Host SDL→evdev mapping** and **native-mode routing decision** are pure functions →
  unit tests in the existing standalone C harness (`tests/`, same pattern as
  `test_controls.c` / `test_controls_conf.c`).
- **Matron patch** → proven by the opening spike (register device + emit one event,
  observed in `maiden`/logs), then on-device integration.
- **`pad` wrapper** → tested against a mock hid device table in Lua.
- **Escape interception** → unit-test the chord/hold state machine host-side.

---

## 8. Requirements this phase imposes on Phases 1 & 2

Discovered by speccing Phase 3 first:

- **Phase 1 revision (D5):** Menu/FN = **home** (short press); **quit** = Menu-hold or
  Select+Menu. (Supersedes the earlier "Menu = quit".)
- **Phase 1 requirement:** the resolver must support a **host-reserved input** that never
  reaches anything downstream — generalize the existing fixed system-chord handling so an
  input can be claimed by the host in a given context.
- **Phase 2 requirement:** the overlay schema **and** the Norns Controls editor must carry
  `mode = native` per script; the deferred **mod** lands in Phase 2 as the menu
  gamepad-shift package.

---

## 9. Implementation plan opener (mandatory spike)

The plan's **first deliverable is a throwaway spike**: read matron's real
`device_hid.c` / `device_monitor.c` / `weaver.c` hid path, then prove that an in-memory
`libevdev` device (no fd) can (a) register via the stock add path and surface in
`hid.vports`, and (b) emit one `EVENT_HID_EVENT` observable from a Lua `device.event`
handler. Only after the spike passes do we commit to the full Transport-B patch; if it
fails, pivot to Transport C (uinput) with the same host-side code.

matron is **not** in this repo (cloned in the Docker build), so all line estimates here
are from knowledge of the norns codebase, not this tree — the spike confirms them.
