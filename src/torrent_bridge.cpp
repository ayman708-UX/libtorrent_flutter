// torrent_bridge.cpp – libtorrent 2.0.11 / Cross-platform (Windows, Linux, macOS, Android)
// Internal alert thread is the ONLY consumer of pop_alerts().
// Dart-facing lt_poll_alerts() reads from a thread-safe queue filled by that thread.

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

// ─── Cross-platform socket abstraction ──────────────────────────────────────────
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  typedef SOCKET socket_t;
  #define SOCKET_INVALID  INVALID_SOCKET
  #define CLOSESOCKET(s)  ::closesocket(s)
  #define INIT_SOCKETS()  { WSADATA _wsa; ::WSAStartup(MAKEWORD(2,2), &_wsa); }
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
#include <string>
#include <sstream>
#include <iomanip>
#include <vector>
#include <deque>
#include <memory>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <tuple>

namespace lt  = libtorrent;
namespace chr = std::chrono;

// Priority constants – lt::high_priority removed in 2.0
static constexpr lt::download_priority_t kPrioHigh{ 6 };

// ─── Thread-local error ───────────────────────────────────────────────────────
static thread_local std::string g_last_error;
static void set_err(const std::string& s) { g_last_error = s; }

// ─── Extension check ──────────────────────────────────────────────────────────
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

// ─── Fill status ──────────────────────────────────────────────────────────────
static void fill_status(lt_torrent_status& out, int64_t id,
                         const lt::torrent_status& st)
{
    out.id             = id;
    out.state          = static_cast<int>(st.state);
    
    // Fix: Premature "finished" state if piece priorities are modified for streaming.
    // We only show "finished" in UI if actually 100% of the WHOLE torrent is done.
    if (st.state == lt::torrent_status::finished && st.progress < 0.999f) {
        out.state = 2; // LT_STATE_DOWNLOADING
    } else if (st.state == lt::torrent_status::seeding && st.progress < 0.999f) {
        out.state = 2; // LT_STATE_DOWNLOADING
    }

    out.progress       = st.progress;
    out.download_rate  = st.download_rate;
    out.upload_rate    = st.upload_rate;
    out.total_done     = st.total_done;
    out.total_wanted   = st.total_wanted;
    out.total_uploaded = st.total_payload_upload;
    out.num_peers      = st.num_peers;
    out.num_seeds      = st.num_seeds;
    out.num_pieces     = (int32_t)st.num_pieces;
    
    // Libtorrent 2.0: Manually count set bits in the bitfield for maximum compatibility
    int have = 0;
    for (int i = 0; i < (int)st.pieces.size(); ++i) {
        if (st.pieces.get_bit(lt::piece_index_t(i))) have++;
    }
    out.pieces_done = (int32_t)have;

    out.is_paused      = (st.flags & lt::torrent_flags::paused) ? 1 : 0;
    out.is_finished    = (st.progress >= 0.999f && st.is_finished) ? 1 : 0;
    out.has_metadata   = st.has_metadata ? 1 : 0;
    
    // Debugging the "Finished" status
    if (st.state == lt::torrent_status::finished && st.progress < 0.99f) {
        static int log_counter = 0;
        if (log_counter++ % 5 == 0) { // Log every ~3 seconds
            fprintf(stderr, "[Native] DEBUG: Torrent %lld state=FINISHED, progress=%.3f, wanted=%lld, done=%lld, metadata=%d\n",
                    id, st.progress, st.total_wanted, st.total_done, (int)st.has_metadata);
        }
    }

    // queue_position is -1 when not in queue – clamp to 0
    int qp = static_cast<int>(st.queue_position);
    out.queue_position = (qp < 0) ? -1 : qp;

    // Fix: Unconditionally copy name - if it's empty, it's empty.
    // However, if we have metadata, use the name from torrent_info which is more reliable.
    std::string name = st.name;
    if (st.has_metadata) {
        auto ti = st.handle.torrent_file();
        if (ti) name = ti->name();
    }
    std::strncpy(out.name, name.c_str(), sizeof(out.name)-1);
    out.name[sizeof(out.name)-1] = 0;
    
    std::strncpy(out.save_path, st.save_path.c_str(), sizeof(out.save_path)-1);
    out.save_path[sizeof(out.save_path)-1] = 0;

    if (st.errc) {
        std::string e = st.errc.message();
        std::strncpy(out.error_msg, e.c_str(), sizeof(out.error_msg)-1);
        out.error_msg[sizeof(out.error_msg)-1] = 0;
        out.state = LT_STATE_ERROR;
        fprintf(stderr, "[Native] Torrent %lld error: %s\n", id, e.c_str());
    } else {
        out.error_msg[0] = 0;
    }
}

// ─── Piece waiter ─────────────────────────────────────────────────────────────
struct PieceData {
    std::vector<char> bytes;
    bool ok = false;
};

struct PieceWaiter {
    std::mutex              mu;
    std::condition_variable cv;
    PieceData               data;
    bool                    piece_finished = false;
    bool                    read_done = false;
};

// ─── Dart-side alert record ───────────────────────────────────────────────────
struct AlertRecord {
    int           type;
    lt_torrent_id torrent_id;
    std::string   message;
};

// ─── StreamSession ────────────────────────────────────────────────────────────
struct StreamSession {
    lt_stream_id  id;
    lt_torrent_id torrent_id;
    int           file_index;

    lt::torrent_handle                      handle;
    std::shared_ptr<const lt::torrent_info> ti;

    int64_t file_offset;
    int64_t file_size;
    int     first_piece;
    int     last_piece;
    int     piece_length;

    std::thread       server_thread;
    std::thread       priority_thread;
    socket_t          listen_sock = SOCKET_INVALID;
    int               port = 0;
    std::atomic<bool> server_running{false};

    // Piece waiters – written by alert thread, read by HTTP threads
    std::mutex waiters_mu;
    std::unordered_map<int, std::shared_ptr<PieceWaiter>> waiters;

    std::atomic<int64_t> read_head{0};
    std::atomic<int64_t> active_request_id{0}; // Track the latest HTTP request
    std::atomic<int32_t> buffer_pct{0};
    std::atomic<bool>    is_ready{false};
    std::atomic<bool>    active{true};
    std::atomic<int64_t> last_seek_time{0}; // ms since epoch — priority loop backs off after seek

    // Cache eviction
    int64_t max_cache_bytes = 0;     // 0 = unlimited
    int     safety_pieces   = 10;    // pieces to keep behind playhead

    static constexpr int64_t ZONE1 = 2  * 1024 * 1024;
    static constexpr int64_t ZONE2 = 32 * 1024 * 1024;

