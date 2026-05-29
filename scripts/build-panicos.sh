#!/usr/bin/env bash
# build-panicos.sh — Full norns-panicos PortMaster package build
# Output: dist/norns-panicos.tar.gz  (self-contained, no on-device downloads)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
DIST="$REPO_ROOT/dist/norns-panicos"
NORNS_DATA="$DIST/norns/data"
NORNS_BIN="$DIST/norns/bin"

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
SDL_FLAGS=$(pkg-config --cflags --libs sdl2 2>/dev/null || echo "-lSDL2")
JACK_FLAGS=$(pkg-config --cflags --libs jack 2>/dev/null || echo "-ljack")
${CROSS_PREFIX}gcc -O2 -Wall src/norns-panicos.c      -o build/norns-panicos      $SDL_FLAGS -lpthread -lm
${CROSS_PREFIX}gcc -O2 -Wall src/norns-input-bridge.c -o build/norns-input-bridge $JACK_FLAGS
echo "[1/4] host binaries OK"
'

# ── 2. Build norns prebuilt tarball ─────────────────────────
echo ""
echo "--- [2/4] Building norns prebuilt tarball ---"
"$SCRIPT_DIR/build-norns.sh"

# ── 3. Build SC plugins ─────────────────────────────────────
echo ""
echo "--- [3/4] Building SC plugins ---"
"$SCRIPT_DIR/build-sc-plugins.sh"

# ── 4. Assemble package ──────────────────────────────────────
echo ""
echo "--- [4/4] Assembling norns-panicos package ---"
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
cp "$REPO_ROOT/build/norns-panicos"      "$NORNS_BIN/"
cp "$REPO_ROOT/build/norns-input-bridge" "$NORNS_BIN/"
chmod +x "$NORNS_BIN/norns-panicos" "$NORNS_BIN/norns-input-bridge"

# norns prebuilt binaries
tar xzf "$REPO_ROOT/dist/norns-move-prebuilt.tar.gz" -C "$NORNS_DATA/"

# SC plugins
mkdir -p "$NORNS_DATA/.local/share/SuperCollider/Extensions"
tar xzf "$REPO_ROOT/dist/sc-plugins-arm64.tar.gz" \
    -C "$NORNS_DATA/.local/share/SuperCollider/"

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
echo ""
echo "--- Packaging ---"
mkdir -p "$REPO_ROOT/dist"
(cd "$REPO_ROOT/dist" && tar czf norns-panicos.tar.gz norns-panicos/)

echo ""
echo "=== Build complete ==="
echo "Output: dist/norns-panicos.tar.gz"
ls -lh "$REPO_ROOT/dist/norns-panicos.tar.gz"
