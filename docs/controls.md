# Controls

Norns is built around six controls: three encoders (**E1/E2/E3**, rotary knobs)
and three keys (**K1/K2/K3**, buttons). Every script and the system menu use only
these. This page maps your handheld's gamepad onto them — and how to remap it.

## Default mapping (dual-stick devices — RG35XX Pro / H, etc.)

Each encoder is driven by **two inputs** — a D-pad/shoulder/trigger button-pair
*and* a stick — so you can use whichever you like:

| Encoder | Button-pair | Stick |
|---|---|---|
| **E1** | D-pad **left/right**, or **L1/R1** | — |
| **E2** | D-pad **up/down** | **left stick** |
| **E3** | **L2/R2** triggers | **right stick** |

| Key | norns | |
|---|---|---|
| **A** | **K3** | Confirm / enter. |
| **X** | **K2** | Back / cancel. |
| **Y** | **K1** | Home / back-to-menu / "shift". |
| **Select** (tap) | — | Restart norns. |
| **Select + Start** | — | Quit to PortMaster. |

All encoders **accelerate when held** — tap for one detent, hold to spin fast
(button-pairs ramp a bit snappier than the sticks; both tunable below).

Mnemonic: **A = enter, X = back, Y = home.**

## Worked example: load and play a script

1. Press **Y** (K1) to back out to the home menu.
2. **D-pad ←/→** (E1) moves the menu cursor; **A** (K3) enters **SELECT**.
3. **D-pad ←/→** scrolls the script list (hold to scroll fast); **A** loads it. (**X** = K2 backs out.)
4. On the play screen, read the on-screen hints (e.g. "E2: cutoff, K3: trig"):
   - **Left stick** turns E2, **right stick** turns E3.
   - **X / A** press K2 / K3; **Y** (or **B**) is K1.

---

## Remapping on the device — the Norns Controls app

The easiest way to remap is the **Norns Controls** tool — a separate entry in the
**Ports** menu, beside Norns. It edits the same `controls.conf` with a gamepad-
driven UI, so you never have to touch a text file or a computer.

It edits three scopes:

- **System (menu) controls** — the `[menu]` overlay (what applies in the norns
  system menu).
- **Global script controls** — the active scheme + keys, inherited by every
  script.
- **Per-script controls** — pick from your **installed scripts** (auto-detected),
  split into **Customised** and **Inherits defaults**. Editing a script that
  inherits creates an override for it; setting everything back to the defaults
  removes the override and it goes back to inheriting.

The editor lists **every input axis as its own row** — D-pad L/R and U/D, each
stick's L/R and U/D, and the L1·R1 / L2·R2 pairs — and you route each one to an
encoder (**E1/E2/E3** or off). Two axes of one stick can go to two different
encoders, and both axes to the *same* encoder (they sum). In an editor screen:

- **D-pad/stick** moves; **◄ ►** set the selected row's encoder.
- **Y** flips a stick **U/D** row's direction (the `⟲` invert marker).
- **A** on a key row opens the **button picker**; on an overlay row, **A** sets it
  back to **inherit**.
- **X** = **input detection**: press a physical control to jump to its row (or, on
  a key row, press a button to bind it).
- **Start** applies the screen's edits to memory; **B** backs out (prompting to
  discard or apply if you have unsaved changes). Nothing touches the file until
  you pick **Save to file** on the main menu; exiting unsaved prompts you.

**Inheritance is the safety net.** In the menu/script scopes, any row you don't
override is shown **dimmed** with a `↳ inherits` tag and carries the global
default's value; overridden rows are bright with a `● override` badge. So you
can't strand yourself by "unmapping" something — it just falls back to the parent.
(If you somehow override *everything* to off, a non-blocking ⚠ warns you.)
Acceleration/deadzone tunables aren't exposed and are preserved untouched on save.

