// torrent_bridge.cpp — libtorrent 2.0 streaming engine
// Cross-platform: Windows, Linux, macOS, Android, iOS

#ifdef _WIN32
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601
  #endif
#endif

#include "torrent_bridge.h"

#include <libtorrent/session.hpp>
#include <libtorrent/settings_pack.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/version.hpp>
#include <libtorrent/file_storage.hpp>
#include <libtorrent/download_priority.hpp>

// ── cross-platform sockets ──────────────────────────────────────────────────────
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET socket_t;
  #define SOCKET_INVALID  INVALID_SOCKET
  #define CLOSESOCKET(s)  ::closesocket(s)
  #define INIT_SOCKETS()  { WSADATA _w; ::WSAStartup(MAKEWORD(2,2), &_w); }
  typedef int socklen_t_;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <signal.h>
  typedef int socket_t;
  #define SOCKET_INVALID  (-1)
  #define CLOSESOCKET(s)  ::close(s)
  #define INIT_SOCKETS()  { signal(SIGPIPE, SIG_IGN); }
  typedef socklen_t socklen_t_;
#endif

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <deque>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cinttypes>

namespace lt  = libtorrent;
namespace chr = std::chrono;

// ── error handling ──────────────────────────────────────────────────────────────
static thread_local std::string g_last_error;
static void set_err(const std::string& s) { g_last_error = s; }

// ── file extension checks ───────────────────────────────────────────────────────
static bool is_streamable(const std::string& name) {
    static const char* exts[] = {
        ".mkv",".mp4",".avi",".mov",".wmv",".flv",".webm",
        ".m4v",".ts",".m2ts",".mpg",".mpeg",".mp3",".aac",
        ".flac",".opus",".ogg",".wav", nullptr
    };
    auto d = name.rfind('.');
    if (d == std::string::npos) return false;
    std::string e = name.substr(d);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    for (int i = 0; exts[i]; ++i) if (e == exts[i]) return true;
    return false;
}

static std::string get_mime(const std::string& name) {
    auto d = name.rfind('.');
    if (d == std::string::npos) return "video/mp4";
    std::string e = name.substr(d);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    if (e == ".mkv")  return "video/x-matroska";
    if (e == ".mp4" || e == ".m4v") return "video/mp4";
    if (e == ".avi")  return "video/x-msvideo";
    if (e == ".mov")  return "video/quicktime";
    if (e == ".webm") return "video/webm";
    if (e == ".ts" || e == ".m2ts") return "video/mp2t";
    if (e == ".flv")  return "video/x-flv";
    if (e == ".wmv")  return "video/x-ms-wmv";
    if (e == ".mp3")  return "audio/mpeg";
    if (e == ".flac") return "audio/flac";
    if (e == ".aac")  return "audio/aac";
    if (e == ".ogg" || e == ".opus") return "audio/ogg";
    return "application/octet-stream";
}

// ── fill torrent status struct ──────────────────────────────────────────────────
static void fill_status(lt_torrent_status& out, int64_t id,
                        const lt::torrent_status& st)
{
    out.id    = id;
    out.state = static_cast<int32_t>(st.state);

    // streaming sets per-file priorities, which can cause premature "finished"
    if ((st.state == lt::torrent_status::finished ||
         st.state == lt::torrent_status::seeding) && st.progress < 0.999f)
        out.state = LT_STATE_DOWNLOADING;

    out.progress      = st.progress;
    out.download_rate = st.download_rate;
    out.upload_rate   = st.upload_rate;
    out.total_done    = st.total_done;
    out.total_wanted  = st.total_wanted;
    out.total_uploaded = st.total_payload_upload;
    out.num_peers     = st.num_peers;
    out.num_seeds     = st.num_seeds;
    out.num_pieces    = (int32_t)st.num_pieces;

    int have = 0;
    for (int i = 0; i < (int)st.pieces.size(); ++i)
        if (st.pieces.get_bit(lt::piece_index_t(i))) have++;
    out.pieces_done = (int32_t)have;

    out.is_paused   = (st.flags & lt::torrent_flags::paused) ? 1 : 0;
    out.is_finished = (st.progress >= 0.999f && st.is_finished) ? 1 : 0;
    out.has_metadata = st.has_metadata ? 1 : 0;

    int qp = static_cast<int>(st.queue_position);
    out.queue_position = (qp < 0) ? -1 : qp;

    // prefer torrent_info name when metadata is available
    std::string name = st.name;
    if (st.has_metadata) {
        auto ti = st.handle.torrent_file();
        if (ti) name = ti->name();
    }
    std::strncpy(out.name, name.c_str(), sizeof(out.name) - 1);
    out.name[sizeof(out.name) - 1] = 0;

    std::strncpy(out.save_path, st.save_path.c_str(), sizeof(out.save_path) - 1);
    out.save_path[sizeof(out.save_path) - 1] = 0;

    if (st.errc) {
        std::string e = st.errc.message();
        std::strncpy(out.error_msg, e.c_str(), sizeof(out.error_msg) - 1);
        out.error_msg[sizeof(out.error_msg) - 1] = 0;
        out.state = LT_STATE_ERROR;
    } else {
        out.error_msg[0] = 0;
    }
}

// ── read result for async piece reads ───────────────────────────────────────────
struct ReadResult {
    std::vector<char> data;
    bool ok = false;
};

// ── alert record for dart queue ─────────────────────────────────────────────────
struct AlertRecord {
    int           type;
    lt_torrent_id torrent_id;
    std::string   message;
};

// ── stream engine ───────────────────────────────────────────────────────────────
struct StreamEngine {
    lt_stream_id  id;
    lt_torrent_id torrent_id;
    int           file_index;

    lt::torrent_handle                      handle;
    std::shared_ptr<const lt::torrent_info> ti;

    int64_t file_offset;
    int64_t file_size;
    int     piece_length;
    int     start_piece;
    int     end_piece;
    int     total_pieces;

    // playback
    std::atomic<int64_t> read_head{0};
    std::atomic<int32_t> stream_state{LT_STREAM_BUFFERING};
    std::atomic<int32_t> seek_generation{0};

    // adaptive readahead — grows with smooth playback, resets on seek
    int64_t contiguous_bytes{0};
    int     readahead_window{3};
    static constexpr int MIN_READAHEAD = 3;
    static constexpr int MAX_READAHEAD = 50;

    // 8MB head/tail for container metadata (moov, cues, etc.)
    static constexpr int64_t PROTECT_BYTES = 8 * 1024 * 1024;
    int head_end_piece;
    int tail_start_piece;

    // HTTP server
    std::thread       server_thread;
    std::atomic<bool> running{false};
    socket_t          listen_sock = SOCKET_INVALID;
    int               server_port = 0;

    // active client — closed by accept loop to abort old connection on seek
    std::mutex        client_mu;
    socket_t          active_client = SOCKET_INVALID;
    std::thread       client_thread;

    // piece availability — signaled by alert thread on piece_finished
    std::mutex              piece_mu;
    std::condition_variable piece_cv;
    std::set<int>           pieces_have;

