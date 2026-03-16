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

    // Test a real magnet link: Ubuntu 24.04 Desktop
    // Very highly seeded official TCP/HTTPS trackers with tiny metadata requirements
    const magnetUri = 'magnet:?xt=urn:btih:eeca4d7a8ce29edfed4b41de452b489d8db1af00&dn=ubuntu-24.04.1-desktop-amd64.iso'
        '&tr=https%3A%2F%2Ftorrent.ubuntu.com%2Fannounce'
        '&tr=https%3A%2F%2Fipv6.torrent.ubuntu.com%2Fannounce';
    final savePath = Directory.systemTemp.createTempSync('lt_test_').path;

    final torrentId = LibtorrentFlutter.instance.addMagnet(magnetUri, savePath);
    expect(torrentId, greaterThanOrEqualTo(0));

    int totalSize = 0;
    String lastState = '';

    final sub = LibtorrentFlutter.instance.torrentUpdates.listen((updates) {
      final info = updates[torrentId];
      if (info != null) {
        if (info.totalWanted > 0) totalSize = info.totalWanted;
        if (info.state.name != lastState) {
          lastState = info.state.name;
          print('==> Torrent State: $lastState | Size: $totalSize bytes');
        }
      }
    });

    // Wait up to 30 seconds for Size to populate (metadata arrived)
    for (int i = 0; i < 60; i++) {
        if (totalSize > 0) {
            print('✅ Torrent metadata downloaded! Total Size: $totalSize bytes');
            break;
        }
        await Future.delayed(const Duration(milliseconds: 500));
    }
    
    expect(totalSize, greaterThan(0), reason: 'Engine should have fetched metadata and calculated torrent size');

    // Verify torrents map reflects the update
    expect(LibtorrentFlutter.instance.torrents.containsKey(torrentId), isTrue);

    // Clean up
    await sub.cancel();
    LibtorrentFlutter.instance.removeTorrent(torrentId, deleteFiles: true);

    // Verify we can access the torrent/stream maps (empty)
    expect(LibtorrentFlutter.instance.torrents, isEmpty);
    expect(LibtorrentFlutter.instance.streams, isEmpty);

    // Clean up
    await LibtorrentFlutter.instance.dispose();
    expect(LibtorrentFlutter.isInitialized, isFalse);
  });
}
