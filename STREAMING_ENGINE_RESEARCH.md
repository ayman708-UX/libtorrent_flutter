# Torrent Streaming Engine Research

This report is based on a read-only pass over the current libtorrent_flutter codebase plus external research into TorrServer, torrest-cpp, Elementum, WebTorrent, torrent-stream/peerflix, qBittorrent, and the Stremio client-side streaming integration.

No code was changed.

## Bottom line

The current engine is not bad. It already has several serious streaming-oriented decisions:

- a native HTTP byte-range server
- piece deadlines
- direct piece reads through libtorrent
- head and tail prebuffering
- seek-triggered reprioritization
- aggressive session tuning

But it is still missing several things that make the best streaming stacks feel faster in practice:

- adaptive buffering instead of fixed heuristics
- short-lived hot piece memory caching
- stronger seek-mode scheduling
- better stale-priority cancellation
- bitrate-aware startup logic
- more nuanced piece priority ladders
- better handling of backward seeks and near-future seeks
- stronger per-peer request strategy under weak swarms

If the goal is "faster than TorrServer and closer to or better than Stremio", the path is not "make libtorrent more sequential". The path is:

1. lean harder into libtorrent's time-critical machinery
2. add a smarter streaming scheduler on top of it
3. add a small hot data cache and better seek heuristics
4. tune the session and deadlines using measured playback needs, not fixed constants

The best possible architecture here is a hybrid:

- libtorrent as the swarm/session engine
- deadline and priority orchestration specialized for media playback
- a short-lived hot piece cache for the active playhead and immediate seeks
- adaptive read windows based on bitrate, peer speed, and piece size
- a local HTTP interface, and optionally an HLS/transcode fallback path for problematic devices

## Research scope

### Local code reviewed

- src/torrent_bridge.cpp
- src/torrent_bridge.h
- lib/src/ffi_bindings.dart
- lib/src/libtorrent_flutter_base.dart
- lib/src/models.dart
- lib/libtorrent_flutter.dart
- macos/Classes/torrent_bridge_macos.cpp
- src/CMakeLists.txt
- CHANGELOG.md
- existing repository memory notes about prior streaming rewrites and libtorrent time-critical fixes

### External projects and docs reviewed

- libtorrent streaming and tuning docs
- YouROK/TorrServer
- i96751414/torrest-cpp
- elgatito/elementum
- webtorrent/webtorrent
- mafintosh/torrent-stream
- mafintosh/peerflix
- qbittorrent/qBittorrent
- stremio/stremio-video
- stremio/stremio-shell

## Current engine: what it is today

## Local architecture summary

The current implementation is a hybrid of two styles:

- TorrServer-inspired cache and reader abstractions
- lt2http or torrest-cpp style direct piece serving over a native HTTP range server

Key local components:

- SessionWrapper owns libtorrent::session, torrents, stream registry, and the alert thread.
- StreamEngine owns per-stream state, HTTP server state, preload state, current read head, and synchronization primitives.
- TorrCache, TorrReader, and CachePiece are locally implemented cache-reader abstractions inspired by TorrServer.
- The Dart layer is thin FFI plus polling wrappers.

## Current stream path

At a high level the current stream pipeline is:

1. select a file, or auto-select the largest streamable file
2. initialize stream state and cache structures
3. prioritize startup pieces and tail pieces
4. set piece deadlines
5. resume torrent and enable sequential download
6. run a local HTTP server
7. on each range request, reprioritize around the requested location
8. wait for piece completion or issue piece reads
9. serve data to the client piece-by-piece

Important local behaviors found in the code:

- head and tail are both protected and preloaded
- seek requests trigger a focused reset of deadlines and priorities
- the serving path keeps the current piece and a small number of following pieces hot
- played pieces are marked dont_download after use
- stream buffer estimation is based on contiguous buffered pieces and a fallback bitrate when no real bitrate is known
- the Dart wrapper polls stream and torrent state and currently discards native alerts instead of using them for richer runtime decisions