    std::string url() const {
        std::string hash = "0000000000000000000000000000000000000000";
        try {
            if (handle.is_valid()) {
                auto ih = handle.info_hashes();
                // Use V1 if available, otherwise V2
                if (ih.has_v1()) {
                    std::stringstream ss;
                    for (auto b : ih.v1) ss << std::hex << std::setw(2) << std::setfill('0') << (int)(uint8_t)b;
                    hash = ss.str();
                } else if (ih.has_v2()) {
                    std::stringstream ss;
                    for (auto b : ih.v2) ss << std::hex << std::setw(2) << std::setfill('0') << (int)(uint8_t)b;
                    hash = ss.str().substr(0, 40); // Clamp to 40 for UI consistency
                }
            }
        } catch (...) {}
        return "http://127.0.0.1:" + std::to_string(port) + "/stream/" + hash + "/" + std::to_string(file_index);
    }

    int byte_to_piece(int64_t off) const {
        if (piece_length <= 0) return first_piece;
        return (int)((file_offset + off) / piece_length);
    }

    void piece_file_range(int p, int64_t& beg, int64_t& end_) const {
        int64_t ps = (int64_t)p * piece_length;
        int64_t pe = ps + piece_length;
        beg  = std::max(ps, file_offset) - file_offset;
        end_ = std::min(pe, file_offset + file_size) - file_offset;
    }

    bool piece_avail(int p) const {
        try {
            if (!handle.is_valid()) return false;
            const lt::bitfield& pcs = handle.status(lt::torrent_handle::query_pieces).pieces;
            if (p < 0 || p >= (int)pcs.size()) return false;
            return pcs.get_bit(p);
        } catch (...) { return false; }
    }

    // Called by alert thread when read_piece_alert fires
    void on_piece_ready(int p, const char* data, int size) {
        std::shared_ptr<PieceWaiter> w;
        {
            std::lock_guard<std::mutex> lk(waiters_mu);
            auto it = waiters.find(p);
            if (it == waiters.end()) return;
            w = it->second;
        }
        {
            std::lock_guard<std::mutex> wl(w->mu);
            if (data && size > 0) {
                w->data.bytes.assign(data, data + size);
                w->data.ok = true;
            } else {
                w->data.ok = false;
            }
            w->read_done = true;
        }
        w->cv.notify_all();
    }

    void on_piece_finished(int p) {
        std::shared_ptr<PieceWaiter> w;
        {
            std::lock_guard<std::mutex> lk(waiters_mu);
            auto it = waiters.find(p);
            if (it == waiters.end()) return;
            w = it->second;
        }
        {
            std::lock_guard<std::mutex> wl(w->mu);
            w->piece_finished = true;
        }
        w->cv.notify_all();
    }

    // Wake all waiters on shutdown
    void wake_all_waiters() {
        std::lock_guard<std::mutex> lk(waiters_mu);
        for (auto& kv : waiters) {
            std::lock_guard<std::mutex> wl(kv.second->mu);
            kv.second->read_done = true; // Break the wait loop
            kv.second->cv.notify_all();
        }
    }