    // piece data reads — signaled by alert thread on read_piece_alert
    std::mutex              read_mu;
    std::condition_variable read_cv;
    std::unordered_map<int, ReadResult> read_results;

    // piece data cache — sliding window around playback position
    std::mutex cache_mu;
    std::unordered_map<int, std::vector<char>> piece_cache;
    size_t max_cache_pieces = 64; // default, overridden by max_cache_bytes
    static constexpr int SAFE_ZONE = 5; // pieces around playhead never evicted
    static constexpr int BACK_BUFFER = 8; // keep this many behind playback

    void cache_put(int piece, const std::vector<char>& data) {
        std::lock_guard<std::mutex> lk(cache_mu);
        if (piece_cache.size() >= max_cache_pieces) {
            int play = byte_to_piece(read_head.load());
            int worst = -1, worst_dist = -1;
            for (auto& kv : piece_cache) {
                // never evict pieces in the safe zone around playback
                int d = std::abs(kv.first - play);
                if (d <= SAFE_ZONE) continue;
                if (d > worst_dist) { worst_dist = d; worst = kv.first; }
            }
            if (worst >= 0) piece_cache.erase(worst);
        }
        piece_cache[piece] = data;
    }

    bool cache_get(int piece, std::vector<char>& out) {
        std::lock_guard<std::mutex> lk(cache_mu);
        auto it = piece_cache.find(piece);
        if (it == piece_cache.end()) return false;
        out = it->second;
        return true;
    }

    // trim cache to pieces near the new position (called on seek)
    void cache_trim(int new_play) {
        std::lock_guard<std::mutex> lk(cache_mu);
        int half = (int)max_cache_pieces / 2;
        for (auto it = piece_cache.begin(); it != piece_cache.end(); ) {
            if (std::abs(it->first - new_play) > half)
                it = piece_cache.erase(it);
            else
                ++it;
        }
    }

    void cache_clear() {
        std::lock_guard<std::mutex> lk(cache_mu);
        piece_cache.clear();
    }

    // telemetry (filled from handle.status() at query time)
    std::atomic<bool> active{true};

    int byte_to_piece(int64_t off) const {
        if (piece_length <= 0) return start_piece;
        return (int)((file_offset + off) / piece_length);
    }

    // byte range for a piece, clamped to this file
    void piece_file_range(int p, int64_t& beg, int64_t& end_) const {
        int64_t ps = (int64_t)p * piece_length;
        int64_t pe = ps + piece_length;
        beg  = std::max(ps, file_offset) - file_offset;
        end_ = std::min(pe, file_offset + file_size) - file_offset;
    }

    std::string make_url() const {
        std::string hash = "0000000000000000000000000000000000000000";
        try {
            if (handle.is_valid()) {
                auto ih = handle.info_hashes();
                std::stringstream ss;
                if (ih.has_v1()) {
                    for (auto b : ih.v1)
                        ss << std::hex << std::setw(2) << std::setfill('0') << (int)(uint8_t)b;
                } else if (ih.has_v2()) {
                    for (auto b : ih.v2)
                        ss << std::hex << std::setw(2) << std::setfill('0') << (int)(uint8_t)b;
                }
                hash = ss.str().substr(0, 40);
            }
        } catch (...) {}
        return "http://127.0.0.1:" + std::to_string(server_port)
             + "/stream/" + hash + "/" + std::to_string(file_index);
    }

    // signal piece_finished from alert thread
    void on_piece_finished(int p) {
        {
            std::lock_guard<std::mutex> lk(piece_mu);
            pieces_have.insert(p);
        }
        piece_cv.notify_all();
    }

    // signal read_piece_alert from alert thread
    void on_piece_read(int p, const char* data, int size, bool ok) {
        {
            std::lock_guard<std::mutex> lk(read_mu);
            ReadResult r;
            if (ok && data && size > 0) {
                r.data.assign(data, data + size);
                r.ok = true;
            }
            read_results[p] = std::move(r);
        }
        read_cv.notify_all();
    }

    void on_hash_failed(int p) {
        std::lock_guard<std::mutex> lk(piece_mu);
        pieces_have.erase(p);
        // re-download at highest priority
        try { handle.piece_priority(lt::piece_index_t(p), lt::top_priority); } catch (...) {}
    }

    void wake_all() {
        piece_cv.notify_all();
        read_cv.notify_all();
    }
};

// ── forward declarations ────────────────────────────────────────────────────────
struct SessionWrapper;
static void update_priorities(StreamEngine* s);
static void run_http_server(SessionWrapper* sw, StreamEngine* stream);

// ── session wrapper ─────────────────────────────────────────────────────────────
struct SessionWrapper {
    lt::session session;

    std::mutex mu;
    std::unordered_map<int64_t, lt::torrent_handle> handles;
    std::unordered_set<int64_t> ephemeral_torrents;
    std::atomic<int64_t> next_id{1};

    std::mutex streams_mu;
    std::unordered_map<int64_t, std::unique_ptr<StreamEngine>> streams;
    std::atomic<int64_t> next_stream_id{1};

    // alert thread — sole consumer of session.pop_alerts()
    std::thread       alert_thread;
    std::atomic<bool> alert_running{false};

    // push callback (called from alert thread)
    lt_alert_callback  dart_callback  = nullptr;
    void*              dart_user_data = nullptr;
    std::mutex         cb_mu;

    // pull queue (for lt_poll_alerts)
    std::mutex              dart_queue_mu;
    std::deque<AlertRecord> dart_queue;

    explicit SessionWrapper(lt::settings_pack sp) : session(std::move(sp)) {}

