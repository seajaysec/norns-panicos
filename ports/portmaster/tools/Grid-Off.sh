#!/bin/bash
# Grid-Off.sh — PanicOS Tools entry: disable the USB monome grid for Norns and
# fall back to the Push 2 input bridge (Move-style emulated grid).
#
# Persistent toggle: sets the disable flag; takes effect the next Norns launch.
#
# Install location: /usr/share/panicos-launcher/tools/
[ -f /etc/profile ] && . /etc/profile

echo "=== Disable monome grid for Norns ==="
echo
touch /storage/.norns-grid-disabled
echo "Grid DISABLED."
echo
echo "Norns will use the Push 2 input bridge instead on the next launch."
echo
echo "Press any key to return..."
read -r -n 1 _ || true
