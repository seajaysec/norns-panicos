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