    int64_t id_for_handle(const lt::torrent_handle& h) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& kv : handles)
            if (kv.second == h) return kv.first;
        return -1;
    }

    void start_alert_thread() {
        alert_running = true;
        alert_thread = std::thread([this]() { process_alerts(); });
    }

    void process_alerts() {
        while (alert_running.load()) {
            try {
                if (!session.wait_for_alert(lt::milliseconds(100)))
                    continue;
                std::vector<lt::alert*> alerts;
                session.pop_alerts(&alerts);

                // track which streams need priority recalc
                std::unordered_set<StreamEngine*> dirty_streams;

                for (auto* a : alerts) {
                    if (!a) continue;
                    try {
                        // read_piece_alert → route to stream
                        if (auto* rpa = lt::alert_cast<lt::read_piece_alert>(a)) {
                            int p = static_cast<int>(rpa->piece);
                            std::lock_guard<std::mutex> slk(streams_mu);
                            for (auto& kv : streams) {
                                auto& s = kv.second;
                                if (!s->active || s->handle != rpa->handle) continue;
                                s->on_piece_read(p,
                                    rpa->error ? nullptr : rpa->buffer.get(),
                                    rpa->error ? 0 : rpa->size,
                                    !rpa->error);
                                break;
                            }
                            continue; // don't forward to dart
                        }

                        // piece_finished_alert → signal waiters, mark priority dirty
                        if (auto* pfa = lt::alert_cast<lt::piece_finished_alert>(a)) {
                            int p = static_cast<int>(pfa->piece_index);
                            std::lock_guard<std::mutex> slk(streams_mu);
                            for (auto& kv : streams) {
                                auto& s = kv.second;
                                if (!s->active || s->handle != pfa->handle) continue;
                                if (p >= s->start_piece && p <= s->end_piece) {
                                    s->on_piece_finished(p);
                                    dirty_streams.insert(s.get());
                                }
                                break;
                            }
                        }

                        // hash_failed_alert → remove from available, re-request
                        else if (auto* hf = lt::alert_cast<lt::hash_failed_alert>(a)) {
                            int p = static_cast<int>(hf->piece_index);
                            std::lock_guard<std::mutex> slk(streams_mu);
                            for (auto& kv : streams) {
                                auto& s = kv.second;
                                if (!s->active || s->handle != hf->handle) continue;
                                if (p >= s->start_piece && p <= s->end_piece)
                                    s->on_hash_failed(p);
                                break;
                            }
                        }

                        // metadata_received → pause and zero-out file priorities
                        // prevents downloading before user picks a file
                        else if (auto* mra = lt::alert_cast<lt::metadata_received_alert>(a)) {
                            try {
                                auto ti = mra->handle.torrent_file();
                                if (ti) {
                                    int nf = ti->files().num_files();
                                    std::vector<lt::download_priority_t> p(
                                        (size_t)nf, lt::dont_download);
                                    mra->handle.prioritize_files(p);
                                    mra->handle.pause();
                                }
                            } catch (...) {}
                        }

                        // queue alert for dart (pull mode)
                        lt_torrent_id tid = -1;
                        if (auto* ta = dynamic_cast<lt::torrent_alert*>(a))
                            tid = id_for_handle(ta->handle);

                        {
                            std::lock_guard<std::mutex> ql(dart_queue_mu);
                            if (dart_queue.size() < 2048)
                                dart_queue.push_back({a->type(), tid, a->message()});
                        }

                        // push callback if registered
                        {
                            std::lock_guard<std::mutex> cl(cb_mu);
                            if (dart_callback)
                                dart_callback(a->type(), tid, a->message().c_str(), dart_user_data);
                        }
                    } catch (...) {}
                }

                // batch priority updates after processing all alerts
                for (auto* s : dirty_streams) {
                    if (s->active) update_priorities(s);
                }
            } catch (...) {}
        }
    }
};

static SessionWrapper* to_sw(lt_session_t h) {
    return reinterpret_cast<SessionWrapper*>(h);
}

// ── priority engine ─────────────────────────────────────────────────────────────
// 5-level gradient mapped to libtorrent priorities 0-7:
//   NOW=7      piece at playback position
//   NEXT=6     next 1-3 pieces + head/tail protection
//   READAHEAD=5  next 4..readahead_window
//   FILL=1     remaining video pieces (forward)
//   SKIP=0     behind playback / non-video

static void update_priorities(StreamEngine* s) {
    try {
        if (!s->handle.is_valid() || !s->ti) return;

        int64_t head = s->read_head.load();
        int play = std::clamp(s->byte_to_piece(head), s->start_piece, s->end_piece);
        int ra   = s->readahead_window;

        std::vector<lt::download_priority_t> prios(
            (size_t)s->ti->num_pieces(), lt::dont_download);

        for (int p = s->start_piece; p <= s->end_piece; ++p) {
            int dist = p - play;

            if (dist == 0) {
                prios[p] = lt::download_priority_t(7);
            } else if (dist >= 1 && dist <= 3) {
                prios[p] = lt::download_priority_t(6);
            } else if (dist >= 4 && dist <= ra) {
                prios[p] = lt::download_priority_t(5);
            } else if (dist < 0 && dist >= -StreamEngine::BACK_BUFFER) {
                // backward buffer — keep recent pieces available for quick rewind
                prios[p] = lt::download_priority_t(1);
            } else if (p <= s->head_end_piece || p >= s->tail_start_piece) {
                // head/tail protection for container metadata
                prios[p] = lt::download_priority_t(6);
            }
            // everything else stays 0 — don't waste bandwidth
        }

        s->handle.prioritize_pieces(prios);

        // deadlines trigger time-critical mode — piece requested from multiple
        // peers, slow requests cancelled and re-sent to faster ones
        try {
            s->handle.set_piece_deadline(lt::piece_index_t(play), 0);
            for (int i = 1; i <= 3 && play + i <= s->end_piece; ++i)
                s->handle.set_piece_deadline(lt::piece_index_t(play + i), i * 400);
        } catch (...) {}

        // update stream state based on contiguous buffer
        int contiguous = 0;
        {
            std::lock_guard<std::mutex> lk(s->piece_mu);
            int p = play;
            while (p <= s->end_piece && s->pieces_have.count(p)) {
                contiguous++;
                p++;
            }
        }

        int32_t prev_state = s->stream_state.load();
        if (prev_state != LT_STREAM_SEEKING) {
            if (contiguous >= 3)
                s->stream_state.store(LT_STREAM_READY);
            else
                s->stream_state.store(LT_STREAM_BUFFERING);
        }
    } catch (...) {}
}

// ── piece waiting ───────────────────────────────────────────────────────────────
// blocks until piece is downloaded and verified — zero CPU polling

static bool wait_for_piece(StreamEngine* s, int piece, int timeout_ms,
                           int gen = -1) {
    if (gen < 0) gen = s->seek_generation.load();
    std::unique_lock<std::mutex> lk(s->piece_mu);
    if (s->pieces_have.count(piece)) return true;

    // deadline + top priority — triggers time-critical download from multiple peers
    try {
        s->handle.set_piece_deadline(lt::piece_index_t(piece), 0);
        s->handle.piece_priority(lt::piece_index_t(piece), lt::top_priority);
    } catch (...) {}

    return s->piece_cv.wait_for(lk, chr::milliseconds(timeout_ms),
        [&]{ return !s->active.load()
                 || s->seek_generation.load() != gen
                 || s->pieces_have.count(piece) > 0; })
        && s->pieces_have.count(piece) > 0;
}

// read piece data via read_piece() + read_piece_alert
static ReadResult read_piece_sync(StreamEngine* s, int piece,
                                  int timeout_ms = 5000, int gen = -1) {
    if (gen < 0) gen = s->seek_generation.load();
    // clear any stale result
    {
        std::lock_guard<std::mutex> lk(s->read_mu);
        s->read_results.erase(piece);
    }

    try { s->handle.read_piece(lt::piece_index_t(piece)); } catch (...) {
        return {};
    }

    std::unique_lock<std::mutex> lk(s->read_mu);
    if (s->read_cv.wait_for(lk, chr::milliseconds(timeout_ms),
            [&]{ return s->read_results.count(piece) > 0
                     || s->seek_generation.load() != gen; })) {
        auto it = s->read_results.find(piece);
        if (it != s->read_results.end()) {
            ReadResult r = std::move(it->second);
            s->read_results.erase(it);
            return r;
        }
    }
    return {};
}

// ── HTTP server ─────────────────────────────────────────────────────────────────

struct RangeReq {
    int64_t start = -1;
    int64_t end   = -1;
    bool    valid = false;
};

