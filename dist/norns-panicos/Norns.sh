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

# FAT32 does not preserve execute bits — chmod every norns binary after extraction
chmod +x ./bin/norns-panicos ./bin/norns-input-bridge ./bin/norns-push2-display 2>/dev/null
find "$HOME/norns/build" -type f -exec chmod +x {} \; 2>/dev/null || true
chmod +x "$HOME/maiden/maiden" 2>/dev/null || true

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
if [ ! -f "$_wp" ]; then
    mkdir -p "$(dirname "$_wp")" 2>/dev/null
    cat > "$_wp" 2>/dev/null <<'WPCONF'
monitor.alsa.rules = [
  {
    matches = [
      { node.name = "~alsa_output.*codec_sound_card0.*" }
    ]
    actions = {
      update-props = {
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
    killall wireplumber pipewire pipewire-pulse 2>/dev/null
    for _i in $(seq 1 15); do
        pgrep -x pipewire >/dev/null 2>&1 && pgrep -x wireplumber >/dev/null 2>&1 && break
        sleep 1
    done
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

$GPTOKEYB "norns-panicos" &
pm_platform_helper "./bin/norns-panicos"
./bin/norns-panicos 2>&1 | tee -a "$GAMEDIR/logs/norns.log"
pm_finish
