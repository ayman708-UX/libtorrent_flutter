#pragma once

#ifdef _WIN32
  #ifdef TORRENT_BRIDGE_EXPORTS
    #define TORRENT_API __declspec(dllexport)
  #else
    #define TORRENT_API __declspec(dllimport)
  #endif
#else
  #define TORRENT_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef void*   lt_session_handle;
typedef int64_t lt_torrent_id;
typedef int64_t lt_stream_id;

#define LT_STATE_ERROR             -2
#define LT_STATE_UNKNOWN           -1
#define LT_STATE_CHECKING_FILES     0
#define LT_STATE_DOWNLOADING_META   1
#define LT_STATE_DOWNLOADING        2
#define LT_STATE_FINISHED           3
#define LT_STATE_SEEDING            4
#define LT_STATE_ALLOCATING         5
#define LT_STATE_CHECKING_RESUME    6

typedef struct {
    lt_torrent_id id;
    char          name[512];
    char          save_path[1024];
    char          error_msg[256];
    int           state;
    float         progress;
    int32_t       download_rate;
    int32_t       upload_rate;
    int64_t       total_done;
    int64_t       total_wanted;
    int64_t       total_uploaded;
    int32_t       num_peers;
    int32_t       num_seeds;
    int32_t       num_pieces;
    int32_t       pieces_done;
    int32_t       is_paused;
    int32_t       is_finished;
    int32_t       has_metadata;
    int32_t       queue_position;
} lt_torrent_status;

typedef struct {
    int32_t index;
    char    name[512];
    char    path[1024];
    int64_t size;
    int32_t is_streamable;
} lt_file_info;

typedef struct {
    lt_stream_id  id;
    lt_torrent_id torrent_id;
    int32_t       file_index;
    char          url[128];
    int64_t       file_size;
    int64_t       read_head;
    int32_t       buffer_pct;
    int32_t       is_ready;
    int32_t       is_active;
} lt_stream_status;

typedef void (*lt_alert_callback)(int alert_type, lt_torrent_id id,
                                  const char* message, void* user_data);

TORRENT_API lt_session_handle lt_create_session(const char* listen_interface,
                                                 int download_limit,
                                                 int upload_limit);
TORRENT_API void lt_destroy_session(lt_session_handle session);

TORRENT_API void lt_poll_alerts(lt_session_handle session,
                                 lt_alert_callback cb, void* user_data);

TORRENT_API lt_torrent_id lt_add_magnet(lt_session_handle session,
                                         const char* magnet_uri,
                                         const char* save_path,
                                         int stream_only);
TORRENT_API lt_torrent_id lt_add_torrent_file(lt_session_handle session,
                                               const char* file_path,
                                               const char* save_path,
                                               int stream_only);
TORRENT_API void lt_remove_torrent(lt_session_handle session,
                                    lt_torrent_id id, int delete_files);
TORRENT_API void lt_pause_torrent(lt_session_handle session, lt_torrent_id id);
TORRENT_API void lt_resume_torrent(lt_session_handle session, lt_torrent_id id);
TORRENT_API void lt_recheck_torrent(lt_session_handle session, lt_torrent_id id);

TORRENT_API int lt_get_torrent_count(lt_session_handle session);
TORRENT_API int lt_get_all_statuses(lt_session_handle session,
                                     lt_torrent_status* statuses, int max_count);
TORRENT_API int lt_get_status(lt_session_handle session, lt_torrent_id id,
                               lt_torrent_status* out_status);

TORRENT_API int lt_get_file_count(lt_session_handle session, lt_torrent_id id);
TORRENT_API int lt_get_files(lt_session_handle session, lt_torrent_id id,
                              lt_file_info* out_files, int max_count);

TORRENT_API lt_stream_id lt_start_stream(lt_session_handle session,
                                          lt_torrent_id torrent_id,
                                          int file_index, int* out_port);
TORRENT_API void lt_stop_stream(lt_session_handle session, lt_stream_id stream_id);
TORRENT_API int lt_get_stream_status(lt_session_handle session,
                                      lt_stream_id stream_id,
                                      lt_stream_status* out_status);
TORRENT_API int lt_get_all_stream_statuses(lt_session_handle session,
                                            lt_stream_status* out_statuses,
                                            int max_count);

TORRENT_API void lt_set_download_limit(lt_session_handle session, int bps);
TORRENT_API void lt_set_upload_limit(lt_session_handle session, int bps);

TORRENT_API const char* lt_last_error(void);
TORRENT_API const char* lt_version(void);

#ifdef __cplusplus
}
#endif