static RangeReq parse_range(const char* buf, int len) {
    RangeReq r;
    std::string s(buf, (size_t)len);
    std::string sl = s;
    for (auto& c : sl) c = (char)tolower((unsigned char)c);

    auto pos = sl.find("range: bytes=");
    if (pos == std::string::npos) return r;

    r.valid = true;
    pos += 13;
    auto end = s.find('\r', pos);
    if (end == std::string::npos) end = s.size();
    std::string rs = s.substr(pos, end - pos);
    auto dash = rs.find('-');
    if (dash != std::string::npos) {
        try {
            if (dash > 0) r.start = std::stoll(rs.substr(0, dash));
            if (dash + 1 < rs.size()) r.end = std::stoll(rs.substr(dash + 1));
        } catch (...) {}
    }
    return r;
}

static int send_all(socket_t sock, const char* data, int len) {
    int sent = 0;
    while (sent < len) {
        int n = ::send(sock, data + sent, len - sent, 0);
        if (n <= 0) return -1;
        sent += n;
    }
    return sent;
}

static bool serve_range(StreamEngine* s, socket_t cli,
                        int64_t range_start, int64_t range_end) {
    int my_gen = s->seek_generation.load();
    int64_t cursor = range_start;

    // detect tail/metadata request vs playback request
    bool is_tail = (range_start > s->file_size - s->piece_length * 10);

    if (!is_tail)
        s->read_head.store(range_start);

    while (cursor <= range_end && s->active.load()) {
        if (s->seek_generation.load() != my_gen) return false;

        int p = std::clamp(s->byte_to_piece(cursor), s->start_piece, s->end_piece);

        int64_t pfbeg, pfend;
        s->piece_file_range(p, pfbeg, pfend);
        pfend -= 1;

        int64_t sbeg = std::max(cursor, pfbeg);
        int64_t send_end = std::min(range_end, pfend);
        if (send_end < sbeg) { cursor = pfend + 1; continue; }

        // wait for piece — 8s initial, 30s extended
        if (!wait_for_piece(s, p, 8000, my_gen)) {
            if (s->seek_generation.load() != my_gen) return false;
            s->stream_state.store(LT_STREAM_BUFFERING);
            if (!wait_for_piece(s, p, 30000, my_gen))
                return false;
        }

        // read piece data — try cache first, then disk
        ReadResult rd;
        std::vector<char> cached;
        if (s->cache_get(p, cached)) {
            rd.ok = true;
            rd.data = std::move(cached);
        } else {
            rd = read_piece_sync(s, p, 5000, my_gen);
            if (!rd.ok || rd.data.empty()) return false;
            s->cache_put(p, rd.data);
        }
        if (!rd.ok || rd.data.empty()) return false;

        int64_t abs_start = s->file_offset + sbeg;
        int64_t piece_start = (int64_t)p * s->piece_length;
        int64_t off = abs_start - piece_start;
        int64_t nb  = send_end - sbeg + 1;

        if (off < 0 || (size_t)off >= rd.data.size()) { cursor = send_end + 1; continue; }
        if ((size_t)(off + nb) > rd.data.size()) nb = (int64_t)rd.data.size() - off;
        if (nb <= 0) { cursor = send_end + 1; continue; }

        if (send_all(cli, rd.data.data() + off, (int)nb) < 0)
            return false;

        cursor = sbeg + nb;
        if (!is_tail) {
            s->read_head.store(cursor);
            // adaptive readahead
            s->contiguous_bytes += nb;
            int nw = std::clamp((int)(s->contiguous_bytes / s->piece_length),
                                StreamEngine::MIN_READAHEAD, StreamEngine::MAX_READAHEAD);
            if (nw != s->readahead_window) {
                s->readahead_window = nw;
                update_priorities(s);
            }
        }

        if (s->stream_state.load() != LT_STREAM_READY)
            s->stream_state.store(LT_STREAM_READY);
    }
    return true;
}

static void handle_connection(StreamEngine* s, socket_t cli) {
    int opt = 1;
    ::setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt));
    int sndbuf = 2 * 1024 * 1024;
    ::setsockopt(cli, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));

    // 10-hour socket timeout — streaming connections should never die from inactivity