    // Wait for a piece, issuing read_piece() or set_piece_deadline() to libtorrent
    PieceData wait_for_piece(int p, int total_timeout_ms = 30000) {
        if (!handle.is_valid()) return {};

        // Fast path: piece already available — read it immediately
        if (piece_avail(p)) {
            std::shared_ptr<PieceWaiter> w;
            {
                std::lock_guard<std::mutex> lk(waiters_mu);
                auto it = waiters.find(p);
                if (it != waiters.end()) w = it->second;
                else {
                    w = std::make_shared<PieceWaiter>();
                    waiters[p] = w;
                }
            }
            try { handle.read_piece(lt::piece_index_t(p)); } catch (...) {}
            // Wait briefly for the read to complete
            auto start_fast = chr::steady_clock::now();
            while (chr::steady_clock::now() - start_fast < chr::milliseconds(3000)) {
                std::unique_lock<std::mutex> lk(w->mu);
                if (w->read_done) {
                    if (w->data.ok) {
                        std::lock_guard<std::mutex> wl(waiters_mu);
                        waiters.erase(p);
                        return w->data;
                    }
                    break;
                }
                w->cv.wait_for(lk, chr::milliseconds(50));
            }
            std::lock_guard<std::mutex> wl(waiters_mu);
            waiters.erase(p);
            return {};
        }

        std::shared_ptr<PieceWaiter> w;
        {
            std::lock_guard<std::mutex> lk(waiters_mu);
            auto it = waiters.find(p);
            if (it != waiters.end()) w = it->second;
            else {
                w = std::make_shared<PieceWaiter>();
                waiters[p] = w;
            }
        }

        // EMERGENCY: The player is literally waiting for this piece right now.
        // Force libtorrent to prioritize it ABOVE EVERYTHING else instantly.
        try {
            handle.set_piece_deadline(lt::piece_index_t(p), 0, lt::torrent_handle::alert_when_available);
            handle.piece_priority(lt::piece_index_t(p), lt::download_priority_t{7});
        } catch (...) {}

        auto start_time = chr::steady_clock::now();
        bool read_requested = false;

        while (chr::steady_clock::now() - start_time < chr::milliseconds(total_timeout_ms)) {
            if (!active.load()) break;

            std::unique_lock<std::mutex> lk(w->mu);
            
            if (w->read_done) {
                if (w->data.ok) {
                    std::lock_guard<std::mutex> wl(waiters_mu);
                    waiters.erase(p);
                    return w->data;
                }
                w->read_done = false;
                read_requested = false;
            }

            if (!read_requested && (w->piece_finished || piece_avail(p))) {
                try { handle.read_piece(lt::piece_index_t(p)); } catch (...) {}
                read_requested = true;
            }

            // OPTIMIZED: 50ms poll instead of 500ms — catches fast pieces quickly
            if (w->cv.wait_for(lk, chr::milliseconds(50), [&]{ 
                return !active.load() || w->read_done || (!read_requested && piece_avail(p)); 
            })) {
                if (!active.load()) break;
                continue;
            }
        }

        std::lock_guard<std::mutex> wl(waiters_mu);
        waiters.erase(p);
        return {};
    }
    void update_priorities() {
        try {
            if (!handle.is_valid() || !ti) return;
            int64_t head = read_head.load();
            int current_piece = std::clamp(byte_to_piece(head), first_piece, last_piece);

            // Fetch bitfield ONCE and cache it for the whole update
            lt::bitfield pieces_bf;
            try {
                pieces_bf = handle.status(lt::torrent_handle::query_pieces).pieces;
            } catch (...) { return; }

            auto have = [&](int p) -> bool {
                if (p < 0 || p >= (int)pieces_bf.size()) return false;
                return pieces_bf.get_bit(p);
            };

            try { handle.unset_flags(lt::torrent_flags::sequential_download); } catch (...) {}

            // ── Cap readahead to cache size ──
            // Tight windows to concentrate bandwidth on the most urgent pieces.
            // libtorrent streaming docs: "Any block you request that is not
            // urgent takes away bandwidth from urgent pieces."
            int max_readahead = 6;  // small default — keep bandwidth focused
            if (max_cache_bytes > 0 && piece_length > 0) {
                int cache_pieces = (int)(max_cache_bytes / piece_length);
                max_readahead = std::max(3, std::min(max_readahead, cache_pieces / 2));
            }

            std::vector<lt::download_priority_t> prios;
            prios.resize((size_t)ti->num_pieces(), lt::dont_download);

            // 1. Readahead buffer (capped to cache, priority 4)
            int readahead_end = std::min(current_piece + max_readahead, last_piece);
            for (int p = current_piece; p <= readahead_end; ++p) {
                prios[p] = lt::download_priority_t{4};
            }

            // 2. Hot window (next 4, priority 7, staggered deadlines)
            //    Each piece gets a progressively later deadline so libtorrent's
            //    time-critical picker focuses on the most urgent first.
            int hot_end = std::min(current_piece + std::min(4, max_readahead), last_piece);
            for (int p = current_piece; p <= hot_end; ++p) {
                prios[p] = lt::download_priority_t{7};
                if (!have(p)) {
                    int offset = p - current_piece;
                    handle.set_piece_deadline(lt::piece_index_t(p), offset * 200);
                }
            }

            // 3. Critical window (current + 1, priority 7, staggered deadlines)
            //    Only 2 pieces at near-zero deadline — concentrates all bandwidth.
            int crit_end = std::min(current_piece + 1, last_piece);
            for (int p = current_piece; p <= crit_end; ++p) {
                prios[p] = lt::download_priority_t{7};
                if (!have(p))
                    handle.set_piece_deadline(lt::piece_index_t(p), (p - current_piece) * 50);
            }

            // ── Sliding window cache eviction ──
            // NEVER use force_recheck — it nukes ALL piece state and kills
            // the stream. Instead, just set evicted pieces to dont_download.
            // Libtorrent won't re-download them, and the OS will reclaim
            // the disk space when it needs it (mmap-backed in lt 2.0).
            //
            // HARD RULE: never evict anything >= (current_piece - 5).
            // Always keep 5 pieces behind playhead as safety margin.
            if (max_cache_bytes > 0 && piece_length > 0) {
                int have_count = 0;
                for (int p = first_piece; p <= last_piece; ++p) {
                    if (have(p)) ++have_count;
                }
                int64_t have_bytes = (int64_t)have_count * piece_length;

                if (have_bytes > max_cache_bytes) {
                    // Evict from the oldest (furthest behind playhead) first
                    int safe_floor = current_piece - 5;
                    for (int p = first_piece; p < safe_floor && p <= last_piece; ++p) {
                        if (have(p)) {
                            prios[p] = lt::dont_download;
                        }
                    }
                    // No force_recheck! Pieces remain on disk but libtorrent
                    // won't track or re-download them.
                }
            }

            handle.prioritize_pieces(prios);
            is_ready.store(true);

            // Buffer % based on critical 2-piece window
            int buf_end = std::min(current_piece + 1, last_piece);
            int hw_total = (buf_end - current_piece + 1);
            int hw_avail = 0;
            for (int p = current_piece; p <= buf_end; ++p) if (have(p)) ++hw_avail;
            buffer_pct.store(hw_total > 0 ? (hw_avail * 100 / hw_total) : 0);
        } catch (...) {}
    }
};

// ─── SessionWrapper ───────────────────────────────────────────────────────────
struct SessionWrapper {
    lt::session session;

    // Torrent handles
    std::mutex mu;
    std::unordered_map<int64_t, lt::torrent_handle> handles;
    std::unordered_set<int64_t> ephemeral_torrents; // Torrents to delete on stop
    std::atomic<int64_t> next_id{1};

    // Streams
    std::mutex streams_mu;
    std::unordered_map<int64_t, std::shared_ptr<StreamSession>> streams;
    std::atomic<int64_t> next_stream_id{1};

    // ── Single alert consumer ─────────────────────────────────────────────────
    // ONLY this thread calls session.pop_alerts(). Dart lt_poll_alerts() reads
    // from dart_queue instead of calling pop_alerts() directly.
    std::thread       alert_thread;
    std::atomic<bool> alert_running{false};

    // Queue for Dart-facing alerts (status, error, etc.)
    std::mutex              dart_queue_mu;
    std::deque<AlertRecord> dart_queue;

    explicit SessionWrapper(lt::settings_pack sp) : session(std::move(sp)) {}

    int64_t torrent_id_for_handle(const lt::torrent_handle& h) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& kv : handles)
            if (kv.second == h) return kv.first;
        return -1;
    }

    void start_alert_thread() {
        alert_running = true;
        alert_thread = std::thread([this]() {
            while (alert_running.load()) {
                try {
                    if (session.wait_for_alert(lt::milliseconds(100))) {
                        std::vector<lt::alert*> alerts;
                        session.pop_alerts(&alerts);

                        for (auto* a : alerts) {
                            if (!a) continue;
                            try {
                                // ── read_piece_alert → dispatch to stream ─────────
                                if (auto* rpa = lt::alert_cast<lt::read_piece_alert>(a)) {
                                    int p = static_cast<int>(rpa->piece);
                                    std::lock_guard<std::mutex> slk(streams_mu);
                                    for (auto& kv : streams) {
                                        if (!kv.second->active) continue;
                                        if (kv.second->handle == rpa->handle) {
                                            kv.second->on_piece_ready(p,
                                                rpa->error ? nullptr : rpa->buffer.get(), 
                                                rpa->error ? 0 : rpa->size);
                                            break;
                                        }
                                    }
                                    continue; 
                                }

                                // ── piece_finished_alert → dispatch to stream ──────
                                if (auto* pfa = lt::alert_cast<lt::piece_finished_alert>(a)) {
                                    int p = static_cast<int>(pfa->piece_index);
                                    std::lock_guard<std::mutex> slk(streams_mu);
                                    for (auto& kv : streams) {
                                        if (!kv.second->active) continue;
                                        if (kv.second->handle == pfa->handle) {
                                            kv.second->on_piece_finished(p);
                                            break;
                                        }
                                    }
                                }

                                // ── metadata_received_alert → halt downloads instantly ──────
                                if (auto* mra = lt::alert_cast<lt::metadata_received_alert>(a)) {
                                    try {
                                        auto ti = mra->handle.torrent_file();
                                        if (ti) {
                                            int nf = ti->files().num_files();
                                            std::vector<lt::download_priority_t> p(
                                                (size_t)nf, lt::dont_download);
                                            mra->handle.prioritize_files(p);
                                            // Pause it explicitly now that we have metadata
                                            mra->handle.pause();
                                        }
                                    } catch (...) {}
                                }

                                // ── All other alerts → dart_queue ─────────────────
                                lt_torrent_id tid = -1;
                                if (auto* ta = dynamic_cast<lt::torrent_alert*>(a))
                                    tid = torrent_id_for_handle(ta->handle);

                                AlertRecord rec;
                                rec.type       = a->type();
                                rec.torrent_id = tid;
                                rec.message    = a->message();
                                {
                                    std::lock_guard<std::mutex> ql(dart_queue_mu);
                                    if (dart_queue.size() < 1024)
                                        dart_queue.push_back(std::move(rec));
                                }
                            } catch (...) {}
                        }
                    }
                } catch (...) {}
            }
        });
    }
};

