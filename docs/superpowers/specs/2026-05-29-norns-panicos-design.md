# norns-panicos — Design Spec

**Date:** 2026-05-29
**Status:** Approved
**Repo:** norns-portmaster (project name: norns-panicos)

---

## 1. Goal

Port Monome Norns to ARM64 Portmaster handhelds (PanicOS / Knulli / Rocknix) as a
self-contained PortMaster package. The device has a full-color display capable of
real greyscale, native JACK/PipeWire audio, and JACK/ALSA MIDI — a substantially
simpler target than Ableton Move because there is no audio bridging, no chroot, and
no proprietary plugin host to integrate with.

The existing norns patches (screen-fifo, input-fifo, device_monome, device_midi) from
`schwung-norns` carry over unchanged. Only the host side changes.

---

## 2. Architecture

The entire Schwung-specific layer (`dsp.so`, `ui.js`, `pw-helper`) is replaced by a
single C binary `norns-panicos` and a PortMaster launch script.

```
Portmaster Handheld (PanicOS / Knulli / Rocknix)
┌──────────────────────────────────────────────────────┐
│  norns-panicos  (SDL2 C binary)                       │
│  ├─ SDL2 GameController → input FIFO                  │
│  ├─ screen FIFO → SDL2 texture (true greyscale)       │
│  ├─ child process manager (matron / crone / sclang)   │
│  └─ grid FIFO → pad LED display (optional visual)     │
│               ↕ FIFOs  /tmp/norns-*                   │
│  ┌──────────────────────────────────────────────┐     │
│  │  matron  (Lua VM + Cairo, patched)            │     │
│  │    ↕ OSC  127.0.0.1                           │     │
│  │  crone  ────────────────────► JACK/PipeWire  │     │
│  │  sclang / scsynth ──────────► JACK/PipeWire  │     │
│  │  norns-input-bridge  (JACK MIDI → input FIFO) │     │
│  │  maiden  (web IDE, port 5000)                 │     │
│  └──────────────────────────────────────────────┘     │
└──────────────────────────────────────────────────────┘
```

### Key differences from the Move (schwung-norns) port

| Concern | Move | norns-panicos |
|---|---|---|
| Host runtime | Schwung DSP plugin (dsp.so + ui.js) | Standalone SDL2 binary |
| Audio bridging | JACK client in dsp.so, SHM rings | None — crone connects natively |
| Chroot | Debian chroot + bind mounts + setuid helper | No chroot — native OS |
| JACK server | RNBO Takeover jackd | Device's JACK/PipeWire |
| Display | 1-bit 128×64 OLED + dithering | Full-color, true greyscale |
| Input | Move MIDI CC from knobs/pads | SDL2 GameController API |
| MIDI (external) | XMos SPI bridge | Native ALSA/JACK MIDI |

---

## 3. norns-panicos Binary

One C binary handles all host responsibilities.

### 3.1 Screen rendering

- Opens `/tmp/norns-screen-1` FIFO (`O_RDWR | O_NONBLOCK`)
- Reads 4096-byte frames (128×64 pixels, 4-bit packed: high nybble = left pixel)
- Unpacks each nybble to 8-bit grey: `gray = nybble * 17`  (0→0, 15→255)
- Writes to an `SDL_TEXTUREACCESS_STREAMING` texture (`SDL_PIXELFORMAT_RGB24`)
- `SDL_RenderSetLogicalSize(renderer, 128, 64)` — SDL handles integer upscaling to
  fill the window at whatever resolution the device reports