#ifdef _WIN32
    DWORD tv = 36000000;
    ::setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    ::setsockopt(cli, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = 36000; tv.tv_usec = 0;
    ::setsockopt(cli, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(cli, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

    // keep-alive: handle multiple requests per connection
    while (s->active.load()) {
        char buf[8192] = {};
        int total = 0;
        bool header_complete = false;
        while (total < (int)sizeof(buf) - 1 && s->active.load()) {
            int n = ::recv(cli, buf + total, (int)sizeof(buf) - 1 - total, 0);
            if (n <= 0) return;
            total += n;
            // look for end of HTTP header
            for (int i = std::max(0, total - 4); i <= total - 4; ++i) {
                if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n') {
                    header_complete = true; break;
                }
            }
            if (header_complete) break;
        }
        if (!header_complete || !s->active.load()) return;

        std::string req(buf, (size_t)total);

        bool is_options = req.find("OPTIONS ") != std::string::npos;
        bool is_head    = req.find("HEAD ") != std::string::npos;
        bool is_get     = req.find("GET ")  != std::string::npos;

        if (is_options) {
            const char* cors =
                "HTTP/1.1 204 No Content\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, HEAD, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Range\r\n"
                "Access-Control-Max-Age: 1728000\r\n"
                "Content-Length: 0\r\n"
                "Connection: keep-alive\r\n\r\n";
            if (send_all(cli, cors, (int)strlen(cors)) < 0) return;
            continue;
        }

        if (!is_get && !is_head) return;

        int64_t fsz = s->file_size;
        if (fsz <= 0) return;

        RangeReq rr = parse_range(buf, total);
        int64_t rstart = (rr.valid && rr.start >= 0) ? rr.start : 0;
        int64_t rend   = (rr.valid && rr.end >= 0)   ? rr.end   : fsz - 1;
        rstart = std::clamp(rstart, (int64_t)0, fsz - 1);
        rend   = std::clamp(rend,   rstart,     fsz - 1);
        int64_t clen = rend - rstart + 1;
        bool is_partial = (rr.valid && rr.start >= 0);

        // seek detection: >64KB jump from current position
        int64_t old_head = s->read_head.load();
        bool is_tail_req = (rstart > fsz - s->piece_length * 10);
        if (!is_tail_req && old_head > 0 && std::abs(rstart - old_head) > 65536) {
            s->seek_generation.fetch_add(1);
            s->stream_state.store(LT_STREAM_SEEKING);
            s->contiguous_bytes = 0;
            s->readahead_window = StreamEngine::MIN_READAHEAD;
            s->read_head.store(rstart);
            s->cache_trim(s->byte_to_piece(rstart));

            // clear old deadlines and set aggressive ones for the seek target
            try { s->handle.clear_piece_deadlines(); } catch (...) {}
            int seek_piece = std::clamp(s->byte_to_piece(rstart),
                                        s->start_piece, s->end_piece);
            try {
                s->handle.set_piece_deadline(lt::piece_index_t(seek_piece), 0);
                for (int i = 1; i <= 5 && seek_piece + i <= s->end_piece; ++i)
                    s->handle.set_piece_deadline(
                        lt::piece_index_t(seek_piece + i), i * 200);
            } catch (...) {}

            update_priorities(s);
        }

        // get filename for MIME type
        std::string filename = "video.mp4";
        if (s->ti) {
            try { filename = s->ti->files().file_name(lt::file_index_t{s->file_index}).to_string(); }
            catch (...) {}
        }

        std::ostringstream hdr;
        if (is_partial) {
            hdr << "HTTP/1.1 206 Partial Content\r\n";
            hdr << "Content-Range: bytes " << rstart << "-" << rend << "/" << fsz << "\r\n";
        } else {
            hdr << "HTTP/1.1 200 OK\r\n";
        }
        hdr << "Content-Type: " << get_mime(filename) << "\r\n";
        hdr << "Content-Length: " << clen << "\r\n";
        hdr << "Accept-Ranges: bytes\r\n";
        hdr << "Access-Control-Allow-Origin: *\r\n";
        hdr << "Access-Control-Allow-Headers: Range\r\n";
        hdr << "transferMode.dlna.org: Streaming\r\n";
        hdr << "contentFeatures.dlna.org: DLNA.ORG_OP=01;DLNA.ORG_CI=0;"
               "DLNA.ORG_FLAGS=01700000000000000000000000000000\r\n";
        hdr << "Cache-Control: no-store, no-cache\r\n";
        hdr << "Connection: keep-alive\r\n\r\n";

        std::string h = hdr.str();
        if (send_all(cli, h.c_str(), (int)h.size()) < 0) return;

        if (is_get) {
            if (!serve_range(s, cli, rstart, rend)) return;
        }
    }
}

static void run_http_server(SessionWrapper* /*sw*/, StreamEngine* stream) {
    try {
        INIT_SOCKETS();

        socket_t sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == SOCKET_INVALID) return;

        int opt = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0; // OS picks a port
        if (::bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
            CLOSESOCKET(sock); return;
        }
        socklen_t_ al = sizeof(addr);
        ::getsockname(sock, (sockaddr*)&addr, (socklen_t*)&al);
        stream->server_port = ntohs(addr.sin_port);
        stream->listen_sock = sock;
        ::listen(sock, 8);
        stream->running = true;

        while (stream->active.load()) {
            fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
            timeval tv{0, 200000}; // 200ms select timeout
            if (::select((int)sock + 1, &fds, nullptr, nullptr, &tv) <= 0)
                continue;

            sockaddr_in ca{}; socklen_t_ cl = sizeof(ca);
            socket_t cli = ::accept(sock, (sockaddr*)&ca, (socklen_t*)&cl);
            if (cli == SOCKET_INVALID) continue;

            // kill previous client connection to abort stuck serve_range
            {
                std::lock_guard<std::mutex> lk(stream->client_mu);
                if (stream->active_client != SOCKET_INVALID) {
                    CLOSESOCKET(stream->active_client);
                    stream->active_client = SOCKET_INVALID;
                    // stop downloading pieces for old position immediately
                    try { stream->handle.clear_piece_deadlines(); } catch (...) {}
                }
            }
            // bump seek gen so blocked waits exit
            stream->seek_generation.fetch_add(1);
            stream->contiguous_bytes = 0;
            stream->readahead_window = StreamEngine::MIN_READAHEAD;
            stream->cache_trim(stream->byte_to_piece(stream->read_head.load()));
            stream->piece_cv.notify_all();
            stream->read_cv.notify_all();

            // wait for previous handler thread to finish
            if (stream->client_thread.joinable())
                stream->client_thread.join();

            {
                std::lock_guard<std::mutex> lk(stream->client_mu);
                stream->active_client = cli;
            }

            stream->client_thread = std::thread([stream, cli]() {
                handle_connection(stream, cli);
                {
                    std::lock_guard<std::mutex> lk(stream->client_mu);
                    if (stream->active_client == cli)
                        stream->active_client = SOCKET_INVALID;
                }
                CLOSESOCKET(cli);
            });
        }

        // clean up client thread
        {
            std::lock_guard<std::mutex> lk(stream->client_mu);
            if (stream->active_client != SOCKET_INVALID) {
                CLOSESOCKET(stream->active_client);
                stream->active_client = SOCKET_INVALID;
            }
        }
        stream->piece_cv.notify_all();
        stream->read_cv.notify_all();
        if (stream->client_thread.joinable())
            stream->client_thread.join();
        CLOSESOCKET(sock);
    } catch (...) {}
}

// ── C API ───────────────────────────────────────────────────────────────────────

