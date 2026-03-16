// Integration test that verifies the native library loads and the engine starts.
// This runs on the native platform (not a mobile emulator).

import 'dart:io';
import 'package:flutter_test/flutter_test.dart';
import 'package:libtorrent_flutter/libtorrent_flutter.dart';

void main() {
  test('LibtorrentFlutter initializes and reports version', () async {
    // Initialize the engine
    await LibtorrentFlutter.init(
      listenInterface: '',
      downloadLimit: 0,
      uploadLimit: 0,
      fetchTrackers: false, // Don't hit network in CI
    );

    expect(LibtorrentFlutter.isInitialized, isTrue);

    final version = LibtorrentFlutter.instance.libraryVersion;
    expect(version, isNotEmpty);
    expect(version, contains('2.')); // libtorrent 2.x

    print('✅ Engine initialized — libtorrent version: $version');

    // Verify we can access the torrent/stream maps (empty)
    expect(LibtorrentFlutter.instance.torrents, isEmpty);
    expect(LibtorrentFlutter.instance.streams, isEmpty);

    // Clean up
    await LibtorrentFlutter.instance.dispose();
    expect(LibtorrentFlutter.isInitialized, isFalse);

    print('✅ Engine disposed cleanly');
  });

  test('LibtorrentFlutter add magnet and get status', () async {
    await LibtorrentFlutter.init(fetchTrackers: false);

    final savePath = Platform.isWindows
        ? '${Platform.environment['TEMP']}\\lt_test'
        : '/tmp/lt_test';

    // Add a well-known Ubuntu torrent (safe, public, and always seeded)
    final id = LibtorrentFlutter.instance.addMagnet(
      'magnet:?xt=urn:btih:3289a56c7a3241f14501e5bd271e31991a8dc5c0&dn=ubuntu-24.04-desktop-amd64.iso',
      savePath,
      streamOnly: true,
    );

    expect(id, greaterThan(0));
    print('✅ Torrent added with ID: $id');

    // Wait briefly for it to appear in the polling
    await Future.delayed(const Duration(seconds: 2));

    // Remove the torrent (we just tested adding, not downloading)
    LibtorrentFlutter.instance.removeTorrent(id, deleteFiles: true);
    print('✅ Torrent removed');

    await LibtorrentFlutter.instance.dispose();
  });
}
