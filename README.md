# norns-panicos

[monome norns](https://monome.org/norns/) — the sound computer — ported to
PanicOS / PortMaster ARM handhelds (Anbernic RG35XX/RG36XX class devices, and
the Ableton Move via Push 2). A native SDL2 host (`src/norns-panicos.c`) runs the
real norns stack (matron, crone, sclang, maiden) and maps the device's gamepad
to the three encoders and three keys norns is built around.

## Install

Grab `norns-panicos.zip` (from a [release](../../releases), or the
`norns-panicos-package` artifact of a green
[build-panicos run](../../actions)) — the zip is fully self-contained, no
on-device downloads.

1. Copy the zip into `roms/ports/PortMaster/autoinstall/` on the device's SD
   card (or `scp` it to `/storage/roms/ports/PortMaster/autoinstall/`).
2. Launch **PortMaster** on the device — it installs the zip automatically.
3. **Norns** now appears in the Ports list. First launch takes a little longer
   (it seeds config, patches audio latency, and may restart the audio stack once).

Manual alternative: unzip directly into `roms/ports/` so `Norns.sh`,
`control.txt`, and `norns/` sit at that level.

To build the package yourself: `./scripts/build-panicos.sh` (Docker, builds
norns from source — fastest on an arm64 host), or trigger the
`build-panicos` GitHub Actions workflow and download the artifact.

## Controls

Mapping is fully configurable in `<port>/norns/cfg/controls.conf` (no rebuild) —
**see [docs/controls.md](docs/controls.md)**. Highlights of the model:

- **Named layouts** — a `scheme = <name>` line at the top selects one (ships with
  `sticks` and `dpad`); add your own `[section]` and point `scheme` at it.
- **Multi-input encoders** — each encoder can be driven by an analog stick *and*
  a button-pair at once. The D-pad (L/R and U/D), L1/R1, and L2/R2 are all
  −/+ pairs that map to any encoder, so there's no hold-to-select modality.
- **Hold-to-accelerate** — encoders ramp from one detent per tap to a fast spin
  the longer an input is held (tunable delay/ramp/peak, separate for sticks).
- **Context overlays** — `[menu]` and `[script:NAME]` blocks layer on top of the
  active scheme, applied the instant you enter that context. The menu flips the
  D-pad to match its scroll direction and lets B page like K2; per-script blocks
  remap anything for one script (e.g. `[script:pixels]` puts both sticks to work).

Defaults on dual-stick devices (RG35XX Pro / H, etc.):

- **E1** = D-pad L/R + L1/R1 · **E2** = D-pad U/D + left stick · **E3** = L2/R2 + right stick
- **K1** = Y · **K2** = X (+ B in the menu) · **K3** = A
- **Menu/FN** = home (tap) · **Menu/FN (hold)** or **Select + Menu/FN** = quit to PortMaster
- **Select**, **Start**, and the stick clicks (**L3/R3**) are freed — map them in `controls.conf`

For the Push 2 / Ableton Move control surface, see
[docs/push2-interface.md](docs/push2-interface.md).

## USB MIDI & monome grid

USB MIDI controllers work out of the box (patched matron bridges them to the
virtual norns MIDI ports). A real **monome grid** is driven over its CDC-ACM
serial port via a hand-rolled `mext` bridge (`src/norns-monome-bridge.c`, no
libmonome/libusb dependency) that feeds matron's virtual-grid FIFO. Enable it
from the PortMaster **Tools** menu (`Grid-On.sh` / `Grid-Off.sh`).

> The grid can take up to ~a minute after boot to light up — this matches stock
> norns behaviour (it waits on `serialosc`/device enumeration), not a port bug.

## Editing on device — ingenue

PanicOS bundles [**ingenue**](https://github.com/seajaysec/ingenue), a modern,
responsive web editor for norns, as a first-class experience. It starts
automatically on **`http://<device-ip>:7777/`**, running alongside maiden
(`:5000`) — open it from any phone, tablet, or laptop on the same network to
browse, edit, install, and manage scripts with a touch-friendly UI. No setup:
it's installed into `dust/code/ingenue` and launched by the port.

## Audio

The launcher pins the codec to **48 kHz** to match norns' engine rate
(eliminating the resample crackle some boards exhibit) and bakes a low-latency
config into startup. The from-source build also guarantees the crone
ADC-optional patch so capture-less boards still get output.

PanicOS also ships **aarch64-compiled SuperCollider UGen plugins** (PortedPlugins,
mi-UGens, f0plugins, and more, from
[seajaysec/sc-plugins-arm64](https://github.com/seajaysec/sc-plugins-arm64)). On a
64-bit port these are otherwise missing — only the `.sc` classes ride along with
scripts, so `scsynth` loads the engine class but silently rejects the absent/
wrong-arch binaries and the script plays **no sound**. Bundling the `.so` (into a
binary-only `Extensions/ingenue-ugens/` dir) makes those engines audible out of
the box; ingenue can also re-heal them at runtime if an OS update wipes them.

## Layout

- `src/` — the SDL2 host, Push 2 display driver, and input bridge
- `patches/` — matron/crone source patches for the headless handheld build
- `scripts/` — Docker-based build of norns; bundles ingenue + aarch64 SC UGens
- `ports/portmaster/` — PortMaster port metadata and launch script
- `docs/` — navigation, interface, and design docs