extern "C" {

TORRENT_API lt_session_t lt_create_session(const char* iface, int dl, int ul) {
    try {
        lt::settings_pack sp;

        // alert categories
        sp.set_int(lt::settings_pack::alert_mask,
            lt::alert_category::status
            | lt::alert_category::error
            | lt::alert_category::storage
            | lt::alert_category::piece_progress);

        sp.set_str(lt::settings_pack::listen_interfaces,
            (iface && *iface) ? iface : "0.0.0.0:6881,[::]:6881");

        if (dl > 0) sp.set_int(lt::settings_pack::download_rate_limit, dl);
        if (ul > 0) sp.set_int(lt::settings_pack::upload_rate_limit,   ul);

        // ── connection speed — get peers fast ──
        sp.set_int (lt::settings_pack::connection_speed,          200);
        sp.set_int (lt::settings_pack::torrent_connect_boost,     200);
        sp.set_bool(lt::settings_pack::smooth_connects,           false);
        sp.set_int (lt::settings_pack::connections_limit,         500);
        sp.set_int (lt::settings_pack::min_reconnect_time,        5);
        sp.set_int (lt::settings_pack::max_failcount,             3);
        sp.set_int (lt::settings_pack::peer_connect_timeout,      5);
        sp.set_int (lt::settings_pack::handshake_timeout,         5);

        // ── timeouts — fast but not so aggressive peers get dropped on seek ──
        sp.set_int (lt::settings_pack::piece_timeout,             8);
        sp.set_int (lt::settings_pack::request_timeout,           8);
        sp.set_int (lt::settings_pack::peer_timeout,              20);
        sp.set_int (lt::settings_pack::inactivity_timeout,        20);

        // ── request pipeline ──
        sp.set_int (lt::settings_pack::request_queue_time,        2);
        sp.set_int (lt::settings_pack::max_out_request_queue,     250);

        // ── piece picking — WE control priorities ──
        sp.set_bool(lt::settings_pack::auto_sequential,           false);
        sp.set_bool(lt::settings_pack::piece_extent_affinity,     false);
        sp.set_bool(lt::settings_pack::strict_end_game_mode,      true);
        sp.set_bool(lt::settings_pack::prioritize_partial_pieces, true);
        sp.set_int (lt::settings_pack::initial_picker_threshold,  0);

        // ── disk I/O ──
        sp.set_int (lt::settings_pack::aio_threads,               4);
        sp.set_int (lt::settings_pack::hashing_threads,           2);
        sp.set_int (lt::settings_pack::max_queued_disk_bytes,     16 * 1024 * 1024);
        sp.set_int (lt::settings_pack::disk_io_read_mode,         lt::settings_pack::enable_os_cache);
        sp.set_int (lt::settings_pack::disk_io_write_mode,        lt::settings_pack::enable_os_cache);
        sp.set_int (lt::settings_pack::file_pool_size,            100);
        sp.set_bool(lt::settings_pack::no_atime_storage,          true);

        // ── upload — minimize while streaming ──
        sp.set_int (lt::settings_pack::unchoke_slots_limit,       2);
        sp.set_int (lt::settings_pack::active_seeds,              0);

        // ── DHT + discovery ──
        sp.set_bool(lt::settings_pack::enable_dht,                true);
        sp.set_bool(lt::settings_pack::enable_lsd,                true);
        sp.set_bool(lt::settings_pack::enable_upnp,               true);
        sp.set_bool(lt::settings_pack::enable_natpmp,             true);
        sp.set_str (lt::settings_pack::dht_bootstrap_nodes,
            "dht.libtorrent.org:25401,"
            "router.bittorrent.com:6881,"
            "dht.transmissionbt.com:6881,"
            "router.utorrent.com:6881");
        sp.set_int (lt::settings_pack::dht_announce_interval,     60);
        sp.set_bool(lt::settings_pack::announce_to_all_trackers,  true);
        sp.set_bool(lt::settings_pack::announce_to_all_tiers,     true);

        // ── general ──
        sp.set_int (lt::settings_pack::active_downloads,          1);
        sp.set_int (lt::settings_pack::active_limit,              10);
        sp.set_int (lt::settings_pack::alert_queue_size,          10000);
        sp.set_bool(lt::settings_pack::close_redundant_connections, true);
        sp.set_int (lt::settings_pack::peer_turnover,             5);
        sp.set_int (lt::settings_pack::peer_turnover_interval,    30);
        sp.set_bool(lt::settings_pack::no_recheck_incomplete_resume, true);
        sp.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
        sp.set_bool(lt::settings_pack::rate_limit_ip_overhead,    false);
        sp.set_int (lt::settings_pack::whole_pieces_threshold,    20);

        // encryption
        sp.set_int (lt::settings_pack::in_enc_policy,  lt::settings_pack::pe_enabled);
        sp.set_int (lt::settings_pack::out_enc_policy, lt::settings_pack::pe_enabled);
        sp.set_int (lt::settings_pack::mixed_mode_algorithm, lt::settings_pack::peer_proportional);

        // buffers
        sp.set_int (lt::settings_pack::send_buffer_watermark,     2 * 1024 * 1024);
        sp.set_int (lt::settings_pack::send_buffer_low_watermark, 64 * 1024);
        sp.set_int (lt::settings_pack::send_buffer_watermark_factor, 150);
        sp.set_int (lt::settings_pack::recv_socket_buffer_size,   1024 * 1024);
        sp.set_int (lt::settings_pack::send_socket_buffer_size,   1024 * 1024);

        auto* sw = new SessionWrapper(std::move(sp));
        sw->start_alert_thread();
        set_err("");
        return reinterpret_cast<lt_session_t>(sw);
    } catch (const std::exception& e) { set_err(e.what()); return nullptr; }
}

TORRENT_API void lt_destroy_session(lt_session_t session) {
    if (!session) return;
    auto* sw = to_sw(session);

    // stop all streams
    {
        std::lock_guard<std::mutex> lk(sw->streams_mu);
        for (auto& kv : sw->streams) {
            kv.second->active = false;
            kv.second->wake_all();
        }
    }
    {
        std::lock_guard<std::mutex> lk(sw->streams_mu);
        for (auto& kv : sw->streams) {
            if (kv.second->server_thread.joinable())
                kv.second->server_thread.join();
        }
        sw->streams.clear();
    }

    sw->alert_running = false;
    if (sw->alert_thread.joinable()) sw->alert_thread.join();

    // flush resume data
    {
        std::lock_guard<std::mutex> lk(sw->mu);
        for (auto& kv : sw->handles)
            if (kv.second.is_valid())
                try { kv.second.save_resume_data(lt::torrent_handle::flush_disk_cache); } catch (...) {}
    }
    std::this_thread::sleep_for(chr::milliseconds(200));
    delete sw;
}

TORRENT_API void lt_set_alert_callback(lt_session_t session,
                                       lt_alert_callback cb, void* ud) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->cb_mu);
    sw->dart_callback  = cb;
    sw->dart_user_data = ud;
}

TORRENT_API void lt_poll_alerts(lt_session_t session,
                                lt_alert_callback cb, void* ud) {
    if (!session || !cb) return;
    auto* sw = to_sw(session);
    std::deque<AlertRecord> local;
    {
        std::lock_guard<std::mutex> lk(sw->dart_queue_mu);
        local.swap(sw->dart_queue);
    }
    for (auto& r : local)
        cb(r.type, r.torrent_id, r.message.c_str(), ud);
}

// ── torrent management ──────────────────────────────────────────────────────────

TORRENT_API lt_torrent_id lt_add_magnet(lt_session_t session,
                                        const char* uri, const char* path,
                                        int stream_only) {
    if (!session || !uri || !path) { set_err("null arg"); return -1; }
    auto* sw = to_sw(session);
    try {
        lt::error_code ec;
        lt::add_torrent_params atp = lt::parse_magnet_uri(uri, ec);
        if (ec) { set_err(ec.message()); return -1; }
        atp.save_path = path;
        atp.flags &= ~lt::torrent_flags::paused;
        atp.flags &= ~lt::torrent_flags::auto_managed;

        if (stream_only) {
            atp.storage_mode = lt::storage_mode_sparse;
            atp.flags |= lt::torrent_flags::stop_when_ready;
        }

        lt::torrent_handle h = sw->session.add_torrent(std::move(atp), ec);
        if (ec) { set_err(ec.message()); return -1; }
        h.resume();

        int64_t id = sw->next_id.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(sw->mu);
            sw->handles[id] = h;
            if (stream_only) sw->ephemeral_torrents.insert(id);
        }
        set_err(""); return id;
    } catch (const std::exception& e) { set_err(e.what()); return -1; }
}

TORRENT_API lt_torrent_id lt_add_torrent_file(lt_session_t session,
                                              const char* fp, const char* path,
                                              int stream_only) {
    if (!session || !fp || !path) { set_err("null arg"); return -1; }
    auto* sw = to_sw(session);
    try {
        lt::error_code ec;
        auto ti = std::make_shared<lt::torrent_info>(fp, ec);
        if (ec) { set_err(ec.message()); return -1; }
        lt::add_torrent_params atp;
        atp.ti = ti; atp.save_path = path;
        atp.flags &= ~lt::torrent_flags::paused;
        atp.flags &= ~lt::torrent_flags::auto_managed;

        if (stream_only) {
            atp.storage_mode = lt::storage_mode_sparse;
            atp.flags |= lt::torrent_flags::stop_when_ready;
        }

        lt::torrent_handle h = sw->session.add_torrent(std::move(atp), ec);
        if (ec) { set_err(ec.message()); return -1; }
        h.resume();

        int64_t id = sw->next_id.fetch_add(1);
        {
            std::lock_guard<std::mutex> lk(sw->mu);
            sw->handles[id] = h;
            if (stream_only) sw->ephemeral_torrents.insert(id);
        }
        set_err(""); return id;
    } catch (const std::exception& e) { set_err(e.what()); return -1; }
}

