# libtorrent_flutter

The only Flutter package wrapping **libtorrent 2.0** — the same C++ engine powering qBittorrent, Deluge, and Transmission. Add a magnet link, pick a file, get a stream URL. Works on Windows, Linux, macOS, iOS, and Android with zero setup — all native binaries are bundled.

```yaml
dependencies:
  libtorrent_flutter: ^1.6.8
```

---

## Why libtorrent?

Every other Flutter torrent package uses either a Java wrapper (Android-only) or a pure-Dart implementation that can't compete on speed. libtorrent 2.0 gives you:

- **`set_piece_deadline`** — tells the engine "I need this piece in 150ms", and it picks the fastest peer automatically. Far smarter than simple sequential download.
- **uTP support** — connects to peers behind NAT that other clients can't reach.
- **On-demand streaming** — focuses 100% of startup bandwidth on the first pieces. Container metadata (MP4 moov, MKV cues) is fetched reactively when the player requests it via HTTP range requests, cutting startup time to seconds.
- **DHT + PEX + LSD** — finds peers without trackers.

## Usage

### Initialize

```dart
import 'package:libtorrent_flutter/libtorrent_flutter.dart';

// Call once, before anything else. 
// Saves to system temp dir by default — no permission setup needed on any platform.
await LibtorrentFlutter.init();

// Advanced options:
await LibtorrentFlutter.init(
  downloadLimit: 5 * 1024 * 1024,  // 5 MB/s cap
  uploadLimit:   1 * 1024 * 1024,  // 1 MB/s cap
  defaultSavePath: '/my/custom/path',
  fetchTrackers: true,  // auto-inject best public trackers (default: true)
  pollInterval: Duration(milliseconds: 500),
);
```

### Add a torrent

```dart
final engine = LibtorrentFlutter.instance;

// From a magnet link — save path defaults to system temp
final id = engine.addMagnet('magnet:?xt=urn:btih:...');

// From a magnet link with a custom save path
final id = engine.addMagnet('magnet:?xt=urn:btih:...', '/downloads');

// From a .torrent file
final id = engine.addTorrentFile('/path/to/file.torrent');

// Remove a torrent
engine.removeTorrent(id, deleteFiles: true);

// Pause / resume
engine.pauseTorrent(id);
engine.resumeTorrent(id);
```

### Listen for status updates

```dart
engine.torrentUpdates.listen((Map<int, TorrentInfo> torrents) {
  final t = torrents[id]!;
  print('${t.name}');          // torrent name
  print('${t.state.label}');   // "Downloading", "Seeding", etc.
  print('${t.progress}');      // 0.0 – 1.0
  print('${t.downloadRate}');  // bytes/sec
  print('${t.uploadRate}');    // bytes/sec
  print('${t.numPeers}');
  print('${t.numSeeds}');
  print('${t.hasMetadata}');   // true once file list is available
  print('${t.totalDone}');     // bytes downloaded
  print('${t.totalWanted}');   // bytes total
});

// Or get a snapshot right now
final Map<int, TorrentInfo> current = engine.torrents;
```

### List files and start streaming

```dart
// Wait for hasMetadata == true, then:
final files = engine.getFiles(id);  // List<FileInfo>

for (final f in files) {
  print('[${f.index}] ${f.name}');
  print('  size: ${f.size} bytes');
  print('  streamable: ${f.isStreamable}');
}

// Stream the largest streamable file (auto-selected)
final StreamInfo stream = engine.startStream(id);

// Stream a specific file by index
final StreamInfo stream = engine.startStream(id, fileIndex: 2);

// With a RAM cache limit — useful on mobile
// A 500MB sliding window: the engine evicts old pieces as you watch,
// keeping a 10% safety buffer behind playback so it never stutters.
final StreamInfo stream = engine.startStream(
  id,
  fileIndex: 2,
  maxCacheBytes: 500 * 1024 * 1024,  // 500 MB
);

print(stream.url);  // http://127.0.0.1:PORT/stream/...
// Hand this URL to any player: media_kit, video_player, VLC, mpv, etc.
```

### Monitor stream status

```dart
engine.streamUpdates.listen((Map<int, StreamInfo> streams) {
  for (final s in streams.values) {
    print('url: ${s.url}');
    print('ready: ${s.isReady}');       // true = player can connect
    print('buffer: ${s.bufferPct}%');   // 0–100 buffered ahead
    print('fileSize: ${s.fileSize}');
    print('readHead: ${s.readHead}');   // current byte position
  }
});

// Check if a specific torrent is being streamed
final bool active = engine.isStreaming(id);

// Get info for a specific stream
final StreamInfo? info = engine.getStreamInfo(streamId);

// Stop a specific stream
engine.stopStream(streamId);

// Stop all streams for a torrent
engine.stopAllStreamsForTorrent(torrentId);
```

### Cleanup

```dart
// Clean up one torrent: stops its streams, removes it, deletes the files
engine.disposeTorrent(id);  // returns false if already gone

// Clean up everything — perfect for your exit button
engine.disposeAll();

// Full shutdown — calls disposeAll(), destroys native session
await engine.dispose();
```

### Speed limits

```dart
engine.setDownloadLimit(2 * 1024 * 1024);  // 2 MB/s
engine.setUploadLimit(512 * 1024);          // 512 KB/s
engine.setDownloadLimit(0);                 // back to unlimited
```

---

## Platform setup

No changes needed on Windows, Linux, and Android — everything is bundled.

**iOS** — add the network entitlement. In your `ios/Runner/Info.plist`, ensure your app allows outgoing connections (this is allowed by default on iOS, but if you use a custom `NetworkExtension` or have restrictive `NSAppTransportSecurity` settings, make sure HTTP to `localhost` is permitted).

**macOS** — add the network entitlement to your `macos/Runner/*.entitlements`:

```xml
<key>com.apple.security.network.client</key>
<true/>
```

**Android** — add to `AndroidManifest.xml`:

```xml
<uses-permission android:name="android.permission.INTERNET" />
```

**Android background streaming** — Android kills your app's process when it goes to the background unless you use a Foreground Service. When streaming, start one to keep the engine alive:

```dart
// Use flutter_foreground_task or a similar package
FlutterForegroundTask.startService(
  notificationTitle: 'Streaming',
  notificationText: 'Torrent engine running',
);
```

---

## How it works

A single C++ file (`torrent_bridge.cpp`) wraps libtorrent 2.0 and compiles to a native static/shared library on every platform. Dart talks to it via FFI — no platform channels, no Kotlin, no Swift.

When you call `startStream()`:
1. The engine sets staggered deadlines (0ms, 500ms, 1000ms…) on the first 5 pieces — piece 0 gets 100% of bandwidth first, so playback starts as soon as that single piece arrives
2. A tiny HTTP server starts on `127.0.0.1` on a random free port
3. The server responds to byte-range requests, blocking until each piece arrives from the swarm
4. When the player needs container metadata (moov atom, cues) from the end of the file, it sends a range request — the engine fetches those pieces on-demand at maximum priority
5. A background loop (100ms interval) continuously updates piece priorities with tight 2-piece critical + 5-piece hot windows around your playback position

The stream URL works with any player that handles HTTP range requests.

---

## License

GPL v3