## Session tuning already present

The current engine is already much more tuned than a default libtorrent client. It uses aggressive values for:

- connection acquisition
- request pipeline depth
- peer timeouts and inactivity timeouts
- aio and hashing threads
- file pool and queued disk bytes
- peer discovery and NAT traversal
- qBittorrent fingerprint and user agent spoofing

This is good. It means the current gap is less about "turn random libtorrent knobs" and more about playback-specific scheduling.

## Where the current engine is already strong

Compared to a naive libtorrent wrapper, the current engine already does several right things:

- It has its own HTTP range server instead of depending on the app to stitch reads together.
- It preloads both the beginning and the end of the media file, which is important because many players probe the tail for metadata.
- It does active reprioritization on seek.
- It uses deadlines instead of relying only on file priorities.
- It exposes stream status and buffer information to Dart.
- It already moved away from a purely old-style TorrServer cache priority model after regressions.

That means the engine does not need a total rewrite. It needs a more advanced streaming scheduler.

## The biggest current weaknesses

## 1. Sequential download is still enabled

This is one of the biggest strategic problems.

Libtorrent's own streaming guidance is clear: plain sequential download is not the best tool for streaming. The better model is time-critical piece scheduling. Sequential mode can help in simple cases, but it also fights smarter deadline-driven behavior and can reduce swarm efficiency when seeking or when peers are uneven.

If the engine wants top-tier playback behavior, the core policy should be:

- normal mode: deadline and priority driven
- not: globally sequential by default

Sequential download is a blunt instrument. Streaming needs a scalpel.

## 2. Read-ahead and buffering are too fixed

The current engine uses fixed-style heuristics in several places, including a fixed readahead window and fallback bitrate assumptions.

That is not enough for real-world streaming because the optimal window depends on:

- piece size
- file bitrate
- current peer count
- actual download rate
- whether the player is startup buffering, steady-state playing, or seeking
- device/player behavior

The fastest streaming engines do not use one static window for every torrent.

## 3. No proper hot piece cache with expiration

The current engine reads pieces and synchronizes through alerts, but it does not appear to keep a strong short-lived in-memory hot piece cache comparable to torrest-cpp's piece store.

That matters for:

- immediate repeated reads from the player
- fast short forward seeks
- backward seeks within the recent playback region
- metadata probes and player re-reads

Without a hot piece cache, the engine pays extra latency for data it just touched.

## 4. Played pieces are dropped too aggressively

The current server marks played pieces as dont_download after use.

That helps keep focus on the forward path, but it is too aggressive for real players. Players do not only move forward once. They often:

- rewind a few seconds
- re-request overlapping ranges
- probe around keyframes
- make small back-and-forth jumps when seeking

Dropping pieces immediately after play hurts the exact scenarios that make a streaming engine feel responsive.

The engine should keep a small trailing hot window, not delete behind the playhead instantly.

## 5. Seek mode is reactive but not rich enough

The current engine does reprioritize on seek, which is good, but the strategy is still relatively simple.

Best-in-class seek behavior usually needs:

- immediate clearing of stale deadlines and stale priority regions
- strong target-piece and target-neighborhood promotion
- a separate seek mode with more aggressive peer acquisition and deadline density
- short-term duplicate requests for late critical pieces when needed
- a tiny backward/forward neighborhood cache around the seek destination

Today the current engine is closer to "reprioritize quickly" than to a dedicated seek profile.

## 6. No bitrate-aware startup policy

The engine reports buffer seconds using a fallback bitrate when a real one is unknown, but startup policy itself is not strongly driven by actual media bitrate.

TorrServer explicitly probes media and uses ffprobe during preload. Stremio's client-side integration also relies on a streaming server that can provide video metadata. Without accurate bitrate knowledge, startup decisions are always weaker than they should be.

The engine should know, as early as possible:

- approximate bitrate
- container and stream structure
- whether the player is likely to tail-probe
- what amount of buffered bytes corresponds to safe startup

## 7. The old dynamic cache priority model is mostly disabled

This is understandable because previous versions caused regressions, but it also means a lot of the adaptive cache logic that made TorrServer feel responsive is currently inactive.

That does not mean the old code should be restored as-is. It means a safer version of that idea should come back:

- active reader windows
- multiple priority bands
- bounded cache retention
- delayed cleanup
- seek-aware reprioritization

## 8. The HTTP serving path is still relatively heavyweight

The current server uses a thread-per-connection style and serves data piece-by-piece with synchronization on alerts and condition variables.

That is workable, but it is not the lowest-latency or most scalable design. The main problem is not raw throughput. The main problem is control-path cost during rapid seeking and repeated short range reads.

The current design is optimized enough for single-client use, but not yet shaped like a purpose-built ultra-low-latency streaming service.

## 9. Observability is not strong enough

The Dart wrapper polls status, but alerts are intentionally discarded. That makes it harder to build smarter heuristics or even verify whether they are working.

If the goal is to beat TorrServer and Stremio, the engine needs better runtime visibility into:

- deadline misses
- seek latency
- piece wait times
- critical piece timeouts
- cache hit rates
- backward seek hits
- stale-priority cancellation counts
- per-peer usefulness

Without this, tuning becomes guesswork.

## External comparison

## TorrServer

### What TorrServer is doing well

TorrServer is not just "sequential download plus HTTP". Its strength is the combination of:

- reader-aware cache windows
- explicit readahead distribution
- responsive reader mode
- configurable cache size
- startup preload and tail preload
- active priority cleanup and reassignment
- protection of file beginning and ending regions

Important code-level findings:

- It exposes ReaderReadAHead, PreloadCache, CacheSize, ConnectionsLimit, and ResponsiveMode as first-class settings.
- It uses file.NewReader and can enable a Responsive reader mode instead of always waiting on full piece completion.
- It computes reader ranges from cache capacity and a configurable forward-weighted readahead percentage.
- It actively clears priorities outside live reader ranges.
- It uses a priority ladder around active readers: current piece, next piece, readahead region, and further lookahead.
- It preserves a beginning and ending keep zone of roughly 8 to 16 MB when cleaning cache.
- It preloads start and end regions and probes media through ffprobe during preload.

### Why TorrServer often feels better than the current engine

The biggest practical reasons are:

- it keeps a more explicit streaming cache model alive
- it is more aware of active reader windows
- it is more deliberate about holding useful nearby data
- it treats preload and bitrate discovery as core playback features

In short: TorrServer behaves like a media server, not just a torrent reader.

### What to copy from TorrServer

- reader-window based retention
- bitrate probing early in preload
- configurable forward-weighted readahead
- protection of start and end regions during cleanup
- optional responsive mode equivalent

### What not to copy blindly

- the exact old cache-priority implementation, because your own history already showed it can fight libtorrent in bad ways if ported naively

The right move is to reintroduce the idea in a safer form, not restore the exact past design.

## torrest-cpp

Torrest-cpp is one of the closest structural comparisons to the current engine.

### What torrest-cpp does well

- It keeps a short-lived in-memory piece store for read_piece data.
- It expires old cached pieces after a configurable duration.
- It models per-file buffering state explicitly.
- It buffers both head and tail regions.
- It uses top priority plus zero deadlines for startup buffer pieces.
- Its Reader applies a gradient of priorities and increasing deadlines ahead of the current read point.

Important findings:

- default buffer_size is 20 MB
- piece_wait_timeout is configurable
- piece_expiration is configurable and defaults to a short window
- File::buffer promotes both startup and tail pieces
- Reader::set_pieces_priorities promotes the requested span hard, then promotes readahead with a softer deadline ramp

### Why torrest-cpp matters for this project