TORRENT_API void lt_remove_torrent(lt_session_t session,
                                   lt_torrent_id id, int del) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it == sw->handles.end()) return;
    sw->session.remove_torrent(it->second,
        del ? lt::session::delete_files : lt::remove_flags_t{});
    sw->handles.erase(it);
    sw->ephemeral_torrents.erase(id);
}

TORRENT_API void lt_pause_torrent(lt_session_t session, lt_torrent_id id) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it != sw->handles.end() && it->second.is_valid())
        try { it->second.pause(); } catch (...) {}
}

TORRENT_API void lt_resume_torrent(lt_session_t session, lt_torrent_id id) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it != sw->handles.end() && it->second.is_valid())
        try { it->second.resume(); } catch (...) {}
}

TORRENT_API void lt_recheck_torrent(lt_session_t session, lt_torrent_id id) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it != sw->handles.end() && it->second.is_valid())
        try { it->second.force_recheck(); } catch (...) {}
}

// ── status queries ──────────────────────────────────────────────────────────────

TORRENT_API int lt_get_torrent_count(lt_session_t session) {
    if (!session) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    return (int)sw->handles.size();
}

TORRENT_API int lt_get_all_statuses(lt_session_t session,
                                    lt_torrent_status* out, int max) {
    if (!session || !out || max <= 0) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    int n = 0;
    for (auto& kv : sw->handles) {
        if (n >= max) break;
        if (!kv.second.is_valid()) continue;
        try { fill_status(out[n++], kv.first,
              kv.second.status(lt::torrent_handle::query_pieces)); } catch (...) {}
    }
    return n;
}

TORRENT_API int lt_get_status(lt_session_t session, lt_torrent_id id,
                              lt_torrent_status* out) {
    if (!session || !out) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it == sw->handles.end() || !it->second.is_valid()) return 0;
    try { fill_status(*out, id,
          it->second.status(lt::torrent_handle::query_pieces)); return 1; } catch (...) { return 0; }
}

// ── file queries ────────────────────────────────────────────────────────────────

TORRENT_API int lt_get_file_count(lt_session_t session, lt_torrent_id id) {
    if (!session) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it == sw->handles.end() || !it->second.is_valid()) return 0;
    try { auto ti = it->second.torrent_file(); return ti ? ti->num_files() : 0; }
    catch (...) { return 0; }
}

TORRENT_API int lt_get_files(lt_session_t session, lt_torrent_id id,
                             lt_file_info* out, int max) {
    if (!session || !out || max <= 0) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it == sw->handles.end() || !it->second.is_valid()) return 0;
    try {
        auto ti = it->second.torrent_file();
        if (!ti) return 0;
        const lt::file_storage& fs = ti->files();
        int n = 0;
        for (int i = 0; i < fs.num_files() && n < max; ++i, ++n) {
            lt::file_index_t fi{i};
            out[n].index = i;
            out[n].size  = fs.file_size(fi);
            out[n].is_streamable = is_streamable(fs.file_name(fi).to_string()) ? 1 : 0;
            std::string nm = fs.file_name(fi).to_string();
            std::string pt = fs.file_path(fi);
            std::strncpy(out[n].name, nm.c_str(), sizeof(out[n].name) - 1);
            std::strncpy(out[n].path, pt.c_str(), sizeof(out[n].path) - 1);
            out[n].name[sizeof(out[n].name) - 1] = 0;
            out[n].path[sizeof(out[n].path) - 1] = 0;
        }
        return n;
    } catch (...) { return 0; }
}

TORRENT_API void lt_set_file_priorities(lt_session_t session, lt_torrent_id id,
                                        const int32_t* priorities, int count) {
    if (!session || !priorities || count <= 0) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it == sw->handles.end() || !it->second.is_valid()) return;
    try {
        auto ti = it->second.torrent_file();
        if (!ti) return;
        int nf = ti->files().num_files();
        std::vector<lt::download_priority_t> p;
        p.reserve(nf);
        for (int i = 0; i < nf; ++i)
            p.push_back((i < count && priorities[i]) ? lt::default_priority : lt::dont_download);
        it->second.prioritize_files(p);
        it->second.unset_flags(lt::torrent_flags::stop_when_ready);
        it->second.resume();
    } catch (...) {}
}

// ── streaming ───────────────────────────────────────────────────────────────────