- Target update rate: 60 Hz (matches matron's screen thread)
- No dithering — the display renders actual greyscale

### 3.2 Gamepad input

Uses `SDL_CONTROLLER_*` events. Writes 4-byte frames to `/tmp/norns-input-1`:

```
[type:1][id:1][val_lo:1][val_hi:1]
type 0 = encoder delta   (id 0-2 = E1-E3, value = signed int16)
type 1 = key state       (id 0-2 = K1-K3, value 0=up 1=down)
```

Same binary protocol as the Move's `norns-input-bridge` — matron's FIFO input
driver (`gpio.c` patch) is unchanged.

### 3.3 Process management

- Fork/exec `matron`, `crone`, `sclang` in sequence with the same startup delays
  as `start-norns.sh`
- `waitpid(WNOHANG)` each main loop tick; restart crashed processes
- `norns-input-bridge` forked as an additional child (handles external JACK MIDI
  devices → input FIFO)
- `maiden` forked last (web IDE, port 5000)
- On clean exit: `SIGTERM` all children

### 3.4 Audio

No audio code in `norns-panicos`. Crone registers as a JACK client and connects to
`system:playback_*` / `system:capture_*` directly. The host binary sets:

```sh
export XDG_RUNTIME_DIR=...   # so JACK/PipeWire socket is found
export JACK_NO_AUDIO_RESERVATION=1
```

---

## 4. Input Scheme

| Button | Norns action |
|---|---|
| Y | K1 |
| X | K2 |
| A | K3 |
| B | K1 (back alias) |
| Hold L1 | Select encoder E1 (default) |
| Hold L2 | Select encoder E2 |
| Hold R1 | Select encoder E3 |
| R2 | Unassigned (reserved) |
| **Select** | **Restart norns** |
| Start | Unassigned (reserved) |
| D-pad left / right | Step selected encoder −1 / +1 |
| Left stick Y | Selected encoder delta, velocity-scaled ±1–3 |
| D-pad up / down | E1 +1 / −1 (always, regardless of selection — quick menu scroll) |
| **Select + Start** | **Exit to PortMaster** |

**Encoder state machine:** `selected_enc` starts at 0 (E1). Shoulder buttons update
it while held. On release, the last explicitly selected encoder is retained (not
reset to E1), so holding L2 to scroll E2 rapidly and releasing keeps E2 selected.

---

## 5. Buildroot Packages

Packages to add to the PanicOS buildroot. SuperCollider and norns binaries are
pre-built via Docker and bundled in the package (not installed via the OS package
manager).

### Runtime (must be in the OS image)

**Audio / JACK:**
- `alsa-lib` (likely already present)
- `pipewire` with JACK compatibility (`pipewire-jack`) or `jack2`
- `libsndfile`
- `libnanomsg` — custom buildroot recipe likely needed

**Lua 5.3 + modules** (must be 5.3; norns is incompatible with 5.4):
- `lua` (5.3)
- `lua-lpeg`
- `lua-cjson`
- `lua-socket`
- `lua-filesystem`
- `lua-posix`
- `lua-sec` (needs openssl)

**norns core deps:**
- `libcairo`
- `liblo` — custom buildroot recipe likely needed
- `libevdev` (matron links against it even with the FIFO input stub)
- `ncurses`
- `avahi-compat-libdnssd` (optional — maiden mDNS; can be stubbed)

**Host binary deps:**
- `sdl2`
- `libjack` or `pipewire-jack` (for `norns-input-bridge`)

**Runtime utilities (script install / Maiden):**
- `git`, `curl`, `wget`, `rsync`
- `sox`, `ffmpeg`
- `python3`
- `jq`, `bc`

### Build-time only (Docker, not in OS image)

- `golang` (maiden binary)
- `gcc`, `g++`, `cmake` (norns build)
- `waf` (norns build system)
- `nodejs`, `yarn` (maiden web UI)

---

## 6. Build System

`scripts/build-panicos.sh` produces one self-contained tarball with everything
pre-populated. No downloads happen on the device.

**Steps (all in Docker, ARM64):**

1. Build `norns-panicos` + `norns-input-bridge`
   - Base image: `arm64v8/debian:bookworm-slim` + `libsdl2-dev`, `libjack-jackd2-dev`
   - `aarch64-linux-gnu-gcc -O2`

2. Build norns prebuilt binaries
   - Same Docker image as `scripts/build-norns.sh`
   - Same `patches/apply-move-patches.sh` — no Portmaster-specific changes needed
   - Produces: `matron`, `crone`, `ws-wrapper`, `maiden-repl`, `sclang` (via scsynth
     from buildroot), Lua core, `sc/`, `resources/`

3. Build maiden
   - Go binary + React web UI (same as Move)

4. Build SC plugins
   - Same `scripts/build-sc-plugins.sh`

5. Assemble `dist/norns-panicos/`:
   - Unpack norns binaries + SC plugins → `norns/data/norns/` and
     `norns/data/.local/share/SuperCollider/Extensions/`
   - Clone starter scripts (awake, molly_the_poly, passersby) → `norns/data/dust/code/`
   - Write `sclang_conf.yaml`, `matronrc.lua`, `sc/startup.scd` at build time
   - Write maiden catalog source JSONs
   - Copy `norns-panicos`, `norns-input-bridge` → `norns/bin/`
   - Copy `Norns.sh`, `control.txt`, `icon.png`

6. `tar czf dist/norns-panicos.tar.gz norns-panicos/`

---

## 7. PortMaster Package Structure

```
norns-panicos/
  Norns.sh
  control.txt
  icon.png
  norns/
    bin/
      norns-panicos          ← SDL2 host binary
      norns-input-bridge     ← JACK MIDI → input FIFO (external devices)
    patches/
      apply-move-patches.sh  ← same patches work for Portmaster
    logs/
    cfg/                     ← runtime config, persisted across launches
    data/
      norns/                 ← pre-populated at build time
        build/
          matron/matron
          crone/crone
          ws-wrapper/ws-wrapper
          maiden-repl/maiden-repl
        lua/
        sc/
        resources/
        sclang_conf.yaml
        matronrc.lua
      maiden/
        maiden
        app/
      .local/share/SuperCollider/Extensions/   ← SC plugins, pre-built
      dust/
        code/
          awake/
          molly_the_poly/
          passersby/
        audio/
        data/
          sources/
            community.json
            base.json
```

### Norns.sh (minimal — no download logic)

```bash
#!/bin/bash
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
if   [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/"        ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/"    ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else                                              controlfolder="/roms/ports/PortMaster"
fi
source "$controlfolder/control.txt"
[ -f "$controlfolder/mod_${CFW_NAME}.txt" ] && source "$controlfolder/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/norns"
cd "$GAMEDIR"

export HOME="$GAMEDIR/data"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export JACK_NO_AUDIO_RESERVATION=1
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# Log rotation every 4 launches
LAUNCH_COUNT_FILE="$GAMEDIR/logs/.launch_count"
LAUNCH_COUNT=0
[ -f "$LAUNCH_COUNT_FILE" ] && LAUNCH_COUNT=$(cat "$LAUNCH_COUNT_FILE" 2>/dev/null || echo 0)
LAUNCH_COUNT=$((LAUNCH_COUNT + 1))
if [ "$LAUNCH_COUNT" -ge 4 ]; then
    : > "$GAMEDIR/logs/norns.log"
    LAUNCH_COUNT=0
fi
echo "$LAUNCH_COUNT" > "$LAUNCH_COUNT_FILE"

mkdir -p "$GAMEDIR/logs" "$GAMEDIR/cfg"
chmod +x ./bin/norns-panicos ./bin/norns-input-bridge 2>/dev/null

$GPTOKEYB "norns-panicos" &
pm_platform_helper "./bin/norns-panicos"
./bin/norns-panicos 2>&1 | tee -a "$GAMEDIR/logs/norns.log"
pm_finish
```

### control.txt

```
Title=Norns
Porter=djhardrich
Locations.consoles=ports
Genres=music
Description=Monome Norns sound computer on ARM handhelds.
porter_url=https://github.com/djhardrich/norns-portmaster
Architecture=aarch64
runtime=none
```

---

## 8. Files Added / Changed vs schwung-norns

| File | Status | Notes |
|---|---|---|
| `src/norns-panicos.c` | **New** | SDL2 host binary (screen + gamepad + process mgr) |
| `scripts/build-panicos.sh` | **New** | Full build + assembly script |
| `ports/portmaster/Norns.sh` | **New** | PortMaster launch script |
| `ports/portmaster/control.txt` | **New** | PortMaster metadata |
| `patches/apply-move-patches.sh` | **Unchanged** | Works for Portmaster as-is |
| `patches/screen-fifo.patch` | **Unchanged** | |
| `patches/input-virtual.patch` | **Unchanged** | |
| `patches/matron-midi-fifo-output.patch` | **Unchanged** | |
| `src/norns-input-bridge.c` | **Unchanged** | Reused as-is |
| `src/dsp/norns_plugin.c` | **Move-only** | Not part of norns-panicos build |
| `src/pw-helper.c` | **Move-only** | Not part of norns-panicos build |
| `src/ui.js` | **Move-only** | Not part of norns-panicos build |
