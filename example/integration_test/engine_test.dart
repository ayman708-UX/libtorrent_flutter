// Integration test that verifies the native library loads and the engine starts.

import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:libtorrent_flutter/libtorrent_flutter.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('LibtorrentFlutter initializes and reports version', (tester) async {
    // Initialize the engine
    await LibtorrentFlutter.init(
      listenInterface: '',
      downloadLimit: 0,
      uploadLimit: 0,
      fetchTrackers: false,
    );

    expect(LibtorrentFlutter.isInitialized, isTrue);

    final version = LibtorrentFlutter.instance.libraryVersion;
    expect(version, isNotEmpty);
    expect(version, contains('2.'));

    // Test a real magnet link (Ubuntu Server 23.10 as an example or Big Buck Bunny)
    // We'll use Big Buck Bunny since it's universally highly seeded and small/legal.
    const magnetUri = 'magnet:?xt=urn:btih:dd8255ecdc7ca55fb0bbf81323d87062db1f6d1c&dn=Big+Buck+Bunny'
        '&tr=udp%3A%2F%2Ftracker.opentrackr.org%3A1337%2Fannounce'
        '&tr=udp%3A%2F%2F9.rarbg.com%3A2810%2Fannounce'
        '&tr=udp%3A%2F%2Ftracker.openbittorrent.com%3A80%2Fannounce'
        '&tr=http%3A%2F%2Ftracker.opentrackr.org%3A1337%2Fannounce';
    final savePath = Directory.systemTemp.createTempSync('lt_test_').path;

    final torrentId = LibtorrentFlutter.instance.addMagnet(magnetUri, savePath);
    expect(torrentId, greaterThanOrEqualTo(0));

    // Wait for the engine to acknowledge and process the torrent addition
    // By checking the first broadcast update, we verify that:
    // 1. The FFI bindings and C++ singleton successfully added the magnet payload
    // 2. The engine correctly parsed the 'dn' (display name) parameter
    // 3. The background polling C++ -> Dart isolate callback stream is alive
    final updates = await LibtorrentFlutter.instance.torrentUpdates.firstWhere(
      (m) => m.containsKey(torrentId),
    );

    final info = updates[torrentId]!;
    expect(info.name, contains('Big Buck Bunny'),
        reason: 'The engine should have parsed the display name from the magnet URI.');
    expect(info.state, isNotNull);

    // Verify torrents map reflects the update
    expect(LibtorrentFlutter.instance.torrents.containsKey(torrentId), isTrue);

    // Clean up
    LibtorrentFlutter.instance.removeTorrent(torrentId, deleteFiles: true);

    // Verify we can access the torrent/stream maps (empty)
    expect(LibtorrentFlutter.instance.torrents, isEmpty);
    expect(LibtorrentFlutter.instance.streams, isEmpty);

    // Clean up
    await LibtorrentFlutter.instance.dispose();
    expect(LibtorrentFlutter.isInitialized, isFalse);
  });
}
