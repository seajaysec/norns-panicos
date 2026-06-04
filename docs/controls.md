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
| **Menu/FN** (tap) | — | Home — return to the norns system menu. |
| **Menu/FN** (hold) or **Select + Menu/FN** | — | Quit to PortMaster. |
| **Select**, **Start**, **L3/R3** | (mappable) | Freed — bind them in the config (default unbound). |

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

## Remapping (the config file)

Controls are read from a text file at launch — no rebuild needed. On a
PortMaster install it lives at:

```
<port>/norns/cfg/controls.conf
```

The launcher seeds a commented default there on first run. Edit it and relaunch.
(The path can be overridden with the `NORNS_PANICOS_CONF` environment variable;
if no file is found, the built-in defaults above are used.)

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
e1 = dpad-x            #   D-pad left/right (up/down unused)
e2 = lstick
e3 = rstick

[dpad]                 # D-pad only — no analog sticks needed
e1 = dpad-y            #   up/down    → E1
e2 = dpad-x            #   left/right → E2
e3 = shoulders         #   L1 = −, R1 = +
```

- Lines **before the first `[section]`** are global and apply to whatever scheme
  is active. A scheme section only needs to list its encoders.
- **Encoder sources:** `dpad`, `dpad-x`, `dpad-y`, `lstick`, `lstick-y`,
  `rstick`, `rstick-y`, `shoulders`, `none`.
- **Key buttons:** `a b x y l1 r1 select start l3 r3` (comma-separated, multiple
  allowed per key). `l3`/`r3` are the stick clicks. `guide`/`menu` (Menu/FN) is
  **host-reserved** and cannot be bound.
- To add your own layout, drop in a new `[my-layout]` section and set
  `scheme = my-layout`.

Unknown keys, values, or scheme names are ignored (and logged) rather than
breaking input, so a typo can't lock you out — you fall back to globals +
defaults. **Menu/FN = home** (tap) and **Menu/FN-hold / Select + Menu/FN = quit**
are host-reserved and cannot be rebound; everything else — including Select,
Start, and the stick clicks — is yours to map. The startup log prints the active scheme:
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

- **Encoders feel backwards:** `stick_invert = 1`.
- **Stick drifts at rest:** raise `stick_deadzone` (try `12000`).
- **Use vertical sticks instead of horizontal:** `e2 = lstick-y`, `e3 = rstick-y`.
- **Put E2 on the right stick, E3 on the left:** `e2 = rstick`, `e3 = lstick`.
- **Bind a shoulder to a key:** `k1 = y, b, l1`.

### Stickless devices (RG36XX and other D-pad-only handhelds)

No analog sticks? Use the built-in **`dpad`** scheme — set `scheme = dpad`. It
drives all three encoders without sticks:

```ini
[dpad]
e1 = dpad-y      # up/down    → E1
e2 = dpad-x      # left/right → E2
e3 = shoulders   # L1 = −, R1 = +
```

E3 lands on the L1/R1 shoulders (stepwise, not a smooth knob), but every encoder
has a home and there's no hold-to-select modality.

---

## Troubleshooting

- **"A stick does nothing."** Check `e2`/`e3` point at `lstick`/`rstick`, and
  that `stick_deadzone` isn't set absurdly high.
- **"The screen froze / no sound."** Tap **Menu/FN** to return to the menu;
  reload the script. (If norns itself is wedged, **hold Menu/FN** to quit and
  relaunch.)
- **"How do I quit?"** **Hold Menu/FN**, or **Select + Menu/FN**.
- **Confirm what loaded:** the log (`<port>/norns/logs/norns.log`) prints
  `controls: loaded <path>` or `controls: no file at <path>` at startup.

For the Push 2 / Ableton Move control surface, see [push2-interface.md](push2-interface.md).