static SessionWrapper* to_sw(lt_session_handle h) {
    return reinterpret_cast<SessionWrapper*>(h);
}

// ─── HTTP server ──────────────────────────────────────────────────────────────
struct HttpReq {
    int64_t range_start = -1;
    int64_t range_end   = -1;
    bool    has_range   = false;
};

static HttpReq parse_req(const char* buf, int len) {
    HttpReq r;
    std::string s(buf, (size_t)len);
    // Case-insensitive range search
    auto pos = s.find("Range: bytes=");
    if (pos == std::string::npos) pos = s.find("range: bytes=");
    if (pos != std::string::npos) {
        r.has_range = true;
        pos += 13;
        auto end = s.find('\r', pos);
        if (end == std::string::npos) end = s.size();
        std::string rs = s.substr(pos, end - pos);
        auto dash = rs.find('-');
        if (dash != std::string::npos) {
            try {
                if (dash > 0)              r.range_start = std::stoll(rs.substr(0, dash));
                if (dash + 1 < rs.size()) r.range_end   = std::stoll(rs.substr(dash + 1));
            } catch (...) {}
        }
    }
    return r;
}

static bool serve_range(StreamSession* ss, socket_t cli,
                         int64_t bstart, int64_t bend, int64_t request_id)
{
    try {
        int64_t expected_next = ss->read_head.load();
        ss->read_head.store(bstart);
        int64_t cursor = bstart;

        // SEEK detected: nuke all old deadlines and focus entirely on new position
        bool is_seek = (expected_next != 0) && (bstart != expected_next) && (std::abs(bstart - expected_next) > 64 * 1024);
        if (is_seek) {
            int seek_piece = std::clamp(ss->byte_to_piece(bstart), ss->first_piece, ss->last_piece);

            // Step 1: Wipe ALL old deadlines — stops libtorrent from chasing stale positions
            try { ss->handle.clear_piece_deadlines(); } catch (...) {}

            // Step 2: Record seek time — minimal cooldown (100ms) just to let
            //         inline setup take effect before priority loop re-evaluates
            ss->last_seek_time.store(
                chr::duration_cast<chr::milliseconds>(
                    chr::steady_clock::now().time_since_epoch()).count());

            // Step 3: TIGHT FOCUS — only 2 pieces at staggered deadlines.
            // All bandwidth goes to the seek position immediately.
            try {
                std::vector<lt::download_priority_t> prios(
                    (size_t)ss->ti->num_pieces(), lt::dont_download);
                int focus_end = std::min(seek_piece + 1, ss->last_piece);
                for (int i = seek_piece; i <= focus_end; ++i) {
                    prios[i] = lt::download_priority_t{7};
                    ss->handle.set_piece_deadline(lt::piece_index_t(i),
                        (i - seek_piece) * 50,
                        lt::torrent_handle::alert_when_available);
                }
                // Also queue the next 2 with staggered deadlines
                for (int i = focus_end + 1; i <= std::min(seek_piece + 3, ss->last_piece); ++i) {
                    prios[i] = lt::download_priority_t{7};
                    ss->handle.set_piece_deadline(lt::piece_index_t(i),
                        (i - seek_piece) * 200);
                }
                ss->handle.prioritize_pieces(prios);
            } catch (...) {}
        }

        while (cursor <= bend && ss->active.load()) {
            if (ss->active_request_id.load() != request_id) return false;

            int p = std::clamp(ss->byte_to_piece(cursor),
                                ss->first_piece, ss->last_piece);
            int64_t pfbeg, pfend;
            ss->piece_file_range(p, pfbeg, pfend);
            pfend -= 1;

            int64_t sbeg  = std::max(cursor, pfbeg);
            int64_t send_ = std::min(bend,   pfend);
            if (send_ < sbeg) { cursor = pfend + 1; continue; }

            // LOOKAHEAD: while we wait for piece p, pre-prime the next 3 pieces
            // with staggered deadlines so the swarm fetches them in order.
            for (int ahead = 1; ahead <= 3 && (p + ahead) <= ss->last_piece; ++ahead) {
                ss->handle.piece_priority(lt::piece_index_t(p + ahead), lt::download_priority_t{7});
                if (!ss->piece_avail(p + ahead))
                    ss->handle.set_piece_deadline(lt::piece_index_t(p + ahead), ahead * 100);
            }

            PieceData pd = ss->wait_for_piece(p);
            if (!pd.ok || pd.bytes.empty()) return false;

            int64_t absolute_sbeg = ss->file_offset + sbeg;
            int64_t piece_start = (int64_t)p * ss->piece_length;
            int64_t off = absolute_sbeg - piece_start;
            int64_t nb  = send_ - sbeg + 1;
            
            // Safety check against piece buffer bounds
            if (off < 0) { cursor = send_ + 1; continue; }
            if ((size_t)off >= pd.bytes.size()) { cursor = send_ + 1; continue; }
            if ((size_t)(off + nb) > pd.bytes.size()) {
                nb = (int64_t)pd.bytes.size() - off;
            }
            if (nb <= 0) { cursor = send_ + 1; continue; }

            const char* ptr = pd.bytes.data() + (size_t)off;
            int64_t rem = nb;
            while (rem > 0 && ss->active.load()) {
                if (ss->active_request_id.load() != request_id) return false;
                int chunk = (int)std::min(rem, (int64_t)524288); // 512KB chunks for higher throughput
                int sent  = ::send(cli, ptr, chunk, 0);
                if (sent <= 0) return false;
                ptr += sent;
                rem -= sent;
            }
            cursor = sbeg + nb;
            ss->read_head.store(cursor);
        }
        return true;
    } catch (...) { return false; }
}

