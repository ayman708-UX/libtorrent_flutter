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

    // Verify we can access the torrent/stream maps (empty)
    expect(LibtorrentFlutter.instance.torrents, isEmpty);
    expect(LibtorrentFlutter.instance.streams, isEmpty);

    // Clean up
    await LibtorrentFlutter.instance.dispose();
    expect(LibtorrentFlutter.isInitialized, isFalse);
  });
}