Your engine is already closer to torrest-cpp than to classic TorrServer. The fastest route forward is probably not to become more like old TorrServer. It is to combine torrest-cpp's hot piece behavior with TorrServer's smarter reader-window retention.

### What to copy from torrest-cpp

- short-lived hot piece cache
- explicit buffering-state accounting
- stronger deadline ramp for readahead ahead of the current target
- per-file buffer modeling instead of only raw piece counts

## Elementum

Elementum is important because it is built for media playback and has years of tuning around real player behavior.

### What Elementum does well

- It treats player access patterns as first-class scheduling inputs.
- It has a strong notion of demandPieces and awaitingPieces.
- It uses a periodic prioritization loop.
- It assigns a multi-band priority ladder around readers.
- It explicitly handles the Kodi behavior of reading from both the start and the end of a file.
- It can use memory storage to make playback smoother.

Important findings:

- startup buffering promotes both pre-buffer and post-buffer regions
- it uses deadlines for startup buffer pieces
- it periodically reprioritizes every second
- it can force reannounce and DHT announce when starting buffer-heavy playback
- it derives partial piece progress from the download queue
- it suppresses redundant reprioritizations when priorities are unchanged

### Why Elementum often feels robust

Elementum behaves like a playback scheduler sitting on top of libtorrent, not just a thin client. That is the key lesson.

### What to copy from Elementum

- explicit separate sets for demand pieces, awaiting pieces, and buffered pieces
- periodic reprioritization with stale-update suppression
- stronger startup pre-buffer and post-buffer handling
- richer per-piece progress tracking from the download queue
- optional memory-backed hot path

## WebTorrent and torrent-stream/peerflix

These are not libtorrent projects, but they are extremely valuable for streaming-specific scheduling ideas.

### What they do well

- stream creation itself creates high-priority temporary selections
- very small critical windows are used to force the next needed pieces
- they use measured peer speed to decide request behavior
- they support hotswapping slow peers off active blocks
- they naturally create a new high-priority selection for each seeked range
- they can switch between sequential and rarest-first strategy instead of living in one mode forever

Important findings:

- torrent-stream and WebTorrent both maintain a distinct concept of critical pieces
- they use speed-based request ranking
- they use hotswap logic to replace slow peers on active blocks
- they keep stream selections separate from low-priority general selections
- WebTorrent dynamically sizes request pipelines based on observed wire speed

### Why this matters even though it is not libtorrent

Because the user-visible problem is the same: get the next seconds of media as fast and reliably as possible. Their piece scheduler logic is often more playback-aware than many libtorrent wrappers.

### What to copy from WebTorrent/torrent-stream

- explicit critical window separate from the general selection window
- speed-aware request policy
- hotswap-like behavior for slow peers on urgent pieces
- separate stream selections for active ranges
- switch between strong sequential bias near the playhead and more swarm-friendly selection away from it

## qBittorrent

qBittorrent is not a streaming server, but it is a very good reference for stable session tuning.

### Useful qBittorrent lessons

It exposes and uses many settings that are relevant to a streaming-focused libtorrent session:

- request queue size
- async IO threads
- hashing threads
- file pool size
- piece extent affinity
- suggest mode
- send buffer watermarks
- socket send and receive buffers
- connection speed
- mixed mode algorithm
- allow_multiple_connections_per_ip
- peer turnover controls
- disk IO mode choices

### Why qBittorrent matters here

It is a good sanity reference for which settings are worth exposing or benchmarking, especially around:

- disk behavior
- socket buffering
- request queues
- peer turnover
- extent affinity and suggest mode

Your engine already tunes some of this. It should probably tune more of it explicitly and benchmark the results.

## Stremio

Stremio is harder to compare directly because the accessible open repos here are not the full standalone torrent-streaming backend source in the same way TorrServer or torrest-cpp are.

### What the accessible Stremio code clearly shows

