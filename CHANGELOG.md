# Changelog

## 1.4.0

- **Platform**: Added iOS support

## 1.3.0

- **Platform**: Added iOS support (arm64 device + x86_64 simulator, universal static library)
- **CI**: New `build-ios` job cross-compiles libtorrent + torrent_bridge as a static `.a` for iOS, merged with `lipo`
- **Packaging**: iOS podspec with `-force_load` so all FFI symbols are visible via `DynamicLibrary.process()`

## 1.2.0

- **License**: Switched to GPL v3 (OSI-approved)
- **Streaming**: Dual-end preloading — fetches first 4MB and last 4MB of the file simultaneously on stream start. The tail contains the MP4/MKV moov atom (seek table), enabling instant seeking without buffering the whole file first
- **Streaming**: Staggered 30ms piece deadlines on the head (down from 45ms) for faster playback start
- **API**: `disposeTorrent(id)` — stop streams, remove torrent, and delete files in one call
- **API**: `disposeAll()` — clean up every torrent and stream at once (ideal for exit button)
- **API**: `startStream()` now accepts `maxCacheBytes` for a sliding window RAM cache limit
- **API**: `init()` now accepts `defaultSavePath` (defaults to system temp dir — no permission setup needed)
- **API**: `addMagnet()` and `addTorrentFile()` `savePath` is now optional (uses `defaultSavePath`)
- **README**: Fully rewritten with complete API documentation and code examples

## 1.1.0

- **Streaming**: Removed `sequential_download` flag (conflicts with `set_piece_deadline`)
- **Streaming**: Reduced readahead from 150 to 30 pieces to avoid excessive pre-buffering
- **Streaming**: `clear_piece_deadlines()` on seek + 1-second cooldown for faster seek response
- **Streaming**: Re-enabled uTP for incoming and outgoing connections (reaches more peers behind NAT)
- **Cache**: Configurable RAM cache limit with percentage-based sliding window eviction (10% safety buffer, min 5 pieces)
- **Save path**: Defaults to system temp directory on all platforms
- **CI**: Automatic GitHub Release creation with zipped native libraries on every build
- **pubspec**: Added `repository`, `issue_tracker`, and `topics` fields for pub.dev discoverability

## 1.0.0

- Initial release
- Native libtorrent 2.0 bindings via Dart FFI
- Built-in HTTP streaming server with byte-range support
- Windows, Linux, macOS, and Android support with prebuilt binaries (no build required)
- Auto-fetches best public trackers on startup
- Magnet link and .torrent file support
- Per-file streaming with automatic largest-file selection
- `torrentUpdates` and `streamUpdates` streams for reactive UI
- DHT, PEX, LSD peer discovery
