#!/bin/bash
# Grid-On.sh — PanicOS Tools entry: enable the real USB monome-protocol grid
# for Norns (OXI One in grid mode, classic monome grids over CDC-ACM, etc.).
#
# Tools run from the launcher while Norns is closed, so this is a persistent
# toggle: it clears the disable flag and takes effect the next time you launch
# Norns (whose launcher reads /storage/.norns-grid-disabled). The bridge no-ops
# harmlessly if no grid is plugged in.
#
# Install location: /usr/share/panicos-launcher/tools/
[ -f /etc/profile ] && . /etc/profile

echo "=== Enable monome grid for Norns ==="
echo
rm -f /storage/.norns-grid-disabled
echo "Grid ENABLED."
echo
echo "Launch Norns and your grid will be picked up automatically."
echo "(Falls back silently to no grid if none is attached.)"
echo
echo "Press any key to return..."
read -r -n 1 _ || true
