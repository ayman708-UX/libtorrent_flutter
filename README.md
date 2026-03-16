# libtorrent_flutter

Native [libtorrent 2.0](https://libtorrent.org) bindings for Flutter with a **built-in HTTP streaming server** for instant torrent video playback.

**Supports**: Windows, Linux, macOS, Android

## Features

- 🧲 **Magnet links & .torrent files** — add torrents from either source
- 📁 **File selection** — choose which files to download with priority control
- 🎬 **Built-in streaming server** — get an HTTP URL to play any file instantly in a video player
- ⚡ **Optimized for streaming** — piece prioritization, seek burst, minimal buffering, instant start
- 📊 **Live status updates** — reactive streams for download progress, speed, peers, etc.
- 🌐 **Auto tracker injection** — automatically fetches and injects best public trackers
- 🚫 **Stream-only mode** — stream without downloading (all bandwidth goes to playback)
- 🔒 **Zero external Dart dependencies** — only `dart:ffi` and `dart:io`

## Quick Start

```dart
import 'package:libtorrent_flutter/libtorrent_flutter.dart';

// Initialize the engine
await LibtorrentFlutter.init();

// Add a torrent
final torrentId = LibtorrentFlutter.instance.addMagnet(
  'magnet:?xt=urn:btih:...',
  '/path/to/save',
  streamOnly: true,  // Don't download, just stream
);

// Wait for metadata, then start streaming
LibtorrentFlutter.instance.torrentUpdates.listen((torrents) {
  final t = torrents[torrentId];
  if (t != null && t.hasMetadata) {
    final stream = LibtorrentFlutter.instance.startStream(torrentId);
    print('Play this URL: ${stream.url}');
    // Pass stream.url to media_kit, vlc, or any video player
  }
});

// Monitor progress
LibtorrentFlutter.instance.torrentUpdates.listen((torrents) {
  for (final t in torrents.values) {
    print('${t.name}: ${(t.progress * 100).toStringAsFixed(1)}% '
          '↓${formatSpeed(t.downloadRate)} ↑${formatSpeed(t.uploadRate)}');
  }
});

// Clean up
await LibtorrentFlutter.instance.dispose();
```

## API Reference

### Initialization

| Method | Description |
|--------|-------------|
| `LibtorrentFlutter.init()` | Initialize the engine with optional speed limits |
| `LibtorrentFlutter.instance` | Access the singleton instance |
| `instance.dispose()` | Shut down and clean up |

### Torrent Management

| Method | Description |
|--------|-------------|
| `addMagnet(uri, savePath)` | Add a magnet link, returns torrent ID |
| `addTorrentFile(path, savePath)` | Add a .torrent file, returns torrent ID |
| `removeTorrent(id)` | Remove a torrent (optionally delete files) |
| `pauseTorrent(id)` / `resumeTorrent(id)` | Pause/resume |
| `getFiles(id)` | List files in a torrent |
| `setFilePriorities(id, priorities)` | Set per-file download priority |

### Streaming

| Method | Description |
|--------|-------------|
| `startStream(torrentId, fileIndex: -1)` | Start streaming, returns `StreamInfo` with HTTP URL |
| `stopStream(streamId)` | Stop a stream |
| `isStreaming(torrentId)` | Check if a torrent is being streamed |

### Live Updates

| Stream | Description |
|--------|-------------|
| `torrentUpdates` | Emits `Map<int, TorrentInfo>` on every status change |
| `streamUpdates` | Emits `Map<int, StreamInfo>` on every stream update |

## Platform Setup

### Windows
Install libtorrent via vcpkg:
```bash
vcpkg install libtorrent:x64-windows
```

### Linux
```bash
sudo apt install libtorrent-rasterbar-dev
```

### macOS
```bash
brew install libtorrent-rasterbar
```

### Android
Place prebuilt libtorrent `.a` + Boost headers in:
```
android/src/main/jniLibs/{arm64-v8a,armeabi-v7a,x86_64}/libtorrent-rasterbar.a
android/src/main/include/  (libtorrent + boost headers)
```

## Architecture

The package is a **pure FFI plugin** — no platform channels, no Kotlin/Swift/ObjC needed. A single C++ source file (`torrent_bridge.cpp`) compiles on all platforms using `#ifdef` socket abstraction (Winsock on Windows, POSIX sockets elsewhere).

```
┌─────────────────────────┐
│  Your Flutter App       │
│  (media_kit / video)    │
├─────────────────────────┤
│  LibtorrentFlutter API  │  ← Dart (dart:ffi)
├─────────────────────────┤
│  torrent_bridge.cpp     │  ← C++ (cross-platform)
│  ├ libtorrent 2.0       │
│  └ HTTP streaming srv   │
└─────────────────────────┘
```

## License

MIT
