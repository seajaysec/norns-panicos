#!/bin/bash
# Norns.sh — PortMaster launch script for norns-panicos

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if   [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/"        ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/"    ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else                                              controlfolder="/roms/ports/PortMaster"
fi

source "$controlfolder/control.txt"
[ -f "$controlfolder/mod_${CFW_NAME}.txt" ] && source "$controlfolder/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)/norns"
cd "$GAMEDIR"

# norns home — all norns processes see their home here
export HOME="$GAMEDIR/data"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
export JACK_NO_AUDIO_RESERVATION=1
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# Rotate log every 4 launches (keeps log small on SD card)
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

# Control mapping config. norns-panicos reads $NORNS_PANICOS_CONF at launch;
# seed a commented default the user can edit. The binary's built-in defaults
# already match this template, so behaviour is identical if seeding is skipped.
export NORNS_PANICOS_CONF="$GAMEDIR/cfg/controls.conf"
if [ ! -f "$NORNS_PANICOS_CONF" ]; then
    cat > "$NORNS_PANICOS_CONF" << 'CTRLCONF'
# norns-panicos controls — edit and relaunch to remap.
#
# Pick a layout by name. Switch this one line to roll between schemes; add your
# own [section] below and point `scheme` at it.
scheme = sticks

# ── Global settings (apply to every scheme) ────────────────────────────────
# Keys (K1/K2/K3). Buttons: a b x y l1 r1 (comma-separated, multiple allowed).
k1 = y
k2 = x
k3 = a
dpad_step      = 2      # encoder delta per detent (2 = one patched-matron detent)
stick_deadzone = 8192   # 0..32000; raise if a stick drifts at rest
stick_throttle = 6      # base stick rate: emit ~every N frames (raise to slow)
stick_invert   = 0      # 1 = flip every stick direction
# Hold-to-accelerate: encoders spin faster the longer an input is held.
# Button-pairs (D-pad/L1-R1/L2-R2) use accel_*; sticks use the gentler stick_accel_*.
accel             = 1   # 0 = constant rate
accel_delay       = 18  # button-pair: frames before ramping (~0.3s)
accel_ramp        = 60  # button-pair: frames to reach top speed (~1s)
stick_accel_delay = 30  # sticks: start later (~0.5s)
stick_accel_ramp  = 110 # sticks: smoother, longer ramp
accel_max         = 5   # peak speed multiplier

# ── Schemes ────────────────────────────────────────────────────────────────
# Each encoder is driven by an analog source AND/OR button-pairs:
#   e1/e2/e3   = none | lstick | lstick-y | rstick | rstick-y   (stick source)
#   dpad_x     = 1|2|3|none   D-pad left/right → that encoder
#   dpad_y     = 1|2|3|none   D-pad up/down    → that encoder
#   shoulders  = 1|2|3|none   L1/R1 pair       → that encoder
#   triggers   = 1|2|3|none   L2/R2 pair       → that encoder

# Default: dual-stick devices (RG35XX Pro / H).
# E1 = D-pad L/R + L1/R1;  E2 = D-pad U/D + left stick;  E3 = L2/R2 + right stick.
[sticks]
e1 = none
e2 = lstick
e3 = rstick
dpad_x    = 1
dpad_y    = 2
shoulders = 1
triggers  = 3

# D-pad only (works without analog sticks, e.g. RG36XX):
#   up/down = E1, left/right = E2, L1/R1 = E3.
[dpad]
dpad_x    = 2
dpad_y    = 1
shoulders = 3
triggers  = none

# ── Context overlays ───────────────────────────────────────────────────────
# Applied ON TOP of the active scheme while you're in that context. [menu] =
# the norns system menu; [script:NAME] = while the script NAME is running
# (NAME = its folder/short name). List only what differs from the scheme.
#
# In the menu the list scrolls "down = +", opposite to D-pad up — so flip the
# D-pad up/down there (up = up), and let B mirror K2 for one-handed paging.
[menu]
k2            = x, b
dpad_y_invert = 1

# Example per-script override: koiboi2 wants its D-pad up/down inverted.
[script:koiboi2]
dpad_y_invert = 1

# pixels: right stick steers E2 (L/R) + E3 (U/D, inverted to taste); left stick
# drives E1 with both axes summed (push up OR right = +, down OR left = −).
# D-pad/shoulders unchanged.
[script:pixels]
e1 = lstick-xy
e2 = rstick
e3 = rstick-y-inv

# Note: Menu/FN = home (tap); Menu/FN-hold or Select+Menu/FN = quit. These are
# host-reserved and can't be rebound. Select, Start, and the stick clicks
# (l3/r3) are free to map.
CTRLCONF
fi

# Generate sclang_conf.yaml with absolute paths for this device.
# Also auto-detect double-nested system extension dirs (OS packaging bug where
# each plugin ships both a top-level .sc file AND a Name/Name/Classes/ tree),
# and exclude the inner copy to prevent duplicate class errors.
EXCL="    - $HOME/.local/share/SuperCollider/Extensions"
for _d in /usr/share/SuperCollider/Extensions/*/; do
    _n=$(basename "$_d")
    [ -d "${_d}${_n}" ] && EXCL="$EXCL
    - ${_d}${_n}"
done
cat > "$HOME/norns/sclang_conf.yaml" << EOF
includePaths:
    - $HOME/norns/sc/core
    - $HOME/norns/sc/engines
    - $HOME/norns/sc
    - $HOME/dust
excludePaths:
$EXCL
postInlinePaths: []
EOF
unset EXCL _d _n

# Use the real monome grid (over its serial port) rather than the Push 2 emulated
# grid. Toggled by the Grid-On / Grid-Off Tools, which set/clear this flag;
# default on (the bridge no-ops harmlessly when no grid is attached).
if [ -f /storage/.norns-grid-disabled ]; then
    export NORNS_USE_MONOME=0
else
    export NORNS_USE_MONOME=1
fi

# FAT32 does not preserve execute bits — chmod every norns binary after extraction
chmod +x ./bin/norns-panicos ./bin/norns-input-bridge ./bin/norns-push2-display \
         ./bin/norns-monome-bridge 2>/dev/null
find "$HOME/norns/build" -type f -exec chmod +x {} \; 2>/dev/null || true
chmod +x "$HOME/maiden/maiden" 2>/dev/null || true
# Bundled aarch64 SC UGen .so (ingenue-ugens) — FAT32 drops exec bits; scsynth
# dlopen()s these at boot, so restore +x or the engines stay silent.
find "$HOME/.local/share/SuperCollider/Extensions" -name '*.so' \
     -exec chmod +x {} \; 2>/dev/null || true

# --- PanicOS audio setup ---------------------------------------------------
# PanicOS runs PipeWire as its JACK server but ships NO JACK CLI tools. norns
# wires its audio graph (engine <-> crone) by shelling out to `jack_connect`
# (see sc/core/Crone.sc); without it the engine's output never reaches crone
# and there is no sound. Provide PipeWire-backed shims for the jack CLI.
mkdir -p "$GAMEDIR/shim"
printf '#!/bin/sh\nexec pw-link "$1" "$2"\n'                         > "$GAMEDIR/shim/jack_connect"
printf '#!/bin/sh\nexec pw-link -d "$1" "$2"\n'                      > "$GAMEDIR/shim/jack_disconnect"
printf '#!/bin/sh\npw-link -o 2>/dev/null; pw-link -i 2>/dev/null\n' > "$GAMEDIR/shim/jack_lsp"
chmod +x "$GAMEDIR/shim/jack_connect" "$GAMEDIR/shim/jack_disconnect" "$GAMEDIR/shim/jack_lsp"
export PATH="$GAMEDIR/shim:$PATH"

# Audio stability WITHOUT inflating latency. norns is a live instrument, so do
# NOT force a large PipeWire quantum — keep native low latency and kill xruns
# via scheduling instead: pin CPUs to performance (the default governor
# downclocks on micro-idles -> periodic dropouts) and rely on SCHED_FIFO below.
for _g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$_g" 2>/dev/null || true
done
unset _g

# One-time low-latency audio drop-ins (persist in the PanicOS rw overlay).
# PanicOS defaults the built-in H616 codec to a 1024-frame period (~21ms);
# norns is a live instrument, so install a 128-frame codec period + matching
# graph quantum (~2.7ms). Written once; the audio stack is only restarted when
# we just created them, so normal launches aren't disrupted. Delete both files
# (and restart wireplumber/pipewire) to revert to PanicOS stock latency.
_wp="/usr/share/wireplumber/wireplumber.conf.d/95-norns-codec-lowlatency.conf"
_pw="/etc/pipewire/pipewire.conf.d/51-norns-lowlatency.conf"
_ll_new=0
# Rewrite when absent OR when it predates the audio.rate pin (self-heals old
# installs). The built-in H616 codec is left at 44.1k by other apps; norns runs
# at 48k, so without pinning the codec's rate PipeWire resamples 48k→44.1k into
# a busy device and norns audio crackles. Pin the codec to 48000 to match the
# graph + HDMI path (all already 48k); 44.1k sources resample to it transparently.
if [ ! -f "$_wp" ] || ! grep -q "audio.rate" "$_wp" 2>/dev/null; then
    mkdir -p "$(dirname "$_wp")" 2>/dev/null
    cat > "$_wp" 2>/dev/null <<'WPCONF'
monitor.alsa.rules = [
  {
    matches = [
      { node.name = "~alsa_output.*codec_sound_card0.*" }
    ]
    actions = {
      update-props = {
        audio.rate           = 48000
        api.alsa.period-size = 128
        api.alsa.headroom    = 128
      }
    }
  }
]
WPCONF
    [ -s "$_wp" ] && _ll_new=1
fi
if [ ! -f "$_pw" ]; then
    mkdir -p "$(dirname "$_pw")" 2>/dev/null
    cat > "$_pw" 2>/dev/null <<'PWCONF'
context.properties = {
    default.clock.quantum     = 128
    default.clock.min-quantum = 32
    default.clock.max-quantum = 2048
}
PWCONF
    [ -s "$_pw" ] && _ll_new=1
fi
if [ "$_ll_new" = 1 ]; then
    # Apply the new low-latency / codec-rate config by restarting the audio
    # stack. PanicOS runs PipeWire as *system-wide* systemd services, where a
    # bare `killall wireplumber` does NOT reliably respawn it — restart through
    # systemd instead (fall back to killall on non-systemd setups).
    if command -v systemctl >/dev/null 2>&1; then
        systemctl restart pipewire pipewire-pulse wireplumber 2>/dev/null
    else
        killall wireplumber pipewire pipewire-pulse 2>/dev/null
    fi
    for _i in $(seq 1 15); do
        pgrep -x pipewire >/dev/null 2>&1 && pgrep -x wireplumber >/dev/null 2>&1 && break
        sleep 1
    done
    # Restarting the stack resets default-sink routing — the speaker goes silent
    # until something re-elects a sink. Re-run PanicOS's own selector (picks the
    # speaker when no HDMI is attached); this is what makes the codec-rate fix
    # safe to apply at launch instead of only after a reboot.
    sleep 1
    command -v hdmi_sense >/dev/null 2>&1 && hdmi_sense >/dev/null 2>&1 || true
fi
unset _wp _pw _ll_new _i

# Audio latency: request a small PipeWire quantum for norns' own clients (this
# is per-app and reverts when norns exits — NOT a global force). 128 frames
# @48kHz ≈ 2.7ms, matching monome's image. Lower = tighter timing but more
# xrun-prone; this SoC (Allwinner H700, quad Cortex-A53 ~1.4GHz) is roughly a
# Pi 3B+ peer, so 128 is a sane floor — drop to 64 (~1.3ms) only for light patches.
export PIPEWIRE_QUANTUM=128/48000

# Promote audio threads to SCHED_FIFO. PanicOS has no rtkit, so PipeWire's
# data-loop otherwise runs SCHED_OTHER and starves on the RT kernel (periodic
# xruns). crone/scsynth are spawned ~15s after launch, so poll in the bg.
(
    PW="" SC="" CR=""
    for _i in $(seq 1 40); do
        PW=$(pgrep -x pipewire | head -1)
        SC=$(pgrep -x scsynth  | head -1)
        CR=$(pgrep -x crone    | head -1)
        [ -n "$PW" ] && [ -n "$SC" ] && [ -n "$CR" ] && break
        sleep 1
    done
    # PipeWire data-loop = highest userspace audio prio
    [ -n "$PW" ] && for _t in /proc/$PW/task/*; do
        case "$(cat "$_t/comm" 2>/dev/null)" in
            data-loop*) chrt -f -p 88 "$(basename "$_t")" 2>/dev/null ;;
        esac
    done
    # engine + audio context just below the server
    [ -n "$CR" ] && chrt -a -f -p 76 "$CR" 2>/dev/null
    [ -n "$SC" ] && chrt -a -f -p 76 "$SC" 2>/dev/null
) &

# ingenue — modern web editor on :7777, alongside maiden (:5000). Lives in
# dust/code/ingenue; runs for the norns session (started here, stopped on exit).
INGENUE_DIR="$HOME/dust/code/ingenue"
if command -v python3 >/dev/null 2>&1 && [ -f "$INGENUE_DIR/server.py" ]; then
    pkill -f 'server.py 7777' 2>/dev/null || true
    ( cd "$INGENUE_DIR" && setsid python3 server.py 7777 >"$GAMEDIR/logs/ingenue.log" 2>&1 & )
    echo "ingenue: web editor starting on :7777" >> "$GAMEDIR/logs/norns.log"
fi

$GPTOKEYB "norns-panicos" &
pm_platform_helper "./bin/norns-panicos"
./bin/norns-panicos 2>&1 | tee -a "$GAMEDIR/logs/norns.log"

# Stop ingenue when norns exits (kill by cmdline — the process has no path).
pkill -f 'server.py 7777' 2>/dev/null || true
pm_finish