TORRENT_API lt_stream_id lt_start_stream(lt_session_t session,
                                         lt_torrent_id torrent_id,
                                         int file_index,
                                         int64_t max_cache_bytes) {
    if (!session) { set_err("null session"); return -1; }
    auto* sw = to_sw(session);

    lt::torrent_handle handle;
    {
        std::lock_guard<std::mutex> lk(sw->mu);
        auto it = sw->handles.find(torrent_id);
        if (it == sw->handles.end()) { set_err("torrent not found"); return -1; }
        handle = it->second;
    }
    if (!handle.is_valid()) { set_err("invalid handle"); return -1; }

    auto ti = handle.torrent_file();
    if (!ti) { set_err("no metadata yet"); return -1; }
    const lt::file_storage& fs = ti->files();

    // auto-select largest streamable file
    if (file_index < 0) {
        int64_t best = -1; file_index = 0;
        for (int i = 0; i < fs.num_files(); ++i) {
            int64_t sz = fs.file_size(lt::file_index_t{i});
            if (sz > best && is_streamable(fs.file_name(lt::file_index_t{i}).to_string())) {
                best = sz; file_index = i;
            }
        }
    }
    if (file_index < 0 || file_index >= fs.num_files()) {
        set_err("invalid file index"); return -1;
    }

    auto s = std::make_unique<StreamEngine>();
    s->id           = sw->next_stream_id.fetch_add(1);
    s->torrent_id   = torrent_id;
    s->file_index   = file_index;
    s->handle       = handle;
    s->ti           = ti;
    s->piece_length = ti->piece_length();
    s->file_size    = fs.file_size(lt::file_index_t{file_index});
    s->file_offset  = fs.file_offset(lt::file_index_t{file_index});
    s->start_piece  = std::max(0, (int)(s->file_offset / s->piece_length));
    s->end_piece    = std::min((int)ti->num_pieces() - 1,
                     (int)((s->file_offset + s->file_size - 1) / s->piece_length));
    s->total_pieces = s->end_piece - s->start_piece + 1;

    // compute cache size from max_cache_bytes (0 = default 64 pieces)
    if (max_cache_bytes > 0 && s->piece_length > 0) {
        size_t pieces = (size_t)(max_cache_bytes / s->piece_length);
        s->max_cache_pieces = std::max((size_t)8, pieces); // minimum 8
    }

    // head/tail protection boundaries
    int prot_pieces = (int)((StreamEngine::PROTECT_BYTES + s->piece_length - 1) / s->piece_length);
    s->head_end_piece  = std::min(s->start_piece + prot_pieces - 1, s->end_piece);
    s->tail_start_piece = std::max(s->end_piece - prot_pieces + 1, s->start_piece);

    try {
        // focus bandwidth on this file
        std::vector<lt::download_priority_t> fp(
            (size_t)fs.num_files(), lt::dont_download);
        fp[(size_t)file_index] = lt::default_priority;
        handle.prioritize_files(fp);
        handle.unset_flags(lt::torrent_flags::stop_when_ready);
        handle.resume();

        // initial piece priorities — only download what we need
        std::vector<lt::download_priority_t> prios(
            (size_t)ti->num_pieces(), lt::dont_download);

        // head/tail for container metadata
        for (int p = s->start_piece; p <= s->head_end_piece; ++p)
            prios[p] = lt::download_priority_t(6);
        for (int p = s->tail_start_piece; p <= s->end_piece; ++p)
            prios[p] = lt::download_priority_t(6);

        // first few pieces at max priority
        int ra_end = std::min(s->start_piece + s->readahead_window, s->end_piece);
        for (int p = s->start_piece; p <= ra_end; ++p)
            prios[p] = lt::download_priority_t(5);
        int next_end = std::min(s->start_piece + 3, s->end_piece);
        for (int p = s->start_piece; p <= next_end; ++p)
            prios[p] = lt::download_priority_t(6);
        prios[s->start_piece] = lt::top_priority;

        handle.prioritize_pieces(prios);

        // deadlines for first pieces — get playback started ASAP
        handle.set_piece_deadline(lt::piece_index_t(s->start_piece), 0);
        for (int i = 1; i <= 5 && s->start_piece + i <= s->end_piece; ++i)
            handle.set_piece_deadline(
                lt::piece_index_t(s->start_piece + i), i * 300);

        // populate pieces we already have
        lt::torrent_status ts = handle.status(lt::torrent_handle::query_pieces);
        for (int p = s->start_piece; p <= s->end_piece; ++p)
            if (ts.pieces.get_bit(lt::piece_index_t(p)))
                s->pieces_have.insert(p);

    } catch (...) {}

    lt_stream_id sid = s->id;
    StreamEngine* raw = s.get();

    {
        std::lock_guard<std::mutex> lk(sw->streams_mu);
        sw->streams[sid] = std::move(s);
    }

    // start HTTP server thread
    raw->server_thread = std::thread([sw, raw]() { run_http_server(sw, raw); });

    // wait for server to bind
    for (int i = 0; i < 200 && !raw->running.load(); ++i)
        std::this_thread::sleep_for(chr::milliseconds(10));

    if (!raw->running.load()) {
        raw->active = false;
        if (raw->server_thread.joinable()) raw->server_thread.join();
        std::lock_guard<std::mutex> lk(sw->streams_mu);
        sw->streams.erase(sid);
        set_err("HTTP server failed to bind"); return -1;
    }

    set_err(""); return sid;
}

TORRENT_API void lt_stop_stream(lt_session_t session, lt_stream_id sid) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::unique_ptr<StreamEngine> stream;
    {
        std::lock_guard<std::mutex> lk(sw->streams_mu);
        auto it = sw->streams.find(sid);
        if (it == sw->streams.end()) return;
        stream = std::move(it->second);
        sw->streams.erase(it);
    }

    lt_torrent_id tid = stream->torrent_id;
    stream->active = false;
    stream->wake_all();
    if (stream->server_thread.joinable()) stream->server_thread.join();

    // clean up ephemeral torrents
    bool ephemeral = false;
    {
        std::lock_guard<std::mutex> lk(sw->mu);
        if (sw->ephemeral_torrents.count(tid)) {
            ephemeral = true;
            sw->ephemeral_torrents.erase(tid);
        }
    }

    if (ephemeral) {
        lt_remove_torrent(session, tid, 1);
    } else {
        // restore default file priorities
        try {
            auto ti2 = stream->handle.torrent_file();
            if (ti2) {
                int nf = ti2->files().num_files();
                std::vector<lt::download_priority_t> p((size_t)nf, lt::default_priority);
                stream->handle.prioritize_files(p);
            }
        } catch (...) {}
    }
}

static void fill_stream_status(lt_stream_status* out, const StreamEngine* s) {
    out->id         = s->id;
    out->torrent_id = s->torrent_id;
    out->file_index = s->file_index;
    out->file_size  = s->file_size;
    out->read_head  = s->read_head.load();
    out->stream_state = s->stream_state.load();
    out->readahead_window = s->readahead_window;

    // contiguous buffer from playback position
    int play = std::clamp(s->byte_to_piece(s->read_head.load()),
                          s->start_piece, s->end_piece);
    int contiguous = 0;
    {
        std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(s->piece_mu));
        int p = play;
        while (p <= s->end_piece && s->pieces_have.count(p)) {
            contiguous++; p++;
        }
    }
    out->buffer_pieces = contiguous;

    // ~5 Mbps bitrate estimate as fallback
    float bitrate = 625000.0f;
    out->buffer_seconds = (float)contiguous * s->piece_length / bitrate;

    // telemetry from handle
    try {
        lt::torrent_status ts = s->handle.status();
        out->active_peers  = ts.num_peers;
        out->download_rate = ts.download_rate;
    } catch (...) {
        out->active_peers  = 0;
        out->download_rate = 0;
    }

    std::string url = s->make_url();
    std::strncpy(out->url, url.c_str(), sizeof(out->url) - 1);
    out->url[sizeof(out->url) - 1] = 0;
}

TORRENT_API int lt_get_stream_status(lt_session_t session,
                                     lt_stream_id sid, lt_stream_status* out) {
    if (!session || !out) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->streams_mu);
    auto it = sw->streams.find(sid);
    if (it == sw->streams.end()) return 0;
    fill_stream_status(out, it->second.get());
    return 1;
}

TORRENT_API int lt_get_all_stream_statuses(lt_session_t session,
                                           lt_stream_status* out, int max) {
    if (!session || !out || max <= 0) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->streams_mu);
    int n = 0;
    for (auto& kv : sw->streams) {
        if (n >= max) break;
        fill_stream_status(&out[n++], kv.second.get());
    }
    return n;
}

// ── speed limits ────────────────────────────────────────────────────────────────

TORRENT_API void lt_set_download_limit(lt_session_t session, int bps) {
    if (!session) return;
    lt::settings_pack sp;
    sp.set_int(lt::settings_pack::download_rate_limit, bps);
    to_sw(session)->session.apply_settings(sp);
}

TORRENT_API void lt_set_upload_limit(lt_session_t session, int bps) {
    if (!session) return;
    lt::settings_pack sp;
    sp.set_int(lt::settings_pack::upload_rate_limit, bps);
    to_sw(session)->session.apply_settings(sp);
}

// ── utility ─────────────────────────────────────────────────────────────────────

TORRENT_API const char* lt_last_error(void) { return g_last_error.c_str(); }
TORRENT_API const char* lt_version(void)    { return LIBTORRENT_VERSION; }

} // extern "C"
