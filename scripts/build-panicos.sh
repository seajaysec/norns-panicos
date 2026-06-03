#!/usr/bin/env bash
# build-panicos.sh — Full norns-panicos PortMaster package build
# Output: dist/norns-panicos.tar.gz  (self-contained, no on-device downloads)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
DIST="$REPO_ROOT/dist/norns-panicos"
NORNS_DATA="$DIST/norns/data"
NORNS_BIN="$DIST/norns/bin"

# Default: build norns from source so the crone ADC-optional patch is baked in
# via patches/apply-move-patches.sh. That patch is REQUIRED on capture-less
# boards (H700/rg35xx etc.) — without it crone aborts on startup with
# "connectAdcPorts() failed" and the engine's audio never reaches the speaker.
# Building from source takes ~60-80 min via QEMU.
# Set BUILD_FROM_SOURCE=0 to download the prebuilt instead, but the published
# prebuilt must already carry that patch or the guard below aborts the build.
BUILD_FROM_SOURCE="${BUILD_FROM_SOURCE:-1}"
PREBUILT_URL="https://github.com/djhardrich/schwung-norns/releases/download/v0.4.0/norns-move-prebuilt.tar.gz"
SC_PLUGINS_URL=""  # SC plugins are provided by the OS — not bundled

trap 'echo ""; echo "ERROR: Build failed — cleaning up..."; rm -rf "$DIST"; exit 1' ERR

if ! command -v docker &>/dev/null; then
    echo "ERROR: Docker is not installed or not in PATH." >&2
    exit 1
fi

check_dependencies() {
    local missing=0
    for f in \
        "src/norns-panicos.c" \
        "src/norns-input-bridge.c" \
        "src/norns-push2-display.c" \
        "$SCRIPT_DIR/Dockerfile.panicos" \
        "$SCRIPT_DIR/build-norns.sh" \
        "$SCRIPT_DIR/build-sc-plugins.sh" \
        "$REPO_ROOT/ports/portmaster/Norns.sh" \
        "$REPO_ROOT/ports/portmaster/control.txt"; do
        if [ ! -f "$f" ]; then
            echo "ERROR: Missing required file: $f" >&2
            missing=$((missing + 1))
        fi
    done
    if [ "$missing" -gt 0 ]; then
        echo "ERROR: $missing required file(s) missing. Check repo state." >&2
        return 1
    fi
}

check_dependencies || exit 1

echo "=== Building norns-panicos PortMaster package ==="

# ── 1. Cross-compile host binaries ──────────────────────────
echo ""
echo "--- [1/4] Building host binaries (arm64 cross-compile) ---"
docker build -t norns-panicos-builder -f "$SCRIPT_DIR/Dockerfile.panicos" "$REPO_ROOT"
mkdir -p "$REPO_ROOT/build"
docker run --rm \
    -v "$REPO_ROOT:/build" \
    -u "$(id -u):$(id -g)" \
    -w /build \
    norns-panicos-builder \
    sh -c '
set -e
SDL_FLAGS=$(pkg-config --cflags --libs sdl2    2>/dev/null || echo "-lSDL2")
JACK_FLAGS=$(pkg-config --cflags --libs jack   2>/dev/null || echo "-ljack")
USB_FLAGS=$(pkg-config --cflags --libs libusb-1.0 2>/dev/null || echo "-lusb-1.0")
${CROSS_PREFIX}gcc -O2 -Wall src/norns-panicos.c       -o build/norns-panicos       $SDL_FLAGS -lpthread -lm
${CROSS_PREFIX}gcc -O2 -Wall src/norns-input-bridge.c  -o build/norns-input-bridge  $JACK_FLAGS $USB_FLAGS -lpthread
${CROSS_PREFIX}gcc -O2 -Wall src/norns-push2-display.c -o build/norns-push2-display $USB_FLAGS
# norns-monome-bridge: real monome grid over its serial port (mext; no extra deps).
${CROSS_PREFIX}gcc -O2 -Wall src/norns-monome-bridge.c -o build/norns-monome-bridge
echo "[1/4] host binaries OK"
'

