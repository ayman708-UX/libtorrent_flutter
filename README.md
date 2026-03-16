# libtorrent_flutter

Native libtorrent 2.0 bindings for Flutter. Add a magnet link, pick a file, get a stream URL — that's it.

Works on Windows, Linux, macOS, and Android. All native libraries are bundled, no system dependencies needed.

## What it does

- Adds torrents from magnet links or .torrent files
- Lists files, lets you pick which ones to download
- Spins up a local HTTP server that streams any file from the torrent
- Hands you a URL like `http://127.0.0.1:PORT/stream/...` — pass it to any video player
- Auto-fetches best public trackers for faster peer discovery
- Optimized piece prioritization for low-latency seeking

## Usage

### Basic streaming

```dart
import 'package:libtorrent_flutter/libtorrent_flutter.dart';

void main() async {
  // Start the engine (saves to system temp by default)
  await LibtorrentFlutter.init();

  // Add a magnet link — no save path needed
  final id = LibtorrentFlutter.instance.addMagnet(
    'magnet:?xt=urn:btih:dd8255ecdc7ca55fb0bbf81323d87062db1f6d1c',
  );

  // Listen for status updates — wait for metadata
  LibtorrentFlutter.instance.torrentUpdates.listen((torrents) {
    final t = torrents[id];
    if (t == null) return;

    print('${t.name} — ${t.state.name} — '
        '${(t.progress * 100).toStringAsFixed(1)}% — '
        '${(t.downloadRate / 1024).toStringAsFixed(0)} KB/s');

    if (t.hasMetadata) {
      // Start streaming — pass maxCacheBytes to limit disk/memory usage
      final stream = LibtorrentFlutter.instance.startStream(
        id,
        maxCacheBytes: 500 * 1024 * 1024,  // 500MB sliding window
      );
      print('Stream URL: ${stream.url}');
      // Pass stream.url to media_kit, VLC, mpv, or whatever you use
    }
  });
}
```

### Picking a specific file

```dart
// After metadata is available, list all files
final files = LibtorrentFlutter.instance.getFiles(torrentId);
for (final f in files) {
  print('[${f.index}] ${f.name} — ${(f.size / 1024 / 1024).toStringAsFixed(1)} MB');
}

// Stream file at index 2
final stream = LibtorrentFlutter.instance.startStream(torrentId, fileIndex: 2);
print(stream.url);
```

### Monitoring streams

```dart
LibtorrentFlutter.instance.streamUpdates.listen((streams) {
  for (final s in streams.values) {
    print('Stream ${s.id}: ready=${s.isReady}, buffer=${s.bufferPct}%');
  }
});
```

### Cleanup

```dart
// Stop streaming
LibtorrentFlutter.instance.stopStream(streamId);

// Remove torrent (and delete files)
LibtorrentFlutter.instance.removeTorrent(torrentId, deleteFiles: true);

// Shut down the engine
await LibtorrentFlutter.instance.dispose();
```

## Install

Add to your `pubspec.yaml`:

```yaml
dependencies:
  libtorrent_flutter:
    git:
      url: https://github.com/ayman708-UX/libtorrent_flutter.git
```

That's it. All native binaries for every platform are already bundled in the package. No vcpkg, no brew, no apt — just add and go.

## How it works

The whole thing is a single C++ file (`torrent_bridge.cpp`) that compiles everywhere using platform-specific socket ifdefs. Dart talks to it through FFI — no platform channels, no Kotlin, no Swift.

When you call `startStream()`, it boots a tiny HTTP server on localhost that serves the torrent data as a seekable byte-range response. Any video player that can handle HTTP range requests will work with it.

## License

Source Available — see [LICENSE](LICENSE) for details.
You can use this package in your apps. You cannot modify, fork, or redistribute the source code.
