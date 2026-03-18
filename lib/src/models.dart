// Models for libtorrent_flutter

/// Torrent download state.
enum TorrentState {
  error,
  unknown,
  checkingFiles,
  downloadingMetadata,
  downloading,
  finished,
  seeding,
  allocating,
  checkingResume,
}

/// Convert native integer state to [TorrentState].
TorrentState stateFromInt(int v) {
  switch (v) {
    case -2: return TorrentState.error;
    case  0: return TorrentState.checkingFiles;
    case  1: return TorrentState.downloadingMetadata;
    case  2: return TorrentState.downloading;
    case  3: return TorrentState.finished;
    case  4: return TorrentState.seeding;
    case  5: return TorrentState.allocating;
    case  6: return TorrentState.checkingResume;
    default: return TorrentState.unknown;
  }
}

extension TorrentStateX on TorrentState {
  String get label {
    switch (this) {
      case TorrentState.error:               return 'Error';
      case TorrentState.unknown:             return 'Unknown';
      case TorrentState.checkingFiles:       return 'Checking files';
      case TorrentState.downloadingMetadata: return 'Getting metadata';
      case TorrentState.downloading:         return 'Downloading';
      case TorrentState.finished:            return 'Finished';
      case TorrentState.seeding:             return 'Seeding';
      case TorrentState.allocating:          return 'Allocating';
      case TorrentState.checkingResume:      return 'Checking resume';
    }
  }

  bool get isActive =>
      this == TorrentState.downloading ||
      this == TorrentState.downloadingMetadata ||
      this == TorrentState.allocating ||
      this == TorrentState.checkingFiles ||
      this == TorrentState.checkingResume;

  bool get isDone =>
      this == TorrentState.finished || this == TorrentState.seeding;
}

/// Information about a torrent.
class TorrentInfo {
  final int id;
  final String name;
  final String savePath;
  final String errorMsg;
  final TorrentState state;
  final double progress;
  final int downloadRate;
  final int uploadRate;
  final int totalDone;
  final int totalWanted;
  final int totalUploaded;
  final int numPeers;
  final int numSeeds;
  final bool isPaused;
  final bool isFinished;
  final bool hasMetadata;
  final int queuePosition;

  const TorrentInfo({
    required this.id, required this.name, required this.savePath,
    required this.errorMsg, required this.state, required this.progress,
    required this.downloadRate, required this.uploadRate,
    required this.totalDone, required this.totalWanted,
    required this.totalUploaded, required this.numPeers,
    required this.numSeeds, required this.isPaused,
    required this.isFinished, required this.hasMetadata,
    required this.queuePosition,
  });

  TorrentInfo copyWith({
    String? name, String? savePath, String? errorMsg,
    TorrentState? state, double? progress,
    int? downloadRate, int? uploadRate,
    int? totalDone, int? totalWanted, int? totalUploaded,
    int? numPeers, int? numSeeds,
    bool? isPaused, bool? isFinished, bool? hasMetadata, int? queuePosition,
  }) => TorrentInfo(
    id: id,
    name: name ?? this.name,
    savePath: savePath ?? this.savePath,
    errorMsg: errorMsg ?? this.errorMsg,
    state: state ?? this.state,
    progress: progress ?? this.progress,
    downloadRate: downloadRate ?? this.downloadRate,
    uploadRate: uploadRate ?? this.uploadRate,
    totalDone: totalDone ?? this.totalDone,
    totalWanted: totalWanted ?? this.totalWanted,
    totalUploaded: totalUploaded ?? this.totalUploaded,
    numPeers: numPeers ?? this.numPeers,
    numSeeds: numSeeds ?? this.numSeeds,
    isPaused: isPaused ?? this.isPaused,
    isFinished: isFinished ?? this.isFinished,
    hasMetadata: hasMetadata ?? this.hasMetadata,
    queuePosition: queuePosition ?? this.queuePosition,
  );

  @override
  String toString() => 'TorrentInfo(id=$id, name=$name, state=$state, '
      'progress=${(progress * 100).toStringAsFixed(1)}%)';
}

/// Information about a file within a torrent.
class FileInfo {
  final int index;
  final String name;
  final String path;
  final int size;
  final bool isStreamable;

  const FileInfo({
    required this.index, required this.name, required this.path,
    required this.size, required this.isStreamable,
  });

  @override
  String toString() => 'FileInfo(index=$index, name=$name, '
      'size=$size, streamable=$isStreamable)';
}

/// Information about an active stream (includes the HTTP URL for playback).
class StreamInfo {
  final int id;
  final int torrentId;
  final int fileIndex;

  /// HTTP URL to pass to a video player (e.g. `http://127.0.0.1:PORT/stream/...`).
  final String url;
  final int fileSize;
  final int readHead;

  /// 0-100 indicating how full the immediate buffer is.
  final int bufferPct;
  final bool isReady;
  final bool isActive;

  const StreamInfo({
    required this.id, required this.torrentId, required this.fileIndex,
    required this.url, required this.fileSize, required this.readHead,
    required this.bufferPct, required this.isReady, required this.isActive,
  });

  @override
  String toString() => 'StreamInfo(id=$id, url=$url, buffer=$bufferPct%, '
      'ready=$isReady, active=$isActive)';
}

// ─── Formatting Utilities ─────────────────────────────────────────────────────

/// Format [bytes] as a human-readable string (e.g. "1.5 GB").
String formatBytes(int bytes, {int decimals = 1}) {
  if (bytes <= 0) return '0 B';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  int i = 0;
  double v = bytes.toDouble();
  while (v >= 1024 && i < units.length - 1) { v /= 1024; i++; }
  return '${v.toStringAsFixed(decimals)} ${units[i]}';
}

/// Format bytes-per-second as a speed string.
String formatSpeed(int bps) => '${formatBytes(bps)}/s';

/// Format estimated time remaining for a torrent.
String formatEta(TorrentInfo t) {
  if (t.downloadRate <= 0) return '∞';
  final remaining = t.totalWanted - t.totalDone;
  if (remaining <= 0) return 'Done';
  final secs = remaining ~/ t.downloadRate;
  if (secs < 60) return '${secs}s';
  if (secs < 3600) return '${secs ~/ 60}m ${secs % 60}s';
  return '${secs ~/ 3600}h ${(secs % 3600) ~/ 60}m';
}
