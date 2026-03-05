#ifndef PLAYER_DEV_ZETMEDIA_INCLUDE_COMMONDEFINE_H
#define PLAYER_DEV_ZETMEDIA_INCLUDE_COMMONDEFINE_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>

#include <fcntl.h>

#include <pthread.h>
#include <chrono>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/stat.h>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h> 

#define ZET_BOOL                    char
#define ZET_TRUE                     1
#define ZET_FALSE                    0
#define FFMPEG_EXE_WITH_CMDLINE      0
#define AUDIO_DEBUG                  0
#define MAX_TYPE_NAME_LENGTH        256
#define MAX_FILE_NAME_LENGTH        4096
#define MAX_READING_LENGTH          8192
/*
#if USE_STATIC_LIB
  #define FFMPEG_PATH "./../ZetMedia/third-party/ffmpeg/tools/static-bin/ffmpeg" // static ffmpeg lib use
#else
  #define FFMPEG_PATH "../ZetMedia/third-party/ffmpeg/tools/ffmpeg"  // shared ffmpeg lib use
  #define FFMPEG_LIB_PATH "../ZetMedia/third-party/ffmpeg/FFmpeg/Build/out/lib"
#endif
*/
//#define HLS_PATH                 "../ZetMedia/third-party/ffmpeg/tools"
//#define TRANSCODE_OUTPUT_PATH    "../ZetMedia/third-party/ffmpeg/tools"

#define HLS_PATH                 "./transcode-output"
#define TRANSCODE_OUTPUT_PATH    "./transcodeMdl"
#define FFMPEG_PATH_CACHE_FILE   "ffmpeg_file_path"

typedef int8_t       INT8;
typedef int16_t      INT16;
typedef int32_t      INT32;
typedef int64_t      INT64;

typedef uint8_t      UINT8;
typedef uint8_t      UCHAR;
typedef uint16_t     UINT16;
typedef uint32_t     UINT32;
typedef uint64_t     UINT64;

#define SAFE_DELETE(p)           do { if(p) { delete (p); (p) = NULL; } } while(0)
#define SAFE_DELETE_ARRAY(p)     do { if(p) { delete[] (p); (p) = NULL; } } while(0)
#define SAFE_FREE(p)             do { if(p) { free(p); (p) = NULL; } } while(0)
#define SAFE_RELEASE_FRAME(frame) \
        do { \
            if (frame) { \
                av_frame_unref(frame); \
                av_frame_free(&frame); \
                frame = NULL; \
            } \
        } while (0)

#define SAFE_RELEASE_PACKET(pkt) \
        do { \
            if (pkt) { \
                av_packet_unref(pkt); \
                av_packet_free(&pkt); \
                pkt = NULL; \
            } \
        } while (0)

#define ZET_ABS(x)              ((x) < (0) ? -(x) : (x))
#define ZET_MAX(a, b)           ((a) > (b) ? (a) : (b))
#define ZET_MAX3(a, b, c)       ZET_MAX(ZET_MAX(a,b),c)
#define ZET_MAX4(a, b, c, d)    ZET_MAX((a), ZET_MAX((b), (c), (d)))
#define ZET_MIN(a,b)            ((a) > (b) ? (b) : (a))
#define ZET_MIN3(a,b,c)         ZET_MIN(ZET_MIN(a,b),c)
#define ZET_MIN4(a, b, c, d)    ZET_MIN((a), ZET_MIN3((b), (c), (d)))

#define ZET_ALIGN(x, a)         (((x) + (a) - 1) & ~((a) - 1))
#define ZET_ALIGN_16(x)         ZET_ALIGN(x, 16)

#define ZET_ARRAY_ELEMS(a)      (sizeof(a) / sizeof((a)[0]))

#define ZET_SWAP(type, a, b) \
    do { \
        type SWAP_tmp = b; \
        b = a; \
        a = SWAP_tmp; \
    } while (0)

typedef struct _optionInfo {
    const char*     shortname;
    const char*     fullName;
    const char*     helpInfo;
} optionInfo;