# ── 2. Norns prebuilt tarball ───────────────────────────────
echo ""
if [ "${REUSE_PREBUILT:-0}" = "1" ] && [ -f "$REPO_ROOT/dist/norns-move-prebuilt.tar.gz" ]; then
    echo "--- [2/4] Reusing existing dist/norns-move-prebuilt.tar.gz (REUSE_PREBUILT=1) ---"
elif [ "$BUILD_FROM_SOURCE" = "1" ]; then
    echo "--- [2/4] Building norns prebuilt tarball (from source, slow) ---"
    "$SCRIPT_DIR/build-norns.sh"
else
    echo "--- [2/4] Downloading norns prebuilt tarball ---"
    mkdir -p "$REPO_ROOT/dist"
    curl -fsSL "$PREBUILT_URL" -o "$REPO_ROOT/dist/norns-move-prebuilt.tar.gz"
    echo "[2/4] norns prebuilt OK"
fi

# ── 3. SC plugins — provided by the OS, not bundled ─────────
echo ""
echo "--- [3/4] SC plugins: using system-installed (skipping bundle) ---"

# ── 4. Assemble package ──────────────────────────────────────
echo ""
echo "--- [4/4] Assembling norns-panicos package ---"

# Verify build outputs exist before assembly
for tarball in \
    "$REPO_ROOT/dist/norns-move-prebuilt.tar.gz"; do
    if [ ! -f "$tarball" ]; then
        echo "ERROR: Expected build output not found: $tarball" >&2
        echo "       Check build-norns.sh output above." >&2
        exit 1
    fi
done

rm -rf "$DIST"
mkdir -p \
    "$NORNS_BIN" \
    "$NORNS_DATA/dust/code" \
    "$NORNS_DATA/dust/audio" \
    "$NORNS_DATA/dust/data/sources" \
    "$NORNS_DATA/dust/data/catalogs" \
    "$DIST/norns/logs" \
    "$DIST/norns/cfg"

# Host binaries
cp "$REPO_ROOT/build/norns-panicos"        "$NORNS_BIN/"
cp "$REPO_ROOT/build/norns-input-bridge"  "$NORNS_BIN/"
cp "$REPO_ROOT/build/norns-push2-display" "$NORNS_BIN/"
cp "$REPO_ROOT/build/norns-monome-bridge" "$NORNS_BIN/"
chmod +x "$NORNS_BIN/norns-panicos" "$NORNS_BIN/norns-input-bridge" \
         "$NORNS_BIN/norns-push2-display" "$NORNS_BIN/norns-monome-bridge"

# norns prebuilt binaries
tar xzf "$REPO_ROOT/dist/norns-move-prebuilt.tar.gz" -C "$NORNS_DATA/"

# Guard: never ship a crone without the ADC-optional patch. On boards with no
# audio-input device an unpatched crone aborts at startup (connectAdcPorts()
# failed) and there is no audio. This catches a stale/unpatched prebuilt before
# it gets packaged (the exact bug that shipped in an earlier zip).
_crone="$NORNS_DATA/norns/build/crone/crone"
if [ ! -f "$_crone" ]; then
    echo "ERROR: crone binary missing from prebuilt: $_crone" >&2
    exit 1
fi
if ! grep -qa "continuing without audio input" "$_crone"; then
    echo "ERROR: crone is NOT patched (crone-adc-optional missing)." >&2
    echo "       Build from source (BUILD_FROM_SOURCE=1) or publish a patched prebuilt." >&2
    exit 1
fi
echo "  crone ADC-optional patch: present"

