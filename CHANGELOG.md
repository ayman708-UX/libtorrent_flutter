# Changelog

## 1.7.3

- **FIX (CRASH)**: Fixed `SIGABRT` crash on stream shutdown — `cache->close()` destroyed `TorrReader` objects and their mutexes while HTTP client threads were still using them. Reordered shutdown to join all threads before closing the cache
- **FIX**: Added guard in `close_reader()` to skip cleanup if the cache is already closed, preventing double-free of reader mutexes
- **FIX**: `read_piece_data()` now checks `active` flag in its wait predicate — threads wake immediately on shutdown instead of blocking for up to 10 seconds
- **FIX**: `lt_destroy_session` no longer holds `streams_mu` while joining threads, preventing deadlock with the alert thread
- **HARDENING**: All thread entry points (`handle_connection`, client thread lambda), shutdown paths (`lt_stop_stream`, `lt_destroy_session`), and cache cleanup (`close_reader`, `TorrCache::close`) wrapped in try-catch — the native library will never crash the app, even on unexpected shutdown races

## 1.7.2

- **FIX (CRASH)**: Fixed `SIGABRT` crash (`pthread_mutex_destroy called on a destroyed mutex`) when stopping a stream — HTTP client handler threads were detached and continued accessing `StreamEngine` mutexes after the engine was destroyed. Client threads are now tracked, joined, and their sockets force-closed during shutdown
- **FIX**: Fixed potential double-close of the HTTP listen socket during stream shutdown
- **FIX**: `lt_destroy_session` now properly closes listen sockets and joins client threads before destroying stream engines

## 1.7.1

- **Streaming**: Replaced TorrServer-style async cache pipeline with lt2http-style direct storage reads — pieces are read straight from libtorrent disk storage instead of going through an intermediate RAM cache, eliminating seek delays
- **Streaming**: Narrowed priority window from 25-piece gradient (Now/Next/Readahead/High/Normal) to just current piece + 2 ahead — focuses 100% of bandwidth on what the player actually needs
- **Streaming**: 100% forward cache — everything ahead of playback, nothing behind. A 64 MB cache can now stream a 60 GB file by aggressively evicting played pieces
- **FIX**: Fixed downloads being killed during gaps between player HTTP requests — `clear_priority_impl()` was setting ALL pieces to `dont_download` when no readers existed
- **FIX**: Protected current reader piece and next piece from cache eviction, preventing evict-download-evict loops at piece boundaries
- **FIX**: Removed detached threads that spawned `clean_pieces()` and `get_removable_pieces()` on every piece write/read — these overrode `serve_range` priorities and stopped downloads
- **Build (Windows)**: Reduced DLL size from 9.4 MB to 146 KB

## 1.7.0

- **FIX (macOS)**: Fixed crash on launch — `libssl.3.dylib` / `libcrypto.3.dylib` were referenced via hardcoded Homebrew paths (`/opt/homebrew/opt/openssl@3/lib/...`), which don't exist on end-user machines. The dylib now uses `@loader_path/` references and bundles OpenSSL alongside the plugin
- **Build (macOS)**: Added `install_name_tool` POST_BUILD step in CMakeLists.txt to automatically rewrite OpenSSL load paths during compilation
- **Build (macOS)**: Added `build_macos.sh` — one-command script that builds the dylib, copies OpenSSL from Homebrew, fixes all dylib cross-references, and places files in `macos/` and `prebuilt/macos/universal/`
- **Packaging (macOS)**: Updated podspec to vendor `libssl.3.dylib` and `libcrypto.3.dylib` alongside the main plugin dylib
- **Streaming**: Improved seek performance — 5 deadline pieces at deadline=0, unlimited unchoke slots, optimized `cancel_non_critical()` timing
- **Engine**: `unchoke_slots_limit` changed from 4 to unlimited (-1) — fixes issue where only 4 peers would upload despite hundreds connected

## 1.6.9

- **Engine**: Complete rewrite of the C++ streaming engine — new priority system, HTTP server, and piece management
- **Streaming**: 5-level priority gradient (NOW=7, NEXT=6, READAHEAD=5, BACK=1, SKIP=0) — only downloads the playback window and head/tail metadata, not the entire file
- **Streaming**: `set_piece_deadline()` for time-critical downloads — pieces requested from multiple peers simultaneously, slow requests auto-cancelled
- **Streaming**: Adaptive readahead window (3–50 pieces) grows with smooth playback, resets on seek
- **Streaming**: 8-piece backward buffer keeps recently-played pieces available for quick rewinds
- **Streaming**: Configurable RAM piece cache via `maxCacheBytes` — from 128MB for smart TVs to 2GB for desktops. Sliding window eviction with safe-zone protection around the playhead
- **Seeking**: Threaded HTTP connection handler — new connections preempt old ones instantly via socket close, no more blocking the accept loop
- **Seeking**: `clear_piece_deadlines()` on seek — immediately stops downloading for old position and redirects bandwidth to new target
- **Seeking**: Aggressive seek deadlines (6 pieces at 200ms spacing) at the new position
- **Seeking**: `seek_generation` counter aborts blocked piece waits within milliseconds
- **Performance**: Condition-variable-based piece waiting — zero CPU polling
- **Performance**: Alert-driven piece tracking via `piece_finished_alert` — no status polling for piece availability
- **Performance**: Piece data cache with safe-zone (5 pieces around playhead never evicted), smart trim on seek preserves nearby cached data
- **Tuning**: `max_failcount=3` — peers survive seek transitions instead of being dropped
- **Tuning**: `peer_turnover` 5% every 30s — faster replacement of slow peers
- **Tuning**: Connection flood on startup (`connection_speed=200`, `torrent_connect_boost=200`) for fast peer acquisition
- **API**: `startStream()` now accepts `maxCacheBytes` to control RAM usage (0 = default ~128MB)
- **API**: Backward-compatible `bufferPct` getter on `StreamInfo`
- **API**: New `StreamState` enum (idle, buffering, ready, seeking, error) and `streamState` field on `StreamInfo`
- **API**: New fields on `StreamInfo`: `bufferSeconds`, `bufferPieces`, `readaheadWindow`, `activePeers`, `downloadRate`
- **Compat**: All existing API methods preserved — `addMagnet()`, `startStream()`, `stopStream()`, `disposeTorrent()`, etc. work unchanged