- The shell launches a separate streaming server process, server.js.
- The video layer talks to a local streaming server, not directly to the torrent engine.
- The client can convert magnet or infoHash inputs into local streaming-server URLs like /{infoHash}/{fileIdx}.
- The client can ask the streaming server for stats.json and opensubHash.
- The client can switch to HLSv2 URLs when direct playback is not suitable.
- The streaming integration includes media probing and conditional transcoding fallback.

### What this implies

Stremio's advantage is not necessarily that its torrent picker alone is magical. Its advantage is that it behaves like a complete playback delivery pipeline:

- torrent acquisition
- local streaming endpoint
- media probing
- HLS fallback
- optional transcoding path
- client-side playback adaptation

### Lesson to copy from Stremio

If you want to beat Stremio on playback quality across devices, the solution is not only better piece scheduling. The solution is also better delivery abstraction. A pure range server is great, but some devices and players benefit from an HLS fallback or a more media-aware delivery path.

## What the current engine is missing, ranked by importance

## Tier 1: highest impact

### 1. Replace global sequential mode with a true streaming scheduler

This is the single most important change.

The scheduler should:

- promote the current playback window hard
- keep a readahead window with softer priority bands
- keep a small trailing retention window
- enter a separate seek mode when a large jump occurs
- return to steady-state streaming mode after seek stabilization

### 2. Add a short-lived hot piece cache

Target behavior:

- keep recently read pieces in memory for a few seconds
- expire them automatically
- use them for repeated reads, short backward seeks, and overlapping player requests

This is one of the clearest gaps versus torrest-cpp and a major contributor to perceived snappiness.

### 3. Make startup buffer sizing bitrate-aware

Startup should be based on:

- measured or probed bitrate
- current download rate
- current peer availability
- piece size
- player tail-probe behavior

Do not treat all media as if one fixed buffer window is enough.

### 4. Improve seek mode substantially

Seek mode should do all of the following:

- immediately clear stale deadlines and stale active windows
- promote the target piece and target neighborhood harder than today
- keep a tiny neighborhood behind and ahead of the seek point
- increase urgency for the first playable keyframe region
- optionally trigger extra peer discovery or aggressive connection behavior for the seek burst

### 5. Stop dropping played-behind data immediately

Replace immediate dont_download of played pieces with:

- a trailing keep window in seconds or bytes
- a trailing expiration timer
- larger retention immediately after a seek

This will materially improve rewind and micro-seek behavior.

## Tier 2: strong next improvements

### 6. Reintroduce safer reader-window based priority management

Not the old broken version, but a bounded version that:

- tracks active readers
- merges their windows
- assigns stable priority bands
- limits how often priorities are rewritten
- never lets stale regions keep top priority too long

### 7. Add richer priority bands and deadline ramps

Current piece plus next two pieces is too simple.

Better structure:

- piece 0 in the active window: top priority, deadline now
- next few pieces: very high priority, short deadlines
- next window: high priority, medium deadlines
- further readahead: normal priority, no harsh deadlines

This is closer to torrest-cpp and Elementum.

### 8. Add more session settings to the benchmark matrix

Especially:

- whole_pieces_threshold
- piece_extent_affinity
- suggest_mode
- send_buffer_watermark
- send_buffer_low_watermark
- send_buffer_watermark_factor
- socket send and receive buffers
- allow_multiple_connections_per_ip
- mixed_mode_algorithm
- file_pool_size
- max_queued_disk_bytes

Not all of these will help, but several are worth benchmarking for streaming workloads.

### 9. Improve telemetry and runtime observability

Track at least:

- time to first playable byte
- time to first frame or safe startup
- p50 and p95 seek latency
- deadline miss rate
- repeated-read cache hit rate
- backward-seek hit rate
- peer usefulness distribution
- number of priority rewrites per minute

## Tier 3: advanced moves

### 10. Add optional HLS delivery mode

For some players and platforms, an HLS facade can outperform raw range streaming in perceived reliability.

This is where Stremio's architecture is ahead.

### 11. Consider asynchronous serving or reduced thread cost

