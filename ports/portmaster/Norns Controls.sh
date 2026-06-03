#!/bin/bash
# Norns Controls.sh — PortMaster launcher for the norns-panicos control remapper.
# A standalone tool (beside the Norns port) that edits the SAME controls.conf the
# Norns port reads. It reads the gamepad directly via SDL, so no gptokeyb.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if   [ -d "/opt/system/Tools/PortMaster/" ]; then controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/"        ]; then controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/"    ]; then controlfolder="$XDG_DATA_HOME/PortMaster"
else                                              controlfolder="/roms/ports/PortMaster"
fi

source "$controlfolder/control.txt"
[ -f "$controlfolder/mod_${CFW_NAME}.txt" ] && source "$controlfolder/mod_${CFW_NAME}.txt"
get_controls

# This launcher sits in the ports dir; its data + the sibling Norns port are too.
PORTSDIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
GAMEDIR="$PORTSDIR/NornsControls"
NORNS_ROOT="$PORTSDIR/norns"
CONF="$NORNS_ROOT/cfg/controls.conf"
SCRIPTS="$NORNS_ROOT/data/dust/code"
DEFAULTS="$NORNS_ROOT/cfg/controls.defaults.conf"
cd "$GAMEDIR" || exit 1

export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"
mkdir -p "$NORNS_ROOT/cfg"
# Keep the factory defaults read-only — the tool restores from it, never writes it.
chmod 0444 "$DEFAULTS" 2>/dev/null

chmod +x ./norns-controls-gui 2>/dev/null
./norns-controls-gui "$CONF" "$SCRIPTS" "$DEFAULTS" > "$GAMEDIR/log.txt" 2>&1
