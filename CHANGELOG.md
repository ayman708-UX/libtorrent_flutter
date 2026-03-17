# Changelog

## 1.6.2

- **CRITICAL FIX**: Removed `force_recheck()` from cache eviction — it was re-hashing the ENTIRE torrent, marking ALL pieces (including currently streaming ones) as unknown, killing the stream
- **Streaming**: Capped readahead window to 50% of cache capacity — prevents downloading so far ahead that the cache overflows and evicts data at the current playback position
- **Streaming**: Hard safety floor — cache eviction NEVER evicts anything within 5 pieces of the current playhead, regardless of cache pressure

## 1.6.1

- **Streaming**: Fixed serial pipeline stall after seek — `serve_range` now pre-primes the next 5 pieces with priority 7 and staggered deadlines before each `wait_for_piece` call, so the swarm downloads them in parallel instead of one at a time
- **Streaming**: Reduced seek cooldown from 3s → 1s — the new serve_range lookahead covers the gap, so the priority loop can resume sooner and set broader readahead windows

## 1.6.0

- **Streaming**: Rewrote priority system using torrest-cpp's proven priority-only-upgrade pattern — never downgrade piece priorities, staggered `i*10ms` deadlines for the hot window
- **Streaming**: Increased critical window from 3 → 5 pieces at deadline=0ms for faster initial playback
- **Streaming**: Total seek focus — on seek, all piece priorities are wiped and ONLY the seek position + 3 pieces get deadline=0ms with a 3-second cooldown before the priority loop resumes
- **Streaming**: `wait_for_piece` timeout increased from 15s → 120s to prevent "Stream ends prematurely" errors during slow torrent startup
- **Streaming**: Reduced readahead buffer from 30 → 15 pieces to concentrate bandwidth closer to the playhead
- **Engine**: Added `no_recheck_incomplete_resume` — skips file recheck on resume for faster startup
- **Engine**: Added `allow_multiple_connections_per_ip` — connects to seedboxes, VPNs, and shared NAT peers
- **Engine**: Added `peer_connect_timeout=3s` for faster peer handshakes
- **Engine**: Tuned timeouts to stop peer churn (`piece_timeout` 2→5s, `request_timeout` 2→4s, `peer_timeout` 5→10s) — proven by libtorrent issue #7666, torrest, and Elementum
- **Engine**: `whole_pieces_threshold` increased 5 → 20, forcing fast peers to complete whole pieces instead of scattering blocks

## 1.5.0

- **Streaming**: Removed speculative tail preloading — the engine no longer downloads the last 4MB of a file at startup. Modern players (MPV, VLC, etc.) fetch container metadata (MP4 moov atom, MKV cues) on-demand via HTTP range requests, which the built-in server already supports
- **Streaming**: Head-only preload now sets 8 pieces at flat deadline=0ms instead of staggered 30ms intervals, focusing 100% of startup bandwidth on the beginning of the file
- **Streaming**: Removed file anchor logic from the priority loop that re-prioritized the last 2 pieces every 200ms, which competed with the playhead for bandwidth
- **Performance**: Startup time reduced from ~30–60s to ~5–15s by eliminating bandwidth competition between head and tail piece downloads

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