static std::string get_mime(const std::string& filename) {
    auto d = filename.rfind('.');
    if (d == std::string::npos) return "video/mp4";
    std::string e = filename.substr(d);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    if (e == ".mkv")  return "video/x-matroska";
    if (e == ".mp4")  return "video/mp4";
    if (e == ".avi")  return "video/x-msvideo";
    if (e == ".mov")  return "video/quicktime";
    if (e == ".webm") return "video/webm";
    if (e == ".ts")   return "video/MP2T";
    return "video/mp4";
}

static void handle_conn(socket_t cli, std::shared_ptr<StreamSession> ss) {
    static std::atomic<int64_t> global_req_counter{1};
    int64_t my_req_id = global_req_counter.fetch_add(1);
    ss->active_request_id.store(my_req_id);

    // Socket tuning for low-latency data delivery to the player
    int opt = 1;
    ::setsockopt(cli, IPPROTO_TCP, TCP_NODELAY, (const char*)&opt, sizeof(opt));
    int sndbuf = 1 * 1024 * 1024; // 1MB send buffer
    ::setsockopt(cli, SOL_SOCKET, SO_SNDBUF, (const char*)&sndbuf, sizeof(sndbuf));

    try {
        // Single request per connection — VLC and other players handle this
        // correctly by opening a new TCP connection for each range request.
        // This avoids the keep-alive mismatch where the server closes the
        // socket but the client thinks it's still alive.
        char buf[8192] = {};
        int  total = 0;
        while (total < (int)sizeof(buf) - 1 && ss->active.load()) {
            int n = ::recv(cli, buf + total, (int)sizeof(buf) - 1 - total, 0);
            if (n <= 0) break;
            total += n;
            // Search for \r\n\r\n header terminator
            if (total >= 4) {
                bool found = false;
                for (int i = total - 4; i >= 0; --i) {
                    if (buf[i]=='\r' && buf[i+1]=='\n' && buf[i+2]=='\r' && buf[i+3]=='\n') {
                        found = true; break;
                    }
                }
                if (found) break;
            }
        }
        if (total > 0 && ss->active.load()) {
            HttpReq req = parse_req(buf, total);
            std::string s(buf, (size_t)total);
            bool is_head = s.find("HEAD /stream/") != std::string::npos;
            bool is_get  = s.find("GET /stream/")  != std::string::npos;

            if (is_get || is_head) {
                int64_t fsz = ss->file_size;
                if (fsz > 0) {
                    int64_t rstart = (req.has_range && req.range_start >= 0) ? req.range_start : 0;
                    int64_t rend   = (req.has_range && req.range_end   >= 0) ? req.range_end   : fsz - 1;
                    rstart = std::clamp(rstart, (int64_t)0, fsz - 1);
                    rend   = std::clamp(rend,   rstart,     fsz - 1);
                    int64_t clen  = rend - rstart + 1;
                    bool    isrng = req.has_range && req.range_start >= 0;

                    std::string filename = "video.mp4";
                    if (ss->ti) try { filename = ss->ti->files().file_name(lt::file_index_t{ss->file_index}).to_string(); } catch(...) {}

                    std::ostringstream hdr;
                    if (isrng) {
                        hdr << "HTTP/1.1 206 Partial Content\r\n";
                        hdr << "Content-Range: bytes " << rstart << "-" << rend << "/" << fsz << "\r\n";
                    } else {
                        hdr << "HTTP/1.1 200 OK\r\n";
                    }
                    hdr << "Content-Type: " << get_mime(filename) << "\r\n";
                    hdr << "Content-Length: " << clen << "\r\n";
                    hdr << "Accept-Ranges: bytes\r\n";
                    hdr << "Access-Control-Allow-Origin: *\r\n";
                    hdr << "Cache-Control: no-store, no-cache\r\n";
                    hdr << "Connection: close\r\n\r\n";

                    std::string h = hdr.str();
                    if (::send(cli, h.c_str(), (int)h.size(), 0) > 0) {
                        if (is_get) {
                            serve_range(ss.get(), cli, rstart, rend, my_req_id);
                        }
                    }
                }
            }
        }
    } catch (...) {}
    CLOSESOCKET(cli);
}

static void run_http_server(std::shared_ptr<StreamSession> ss) {
    try {
        INIT_SOCKETS();

        socket_t sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == SOCKET_INVALID) return;

        int opt = 1;
        ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        if (::bind(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
            CLOSESOCKET(sock); return;
        }
        socklen_t_ addrlen = sizeof(addr);
        ::getsockname(sock, (sockaddr*)&addr, (socklen_t*)&addrlen);
        ss->port        = ntohs(addr.sin_port);
        ss->listen_sock = sock;
        int rcvbuf = 64 * 1024;
        ::setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf));
        ::listen(sock, 64);
        ss->server_running = true;

        while (ss->active.load()) {
            fd_set fds; FD_ZERO(&fds); FD_SET(sock, &fds);
            timeval tv{0, 100000}; // 100ms
            if (::select((int)sock + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
            sockaddr_in ca{}; socklen_t_ cl = sizeof(ca);
            socket_t cli = ::accept(sock, (sockaddr*)&ca, (socklen_t*)&cl);
            if (cli == SOCKET_INVALID) continue;
            std::thread([cli, ss]() {
                handle_conn(cli, ss);
            }).detach();
        }
        CLOSESOCKET(sock);
    } catch (const std::exception& e) {
        fprintf(stderr, "[Native] run_http_server exception: %s\n", e.what());
    } catch (...) {}
}

static void run_priority_loop(std::shared_ptr<StreamSession> ss) {
    while (ss->active.load()) {
        // Short 100ms cooldown after seek — just enough for inline setup,
        // then immediately resume priority management
        auto now_ms = chr::duration_cast<chr::milliseconds>(
            chr::steady_clock::now().time_since_epoch()).count();
        if (now_ms - ss->last_seek_time.load() > 100) {
            try { ss->update_priorities(); } catch (...) {}
        }
        std::this_thread::sleep_for(chr::milliseconds(100));
    }
}