# Encoder feel for D-pad/stick input (no real rotary encoders here): the norns
# menu desensitises E1 (sens 8) and time-accelerates E3, which makes discrete
# D-pad presses need 4 taps per move and gives E3 a different feel. Normalise to
# sens 2 / accel off on all three so they respond uniformly to norns-panicos's
# own hold-acceleration. Idempotent.
_menu="$NORNS_DATA/norns/lua/core/menu.lua"
if [ -f "$_menu" ]; then
    sed -i 's/set_sens(1,8)/set_sens(1,2)/; s/set_accel(3,true)/set_accel(3,false)/' "$_menu"
    echo "  menu encoder sens normalised (E1 sens 2, E3 accel off)"
    # Export the norns context ("menu" or the running script name) so the host
    # can apply per-context control overlays ([menu] / [script:NAME]). Idempotent.
    grep -q "norns-context" "$_menu" || sed -i \
        's|_menu.set_mode = function(mode)|_menu.set_mode = function(mode) do local f=io.open("/tmp/norns-context","w") if f then f:write(mode and "menu" or (norns.state.shortname or "")) f:close() end end|' \
        "$_menu"
    echo "  norns context exported to /tmp/norns-context"
fi


# Starter scripts (cloned from source)
echo "  Cloning starter scripts..."
cd "$NORNS_DATA/dust/code"
for REPO in \
    "https://github.com/tehn/awake.git" \
    "https://github.com/markwheeler/molly_the_poly.git" \
    "https://github.com/markwheeler/passersby.git"; do
    NAME="$(basename "$REPO" .git)"
    [ -d "$NAME" ] || git clone --depth 1 "$REPO"
done
cd "$REPO_ROOT"

# Maiden catalog sources
cat > "$NORNS_DATA/dust/data/sources/community.json" << 'SRCEOF'
{"file_info":{"version":1,"kind":"catalog_source"},"source":{"name":"community","method":"download","parameters":{"url":"https://raw.githubusercontent.com/monome/norns-community/main/community.json"}}}
SRCEOF
cat > "$NORNS_DATA/dust/data/sources/base.json" << 'SRCEOF'
{"file_info":{"version":1,"kind":"catalog_source"},"source":{"name":"base","method":"download","parameters":{"url":"https://raw.githubusercontent.com/monome/norns-community/main/base.json"}}}
SRCEOF

# sc/startup.scd — force 44100 Hz (norns expects this)
mkdir -p "$NORNS_DATA/norns/sc"
cat > "$NORNS_DATA/norns/sc/startup.scd" << 'SCDEOF'
s.options.sampleRate = 44100;
SCDEOF

# sclang_conf.yaml — placeholder; overwritten at runtime by Norns.sh
# because the include paths must contain the actual $HOME value on the device.
cat > "$NORNS_DATA/norns/sclang_conf.yaml" << 'SCCONF'
# Generated at launch time by Norns.sh — do not edit
SCCONF

# PortMaster metadata + launcher
cp "$REPO_ROOT/ports/portmaster/Norns.sh"    "$DIST/"
cp "$REPO_ROOT/ports/portmaster/control.txt" "$DIST/"

# ── 5. Package ───────────────────────────────────────────────
# PortMaster expects Norns.sh + control.txt + norns/ at the ZIP ROOT so they
# extract straight into /roms/ports/. Zip the staging CONTENTS, not the staging
# dir (a wrapping norns-panicos/ dir makes the installer nest the port one level
# too deep / fail to find Norns.sh).
echo ""
echo "--- Packaging ---"
mkdir -p "$REPO_ROOT/dist"
rm -f "$REPO_ROOT/dist/norns-panicos.zip"
( cd "$DIST" && zip -rq "$REPO_ROOT/dist/norns-panicos.zip" Norns.sh control.txt norns )

echo ""
echo "=== Build complete ==="
echo "Output: $REPO_ROOT/dist/norns-panicos.zip"
ls -lh "$REPO_ROOT/dist/norns-panicos.zip" 2>/dev/null || true
exit 0