## 1.6.8

- **iOS**: Built XCFramework with both device (arm64-iphoneos) and simulator (arm64+x86_64-iphonesimulator) slices — iOS Simulator now works on Apple Silicon and Intel Macs
- **iOS**: Replaced fat `.a` binary with `.xcframework` — Apple's recommended approach for multi-platform static libraries
- **iOS**: Updated podspec to use `vendored_frameworks` with SDK-conditional `-force_load` linker flags
- **CI**: Build workflow now compiles libtorrent + torrent_bridge for three iOS targets (arm64 device, arm64 simulator, x86_64 simulator) and packages them via `xcodebuild -create-xcframework`
- **Publish**: Dropped Android x86_64 prebuilt from pub package to stay under 100 MB limit



## 1.6.6

IOS fixes

## 1.6.6

- **FIX (iOS)**: Added `SystemConfiguration` framework dependency to podspec — resolves `Undefined symbol: _SCNetworkReachabilityCreateWithAddress` linker errors when building for iOS release

## 1.6.5

- **CRITICAL FIX**: Concurrent connections (head + tail) were killing each other — every new HTTP connection overwrote a global request ID, instantly aborting the other. Players that open two connections (VLC, mpv, ExoPlayer) would loop endlessly until enough pieces were cached. Replaced with a seek-generation counter that only aborts stale connections on actual seeks
- **FIX**: Tail/metadata range requests no longer hijack `read_head` — the priority loop stays focused on the playback position instead of jumping to the end of the file
- **Streaming**: LRU piece cache (48 entries) — avoids repeated disk reads for recently served pieces
- **Streaming**: Head + tail preload on stream start — first 5 pieces get staggered deadlines, last ~512KB is pre-fetched for container metadata (MKV cues, MP4 moov)
- **Streaming**: Dynamic readahead window (8–40 pieces) scales with download speed, targeting ~15s of buffer
- **Streaming**: Wider inline lookahead (6 pieces) with staggered deadlines during serve
- **Seeking**: Cache invalidation on seek — evicts stale pieces, preserves tail pieces
- **Seeking**: Wider post-seek focus (5 queued pieces instead of 2)
- **Engine**: Fixed `suggest_mode` — was set as bool instead of the correct enum value
- **Engine**: Fixed `torrent_connect_boost` exceeding documented max (255)
- **Engine**: `mixed_mode_algorithm` set to `peer_proportional` — stops starving uTP peers behind NAT
- **Engine**: Added DHT bootstrap nodes, `piece_extent_affinity`, `auto_sequential`
- **Engine**: Tuned send buffer watermarks, peer turnover, socket buffer sizes
- **Serve**: DLNA headers for smart TV compatibility
- **Serve**: CORS OPTIONS preflight support
- **Serve**: 2MB send buffer, 1MB send chunks
- **Serve**: Case-insensitive HTTP Range header parsing
- **Serve**: Adaptive priority loop interval (100ms buffering, 250ms steady)

## 1.6.4

- **CRITICAL FIX**: HTTP server sent `Connection: keep-alive` but closed the socket after every request — VLC trusted the keep-alive promise, tried to reuse the dead connection, got a broken pipe, and rendered a black screen. Fixed by switching to `Connection: close`, which correctly tells the player to open a new TCP connection for each range request. This is the standard model used by other torrent streaming servers.
- **Serve**: HEAD requests now return headers and close cleanly without falling through to the data-serving path
- **Serve**: Removed redundant `while` loop around single-use connection handling — the code now clearly reflects the one-request-per-connection model

## 1.6.3

- **Streaming**: Instant start — staggered piece deadlines (0ms, 500ms, 1000ms…) on startup instead of flat 0ms for all. libtorrent's time-critical picker now funnels all bandwidth to piece 0 first, so playback can begin as soon as 1 piece arrives instead of waiting for 8
- **Streaming**: Shrunk critical window from 6 pieces → 2, hot window from 9 → 5, readahead from 15 → 6 — concentrates bandwidth on the most urgent data per libtorrent's streaming docs: "any block you request that is not urgent takes away bandwidth from urgent pieces"
- **Streaming**: Buffer percentage now based on the 2-piece critical window — reports "ready" faster since it no longer waits for 6 pieces
- **Seeking**: Tighter seek focus — 2 pieces at staggered near-zero deadlines + 2 more at 200ms stagger (was 4 all at 0ms), reducing bandwidth dilution on seek
- **Seeking**: Seek cooldown reduced from 1000ms → 100ms — priority loop resumes almost immediately after a seek instead of staying blind for a full second
- **Seeking**: Wait timeout reduced from 120s → 30s — fails faster on dead peers instead of hanging
- **Performance**: Priority loop now runs every 100ms (was 200ms) for faster reaction to playback position changes
- **Performance**: libtorrent `tick_interval` set to 100ms (was 500ms default) — internal scheduler reacts 5x faster to deadline changes and priority updates
- **Serve**: Inline lookahead reduced from 5 → 3 pieces with 100ms stagger (was 10ms) — less bandwidth competition with the current piece

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