// ─── Exported C API ───────────────────────────────────────────────────────────
extern "C" {

TORRENT_API lt_session_handle lt_create_session(const char* iface,
                                                  int dl, int ul)
{
    try {
        lt::settings_pack sp;
        sp.set_int(lt::settings_pack::alert_mask,
            lt::alert_category::status  |
            lt::alert_category::error   |
            lt::alert_category::storage |
            lt::alert_category::piece_progress);
        sp.set_str(lt::settings_pack::listen_interfaces,
            (iface && *iface) ? iface : "0.0.0.0:6881,[::]:6881");

        if (dl > 0) sp.set_int(lt::settings_pack::download_rate_limit, dl);
        if (ul > 0) sp.set_int(lt::settings_pack::upload_rate_limit,   ul);

        // ─── Ultimate Streaming Settings 3.0 (MAX SPEED) ─────────────────────
        sp.set_int (lt::settings_pack::connections_limit,         2000);
        sp.set_int (lt::settings_pack::unchoke_slots_limit,       500);
        sp.set_bool(lt::settings_pack::enable_dht,                true);
        sp.set_bool(lt::settings_pack::enable_lsd,                true);
        sp.set_bool(lt::settings_pack::enable_upnp,               true);
        sp.set_bool(lt::settings_pack::enable_natpmp,             true);
        sp.set_bool(lt::settings_pack::suggest_mode,              true);
        
        // Max discovery speed
        sp.set_int (lt::settings_pack::active_downloads,          -1);
        sp.set_int (lt::settings_pack::active_limit,              -1);
        sp.set_int (lt::settings_pack::active_tracker_limit,      -1);
        sp.set_int (lt::settings_pack::active_dht_limit,          -1);
        sp.set_bool(lt::settings_pack::announce_to_all_trackers,  true);
        sp.set_bool(lt::settings_pack::announce_to_all_tiers,     true);
        sp.set_int (lt::settings_pack::connection_speed,          1000);
        sp.set_bool(lt::settings_pack::smooth_connects,          false);
        sp.set_int (lt::settings_pack::torrent_connect_boost,     200);

        // Streaming-tuned timeouts (issue #7666, torrest, Elementum all use 5-10s)
        // 2s was causing peer churn — slow but valuable peers got dropped mid-piece
        sp.set_int (lt::settings_pack::request_timeout,           4);
        sp.set_int (lt::settings_pack::peer_timeout,              10);
        sp.set_int (lt::settings_pack::min_reconnect_time,        1);
        sp.set_int (lt::settings_pack::piece_timeout,              5);
        sp.set_int (lt::settings_pack::inactivity_timeout,        10);
        sp.set_int (lt::settings_pack::peer_connect_timeout,      3);

        // Skip full file recheck on resume — network is faster than disk verify
        sp.set_bool(lt::settings_pack::no_recheck_incomplete_resume, true);

        // Allow multiple connections from same IP (seedboxes, VPNs, shared NAT)
        sp.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);

        // Security / Throttling evasion
        sp.set_int (lt::settings_pack::in_enc_policy,             lt::settings_pack::pe_enabled);
        sp.set_int (lt::settings_pack::out_enc_policy,            lt::settings_pack::pe_enabled);
        sp.set_int (lt::settings_pack::mixed_mode_algorithm,      lt::settings_pack::prefer_tcp);

        // Piece Picking & Throughput
        sp.set_bool(lt::settings_pack::predictive_piece_announce, false);
        sp.set_int (lt::settings_pack::whole_pieces_threshold,    20); // force fast peers to finish whole pieces
        sp.set_bool(lt::settings_pack::prioritize_partial_pieces, true);
        sp.set_bool(lt::settings_pack::strict_end_game_mode,      false); // duplicate requests for critical pieces

        // Extreme Disk & Buffer settings
        sp.set_int (lt::settings_pack::max_out_request_queue,     5000);
        sp.set_int (lt::settings_pack::max_allowed_in_request_queue, 10000);
        sp.set_int (lt::settings_pack::disk_io_read_mode,         lt::settings_pack::enable_os_cache);
        sp.set_int (lt::settings_pack::disk_io_write_mode,        lt::settings_pack::enable_os_cache);
        sp.set_int (lt::settings_pack::send_buffer_watermark,     1024 * 1024);

        // Parallel async disk I/O threads for hashing + read
        sp.set_int (lt::settings_pack::aio_threads,               4);

        // Note: cache_size was removed in libtorrent 2.0 — the OS handles caching via mmap.
        // Keep uTP enabled — many NAT'd peers only support uTP, disabling it shrinks the peer pool

        // Aggressive per-peer requesting: target 1s of queue instead of default 3s
        sp.set_int (lt::settings_pack::request_queue_time,        1);

        // Faster internal tick for quicker scheduler response to deadline changes
        sp.set_int (lt::settings_pack::tick_interval,             100);

        // Large socket buffers so libtorrent can receive data faster from peers
        sp.set_int (lt::settings_pack::recv_socket_buffer_size,   1024 * 1024);
        sp.set_int (lt::settings_pack::send_socket_buffer_size,   1024 * 1024);
        
        auto* sw = new SessionWrapper(std::move(sp));
        sw->start_alert_thread();
        set_err("");
        return reinterpret_cast<lt_session_handle>(sw);
    } catch (const std::exception& e) { set_err(e.what()); return nullptr; }
}

TORRENT_API void lt_destroy_session(lt_session_handle session) {
    if (!session) return;
    auto* sw = to_sw(session);

    // Stop all streams first
    {
        std::lock_guard<std::mutex> lk(sw->streams_mu);
        for (auto& kv : sw->streams) {
            kv.second->active = false;
            kv.second->wake_all_waiters();
        }
    }
    {
        std::lock_guard<std::mutex> lk(sw->streams_mu);
        for (auto& kv : sw->streams) {
            if (kv.second->server_thread.joinable())   kv.second->server_thread.join();
            if (kv.second->priority_thread.joinable()) kv.second->priority_thread.join();
        }
        sw->streams.clear();
    }

    // Stop alert thread
    sw->alert_running = false;
    if (sw->alert_thread.joinable()) sw->alert_thread.join();

    // Save resume data
    {
        std::lock_guard<std::mutex> lk(sw->mu);
        for (auto& kv : sw->handles)
            if (kv.second.is_valid())
                try { kv.second.save_resume_data(
                    lt::torrent_handle::flush_disk_cache); } catch (...) {}
    }
    std::this_thread::sleep_for(chr::milliseconds(200));
    delete sw;
}

// Dart reads from our pre-filled queue – NO pop_alerts() here
TORRENT_API void lt_poll_alerts(lt_session_handle session,
                                  lt_alert_callback cb, void* user_data)
{
    if (!session || !cb) return;
    auto* sw = to_sw(session);

    std::deque<AlertRecord> local;
    {
        std::lock_guard<std::mutex> lk(sw->dart_queue_mu);
        local.swap(sw->dart_queue);
    }
    for (auto& rec : local)
        cb(rec.type, rec.torrent_id, rec.message.c_str(), user_data);
}

