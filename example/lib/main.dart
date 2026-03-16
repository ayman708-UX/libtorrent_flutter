import 'package:flutter/material.dart';
import 'package:libtorrent_flutter/libtorrent_flutter.dart';

void main() async {
  WidgetsFlutterBinding.ensureInitialized();
  await LibtorrentFlutter.init();
  runApp(const ExampleApp());
}

class ExampleApp extends StatelessWidget {
  const ExampleApp({super.key});

  @override
  Widget build(BuildContext context) => MaterialApp(
    title: 'libtorrent_flutter Example',
    theme: ThemeData.dark(useMaterial3: true),
    home: const HomePage(),
  );
}

class HomePage extends StatefulWidget {
  const HomePage({super.key});
  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> {
  final _magnetCtrl = TextEditingController();
  final _engine = LibtorrentFlutter.instance;

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('libtorrent_flutter')),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(children: [
          Row(children: [
            Expanded(
              child: TextField(
                controller: _magnetCtrl,
                decoration: const InputDecoration(
                  hintText: 'magnet:?xt=urn:btih:...',
                  border: OutlineInputBorder(),
                ),
              ),
            ),
            const SizedBox(width: 8),
            FilledButton(
              onPressed: _addTorrent,
              child: const Text('Add'),
            ),
          ]),
          const SizedBox(height: 16),
          Expanded(
            child: StreamBuilder<Map<int, TorrentInfo>>(
              stream: _engine.torrentUpdates,
              initialData: _engine.torrents,
              builder: (context, snap) {
                final torrents = snap.data ?? {};
                if (torrents.isEmpty) {
                  return const Center(child: Text('No torrents'));
                }
                return ListView(
                  children: torrents.values.map(_buildTorrentTile).toList(),
                );
              },
            ),
          ),
        ]),
      ),
    );
  }

  void _addTorrent() {
    final magnet = _magnetCtrl.text.trim();
    if (magnet.isEmpty) return;

    // No save path needed — defaults to system temp
    _engine.addMagnet(magnet, null, true);
    _magnetCtrl.clear();
  }

  Widget _buildTorrentTile(TorrentInfo t) {
    return Card(
      child: ListTile(
        title: Text(t.name.isEmpty ? 'Loading...' : t.name),
        subtitle: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            LinearProgressIndicator(value: t.progress),
            const SizedBox(height: 4),
            Text('${t.state.label} · '
                '${(t.progress * 100).toStringAsFixed(1)}% · '
                '↓${formatSpeed(t.downloadRate)} · '
                '${t.numPeers} peers'),
          ],
        ),
        trailing: Row(mainAxisSize: MainAxisSize.min, children: [
          if (t.hasMetadata && !_engine.isStreaming(t.id))
            IconButton(
              icon: const Icon(Icons.play_arrow),
              tooltip: 'Stream',
              onPressed: () => _startStream(t.id),
            ),
          if (_engine.isStreaming(t.id))
            IconButton(
              icon: const Icon(Icons.stop, color: Colors.red),
              tooltip: 'Stop stream',
              onPressed: () => _engine.stopAllStreamsForTorrent(t.id),
            ),
          IconButton(
            icon: const Icon(Icons.delete_outline),
            onPressed: () => _engine.removeTorrent(t.id, deleteFiles: true),
          ),
        ]),
      ),
    );
  }

  void _startStream(int torrentId) {
    try {
      final info = _engine.startStream(torrentId);
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text('Streaming: ${info.url}'),
        duration: const Duration(seconds: 5),
      ));
    } catch (e) {
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(
        content: Text('Error: $e'),
        backgroundColor: Colors.red,
      ));
    }
  }

  @override
  void dispose() {
    _magnetCtrl.dispose();
    super.dispose();
  }
}