A dedicated event-driven serving path can reduce control-path overhead under many short range requests.

### 12. Consider peer-quality scoring for urgent pieces

Maintain a per-peer score based on:

- recent deadline success
- queue latency
- actual delivery speed for critical pieces
- timeouts or late deliveries

Then bias urgent requests toward reliable peers.

## Specific recommendations for faster initial playback

## Must-have

### Use real bitrate as early as possible

Do not wait until later status reporting. Probe or infer bitrate during preload, the way TorrServer does.

### Startup should prefetch by seconds, not only by pieces

For example, target a startup threshold like:

- min of X seconds of media or Y MB
- adjusted upward if peer speed is unstable
- adjusted downward for strong swarms and low bitrate files

### Keep tail prefetch, but only as much as the player actually needs

Tail reads are important, but do not overinvest in them at the expense of startup pieces.

### Burst the first window aggressively

The first playable region should have:

- the hardest priorities
- the shortest deadlines
- the strongest peer pressure

### Force early metadata confidence

Early media metadata reduces bad startup decisions.

## Nice-to-have

### Warm a short trailing window at startup too

Some players probe oddly even during startup. A tiny trailing or neighboring window can reduce weird early stalls.

### Use startup mode distinct from steady-state mode

Do not treat them the same. Startup wants fast first frame. Steady-state wants smooth future buffer growth.

## Specific recommendations for faster seek

## Must-have

### A separate seek state machine

The engine should have an explicit state like:

- startup
- steady streaming
- seeking
- rebuffer recovery

Each state should have its own window size, deadline pattern, and cleanup policy.

### Clear stale work immediately on seek

Any old non-critical deadlines or elevated priorities should be torn down fast when the user jumps far.

### Promote a neighborhood, not a single point

The engine should prioritize:

- the exact seek target
- the following key playback window
- a very small buffer behind it

### Keep recent pieces for instant reverse and repeated seeks

This is where the hot cache and trailing retention window pay off the most.

## Strong next steps

### Duplicate late critical requests when needed

Libtorrent's time-critical logic already understands the principle that late urgent pieces may justify duplicate requests. The engine should align with that behavior and not dilute it with too much non-critical traffic.

### Favor peers with proven critical-piece performance

For seek recovery, average peers are not enough. You want the peers that actually deliver on time.

## Concrete feature gap list versus the best competitors

Compared with TorrServer, the current engine is weaker in:

- reader-window based cache retention
- bitrate-aware preload and media probing
- configurable forward-weighted readahead
- keeping near-useful data instead of aggressively dropping it

Compared with torrest-cpp, the current engine is weaker in:

- hot read_piece caching with expiration
- explicit buffering-state accounting
- more deliberate deadline ramps beyond the immediate playhead

Compared with Elementum, the current engine is weaker in:

- explicit demand and awaiting piece modeling
- periodic playback-oriented reprioritization
- richer priority ladders around active readers
- playback-specific heuristics refined around real player behavior

Compared with WebTorrent and torrent-stream, the current engine is weaker in:

- critical-window modeling
- speed-aware request scheduling
- hotswap of slow peers on urgent blocks
- automatic range-specific stream selections

Compared with Stremio as a product pipeline, the current engine is weaker in:

- delivery abstraction
- media probing integration
- HLS fallback path
- broader device-playback compatibility strategy

## What I would do first if the goal is pure performance

## Phase 1: biggest wins with the least architectural risk

1. Remove or sharply reduce dependence on sequential_download for active streaming.
2. Add a short-lived hot piece cache with expiration.
3. Replace immediate played-piece dropping with trailing retention.
4. Make startup buffer sizing bitrate-aware.
5. Introduce an explicit seek mode with stronger cleanup and priority ramps.

These are the highest-value changes.

## Phase 2: make scheduling actually smarter

