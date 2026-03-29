#!/bin/bash
# Build a universal (arm64 + x86_64) liblibtorrent_flutter.dylib for macOS
# and bundle universal OpenSSL dylibs.
#
# Prerequisites — both ARM and Intel Homebrew with deps installed:
#   ARM:   brew install libtorrent-rasterbar openssl@3
#   Intel: arch -x86_64 /usr/local/bin/brew install libtorrent-rasterbar openssl@3
#
#   chmod +x build_macos.sh && ./build_macos.sh
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_DIR="$SCRIPT_DIR/src"
MACOS_DIR="$SCRIPT_DIR/macos"
PREBUILT_DIR="$SCRIPT_DIR/prebuilt/macos/universal"

ARM_BREW="/opt/homebrew"
INTEL_BREW="/usr/local"

ARM_OPENSSL="$ARM_BREW/opt/openssl@3"
INTEL_OPENSSL="$INTEL_BREW/opt/openssl@3"

# ── Preflight checks ──────────────────────────────────────────────────────────
echo "=== Preflight checks..."
for d in "$ARM_OPENSSL" "$INTEL_OPENSSL"; do
    if [ ! -d "$d" ]; then
        echo "ERROR: Required OpenSSL not found at $d"
        echo "Install both ARM and Intel Homebrew OpenSSL (see header comment)."
        exit 1
    fi
done
echo "  ARM  OpenSSL: $ARM_OPENSSL"
echo "  Intel OpenSSL: $INTEL_OPENSSL"

# ── Build arm64 ────────────────────────────────────────────────────────────────
echo ""
echo "=== Building arm64..."
BUILD_ARM64="$SRC_DIR/build_macos_arm64"
cmake -S "$SRC_DIR" -B "$BUILD_ARM64" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=arm64 \
    -DCMAKE_PREFIX_PATH="$ARM_BREW/opt/libtorrent-rasterbar;$ARM_BREW;$ARM_OPENSSL"
cmake --build "$BUILD_ARM64" --config Release --parallel

ARM64_DYLIB="$BUILD_ARM64/liblibtorrent_flutter.dylib"
if [ ! -f "$ARM64_DYLIB" ]; then
    echo "ERROR: arm64 build failed — dylib not found at $ARM64_DYLIB"
    exit 1
fi

# ── Build x86_64 ──────────────────────────────────────────────────────────────
echo ""
echo "=== Building x86_64..."
BUILD_X86_64="$SRC_DIR/build_macos_x86_64"
cmake -S "$SRC_DIR" -B "$BUILD_X86_64" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
    -DCMAKE_PREFIX_PATH="$INTEL_BREW/opt/libtorrent-rasterbar;$INTEL_BREW;$INTEL_OPENSSL"
cmake --build "$BUILD_X86_64" --config Release --parallel

X86_64_DYLIB="$BUILD_X86_64/liblibtorrent_flutter.dylib"
if [ ! -f "$X86_64_DYLIB" ]; then
    echo "ERROR: x86_64 build failed — dylib not found at $X86_64_DYLIB"
    exit 1
fi

# ── Lipo: merge into universal binaries ────────────────────────────────────────
echo ""
echo "=== Creating universal dylibs with lipo..."
UNIVERSAL_DIR="$SRC_DIR/build_macos_universal"
mkdir -p "$UNIVERSAL_DIR"

lipo -create "$ARM64_DYLIB" "$X86_64_DYLIB" \
    -output "$UNIVERSAL_DIR/liblibtorrent_flutter.dylib"

lipo -create \
    "$ARM_OPENSSL/lib/libssl.3.dylib" \
    "$INTEL_OPENSSL/lib/libssl.3.dylib" \
    -output "$UNIVERSAL_DIR/libssl.3.dylib"

lipo -create \
    "$ARM_OPENSSL/lib/libcrypto.3.dylib" \
    "$INTEL_OPENSSL/lib/libcrypto.3.dylib" \
    -output "$UNIVERSAL_DIR/libcrypto.3.dylib"

UNIVERSAL_DYLIB="$UNIVERSAL_DIR/liblibtorrent_flutter.dylib"

# ── Fix OpenSSL load paths ─────────────────────────────────────────────────────
echo "=== Fixing OpenSSL load paths with install_name_tool..."

for ssl_path in \
    "$ARM_BREW/opt/openssl@3/lib" "$ARM_BREW/opt/openssl/lib" \
    "$INTEL_BREW/opt/openssl@3/lib" "$INTEL_BREW/opt/openssl/lib"; do
    install_name_tool -change "$ssl_path/libssl.3.dylib" "@loader_path/libssl.3.dylib" "$UNIVERSAL_DYLIB" 2>/dev/null || true
    install_name_tool -change "$ssl_path/libcrypto.3.dylib" "@loader_path/libcrypto.3.dylib" "$UNIVERSAL_DYLIB" 2>/dev/null || true
done

# ── Copy & fix ─────────────────────────────────────────────────────────────────
echo "=== Copying to output directories..."

for dest in "$MACOS_DIR" "$PREBUILT_DIR"; do
    mkdir -p "$dest"
    cp "$UNIVERSAL_DYLIB" "$dest/"
    cp "$UNIVERSAL_DIR/libssl.3.dylib" "$dest/"
    cp "$UNIVERSAL_DIR/libcrypto.3.dylib" "$dest/"

    # Fix libssl's reference to libcrypto
    for ssl_path in "$ARM_OPENSSL/lib" "$INTEL_OPENSSL/lib"; do
        install_name_tool -change "$ssl_path/libcrypto.3.dylib" "@loader_path/libcrypto.3.dylib" "$dest/libssl.3.dylib" 2>/dev/null || true
    done

    # Fix install names to use @loader_path
    install_name_tool -id "@loader_path/libssl.3.dylib" "$dest/libssl.3.dylib"
    install_name_tool -id "@loader_path/libcrypto.3.dylib" "$dest/libcrypto.3.dylib"

    echo "  -> $dest/"
done

# ── Verify ─────────────────────────────────────────────────────────────────────
echo ""
echo "=== Architecture verification ==="
for f in "$MACOS_DIR"/lib*.dylib; do
  echo "$(basename "$f"): $(lipo -archs "$f")"
done
# Should print "arm64 x86_64" for each

echo ""
echo "=== Verifying linkage..."
echo "--- liblibtorrent_flutter.dylib ---"
otool -L "$MACOS_DIR/liblibtorrent_flutter.dylib" | grep -E "ssl|crypto|homebrew" || echo "  (no Homebrew references — OK)"
echo "--- libssl.3.dylib ---"
otool -L "$MACOS_DIR/libssl.3.dylib" | grep -E "crypto|homebrew" || echo "  (no Homebrew references — OK)"

echo ""
echo "=== Done! Universal (arm64 + x86_64) files placed in:"
echo "  $MACOS_DIR/"
echo "  $PREBUILT_DIR/"