TORRENT_API lt_torrent_id lt_add_magnet(lt_session_handle session,
                                          const char* uri, const char* path,
                                          int stream_only)
{
    if (!session || !uri || !path) { set_err("null arg"); return -1; }
    auto* sw = to_sw(session);
    try {
        lt::error_code ec;
        lt::add_torrent_params atp = lt::parse_magnet_uri(uri, ec);
        if (ec) { set_err(ec.message()); return -1; }
        atp.save_path = path;
        
        // Start actively to fetch metadata, but stop automatically once metadata is received
        atp.flags &= ~lt::torrent_flags::paused;
        atp.flags &= ~lt::torrent_flags::auto_managed;
        // Don't use stop_when_ready, instead we will set priorities to 0 in the alert handler
        // or just let it pause, but the user says paused stops metadata fetching.
        // The issue is libtorrent starts downloading right after metadata is fetched before dart can react.
        // We can solve this by listening to metadata_received_alert.

        if (stream_only) {
            atp.storage_mode = lt::storage_mode_sparse;
            atp.flags |= lt::torrent_flags::stop_when_ready;
            // We want to fetch metadata first, then pick file.
            // set_piece_deadline will handle the rest.
        }

        lt::torrent_handle h = sw->session.add_torrent(std::move(atp), ec);
        if (ec) { set_err(ec.message()); return -1; }
        
        // Force resume just to be absolutely sure it fetches metadata
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

TORRENT_API lt_torrent_id lt_add_torrent_file(lt_session_handle session,
                                               const char* fp, const char* path,
                                               int stream_only)
{
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
        atp.flags |= lt::torrent_flags::stop_when_ready;
        
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

TORRENT_API void lt_remove_torrent(lt_session_handle session,
                                    lt_torrent_id id, int del)
{
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it == sw->handles.end()) return;
    sw->session.remove_torrent(it->second,
        del ? lt::session::delete_files : lt::remove_flags_t{});
    sw->handles.erase(it);
}

TORRENT_API void lt_pause_torrent(lt_session_handle session, lt_torrent_id id) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it != sw->handles.end() && it->second.is_valid())
        try { it->second.pause(); } catch (...) {}
}

TORRENT_API void lt_resume_torrent(lt_session_handle session, lt_torrent_id id) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it != sw->handles.end() && it->second.is_valid())
        try { it->second.resume(); } catch (...) {}
}

TORRENT_API void lt_recheck_torrent(lt_session_handle session, lt_torrent_id id) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it != sw->handles.end() && it->second.is_valid())
        try { it->second.force_recheck(); } catch (...) {}
}

TORRENT_API int lt_get_torrent_count(lt_session_handle session) {
    if (!session) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    return (int)sw->handles.size();
}

TORRENT_API int lt_get_all_statuses(lt_session_handle session,
                                     lt_torrent_status* out, int max)
{
    if (!session || !out || max <= 0) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    int n = 0;
    for (auto& kv : sw->handles) {
        if (n >= max) break;
        if (!kv.second.is_valid()) continue;
        try { 
            fill_status(out[n++], kv.first, kv.second.status(lt::torrent_handle::query_pieces)); 
        } catch (...) {}
    }
    return n;
}

TORRENT_API int lt_get_status(lt_session_handle session, lt_torrent_id id,
                               lt_torrent_status* out)
{
    if (!session || !out) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it == sw->handles.end() || !it->second.is_valid()) return 0;
    try { 
        fill_status(*out, id, it->second.status(lt::torrent_handle::query_pieces)); 
        return 1; 
    } catch (...) { return 0; }
}

TORRENT_API int lt_get_file_count(lt_session_handle session, lt_torrent_id id) {
    if (!session) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it == sw->handles.end() || !it->second.is_valid()) return 0;
    try {
        auto ti = it->second.torrent_file();
        return ti ? ti->num_files() : 0;
    } catch (...) { return 0; }
}

TORRENT_API int lt_get_files(lt_session_handle session, lt_torrent_id id,
                              lt_file_info* out, int max)
{
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
            out[n].index        = i;
            out[n].size         = fs.file_size(fi);
            out[n].is_streamable = is_streamable(
                fs.file_name(fi).to_string()) ? 1 : 0;
            std::string nm = fs.file_name(fi).to_string();
            std::string pt = fs.file_path(fi);
            std::strncpy(out[n].name, nm.c_str(), sizeof(out[n].name)-1);
            std::strncpy(out[n].path, pt.c_str(), sizeof(out[n].path)-1);
            out[n].name[sizeof(out[n].name)-1] = 0;
            out[n].path[sizeof(out[n].path)-1] = 0;
        }
        return n;
    } catch (...) { return 0; }
}

TORRENT_API void lt_set_file_priorities(lt_session_handle session, lt_torrent_id id, int* priorities, int count) {
    if (!session || !priorities || count <= 0) return;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->mu);
    auto it = sw->handles.find(id);
    if (it == sw->handles.end() || !it->second.is_valid()) return;
    try {
        auto ti = it->second.torrent_file();
        if (!ti) return;
        int num_files = ti->files().num_files();
        std::vector<lt::download_priority_t> p;
        p.reserve(num_files);
        for (int i = 0; i < num_files; ++i) {
            if (i < count) {
                p.push_back(priorities[i] ? lt::default_priority : lt::dont_download);
            } else {
                p.push_back(lt::dont_download);
            }
        }
        it->second.prioritize_files(p);
        
        // Start downloading now that files are selected
        it->second.unset_flags(lt::torrent_flags::stop_when_ready);
        it->second.resume();
    } catch (...) {}
}