**Copy controls between scripts:** in the per-script list, **X** → pick a source
script → its overrides are copied onto the selected script (which keeps
inheriting the base for everything the source didn't override).

### Factory defaults

The shipped baseline lives in a separate **read-only** file,
`<port>/norns/cfg/controls.defaults.conf`. The Norns port copies it to
`controls.conf` on first run, and the tool restores from it — but nothing in the
port ever writes to it (the editor only ever writes `controls.conf`):

- **Restore all defaults** (main menu, with confirmation) replaces the whole
  working set with the factory baseline. As always, nothing hits disk until you
  pick **Save to file**.

The defaults file ships `chmod 0444`. That stops casual edits but not `root`
(which the handheld runs as); the real guarantee is that no port code writes it.
For OS-hard immutability you can `chattr +i` it on the device's ext4 — at the
cost of needing `chattr -i` before any future update can replace it.

## Remapping by hand (the config file)

Controls are read from a text file at launch — no rebuild needed. On a
PortMaster install it lives at:

```
<port>/norns/cfg/controls.conf
```

The launcher seeds a commented default there on first run. Edit it and relaunch.
(The path can be overridden with the `NORNS_PANICOS_CONF` environment variable;
if no file is found, the built-in defaults above are used.) The Norns Controls
app above writes this same file, so the two are interchangeable.

### Named schemes

The file holds one or more **named layouts**, and a single line at the top picks
which is active. Switch that one line to roll between layouts — including back to
the shipped default — without retyping anything:

```ini
scheme = sticks      # ← change this to switch layout

# Global settings (apply to every scheme).
k1 = y, b
k2 = x
k3 = a
dpad_step      = 2      # encoder delta per detent (2 = one patched-matron detent)
stick_deadzone = 8192   # 0..32000; raise if a stick drifts at rest
stick_throttle = 6      # base stick rate: emit ~every N frames (raise to slow)
stick_invert   = 0      # 1 = flip every stick direction
accel       = 1         # hold-to-accelerate (0 = constant rate)
accel_delay = 15        # frames at base rate before ramping (~0.25s)
accel_ramp  = 45        # frames to reach top speed (~0.75s)
accel_max   = 6         # peak speed multiplier

[sticks]               # default — dual-stick devices
dpad_x   = 1           #   D-pad L/R   → E1
dpad_y   = 2           #   D-pad U/D   → E2
lstick_x = 2           #   left stick  L/R → E2
rstick_x = 3           #   right stick L/R → E3
shoulders = 1          #   L1/R1 → E1
triggers  = 3          #   L2/R2 → E3

[dpad]                 # D-pad only — no analog sticks needed
dpad_y    = 1          #   up/down    → E1
dpad_x    = 2          #   left/right → E2
shoulders = 3          #   L1 = −, R1 = +
```

- Lines **before the first `[section]`** are global and apply to whatever scheme
  is active. A scheme section only needs to list the inputs it routes.
- **Routing keys** (each = `1|2|3|none`): `dpad_x`, `dpad_y`, `lstick_x`,
  `lstick_y`, `rstick_x`, `rstick_y`, `shoulders`, `triggers`. The two U/D stick
  routes also take `lstick_y_invert` / `rstick_y_invert` (`0|1`). Both axes of one
  stick on the same encoder sum (up OR right = +).
- *(Legacy `e1|e2|e3 = lstick|rstick-y-inv|…` per-encoder lines still load via a
  compat shim, so old configs keep working.)*
- **Key buttons:** `a b x y l1 r1` (comma-separated, multiple allowed per key).
- To add your own layout, drop in a new `[my-layout]` section and set
  `scheme = my-layout`.

Unknown keys, values, or scheme names are ignored (and logged) rather than
breaking input, so a typo can't lock you out — you fall back to globals +
defaults. **Select = restart** and **Select + Start = quit** are fixed and
cannot be unbound. The startup log prints the active scheme:
`controls: <path> (scheme: sticks)`.

### Context overlays — per-menu and per-script tweaks

On top of the active scheme you can layer **context overlays** that apply only
while you're somewhere specific: the norns **system menu**, or a **named
script**. An overlay lists only what differs; everything else stays as the
scheme left it. The host watches the running context and re-resolves the mapping
the instant it changes.

```ini
[menu]                 # only while in the norns system menu
k2            = x, b   #   let B mirror K2 for one-handed paging
dpad_y_invert = 1      #   menu list scrolls down=+, so flip D-pad up/down → up = up

[script:koiboi2]       # only while the script "koiboi2" is running
dpad_y_invert = 1      #   this script wants its D-pad up/down inverted
```

- **`[menu]`** matches the system menu (script list, params, etc.).
- **`[script:NAME]`** matches while that script runs. `NAME` is the script's
  short name — its folder name under `dust/code/` (what the menu shows). If an
  overlay seems not to apply, the startup/transition log line
  `[norns-panicos] context: <name>` tells you the exact name to key on.
- **`dpad_y_invert = 1`** flips D-pad up/down for that context only. It ships on
  in `[menu]` (the menu scrolls the opposite way to D-pad up) and off
  everywhere else.
- Overlays can set any global/key tunable — `k1..k3`, `dpad_x/y`, `shoulders`,
  `triggers`, `accel_*`, etc. — not just `dpad_y_invert`.

### Example tweaks

- **Encoders feel backwards:** `stick_invert = 1` (all sticks), or flip one stick's
  U/D with `lstick_y_invert = 1` / `rstick_y_invert = 1`.
- **Stick drifts at rest:** raise `stick_deadzone` (try `12000`).
- **Use a stick's U/D instead of L/R:** route `lstick_y`/`rstick_y` and clear
  `lstick_x`/`rstick_x` (`= none`).
- **Both right-stick axes on two encoders (pixels-style):** `rstick_x = 2`,
  `rstick_y = 3`.
- **Bind a shoulder to a key:** `k1 = y, b, l1`.

### Stickless devices (RG36XX and other D-pad-only handhelds)

No analog sticks? Use the built-in **`dpad`** scheme — set `scheme = dpad`. It
drives all three encoders without sticks:

```ini
[dpad]
dpad_y    = 1    # up/down    → E1
dpad_x    = 2    # left/right → E2
shoulders = 3    # L1 = −, R1 = +
```

E3 lands on the L1/R1 shoulders (stepwise, not a smooth knob), but every encoder
has a home and there's no hold-to-select modality.

---

## Troubleshooting

- **"A stick does nothing."** Check `lstick_x`/`rstick_x` (or `…_y`) are routed
  to an encoder (`1|2|3`) and `stick_deadzone` isn't set absurdly high.
- **"The screen froze / no sound."** Tap **Select** to restart norns.
- **"How do I quit?"** **Select + Start**.
- **Confirm what loaded:** the log (`<port>/norns/logs/norns.log`) prints
  `controls: loaded <path>` or `controls: no file at <path>` at startup.

For the Push 2 / Ableton Move control surface, see [push2-interface.md](push2-interface.md).
