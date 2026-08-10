#!/usr/bin/env bash
# download_prebuilt.sh — Download prebuilt native libraries from a GitHub Release
# Usage: ./scripts/download_prebuilt.sh [version]
# Example: ./scripts/download_prebuilt.sh 1.9.2
set -euo pipefail

REPO="ayman708-UX/libtorrent_flutter"
VERSION="${1:-}"

if [ -z "$VERSION" ]; then
  # Get latest release version
  VERSION=$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" | \
    grep '"tag_name"' | sed 's/.*"tag_name": *"v\?\([^"]*\)".*/\1/')
  echo "Latest version: $VERSION"
fi

BASE_URL="https://github.com/${REPO}/releases/download/v${VERSION}"

download_and_extract() {
  local name="$1"
  local dest="$2"
  local zip="${name}.zip"
  echo "→ Downloading ${zip}..."
  curl -fsSL "${BASE_URL}/${zip}" -o "/tmp/${zip}"
  mkdir -p "$dest"
  unzip -o "/tmp/${zip}" -d "$dest"
  rm "/tmp/${zip}"
  echo "  ✅ Extracted to ${dest}"
}

# Linux
download_and_extract "linux-native-lib-x64"   "prebuilt/linux/x64"
download_and_extract "linux-native-lib-arm64" "prebuilt/linux/arm64"

# macOS
download_and_extract "macos-native-lib" "prebuilt/macos"
mkdir -p macos
cp prebuilt/macos/universal/liblibtorrent_flutter.dylib macos/ 2>/dev/null || true

# Windows
download_and_extract "windows-native-lib-x64"   "prebuilt/windows/x64"
download_and_extract "windows-native-lib-arm64" "prebuilt/windows/arm64"

# Android
download_and_extract "android-native-lib-arm64-v8a"   "prebuilt/android/arm64-v8a"
download_and_extract "android-native-lib-armeabi-v7a" "prebuilt/android/armeabi-v7a"
download_and_extract "android-native-lib-x86_64"      "prebuilt/android/x86_64"

# iOS (xcframework)
mkdir -p prebuilt/ios
download_and_extract "ios-native-lib" "prebuilt/ios/libtorrent_flutter.xcframework"
# Also copy to ios/ for CocoaPods
rm -rf ios/libtorrent_flutter.xcframework
cp -R prebuilt/ios/libtorrent_flutter.xcframework ios/ 2>/dev/null || true

# Download CA bundle for Android WebTorrent
echo "→ Downloading Mozilla CA bundle..."
curl -fsSL "https://curl.se/ca/cacert.pem" -o assets/cacert.pem
echo "  ✅ cacert.pem downloaded ($(wc -c < assets/cacert.pem) bytes)"

echo ""
echo "✅ All prebuilt libraries downloaded for version ${VERSION}"