TORRENT_API lt_stream_id lt_start_stream(lt_session_handle session,
                                          lt_torrent_id torrent_id,
                                          int file_index, int* out_port,
                                          int64_t max_cache_bytes)
{
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

    // Auto-select largest streamable file
    if (file_index < 0) {
        int64_t best = -1; file_index = 0;
        for (int i = 0; i < fs.num_files(); ++i) {
            int64_t sz = fs.file_size(lt::file_index_t{i});
            if (sz > best && is_streamable(
                    fs.file_name(lt::file_index_t{i}).to_string())) {
                best = sz; file_index = i;
            }
        }
    }
    if (file_index < 0 || file_index >= fs.num_files()) {
        set_err("invalid file index"); return -1;
    }

    auto ss = std::make_shared<StreamSession>();
    ss->id           = sw->next_stream_id.fetch_add(1);
    ss->torrent_id   = torrent_id;
    ss->file_index   = file_index;
    ss->handle       = handle;
    ss->ti           = ti;
    ss->piece_length = ti->piece_length();
    ss->file_size    = fs.file_size(lt::file_index_t{file_index});
    ss->file_offset  = fs.file_offset(lt::file_index_t{file_index});

    // Cache eviction config
    ss->max_cache_bytes = max_cache_bytes;
    if (max_cache_bytes > 0 && ss->piece_length > 0) {
        // Safety buffer = 10% of limit, but at least 5 pieces
        int pct_pieces = (int)((max_cache_bytes / 10) / ss->piece_length);
        ss->safety_pieces = std::max(5, pct_pieces);
    }

    ss->first_piece  = std::max(0,
        (int)(ss->file_offset / ss->piece_length));
    ss->last_piece   = std::min((int)ti->num_pieces() - 1,
        (int)((ss->file_offset + ss->file_size - 1) / ss->piece_length));

    // Focus bandwidth on this file only
    try {
        std::vector<lt::download_priority_t> fp(
            (size_t)fs.num_files(), lt::dont_download);
        fp[(size_t)file_index] = lt::default_priority;
        handle.prioritize_files(fp);
        
        // Clear stop_when_ready and resume in case it was paused
        handle.unset_flags(lt::torrent_flags::stop_when_ready);
        handle.resume();

        // INSTANT-START PRELOAD: stagger deadlines so piece 0 completes first.
        // libtorrent's time-critical picker distributes block requests across
        // ALL pieces with the same deadline. By staggering, piece 0 gets ALL
        // the bandwidth first, then piece 1, etc.
        int head_end = std::min(ss->first_piece + 4, ss->last_piece);
        for (int i = ss->first_piece; i <= head_end; ++i) {
            int deadline_ms = (i - ss->first_piece) * 500; // 0, 500, 1000, 1500, 2000
            handle.set_piece_deadline(lt::piece_index_t(i), deadline_ms,
                                      lt::torrent_handle::alert_when_available);
            handle.piece_priority(lt::piece_index_t(i), lt::download_priority_t{7});
        }

        // Mark ready — HTTP thread will block until each requested piece arrives
        ss->is_ready.store(true);
    } catch (...) {}

    lt_stream_id sid = ss->id;

    {
        std::lock_guard<std::mutex> lk(sw->streams_mu);
        sw->streams[sid] = ss;
    }

    fprintf(stderr, "[Native] Starting HTTP server for stream %lld\n", sid);
    ss->server_thread = std::thread([ss]() { run_http_server(ss); });

    // Wait for server to bind
    for (int i = 0; i < 200 && !ss->server_running.load(); ++i)
        std::this_thread::sleep_for(chr::milliseconds(10));

    if (!ss->server_running.load()) {
        fprintf(stderr, "[Native] HTTP server failed to bind for stream %lld\n", sid);
        ss->active = false;
        if (ss->server_thread.joinable()) ss->server_thread.join();
        std::lock_guard<std::mutex> lk(sw->streams_mu);
        sw->streams.erase(sid);
        set_err("HTTP server failed to bind"); return -1;
    }

    if (out_port) *out_port = ss->port;
    fprintf(stderr, "[Native] HTTP server bound to port %d\n", ss->port);

    ss->priority_thread = std::thread([ss]() { run_priority_loop(ss); });
    try { ss->update_priorities(); } catch (...) {}

    set_err(""); return sid;
}

TORRENT_API void lt_stop_stream(lt_session_handle session, lt_stream_id sid) {
    if (!session) return;
    auto* sw = to_sw(session);
    std::shared_ptr<StreamSession> ss;
    {
        std::lock_guard<std::mutex> lk(sw->streams_mu);
        auto it = sw->streams.find(sid);
        if (it == sw->streams.end()) return;
        ss = it->second;
        sw->streams.erase(it);
    }
    
    lt_torrent_id tid = ss->torrent_id;
    ss->active = false;
    ss->wake_all_waiters();
    if (ss->server_thread.joinable())   ss->server_thread.join();
    if (ss->priority_thread.joinable()) ss->priority_thread.join();
    
    bool ephemeral = false;
    {
        std::lock_guard<std::mutex> lk(sw->mu);
        if (sw->ephemeral_torrents.count(tid)) {
            ephemeral = true;
            sw->ephemeral_torrents.erase(tid);
        }
    }

    if (ephemeral) {
        fprintf(stderr, "[Native] Deleting ephemeral torrent %lld\n", tid);
        lt_remove_torrent(session, tid, 1); // 1 = delete files
    } else {
        try {
            auto ti2 = ss->handle.torrent_file();
            if (ti2) {
                int nf = ti2->files().num_files();
                std::vector<lt::download_priority_t> p(
                    (size_t)nf, lt::default_priority);
                ss->handle.prioritize_files(p);
                ss->handle.unset_flags(lt::torrent_flags::sequential_download);
            }
        } catch (...) {}
    }
}

static void fill_stream_status(lt_stream_status* out, const std::shared_ptr<StreamSession>& ss) {
    out->id         = ss->id;
    out->torrent_id = ss->torrent_id;
    out->file_index = ss->file_index;
    out->file_size  = ss->file_size;
    out->read_head  = ss->read_head.load();
    out->buffer_pct = ss->buffer_pct.load();
    out->is_ready   = ss->is_ready.load() ? 1 : 0;
    out->is_active  = ss->active.load()   ? 1 : 0;
    std::string url = ss->url();
    std::strncpy(out->url, url.c_str(), sizeof(out->url)-1);
    out->url[sizeof(out->url)-1] = 0;
}

TORRENT_API int lt_get_stream_status(lt_session_handle session,
                                      lt_stream_id sid, lt_stream_status* out)
{
    if (!session || !out) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->streams_mu);
    auto it = sw->streams.find(sid);
    if (it == sw->streams.end()) return 0;
    fill_stream_status(out, it->second);
    return 1;
}

TORRENT_API int lt_get_all_stream_statuses(lt_session_handle session,
                                            lt_stream_status* out, int max)
{
    if (!session || !out || max <= 0) return 0;
    auto* sw = to_sw(session);
    std::lock_guard<std::mutex> lk(sw->streams_mu);
    int n = 0;
    for (auto& kv : sw->streams) {
        if (n >= max) break;
        fill_stream_status(&out[n++], kv.second);
    }
    return n;
}

TORRENT_API void lt_set_download_limit(lt_session_handle session, int bps) {
    if (!session) return;
    lt::settings_pack sp;
    sp.set_int(lt::settings_pack::download_rate_limit, bps);
    to_sw(session)->session.apply_settings(sp);
}

TORRENT_API void lt_set_upload_limit(lt_session_handle session, int bps) {
    if (!session) return;
    lt::settings_pack sp;
    sp.set_int(lt::settings_pack::upload_rate_limit, bps);
    to_sw(session)->session.apply_settings(sp);
}

TORRENT_API const char* lt_last_error(void) { return g_last_error.c_str(); }
TORRENT_API const char* lt_version(void)    { return LIBTORRENT_VERSION;   }

} // extern "C"