1. Add distinct state machines for startup, steady-state, seek, and recovery.
2. Add reader-window based priority bands.
3. Track deadline misses and critical-piece delivery quality per peer.
4. Benchmark qBittorrent-style session settings like piece_extent_affinity, suggest_mode, and send buffer watermarks.

## Phase 3: beat Stremio-class user experience

1. Add media probing before or during preload.
2. Add optional HLS delivery for problematic playback targets.
3. Improve HTTP serving cost model and maybe move away from thread-per-connection over time.

## A realistic target architecture for "fastest torrent streaming"

If the target is genuinely world-class streaming behavior, the architecture should look like this:

### Swarm layer

- libtorrent session tuned for low-latency acquisition
- time-critical piece strategy favored over pure sequential mode
- explicit peer-quality scoring for urgent delivery

### Scheduler layer

- playback state machine: startup, steady, seek, recovery
- adaptive windows by bitrate, peer speed, and piece size
- critical window plus softer readahead bands
- stale-priority cancellation on every large seek

### Data layer

- short-lived hot piece memory cache
- bounded trailing retention window
- optional disk and memory hybrid policy

### Delivery layer

- HTTP range serving for normal clients
- optional HLS fallback for difficult clients
- optional transcode path only when strictly needed

### Observability layer

- hard metrics for startup and seek latency
- cache hit metrics
- deadline miss metrics
- per-peer usefulness metrics

That is the design that can realistically beat both "good enough libtorrent wrappers" and older streaming servers.

## Recommended benchmark plan

You should not trust any tuning change unless it wins on a fixed benchmark set.

Measure at least:

- time to metadata
- time to first HTTP byte
- time to first playable frame
- time to safe playback start
- p50 seek latency
- p95 seek latency
- backward seek latency
- rebuffer count in first 5 minutes
- average buffer growth rate
- deadline miss count
- cache hit rate for repeated reads

Run those against:

- current engine
- TorrServer
- Stremio desktop streaming path

Use several swarm conditions:

- strong swarm
- medium swarm
- weak swarm
- very large pieces
- low bitrate media
- high bitrate media
- single-file torrents
- multi-file torrents

Without this, it is too easy to optimize for one case and regress another.

## Direct answers to the core question

## Is the current engine fundamentally wrong?

No.

It already has the right broad direction: native HTTP streaming, deadlines, seek reprioritization, and direct piece serving.

## Is it currently at the level of TorrServer or Stremio for streaming feel?

Not yet.

The missing pieces are not basic torrent features. They are playback-specific scheduling and delivery refinements.

## What is the single most important conceptual change?

Stop thinking in terms of "download in order" and think in terms of "serve the next playable seconds with the lowest possible latency while protecting near-future seeks".

That means:

- less blunt sequential behavior
- more adaptive deadline-driven scheduling
- more hot-data retention

## What would most likely produce the biggest real-world improvement?

This combination:

1. remove global sequential bias
2. add hot piece cache with expiry
3. keep a small trailing window instead of dropping played pieces immediately
4. add real seek mode and bitrate-aware startup mode

That combination is the most likely to produce immediate noticeable wins in startup and seek.

## Final verdict

The current engine is already closer to a serious streaming engine than a normal FFI wrapper around libtorrent. The problem is that it is in the middle ground:

- smarter than a basic client
- not yet as playback-specialized as the best stream-focused systems

TorrServer feels better because it treats the torrent as a cache-backed media reader.

Torrest-cpp feels good because it has cleaner hot piece handling and explicit buffering semantics.

Elementum feels battle-tested because it was tuned around real player behavior.

WebTorrent and torrent-stream feel clever because their request scheduler is unapologetically stream-oriented.

Stremio feels better as a product because it combines torrent acquisition with a smarter delivery pipeline.

If you want the fastest torrent engine in this project, the next step is not a total rewrite. It is a focused evolution into a true playback scheduler with:

- adaptive startup
- aggressive seek mode
- hot data retention
- stronger peer and deadline logic
- better media-aware delivery

That is the path most likely to beat the current engine by a lot.