static optionInfo zet_cmd_list[] = {
    {"hwaccels",        "hw acceleration enabled",   "for video processing, must use it before -i command if used!!!!!" },
    {"i",               "input_file",                "input test file"},
    {"o",               "output_file",               "output test file"},
    {"w",               "width",                     "the width of target file"},
    {"h",               "height",                    "the height of target file"},
    {"f",               "framerate",                 "the frameRate of target file"},
    {"a",               "audcodingType",             "audio coding type"},
    {"v",               "vidcodingType",             "video coding type"},
    {"s",               "audsamplerate",             "audio sample rate"},
    {"c",               "channels",                  "audio channels"},
    {"b",               "bitrate",                   "video bitrate"},
    {"t",               "filetype",                  "output file type"},
    {"n",               "thread num",                "the max thread num used in ffmpeg"},
    {"hls",             "hls protocol",              "hls protocol mark, no need to add parameters behind it"},
    {"live",            "hls live stream",           "use this index to enable hls live stream"},
};

typedef enum _zetStatus {
  ZET_OK,
  ZET_NOK,
  ZET_ERR_UNKNOWN,
  ZET_ERR_NULL_PTR,
  ZET_ERR_MALLOC,
  ZET_ERR_OPEN_FILE,
  ZET_ERR_VALUE,
  ZET_ERR_TIMEOUT,
  ZET_ERR_CONTINUE
} zetStatus;

enum LogLevel {
    LOG_VERBOSE= 0,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
    LOG_CRITICAL
};

#ifndef CURRENT_LOG_LEVEL
#define CURRENT_LOG_LEVEL 2
#endif

extern LogLevel currentLogLevel;

extern const char* FFMPEG_BIN_PATH;
extern const char* FFMPEG_SHARED_LIB_PATH;
static std::string ffmpeg_bin_path;
static std::string ffmpeg_shareLib_path;

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

#define LOG(level, format, ...) \
    do { \
        if (level >= currentLogLevel) { \
            pthread_mutex_lock(&log_mutex); \
            struct timespec ts; \
            clock_gettime(CLOCK_REALTIME, &ts); \
            struct tm tm_info; \
            localtime_r(&ts.tv_sec, &tm_info); \
            char time_str[64]; \
            snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d.%03ld", \
                     tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday, \
                     tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, \
                     ts.tv_nsec / 1000000); \
            const char* levelStr = ""; \
            const char* color    = ""; \
            const char* reset    = "\033[0m"; \
            \
            switch (level) { \
                case LOG_DEBUG:    color = "\033[36m";   levelStr = "DEBUG";    break; \
                case LOG_INFO:     color = "\033[32m";   levelStr = "INFO";     break; \
                case LOG_WARNING:  color = "\033[33m";   levelStr = "WARNING";  break; \
                case LOG_ERROR:    color = "\033[31m";   levelStr = "ERROR";    break; \
                case LOG_CRITICAL: color = "\033[1;31m"; levelStr = "CRITICAL"; break; \
            } \
            \
            printf("%s[%s] %s %s:%s:%d: " format "%s\n", \
                   color, time_str, levelStr, __FILE__, __FUNCTION__, __LINE__, \
                   ##__VA_ARGS__, reset); \
            \
            pthread_mutex_unlock(&log_mutex); \
        } \
    } while (0)

#define ZETCHECK_PTR_IS_NULL(ptr) \
	do { \
        if ((ptr) == NULL) { \
            printf("[%s:%d] Error: Null pointer detected for %p\n", \
                       __FUNCTION__, __LINE__, ptr); \
            return ZET_FALSE; \
        } \
    } while (0)

void          optionsExplain();
void          appendOption(char* &ptr, const char* opt);
bool          cache_file_exists();
bool          read_ffmpegPaths_from_cache();
void          write_paths_to_cache(const std::string& bin_path, const std::string& lib_path);
bool          is_regular_file(const std::string& path);
bool          is_directory(const std::string& path);
std::string   path_join(const std::string& dir, const std::string& filename);
bool          search_file_recursive(const std::string& root_dir,
                                             const std::string& target_filename,
                                             std::string& result_path);
const char**  get_ffmpeg_search_roots(int* count);
std::string   find_ffmpeg_executable();
std::string   find_ffmpeg_library_dir();
void          generateNewCmd(char* info, int target_seekTime);
#endif
