#include <iostream>
#include <sys/stat.h> 
#include <unistd.h>
#include <sys/types.h>
#include <cmath>
#include "zetHlsServerMdl.h"
#include <time.h>

using namespace std;

#if ARM
// ARM-only switch: when true, use RGA index-based PTS generation for
// SW-decode + RGA + HW-encode on non-TS inputs (e.g. some MEncoder AVIs).
static bool g_arm_rga_use_index_ts = false;
#endif

static bool   g_first_video_written    = ZET_FALSE;
static INT64  g_first_video_pts        = AV_NOPTS_VALUE;
static double g_first_video_time_sec   = 0.0;
static bool   g_first_video_time_valid = ZET_FALSE;
static INT64  clip_start_pts	       = AV_NOPTS_VALUE;
static double clip_duration_sec        = 0.0;
static INT64  base_video_pts	       = AV_NOPTS_VALUE; // first encoded video PTS before normalization

typedef struct _zetHlsGenInfo {
    char            file_input[MAX_FILE_NAME_LENGTH];
    char            file_output[MAX_FILE_NAME_LENGTH];
    char            audCodingType[MAX_TYPE_NAME_LENGTH];
    char            vidCodingType[MAX_TYPE_NAME_LENGTH];
    char            fileType[MAX_TYPE_NAME_LENGTH];
    float           framerate;
    double          seekTime;
    double          seekEndTime;
    UINT32          width;
    UINT32          height;
    UINT32          bitrate;
    UINT32          audBitrate;
    UINT32          audIndex;
    UINT32          threads;
    UINT32          startNumber;
    UINT32          sampleRate;
    UINT32          channels;
    UINT32          hls_timeSec;
    bool            need_hls;
    bool            need_input;
    bool            need_output;
    bool            need_probe;
    bool            need_scale;
    bool            isLive;
    bool            seek_requested;
} zetHlsGenInfo;

// Helper to configure decoder threading based on codec type
static void zet_configure_decoder_threads(AVCodecContext *ctx, zetHlsGenInfo *info) {
    if (!ctx) return;

    int desired_threads = 0;
    if (info && info->threads > 0) {
        desired_threads = (int)info->threads;
    } else {
        desired_threads = av_cpu_count();
        if (desired_threads <= 0) {
            desired_threads = 2; // safe fallback when av_cpu_count is not available
        }
    }

    ctx->thread_count = desired_threads;

    int thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    switch (ctx->codec_id) {
        case AV_CODEC_ID_MPEG2VIDEO:
        case AV_CODEC_ID_MPEG1VIDEO:
            // For MPEG-2, slice threading is usually more stable and effective
            thread_type = FF_THREAD_SLICE;
            // Enable fast decode: skip loop filter on non-reference frames only, and allow fast non-spec compliant optimizations
            ctx->skip_loop_filter = AVDISCARD_NONREF;
            ctx->flags2          |= AV_CODEC_FLAG2_FAST;
            break;
        case AV_CODEC_ID_MPEG4:
            thread_type = FF_THREAD_FRAME;
            break;
        case AV_CODEC_ID_THEORA:
        case AV_CODEC_ID_H264:
        case AV_CODEC_ID_HEVC:
        default:
            // Keep both frame and slice threading enabled as a default for modern codecs
            thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
            break;
    }

    ctx->thread_type = thread_type;
    LOG(LOG_DEBUG, "Decoder threading configured: codec=%s, threads=%d, type=%d",
                   avcodec_get_name(ctx->codec_id), ctx->thread_count, ctx->thread_type);
}

static void zet_ffmpeg_log_callback(void* ptr, int level, const char* fmt, va_list vl)
{

    if (level > av_log_get_level()) {
        return;
    }

    va_list vl_copy;
    va_copy(vl_copy, vl);

    char line[1024];
    vsnprintf(line, sizeof(line), fmt, vl_copy);
    line[sizeof(line) - 1] = '\0';
    va_end(vl_copy);

    if (strstr(line, "Could not find codec parameters for stream") &&
        strstr(line, "hdmv_pgs_subtitle")) {
        // Drop this message silently.
        return;
    }
    if (strstr(line, "Skipping NAL unit 63")) {
        return;
    }
    av_log_default_callback(ptr, level, fmt, vl);
}

static double packet_get_seconds(AVPacket *packet, AVStream *stream) {
    if (!packet || !stream) return -1.0;
    INT64 ts = (packet->pts != AV_NOPTS_VALUE) ? packet->pts : packet->dts;
    if (ts == AV_NOPTS_VALUE) return -1.0;
    return av_q2d(stream->time_base) * ts;
}


// Lightweight profiling helper: wall-clock seconds
static inline double zet_prof_now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1000000000.0;
}

// ---------------- H.264 SPS parser for MBAFF detection ----------------

typedef struct {
    const uint8_t *data;
    int            size_in_bits;
    int            bit_pos;
} ZetBitReader;

static unsigned getBits1(ZetBitReader *br) {
    if (br->bit_pos >= br->size_in_bits) return 0;
    int byte_offset = br->bit_pos >> 3;
    int bit_offset  = 7 - (br->bit_pos & 7);
    br->bit_pos++;
    return (br->data[byte_offset] >> bit_offset) & 0x01;
}

static unsigned getBits(ZetBitReader *br, int n) {
    unsigned v = 0;
    while (n-- > 0) {
        v = (v << 1) | getBits1(br);
    }
    return v;
}

static unsigned getUe(ZetBitReader *br) {
    int zeros = 0;
    while (getBits1(br) == 0 && br->bit_pos < br->size_in_bits) {
        zeros++;
    }
    if (zeros == 0) return 0;
    unsigned suffix = 0;
    if (zeros > 0) {
        suffix = getBits(br, zeros);
    }
    return ((1u << zeros) - 1u) + suffix;
}

static int getSe(ZetBitReader *br) {
    unsigned ue = getUe(br);
    int val = (int)((ue + 1) / 2);
    return (ue & 1) ? val : -val;
}

static void skipScalingList(ZetBitReader *br, int size) {
    int last_scale = 8;
    int next_scale = 8;
    for (int j = 0; j < size; j++) {
        if (next_scale != 0) {
            int delta_scale = getSe(br);
            next_scale = (last_scale + delta_scale + 256) % 256;
        }
        last_scale = (next_scale == 0) ? last_scale : next_scale;
    }
}

// Minimal H.264 SPS MBAFF detector: returns true when frame_mbs_only_flag==0 and mb_adaptive_frame_field_flag==1
static bool isH264StreamMbaff(const AVCodecParameters *par) {
    if (!par || par->codec_id != AV_CODEC_ID_H264 || !par->extradata || par->extradata_size <= 4)
        return false;

    const uint8_t *data = par->extradata;
    int size            = par->extradata_size;

    // Find first SPS NAL (type 7)
    int sps_start = -1, sps_end = -1;
    for (int i = 0; i + 4 < size; i++) {
        if (data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x01) {
            uint8_t nal_type = data[i+3] & 0x1F;
            if (nal_type == 7) {
                sps_start = i + 3; // nal header index
                // find end (next start code or end)
                int j = i + 3;
                for (; j + 3 < size; j++) {
                    if (data[j] == 0x00 && data[j+1] == 0x00 &&
                        (data[j+2] == 0x01 || (data[j+2] == 0x00 && data[j+3] == 0x01))) {
                        break;
                    }
                }
                sps_end = j;
                break;
            }
        } else if (i + 5 < size && data[i] == 0x00 && data[i+1] == 0x00 && data[i+2] == 0x00 && data[i+3] == 0x01) {
            uint8_t nal_type = data[i+4] & 0x1F;
            if (nal_type == 7) {
                sps_start = i + 4;
                int j = i + 4;
                for (; j + 4 < size; j++) {
                    if (data[j] == 0x00 && data[j+1] == 0x00 &&
                        (data[j+2] == 0x01 || (data[j+2] == 0x00 && data[j+3] == 0x01))) {
                        break;
                    }
                }
                sps_end = j;
                break;
            }
        }
    }

    if (sps_start < 0 || sps_end <= sps_start + 1)
        return false;

    // Build RBSP (remove emulation prevention bytes)
    uint8_t rbsp_buf[256];
    int     rbsp_size = 0;
    memset(rbsp_buf, 0, sizeof(rbsp_buf));
    int i = sps_start + 1; // skip NAL header byte
    int zeros = 0;
    for (; i < sps_end && rbsp_size < (int)sizeof(rbsp_buf); i++) {
        uint8_t b = data[i];
        if (zeros == 2 && b == 0x03) {
            // skip emulation prevention byte
            zeros = 0;
            continue;
        }
        rbsp_buf[rbsp_size++] = b;
        if (b == 0x00) zeros++;
        else           zeros = 0;
    }
    if (rbsp_size <= 0)
        return ZET_FALSE;

    ZetBitReader br;
    br.data        = rbsp_buf;
    br.size_in_bits= rbsp_size * 8;
    br.bit_pos     = 0;

    // profile_idc, constraint flags, level_idc
    unsigned profile_idc = getBits(&br, 8);
    getBits(&br, 8); // constraint_set_flags + reserved_zero_2bits
    getBits(&br, 8); // level_idc
    (void)profile_idc;
    getUe(&br);      // seq_parameter_set_id

    // High profiles have additional fields
    if (profile_idc == 100 || profile_idc == 110 || profile_idc == 122 || profile_idc == 244 ||
        profile_idc == 44  || profile_idc == 83  || profile_idc == 86  || profile_idc == 118 ||
        profile_idc == 128 || profile_idc == 138 || profile_idc == 144) {
        unsigned chroma_format_idc = getUe(&br);
        if (chroma_format_idc == 3) {
            getBits1(&br); // separate_colour_plane_flag
        }
        getUe(&br); // bit_depth_luma_minus8
        getUe(&br); // bit_depth_chroma_minus8
        getBits1(&br); // qpprime_y_zero_transform_bypass_flag
        unsigned seq_scaling_matrix_present_flag = getBits1(&br);
        if (seq_scaling_matrix_present_flag) {
            int scaling_list_count = (chroma_format_idc == 3) ? 12 : 8;
            for (int idx = 0; idx < scaling_list_count; idx++) {
                unsigned present = getBits1(&br);
                if (present) {
                    if (idx < 6) skipScalingList(&br, 16);
                    else         skipScalingList(&br, 64);
                }
            }
        }
    }

    getUe(&br); // log2_max_frame_num_minus4
    unsigned pic_order_cnt_type = getUe(&br);
    if (pic_order_cnt_type == 0) {
        getUe(&br); // log2_max_pic_order_cnt_lsb_minus4
    } else if (pic_order_cnt_type == 1) {
        getBits1(&br); // delta_pic_order_always_zero_flag
        getSe(&br);    // offset_for_non_ref_pic
        getSe(&br);    // offset_for_top_to_bottom_field
        unsigned num_ref_frames_in_pic_order_cnt_cycle = getUe(&br);
        for (unsigned i2 = 0; i2 < num_ref_frames_in_pic_order_cnt_cycle; i2++) {
            getSe(&br);
        }
    }

    getUe(&br); // max_num_ref_frames
    getBits1(&br); // gaps_in_frame_num_value_allowed_flag
    getUe(&br); // pic_width_in_mbs_minus1
    getUe(&br); // pic_height_in_map_units_minus1

    unsigned frame_mbs_only_flag = getBits1(&br);
    if (frame_mbs_only_flag) {
        // Pure frame coding, no MBAFF
        LOG(LOG_INFO, "H.264 SPS: frame_mbs_only_flag=1 (no MBAFF)");
        return ZET_FALSE;
    }
    unsigned mb_adaptive_frame_field_flag = getBits1(&br);
    LOG(LOG_INFO, "H.264 SPS: frame_mbs_only_flag=0, mb_adaptive_frame_field_flag=%u", mb_adaptive_frame_field_flag);
    return (mb_adaptive_frame_field_flag != 0);
}

static void showHlsServerCmdInfo(int argc, char*argv[]) {
    printf(" %s %d the full input command info is :\n", __FUNCTION__, __LINE__);
    for (int i = 0; i< argc; i++) {
        printf(" %s", argv[i]);
    }
    printf(" \n");
}

#if FFMPEG_EXE_WITH_CMDLINE

INT32 zetHlsServerMdl::processWithCmdLine(void* msg) {
    ZETCHECK_PTR_IS_NULL(hlsCmdInfo);
    LOG(LOG_DEBUG, "called, hlsCmdInfo: %s", hlsCmdInfo);

    char hlsGenFullCmd[MAX_READING_LENGTH];

#if USE_STATIC_LIB
    if (strlen(hlsCmdInfo) < MAX_READING_LENGTH) {
        snprintf(hlsGenFullCmd, sizeof(hlsGenFullCmd), "%s", hlsCmdInfo);
    } else {
        LOG(LOG_ERROR, "command info longer than default size, quit");
        return ZET_NOK;
    }
#else
    snprintf(hlsGenFullCmd, sizeof(hlsGenFullCmd), "export LD_LIBRARY_PATH=%s:$LD_LIBRARY_PATH && %s", FFMPEG_SHARED_LIB_PATH, hlsCmdInfo);
#endif
    LOG(LOG_DEBUG, "full process command is: %s", hlsGenFullCmd);

 {
    if (ffmpeg_pid != -1) {
        kill(ffmpeg_pid, SIGTERM);
        waitpid(ffmpeg_pid, NULL, WNOHANG);
        ffmpeg_pid = -1;
    }

    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", hlsGenFullCmd, NULL);
        LOG(LOG_ERROR, "execl failed: %s", strerror(errno));
        exit(1);
    } else if (pid > 0) {
        ffmpeg_pid = pid;
        LOG(LOG_DEBUG, "FFmpeg process mark, ID: %d", ffmpeg_pid);
        return ZET_OK;
    } else {
        LOG(LOG_ERROR, "fork FFmpeg process failed");
        return ZET_NOK;
    }
 }

}

#else

zetHlsServerMdl* zetHlsServerMdl::s_current_instance = NULL;

typedef struct {
    char            cmdType[128];
    double          seekTime;
    double          seekEndTime;
    pthread_mutex_t mutex;
    pthread_cond_t  cond;
} CommandMsg;

static CommandMsg zet_cmd_msg;
std::atomic<bool> stopdemuxerThreads(false);
std::atomic<bool> stopVideoThreads(false);
std::atomic<bool> stopAudioThreads(false);

typedef struct {
    zetHlsServerMdl            *instance;
    AVFormatContext            *in_fmt_ctx;
    AVFormatContext            *out_fmt_ctx;
    AVCodecContext             *in_video_ctx;
    AVCodecContext             *in_audio_ctx;
    AVCodecContext             *out_video_ctx;
    AVCodecContext             *out_audio_ctx;
    AVStream                   *out_video_stream;
    AVStream                   *out_audio_stream; 
    SwsContext                 *sws_ctx;
    SwrContext                 *swr_ctx;
    ThreadSafeQueue<AVPacket*> *videoQueue;
    ThreadSafeQueue<AVPacket*> *audioQueue;
    bool                        video_needs_transcode;
    bool                        audio_needs_transcode;
    bool                        isVideo;
} zetHlsTransThreadArgs;


bool zetHlsServerMdl::checkAndProcessCommands(AVFormatContext* in_fmt_ctx) {
    //pthread_mutex_lock(&zet_cmd_msg.mutex);
    bool ret = ZET_FALSE;

    if ((hlsGenInfo && hlsGenInfo->seek_requested && hlsGenInfo->seekTime >=0)) {
        LOG(LOG_INFO, "check seek command, Processing seek command to %f", zet_cmd_msg.seekTime);
        last_video_pts       = AV_NOPTS_VALUE;
        last_video_dts       = AV_NOPTS_VALUE;
        last_audio_pts       = AV_NOPTS_VALUE;
        last_audio_dts       = AV_NOPTS_VALUE;

        // Perform seek operation
        if (avformat_seek_file(in_fmt_ctx, -1, INT64_MIN, (INT64)(hlsGenInfo->seekTime * AV_TIME_BASE), INT64_MAX, 0) < 0) {
            LOG(LOG_ERROR, "Failed to seek to %f seconds", zet_cmd_msg.seekTime);
            ret = ZET_FALSE;
        } else {
            ret = ZET_TRUE;
        }
    }  else if (!strcasecmp(zet_cmd_msg.cmdType, "stop")) {
        process_stop_requested.store(ZET_TRUE);
        zet_cmd_msg.seekTime       = -1;
        hlsGenInfo->seekTime       = zet_cmd_msg.seekTime;
        hlsGenInfo->seek_requested = ZET_FALSE;
        ret                        = ZET_TRUE;
    }
    //pthread_mutex_unlock(&zet_cmd_msg.mutex);
    return ret;
}

void zetHlsServerMdl::signalHandlerForward(int signum, siginfo_t* info, void* ucontext) {
    if (s_current_instance != NULL) {
        s_current_instance->handleSignal(signum);
    }
}

void zetHlsServerMdl::handleSignal(int signum) {
    if (signum == SIGINT || signum == SIGTERM || signum == SIGQUIT) {
        process_stop_requested.store(ZET_TRUE, std::memory_order_relaxed); // force to use memory order to make sure it can be receivedd by thread
        LOG(LOG_INFO, "Received Ctrl+C (SIGINT), setting process_stop_requested to true");
    }
}

INT32 zetHlsServerMdl::initHlsCtx(struct _zetHlsGenInfo* info, AVFormatContext* in_fmt_ctx, AVCodecContext* in_video_ctx,
                                  AVCodecContext* in_audio_ctx, AVCodecContext* out_video_ctx, AVCodecContext* out_audio_ctx,
                                  SwsContext* sws_ctx, SwrContext* swr_ctx, AVFormatContext**out_fmt_ctx,
                                  AVDictionary** hls_opts, std::string& hls_playlist_path, std::string& hls_segment_pattern) {
    hls_playlist_path   = info->file_output;
    size_t last_slash   = hls_playlist_path.find_last_of("/");
    std::string hls_dir = (last_slash != std::string::npos) ? hls_playlist_path.substr(0, last_slash): ".";
    hls_segment_pattern = hls_dir + "/segment_%06d_tmp.ts";

    *out_fmt_ctx = NULL;
    if (avformat_alloc_output_context2(out_fmt_ctx, NULL, "hls", hls_playlist_path.c_str()) < 0) {
        LOG(LOG_ERROR, "unable to alloc output context");
        freeResources(&in_fmt_ctx, out_fmt_ctx, &in_video_ctx, &in_audio_ctx,
        &out_video_ctx, &out_audio_ctx, &sws_ctx, &swr_ctx, NULL, NULL);
        return ZET_NOK;
    }    

    *hls_opts = NULL;
    info->need_scale ? av_dict_set(hls_opts, "hls_time", "36000", 0) : av_dict_set(hls_opts, "hls_time", "2", 0);
    //av_dict_set(hls_opts, "hls_time", "2", 0);
    av_dict_set(hls_opts, "hls_segment_filename", hls_segment_pattern.c_str(), 0);
    av_dict_set(hls_opts, "hls_list_size", "0", 0);
    //av_dict_set(hls_opts, "hls_start_number", "0", 0);
    if (hlsGenInfo->seekEndTime > 0) {
        INT32 start_number = hlsGenInfo->startNumber;
        char  start_number_str[32];
        snprintf(start_number_str, sizeof(start_number_str), "%d", start_number);
        av_dict_set(hls_opts, "start_number", start_number_str, 0);
    } else {
        av_dict_set(hls_opts, "start_number", "0", 0);
    }
    av_dict_set(hls_opts, "hls_flags", "split_by_time", 0);
    av_dict_set(hls_opts, "hls_segment_type", "mpegts", 0);
    return ZET_OK;
}

// if codec type meet the recommend type as HLS need
static bool isNeedTranscode(AVCodecParameters * para, struct _zetHlsGenInfo* info, AVCodecID codecID) {
    ZETCHECK_PTR_IS_NULL(info);

    if (para->codec_type == AVMEDIA_TYPE_VIDEO) {
        bool paraSettings = (info->height > 0 && info->width > 0) ? ZET_TRUE : ZET_FALSE;
        if (paraSettings && codecID == para->codec_id && (info->height == para->height && info->width== para->width)) {
            LOG(LOG_INFO, "video no need to transcode, src codec id: %s, dst codec id: %s, src width: %d, dst width: %d, src height: %d, dst height: %d",
                            avcodec_get_name(para->codec_id), avcodec_get_name(codecID), para->width, info->width, para->height, info->height);
            return ZET_FALSE;
        } else if (!paraSettings) {
            if (codecID == para->codec_id) {
                LOG(LOG_INFO, "no w or h parasettings and has the same codecId, no need to transcode");
                return ZET_FALSE;
            }
            return ZET_TRUE;
        } else {
            LOG(LOG_INFO, "video need to transcode, src codec id: %s, dst codec id: %s, src width: %d, dst width: %d, src height: %d, dst height: %d",
                            avcodec_get_name(para->codec_id), avcodec_get_name(codecID), para->width, info->width, para->height, info->height);
            return ZET_TRUE;
        }

    } else {
        bool paraSettings = (info->sampleRate > 0 || (info->audCodingType[0] != '\0')) ? ZET_TRUE : ZET_FALSE; 
        // For those audio codec type warning info: muxed as a private data stream and may not be recognized upon reading.
        // switch these type to aac for hls protocol
        if (info->need_hls && para->codec_id != AV_CODEC_ID_AAC && para->codec_id != AV_CODEC_ID_MP3
			               && para->codec_id != AV_CODEC_ID_AC3 && para->codec_id != AV_CODEC_ID_EAC3 
			               && para->codec_id != AV_CODEC_ID_OPUS
			/*para->codec_id == AV_CODEC_ID_WMAV2 || para->codec_id == AV_CODEC_ID_VORBIS
            || para->codec_id == AV_CODEC_ID_DTS*/) {
            LOG(LOG_INFO, "HLS output does not support direct copy of WMA (codec=%s), force audio transcode to %s",
            avcodec_get_name(para->codec_id),avcodec_get_name(codecID));
            return ZET_TRUE;
        }

        if (info->need_scale) {
            LOG(LOG_INFO, "only scale video, audio no need to transcode");
            return ZET_FALSE;
        }
        if (paraSettings && codecID == para->codec_id && info->sampleRate== para->sample_rate) {
            LOG(LOG_INFO, "audio no need to transcode, src codec id: %s, dst codec id: %s, src sampleRate: %d, dst sampleRate: %d",
                            avcodec_get_name(para->codec_id), avcodec_get_name(codecID), para->sample_rate, info->sampleRate);
            return ZET_FALSE;
        } else if (!paraSettings) {
            if (codecID == para->codec_id) {
                LOG(LOG_INFO, "audio no sampleRate parasettings and no input codecId, no need to transcode");
                return ZET_FALSE;
            }
            return ZET_TRUE;
        } else {
            LOG(LOG_INFO, "audio need to transcode, src codec id: %s, dst codec id: %s, src sampleRate: %d, dst sampleRate: %d",
                            avcodec_get_name(para->codec_id), avcodec_get_name(codecID), para->sample_rate, info->sampleRate);
            return ZET_TRUE;
        }
    }
}

AVCodecID zetHlsServerMdl::findTargetCodec(struct _zetHlsGenInfo* hlsGenInfo, bool isVideo) {
    AVCodecID out_codecId = isVideo ? AV_CODEC_ID_H264 : AV_CODEC_ID_AAC;
    if (!hlsGenInfo || (isVideo && hlsGenInfo->vidCodingType[0] == '\0') || (!isVideo && hlsGenInfo->audCodingType[0] == '\0')) {
        LOG(LOG_INFO, "input %s has no codec parameters, using default settings: %s", isVideo ? "Video" : "Audio", avcodec_get_name(out_codecId));
        return out_codecId;
    }

    if (hlsGenInfo) {
        for (int i = 0; i < ZET_ARRAY_ELEMS(codecMappingList); i++) {
            if (isVideo &&!strcmp(codecMappingList[i].mine, hlsGenInfo->vidCodingType)) {
                out_codecId = codecMappingList[i].av_codec_id;
                return out_codecId;
            }
            if (!isVideo && !strcmp(codecMappingList[i].mine, hlsGenInfo->audCodingType)) {
                out_codecId = codecMappingList[i].av_codec_id;
                return out_codecId;
            }
        }
    }
    return out_codecId;
}

static AVCodecContext* initOutputCodecCtx(AVCodecContext* in_codec_ctx, AVStream* in_stream, AVCodecID out_codec_id, struct _zetHlsGenInfo* info) {
    LOG(LOG_DEBUG, "input codec id : %s, codec_type: %s", avcodec_get_name(out_codec_id), (in_codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) ? "Video" : "Audio");
    const AVCodec* out_codec = avcodec_find_encoder(out_codec_id);
    if (!out_codec) {
        LOG(LOG_ERROR, "can not find output encoder: %s", avcodec_get_name(out_codec_id));
        return NULL;
    }

    AVCodecContext* out_codec_ctx = avcodec_alloc_context3(out_codec);

    if (!out_codec_ctx) {
        LOG(LOG_ERROR, "can not execute avcodec_alloc_context3");
        return NULL;
    }

    if (info && info->threads != 0) {
        out_codec_ctx->thread_count = info->threads;
    } else {
        //out_codec_ctx->thread_count = av_cpu_count();
        //out_codec_ctx->thread_type = FF_THREAD_SLICE;
        //out_codec_ctx->flags        = AV_CODEC_FLAG_LOW_DELAY;
    }

    if (in_codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        out_codec_ctx->width        = (info->width > 0)  ? info->width  : in_codec_ctx->width;
        out_codec_ctx->height       = (info->height > 0) ? info->height : in_codec_ctx->height;
        out_codec_ctx->bit_rate     = (info->bitrate > 0) ? info->bitrate* 1000 : in_codec_ctx->bit_rate;

        if (in_codec_ctx->framerate.num <= 0 || in_codec_ctx->framerate.den <= 0) {
            if (in_stream) {
                if (in_stream->r_frame_rate.num > 0 && in_stream->r_frame_rate.den > 0) {
                    out_codec_ctx->framerate = in_stream->r_frame_rate;
                } else if (in_stream->avg_frame_rate.num > 0 && in_stream->avg_frame_rate.den > 0) {
                    out_codec_ctx->framerate = in_stream->avg_frame_rate;
                } else {
                    out_codec_ctx->framerate = (AVRational){25, 1};
                }
            } else {
                out_codec_ctx->framerate = (AVRational){25, 1};
            }
            LOG(LOG_DEBUG, "Input framerate invalid, use: %d/%d", out_codec_ctx->framerate.num, out_codec_ctx->framerate.den);
        } else {
            out_codec_ctx->framerate = in_codec_ctx->framerate;
        }

#if USE_STATIC_LIB
        out_codec_ctx->pix_fmt    = out_codec->pix_fmts ? out_codec->pix_fmts[0] : in_codec_ctx->pix_fmt;
        //out_codec_ctx->pix_fmt  = AV_PIX_FMT_YUV420P;
#else

#if X86_64
        out_codec_ctx->pix_fmt    = in_codec_ctx->pix_fmt;
#elif ARM

     #if defined(__GNUC__) && !defined(__clang__)
     #  pragma GCC diagnostic push
     #  pragma GCC diagnostic ignored "-Wdeprecated-declarations"
     #endif
            if (out_codec->pix_fmts) {
                // Prefer the first supported SW pixel format advertised by the encoder
                    out_codec_ctx->pix_fmt = out_codec->pix_fmts[0];
            } else {
                // Fallback: if input pix_fmt is HW (DRM_PRIME / VAAPI / etc.), use yuv420p           
                const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get((AVPixelFormat)in_codec_ctx->pix_fmt);
                if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
                    out_codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
                } else {
                    out_codec_ctx->pix_fmt = in_codec_ctx->pix_fmt;
                }
            }
    #if defined(__GNUC__) && !defined(__clang__)
    #  pragma GCC diagnostic pop
    #endif

#endif

#endif
        if (out_codec_ctx->framerate.num > 0 && out_codec_ctx->framerate.den > 0) {
            out_codec_ctx->time_base = av_inv_q(out_codec_ctx->framerate);  // time_base = 1/framerate
        } else {
            out_codec_ctx->time_base = (AVRational){1, 25};
        }

        // Configure GOP length based on effective framerate so that
        // keyframes align with ~2 second HLS segments on both x86 and ARM.
        double fps = 0.0;
        if (out_codec_ctx->framerate.num > 0 && out_codec_ctx->framerate.den > 0) {
            fps = (double)out_codec_ctx->framerate.num / out_codec_ctx->framerate.den;
        }
        if (fps <= 0.0) {
            fps = 25.0;
        }
        int gop = (int)lrint(fps * 2.0); // target ~2s per GOP
        if (gop < 1) {
            gop = 1;
        }

        out_codec_ctx->max_b_frames    = 0;
        out_codec_ctx->gop_size        = gop;

        if (out_codec_id == AV_CODEC_ID_H264) {
            if (!info->need_scale) {
                out_codec_ctx->max_b_frames = 0;
                out_codec_ctx->gop_size     = gop;
                out_codec_ctx->keyint_min   = gop;
                // Use time based force_key_frames so that every ~2 seconds a
                // keyframe is inserted regardless of framerate.
                av_opt_set(out_codec_ctx->priv_data, "force_key_frames", "expr:gte(t,n_forced*2)", 0);
                av_opt_set(out_codec_ctx->priv_data, "preset", "ultrafast", 0);
                av_opt_set(out_codec_ctx->priv_data, "tune", "zerolatency", 0);
                av_opt_set(out_codec_ctx->priv_data, "profile", "main", 0);
                char x264_params[128];
                snprintf(x264_params, sizeof(x264_params), "keyint=%d:min-keyint=%d:scenecut=0", gop, gop);
                av_opt_set(out_codec_ctx->priv_data, "x264-params", x264_params, 0);
            }
        }

        LOG(LOG_DEBUG, "video Encoder config: time_base=%d/%d, framerate=%d/%d, width=%d, height=%d, pix_fmt: %d",
                        out_codec_ctx->time_base.num, out_codec_ctx->time_base.den,
                        out_codec_ctx->framerate.num, out_codec_ctx->framerate.den,
                        out_codec_ctx->width, out_codec_ctx->height, out_codec_ctx->pix_fmt);

    } else if (in_codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {

        out_codec_ctx->sample_rate = (info->sampleRate > 0) ? info->sampleRate : in_codec_ctx->sample_rate;

        if (out_codec_ctx->sample_rate != 44100 && out_codec_ctx->sample_rate != 48000) {
            out_codec_ctx->sample_rate = 44100;
        }

        out_codec_ctx->bit_rate              = (info->audBitrate > 0) ? info->audBitrate * 1000: in_codec_ctx->bit_rate;
        out_codec_ctx->ch_layout             = in_codec_ctx->ch_layout;
        out_codec_ctx->ch_layout.nb_channels = in_codec_ctx->ch_layout.nb_channels;

#if USE_STATIC_LIB
        out_codec_ctx->sample_fmt            = out_codec->sample_fmts ? out_codec->sample_fmts[0] : in_codec_ctx->sample_fmt;
#else
        out_codec_ctx->sample_fmt            = in_codec_ctx->sample_fmt;
#endif

        if (out_codec_id == AV_CODEC_ID_AAC) {
            int channels                         = in_codec_ctx->ch_layout.nb_channels;
            //out_codec_ctx->frame_size            = 1024;
            out_codec_ctx->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
            av_channel_layout_default(&out_codec_ctx->ch_layout, channels);
            //av_channel_layout_copy(&out_codec_ctx->ch_layout, &in_codec_ctx->ch_layout);
            out_codec_ctx->sample_fmt            = AV_SAMPLE_FMT_FLTP;
            out_codec_ctx->time_base             = av_make_q(1, out_codec_ctx->sample_rate);
            LOG(LOG_DEBUG, "AAC encoder config: frame_size=%d, channels=%d, out_codec_ctx->frame_size: %d, out_codec_ctx->sample_fmt: %d, timebase num/den: %d/%d",
                            out_codec_ctx->frame_size, channels, out_codec_ctx->frame_size, out_codec_ctx->sample_fmt, out_codec_ctx->time_base.num, out_codec_ctx->time_base.den);
        } else {
            out_codec_ctx->frame_size            = in_codec_ctx->frame_size;
            av_channel_layout_copy(&out_codec_ctx->ch_layout, &in_codec_ctx->ch_layout);
        }
        LOG(LOG_VERBOSE, "audio Encoder config:bit_rate= %ld sample_rate=%ld, sample_fmt = %d",
                          (long)out_codec_ctx->bit_rate, (long)out_codec_ctx->sample_rate,
                          out_codec_ctx->sample_fmt);
    }

    AVDictionary* enc_opts = NULL;
    av_dict_set(&enc_opts, "preset", "ultrafast", 0);

    if (avcodec_open2(out_codec_ctx, out_codec, &enc_opts) < 0) { // this will automatically set out_codec_ctx->frame_size to 1024
        LOG(LOG_ERROR, "can not open output encoder: %s", avcodec_get_name(out_codec_id));
        avcodec_free_context(&out_codec_ctx);
        return NULL;
    }

    av_dict_free(&enc_opts);
    return out_codec_ctx;
}

static enum AVPixelFormat hw_get_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    HWAccelCtx *hw_ctx = (HWAccelCtx*)ctx->opaque;
    // Fallback to the first available pix_fmt when HW fmt is not offered, to avoid AV_PIX_FMT_NONE errors
    if (!hw_ctx) {
        return pix_fmts && pix_fmts[0] != AV_PIX_FMT_NONE ? pix_fmts[0] : AV_PIX_FMT_YUV420P;
    }

    for (const enum AVPixelFormat *p = pix_fmts; *p != -1; p++) {
        if (*p == hw_ctx->hw_pix_fmt) {
            return *p;
        }
    }

    // HW format not available for this codec/profile; mark decoder HW as disabled and fallback to SW fmt
    hw_ctx->hwDecenabled = ZET_FALSE;
    if (pix_fmts && pix_fmts[0] != AV_PIX_FMT_NONE) {
        LOG(LOG_WARNING, "HW pix_fmt not offered by decoder; falling back to %s", av_get_pix_fmt_name(pix_fmts[0]));
        return pix_fmts[0];
    }
    return AV_PIX_FMT_YUV420P;
}

INT32 zetHlsServerMdl::initHWDecoder(AVFormatContext* in_fmt_ctx, AVStream* in_video_stream, AVCodecContext* in_audio_ctx, HWAccelCtx& hwAccelCtx) {
    const AVCodec* invideo_hw_decode = NULL;
    int            ret;

#if X86_64
    if (in_video_stream && (in_video_stream->codecpar->codec_id != AV_CODEC_ID_H264
                        && in_video_stream->codecpar->codec_id != AV_CODEC_ID_HEVC
                        && in_video_stream->codecpar->codec_id != AV_CODEC_ID_VP9
                        && in_video_stream->codecpar->codec_id != AV_CODEC_ID_VP8
                        && in_video_stream->codecpar->codec_id != AV_CODEC_ID_AV1
                        && in_video_stream->codecpar->codec_id != AV_CODEC_ID_H263
                        && in_video_stream->codecpar->codec_id != AV_CODEC_ID_MJPEG)) { // skip specified decoder format for hw decoder init
        LOG(LOG_INFO, "x86_64 Skip hardware decode for theora, mpeg4, mpeg2, mpeg1; fallback to software decoder");
        return ZET_NOK;
    }

    ret = av_find_best_stream(in_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &invideo_hw_decode, 0);
    if (in_video_stream && in_video_stream->codecpar->codec_id == AV_CODEC_ID_AV1) { // skip specified decoder format for hw decoder init
    	LOG(LOG_INFO, "Skip VAAPI hardware decode for av1; fallback to software decoder");
    	return ZET_NOK;
    }

#elif ARM
    if (in_video_stream && (in_video_stream->codecpar->codec_id != AV_CODEC_ID_H264
                        && in_video_stream->codecpar->codec_id != AV_CODEC_ID_HEVC
                        && in_video_stream->codecpar->codec_id != AV_CODEC_ID_VP9
                        && in_video_stream->codecpar->codec_id != AV_CODEC_ID_VP8
                        && in_video_stream->codecpar->codec_id != AV_CODEC_ID_AV1
                        && in_video_stream->codecpar->codec_id != AV_CODEC_ID_H263)) { // skip specified decoder format for hw decoder init
        LOG(LOG_INFO, ".arm Skip hardware decode for theora, mpeg4, mpeg2, mpeg1; fallback to software decoder");
        return ZET_NOK;
    }

    if (in_fmt_ctx && in_fmt_ctx->iformat && in_fmt_ctx->iformat->name && in_video_stream && in_video_stream->codecpar &&
        in_video_stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
        const char *fmt_name  = in_fmt_ctx->iformat->name;
        bool  is_mov_family   = (strstr(fmt_name, "mov")  != NULL ||  strstr(fmt_name, "mp4")  != NULL
                                || strstr(fmt_name, "m4a")	!= NULL ||	strstr(fmt_name, "3gp")  != NULL
                                || strstr(fmt_name, "3g2")  != NULL ||	strstr(fmt_name, "mj2")  != NULL);
        AVCodecParameters *cp = in_video_stream->codecpar;
        bool is_10bit_profile = (cp->profile == FF_PROFILE_HEVC_MAIN_10);
        bool is_10bit_format  =	(cp->format == AV_PIX_FMT_YUV420P10LE ||cp->format == AV_PIX_FMT_YUV420P10BE
                                || cp->format == AV_PIX_FMT_YUV422P10LE || cp->format == AV_PIX_FMT_YUV422P10BE 
                                || cp->format == AV_PIX_FMT_YUV444P10LE || cp->format == AV_PIX_FMT_YUV444P10BE);
        bool is_10bit_depth   = (cp->bits_per_raw_sample > 8);
        bool is_10bit         = (is_10bit_profile || is_10bit_format || is_10bit_depth); 
        bool is_high_res      = (cp->width >= 3840 || cp->height >= 2160);
        if (is_mov_family && is_10bit && is_high_res && cp->bit_rate > 190*1000*1000) {
            LOG(LOG_WARNING,".ARM: disable HEVC RKMPP hw decoder for high-bit-depth MOV/MP4 (w=%d h=%d profile=%d format=%d bits=%d), use SW decode + RGA + HW encode, bitrate :%ld",
                             cp->width, cp->height, cp->profile, cp->format, cp->bits_per_raw_sample, cp->bit_rate);	  
            return ZET_NOK;
        }
    }

    if (in_fmt_ctx && in_fmt_ctx->iformat && in_video_stream && in_video_stream->codecpar) {
        AVCodecParameters *cp = in_video_stream->codecpar;
        if (cp->codec_id == AV_CODEC_ID_H264) {
            AVDictionaryEntry *comment_tag = av_dict_get(in_fmt_ctx->metadata, "comment", NULL, 0);
            const char        *comment_val = comment_tag ? comment_tag->value : NULL;
            if (comment_val && strstr(comment_val, "Bilibili XCoder v2.0.2") != NULL) {
                AVDictionaryEntry *compatible_brands_tag = av_dict_get(in_fmt_ctx->metadata, "compatible_brands", NULL, 0);
                AVDictionaryEntry *major_brand_tag       = av_dict_get(in_fmt_ctx->metadata, "major_brand", NULL, 0);
                const char        *brands                = compatible_brands_tag ? compatible_brands_tag->value : NULL;
                const char        *major_brands          = major_brand_tag ? major_brand_tag->value : NULL;
                if ((brands && strcmp(brands, "isomiso2avc1mp41") == 0) || (major_brands && strcmp(brands, "isom") == 0)) {
                    LOG(LOG_WARNING, "ARM: disable RKMPP H.264 hw decoder for ASF+Bilibili XCoder source, fallback to software decoder");
                    return ZET_NOK;
                }
            }
        }
    }

    invideo_hw_decode = NULL;
    if (in_video_stream && in_video_stream->codecpar) {
        if (in_video_stream->codecpar->codec_id == AV_CODEC_ID_H264) {
            invideo_hw_decode = avcodec_find_decoder_by_name("h264_rkmpp");
        } else if (in_video_stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            invideo_hw_decode = avcodec_find_decoder_by_name("hevc_rkmpp");
        } else if (in_video_stream->codecpar->codec_id == AV_CODEC_ID_VP9) {
            invideo_hw_decode = avcodec_find_decoder_by_name("vp9_rkmpp");
        } else if (in_video_stream->codecpar->codec_id == AV_CODEC_ID_VP8) {
            invideo_hw_decode = avcodec_find_decoder_by_name("vp8_rkmpp");
        } else if (in_video_stream->codecpar->codec_id == AV_CODEC_ID_AV1) {
            invideo_hw_decode = avcodec_find_decoder_by_name("av1_rkmpp");
        } else if (in_video_stream->codecpar->codec_id == AV_CODEC_ID_MPEG4 && in_video_stream->codecpar->width > 1920 && in_video_stream->codecpar->height > 1080) {
            invideo_hw_decode = avcodec_find_decoder_by_name("mpeg4_rkmpp");
        } else if (in_video_stream->codecpar->codec_id == AV_CODEC_ID_MPEG2VIDEO && in_video_stream->codecpar->width > 1920 && in_video_stream->codecpar->height > 1080) {
            invideo_hw_decode = avcodec_find_decoder_by_name("mpeg2_rkmpp");
        } else if (in_video_stream->codecpar->codec_id == AV_CODEC_ID_MPEG1VIDEO && in_video_stream->codecpar->width > 1920 && in_video_stream->codecpar->height > 1080) {
            invideo_hw_decode = avcodec_find_decoder_by_name("mpeg1_rkmpp");
        } else if (in_video_stream->codecpar->codec_id == AV_CODEC_ID_H263) {
            invideo_hw_decode = avcodec_find_decoder_by_name("h263_rkmpp");
        }
    }

    if (!invideo_hw_decode) {
        LOG(LOG_INFO, "tried but not find invideo_hw_decode: %p, codec name: %s, using av_find_best_stream instead",invideo_hw_decode, avcodec_get_name(in_video_stream->codecpar->codec_id));
        ret = av_find_best_stream(in_fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &invideo_hw_decode, 0);
    } else {
        LOG(LOG_DEBUG, "now find invideo_hw_decode: %p, codec name: %s",invideo_hw_decode, avcodec_get_name(in_video_stream->codecpar->codec_id));
        ret = 0;
    }
#endif

    if (!invideo_hw_decode || ret < 0) {
        LOG(LOG_ERROR, "~Hardware decoder support, invideo_hw_decode: %p, ret: %d", invideo_hw_decode, ret);
        return ZET_NOK;
    }

    AVHWDeviceType hw_type       = AV_HWDEVICE_TYPE_NONE;
    AVBufferRef*   hw_device_ctx = NULL;
#if X86_64
    hw_type = AV_HWDEVICE_TYPE_VAAPI;
    if (av_hwdevice_ctx_create(&hw_device_ctx, hw_type, "/dev/dri/renderD128", NULL, 0) < 0) {
        LOG(LOG_ERROR, "Failed to create %d device", hw_type);
        return ZET_FALSE;
    }

#elif ARM
    hw_type = AV_HWDEVICE_TYPE_RKMPP;
    if (av_hwdevice_ctx_create(&hw_device_ctx, hw_type, NULL, NULL, 0) < 0) {
    	LOG(LOG_ERROR, "Failed to create %d device", hw_type);
    	return ZET_FALSE;
    }

#endif

    hwAccelCtx.video_dec_ctx = avcodec_alloc_context3(invideo_hw_decode);
    if (!hwAccelCtx.video_dec_ctx) {
        LOG(LOG_INFO, "Failed to allocate hardware decoder context");
        return ZET_NOK;
    }

    avcodec_parameters_to_context(hwAccelCtx.video_dec_ctx, in_video_stream->codecpar);

    hwAccelCtx.hw_pix_fmt = getHardwarePixelFormat();

    LOG(LOG_DEBUG, "hw dec accel, pixformat : %d, name: %s", hwAccelCtx.hw_pix_fmt, invideo_hw_decode->name);
    if (hwAccelCtx.hw_pix_fmt == AV_PIX_FMT_NONE) {
        LOG(LOG_ERROR, "Invalid hardware pixel format for hw device");
        return ZET_NOK;
    }

    hwAccelCtx.hw_dec_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
    if (!hwAccelCtx.hw_dec_frames_ctx) {
        LOG(LOG_INFO, "Failed to allocate HW frames context");
        freeResources(NULL, NULL, &hwAccelCtx.video_dec_ctx, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        return ZET_NOK;
    }

#if ARM
    AVPixelFormat dec_sw_format = AV_PIX_FMT_NV12;  // Default to 8-bit
    if (in_video_stream && in_video_stream->codecpar) {
        bool is_10bit = ZET_FALSE;
        // HEVC Main10 profile or bit_depth > 8 indicates 10-bit content
        if (in_video_stream->codecpar->codec_id == AV_CODEC_ID_HEVC) {
            if (in_video_stream->codecpar->profile == FF_PROFILE_HEVC_MAIN_10) {
                is_10bit = ZET_TRUE;
            }
            else if (in_video_stream->codecpar->profile == 2) {
                is_10bit = ZET_TRUE;
            }
        }
        if (in_video_stream->codecpar->format == AV_PIX_FMT_YUV420P10LE ||
            in_video_stream->codecpar->format == AV_PIX_FMT_YUV420P10BE ||
            in_video_stream->codecpar->format == AV_PIX_FMT_YUV422P10LE ||
            in_video_stream->codecpar->format == AV_PIX_FMT_YUV422P10BE ||
            in_video_stream->codecpar->format == AV_PIX_FMT_YUV444P10LE ||
            in_video_stream->codecpar->format == AV_PIX_FMT_YUV444P10BE) {
            is_10bit = ZET_TRUE;
        }
        if (is_10bit) {
            dec_sw_format = AV_PIX_FMT_P010LE;  // 10-bit format for RKMPP
            LOG(LOG_INFO, "Detected 10-bit input video (profile=%d, format=%s), using P010LE sw_format for decoder",
                           in_video_stream->codecpar->profile,
                           av_get_pix_fmt_name((AVPixelFormat)in_video_stream->codecpar->format));
        }
    }

    AVHWFramesContext* frames_ctx = (AVHWFramesContext*)hwAccelCtx.hw_dec_frames_ctx->data;
    frames_ctx->format            = hwAccelCtx.hw_pix_fmt;
    frames_ctx->sw_format         = dec_sw_format;
    frames_ctx->width             = hwAccelCtx.video_dec_ctx->width;    
    frames_ctx->height            = hwAccelCtx.video_dec_ctx->height;    
    frames_ctx->initial_pool_size = 32;
    LOG(LOG_DEBUG, "ARM HW decoder frames_ctx: format=%s, sw_format=%s, %dx%d",
                    av_get_pix_fmt_name(frames_ctx->format), av_get_pix_fmt_name(frames_ctx->sw_format),
                    frames_ctx->width, frames_ctx->height);
#else
    AVHWFramesContext* frames_ctx = (AVHWFramesContext*)hwAccelCtx.hw_dec_frames_ctx->data;
    frames_ctx->format            = hwAccelCtx.hw_pix_fmt;
    frames_ctx->sw_format         = AV_PIX_FMT_NV12;
    frames_ctx->width             = hwAccelCtx.video_dec_ctx->width;    
    frames_ctx->height            = hwAccelCtx.video_dec_ctx->height;    
    frames_ctx->initial_pool_size = 32;
#endif
    ret = av_hwframe_ctx_init(hwAccelCtx.hw_dec_frames_ctx);
    if (ret < 0) {
        LOG(LOG_ERROR, "hwaccel support but failed to initialize HW frames context");
        freeResources(NULL, NULL, &hwAccelCtx.video_dec_ctx, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        return ZET_NOK;
    }

#if X86_64
    initVAAPIContext(hwAccelCtx.video_dec_ctx, hwAccelCtx, ZET_FALSE);
#elif ARM
    initRKMPPContext(hwAccelCtx.video_dec_ctx, hwAccelCtx, ZET_FALSE);
#endif

    hwAccelCtx.video_dec_ctx->hw_frames_ctx = av_buffer_ref(hwAccelCtx.hw_dec_frames_ctx);
    hwAccelCtx.video_dec_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    if (hwAccelCtx.video_dec_ctx && !hwAccelCtx.video_dec_ctx->hw_frames_ctx) {
        LOG(LOG_ERROR, "Failed to set hw_frames_ctx to decoder context");
        return ZET_NOK;
    }

    hwAccelCtx.video_dec_ctx->get_format = hw_get_format; //get_hwAccelFrame_format;
    hwAccelCtx.video_dec_ctx->opaque     = &hwAccelCtx;
    av_buffer_unref(&hwAccelCtx.hw_dec_frames_ctx);

    if (avcodec_open2(hwAccelCtx.video_dec_ctx, invideo_hw_decode, NULL) < 0) {
        LOG(LOG_ERROR, "unable to open hw video decoder, please check");
        freeResources(NULL, NULL, &hwAccelCtx.video_dec_ctx, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        return ZET_NOK;
    }
    hwAccelCtx.hwDecenabled = ZET_TRUE;
    LOG(LOG_INFO, "init hw decoder successfully!!!, hwDecenabled: %d!!!", hwAccelCtx.hwDecenabled);
    return ZET_OK;
}

INT32 zetHlsServerMdl::initHWEncoder(struct _zetHlsGenInfo* hlsGenInfo, AVFormatContext* in_fmt_ctx, AVStream* in_video_stream,
                                           AVCodecContext* in_video_ctx, AVCodecContext* in_audio_ctx, HWAccelCtx& hwAccelCtx, AVCodecID curVideoCodecID) {
    const AVCodec* outvideo_hw_encode = NULL;

    if (!isHWCodecSupport(curVideoCodecID, &outvideo_hw_encode, ZET_TRUE)) {
        LOG(LOG_ERROR, "HW enc codec do not support, name: %s", avcodec_get_name(curVideoCodecID));
        return ZET_NOK;
    }

    if (!outvideo_hw_encode) {
        LOG(LOG_ERROR, "Hardware encoder is NULL");
        return ZET_NOK;
    }

    AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_NONE;
#if X86_64
    hw_type                = AV_HWDEVICE_TYPE_VAAPI;
#elif ARM
    hw_type                = AV_HWDEVICE_TYPE_RKMPP;
#endif
    //outvideo_hw_encode = hw_accel_find_codec_by_hw_type(hw_type, curVideoCodecID, ZET_TRUE);
    AVBufferRef* hw_device_ctx = NULL;
    // Reuse decoder's device if available to guarantee compatibility of DRM_PRIME frames
    if (hwAccelCtx.video_dec_ctx && hwAccelCtx.video_dec_ctx->hw_device_ctx) {
        hw_device_ctx = av_buffer_ref(hwAccelCtx.video_dec_ctx->hw_device_ctx);
    }

#if X86_64
    hw_type = AV_HWDEVICE_TYPE_VAAPI;
    if (!hw_device_ctx) {
        if (av_hwdevice_ctx_create(&hw_device_ctx, hw_type, "/dev/dri/renderD128", NULL, 0) < 0) {
            LOG(LOG_ERROR, "Failed to create %d device", hw_type);
            return ZET_FALSE;
        }
    }
#elif ARM
    hw_type = AV_HWDEVICE_TYPE_RKMPP;
    if (!hw_device_ctx) {
        if (av_hwdevice_ctx_create(&hw_device_ctx, hw_type, NULL, NULL, 0) < 0) {
            LOG(LOG_ERROR, "Failed to create %d device", hw_type);
            return ZET_FALSE;
        }
    }
#endif
    LOG(LOG_DEBUG, "in_video_ctx: %p, in_video_stream: %p, codec name: %s, outvideo_hw_encode: %p, hw_device_ctx: %p",
                    in_video_ctx, in_video_stream, avcodec_get_name(curVideoCodecID), outvideo_hw_encode, hw_device_ctx);

    hwAccelCtx.video_enc_ctx = avcodec_alloc_context3(outvideo_hw_encode);
    if (!hwAccelCtx.video_enc_ctx) {
        LOG(LOG_INFO, "Failed to allocate hardware encoder context");
        return ZET_NOK;
    }

    if (hlsGenInfo && hlsGenInfo->threads > 0) {
        hwAccelCtx.video_enc_ctx->thread_count = hlsGenInfo->threads;
    } else {
        //hwAccelCtx.video_enc_ctx->thread_count = av_cpu_count();
        //hwAccelCtx.video_enc_ctx->thread_type  = FF_THREAD_SLICE;
		//hwAccelCtx.video_enc_ctx->flags        = AV_CODEC_FLAG_LOW_DELAY;
    }

    if (in_video_ctx->framerate.num <= 0 || in_video_ctx->framerate.den <= 0) {
        if (in_video_stream) {
            if (in_video_stream->r_frame_rate.num > 0 && in_video_stream->r_frame_rate.den > 0) {
                hwAccelCtx.video_enc_ctx->framerate = in_video_stream->r_frame_rate;
            } else if (in_video_stream->avg_frame_rate.num > 0 && in_video_stream->avg_frame_rate.den > 0) {
                hwAccelCtx.video_enc_ctx->framerate = in_video_stream->avg_frame_rate;
            } else {
                hwAccelCtx.video_enc_ctx->framerate = (AVRational){25, 1};
            }
        } else {
            hwAccelCtx.video_enc_ctx->framerate = (AVRational){25, 1};
        }
    } else {
        hwAccelCtx.video_enc_ctx->framerate = in_video_ctx->framerate;
    }

    hwAccelCtx.hw_pix_fmt                   = getHardwarePixelFormat();
    hwAccelCtx.video_enc_ctx->opaque        = this;
    hwAccelCtx.video_enc_ctx->width         = hlsGenInfo->width > 0 ? hlsGenInfo->width : in_video_stream->codecpar->width;
    hwAccelCtx.video_enc_ctx->height        = hlsGenInfo->height > 0 ? hlsGenInfo->height : in_video_stream->codecpar->height;
    hwAccelCtx.video_enc_ctx->bit_rate      = hlsGenInfo->bitrate > 0 ? hlsGenInfo->bitrate * 1000 : in_video_stream->codecpar->bit_rate;
    // hwAccelCtx.video_enc_ctx->time_base     = hwAccelCtx.hwDecenabled ? av_inv_q(hwAccelCtx.video_enc_ctx->framerate) : av_inv_q(in_video_ctx->framerate);

    // Derive GOP length from encoder framerate so we get ~2 second GOPs
    // independently of platform.
    double enc_fps = 0.0;
    if (hwAccelCtx.video_enc_ctx->framerate.num > 0 && hwAccelCtx.video_enc_ctx->framerate.den > 0) {
        enc_fps = (double)hwAccelCtx.video_enc_ctx->framerate.num / hwAccelCtx.video_enc_ctx->framerate.den;
    }
    if (enc_fps <= 0.0) {
        enc_fps = 25.0;
    }
    int enc_gop = (int)lrint(enc_fps * 2.0);
    if (enc_gop < 1) {
        enc_gop = 1;
    }

    hwAccelCtx.video_enc_ctx->gop_size        = enc_gop;
    hwAccelCtx.video_enc_ctx->keyint_min      = enc_gop;
    hwAccelCtx.video_enc_ctx->max_b_frames    = 0;
    hwAccelCtx.video_enc_ctx->extra_hw_frames = 24;

    // RKMPP requires DRM_PRIME (drm_prime) input; use HW pix fmt and provide hw_frames_ctx
    hwAccelCtx.video_enc_ctx->pix_fmt        = hwAccelCtx.hw_pix_fmt;

    LOG(LOG_DEBUG, "Encoder pix_fmt=%s, hw_pix_fmt=%s", av_get_pix_fmt_name(hwAccelCtx.video_enc_ctx->pix_fmt), av_get_pix_fmt_name(hwAccelCtx.hw_pix_fmt));

    if (in_video_stream->r_frame_rate.num > 0 && in_video_stream->r_frame_rate.den > 0) {
        hwAccelCtx.video_enc_ctx->framerate = in_video_stream->r_frame_rate;
    } else if (in_video_stream->avg_frame_rate.num > 0 && in_video_stream->avg_frame_rate.den > 0) {
        hwAccelCtx.video_enc_ctx->framerate = in_video_stream->avg_frame_rate;
    } else {
        hwAccelCtx.video_enc_ctx->framerate = (AVRational){25, 1};    
    }

    if (hwAccelCtx.video_enc_ctx->framerate.num > 0 && hwAccelCtx.video_enc_ctx->framerate.den > 0) {
        hwAccelCtx.video_enc_ctx->time_base = av_inv_q(hwAccelCtx.video_enc_ctx->framerate);	// time_base = 1/framerate
    } else {
        hwAccelCtx.video_enc_ctx->time_base = (AVRational){1, 25};
    }

    // Ensure valid bitrate for RKMPP; fall back to a computed default if missing
    if (hwAccelCtx.video_enc_ctx->bit_rate <= 0) {
        double fps = (hwAccelCtx.video_enc_ctx->framerate.num > 0 && hwAccelCtx.video_enc_ctx->framerate.den > 0) ?
                     (double)hwAccelCtx.video_enc_ctx->framerate.num / hwAccelCtx.video_enc_ctx->framerate.den : 25.0;
        // ~0.07 bits per pixel per frame
        INT64 calc_bps = (INT64)(hwAccelCtx.video_enc_ctx->width * (INT64)hwAccelCtx.video_enc_ctx->height * fps * 0.07);
        // clamp to [300k, 20M]
        if (calc_bps < 300000) calc_bps    = 300000;
        if (calc_bps > 20000000) calc_bps  = 20000000;
        hwAccelCtx.video_enc_ctx->bit_rate = calc_bps;
    }

    LOG(LOG_DEBUG, "hw_pix_fmt is: %s", av_get_pix_fmt_name(hwAccelCtx.hw_pix_fmt));

    // create hw_frames_ctx only if encoder need dri-prime format input
    INT32 ret = 0;
    if (hwAccelCtx.video_enc_ctx->pix_fmt == hwAccelCtx.hw_pix_fmt) {
        hwAccelCtx.hw_enc_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
        if (!hwAccelCtx.hw_enc_frames_ctx) {
            LOG(LOG_INFO, "Failed to allocate HW frames context");
            freeResources(NULL, NULL, NULL, NULL, NULL, &hwAccelCtx.video_enc_ctx, NULL, NULL, NULL, NULL);
            return ZET_NOK;
        }

        AVHWFramesContext* frames_ctx = (AVHWFramesContext*)hwAccelCtx.hw_enc_frames_ctx->data;
        frames_ctx->format            = hwAccelCtx.hw_pix_fmt;
        frames_ctx->sw_format         = AV_PIX_FMT_NV12;
        frames_ctx->width             = hlsGenInfo->width > 0 ? hlsGenInfo->width : in_video_stream->codecpar->width; //hwAccelCtx.video_dec_ctx->width;
        frames_ctx->height            = hlsGenInfo->height > 0 ? hlsGenInfo->height : in_video_stream->codecpar->height; //hwAccelCtx.video_dec_ctx->height;
        frames_ctx->initial_pool_size = 32;

        LOG(LOG_DEBUG, "HWFramesContext: sw_format=%d, sw_format=%s, width=%d, height=%d, format: %s",
                        frames_ctx->format, av_get_pix_fmt_name(frames_ctx->sw_format), frames_ctx->width,
                        frames_ctx->height, av_get_pix_fmt_name(frames_ctx->format));

        // init hw frame ctx
        ret = av_hwframe_ctx_init(hwAccelCtx.hw_enc_frames_ctx);
        if (ret < 0) {
            LOG(LOG_INFO, "hwaccel support but failed to initialize HW frames context");
            freeResources(NULL, NULL, NULL, NULL, NULL, &hwAccelCtx.video_enc_ctx, NULL, NULL, NULL, NULL);
            return ZET_NOK;
        }

        hwAccelCtx.video_enc_ctx->hw_frames_ctx = av_buffer_ref(hwAccelCtx.hw_enc_frames_ctx);
        if (!hwAccelCtx.video_enc_ctx->hw_frames_ctx) {
            LOG(LOG_ERROR, "Failed to set hw_frames_ctx to encoder context");
            return ZET_NOK;
        }
        av_buffer_unref(&hwAccelCtx.hw_enc_frames_ctx);
    } else {
        LOG(LOG_INFO, "ARM encoder will consume SW NV12 directly (no hw_frames_ctx)");
    }

    // init platform-specific hw ctx
#if X86_64
    initVAAPIContext(hwAccelCtx.video_enc_ctx, hwAccelCtx, ZET_TRUE);
#elif ARM
    if (hwAccelCtx.video_enc_ctx->pix_fmt == hwAccelCtx.hw_pix_fmt) {
        initRKMPPContext(hwAccelCtx.video_enc_ctx, hwAccelCtx, ZET_TRUE);
    }
#endif

    hwAccelCtx.video_enc_ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);
    LOG(LOG_DEBUG, "hwAccelCtx.video_enc_ctx->hw_device_ctx: %p", hwAccelCtx.video_enc_ctx->hw_device_ctx);
    AVDictionary* enc_opts = NULL;
#if ARM
    // For RKMPP, prefer context fields over private options
    if (outvideo_hw_encode && !strcmp(outvideo_hw_encode->name, "h264_rkmpp")) {
        hwAccelCtx.video_enc_ctx->profile         = FF_PROFILE_H264_MAIN;
        hwAccelCtx.video_enc_ctx->rc_max_rate     = hwAccelCtx.video_enc_ctx->bit_rate;
        hwAccelCtx.video_enc_ctx->rc_buffer_size  = hwAccelCtx.video_enc_ctx->bit_rate * 2;
        char g_buf[16];
        snprintf(g_buf, sizeof(g_buf), "%d", enc_gop);
        av_dict_set(&enc_opts, "g", g_buf, 0);
        av_dict_set(&enc_opts, "keyint_min", g_buf, 0);
        av_dict_set(&enc_opts, "force_key_frames", "expr:gte(t,n_forced*2)", 0);
    }
#endif
    av_dict_set(&enc_opts, "preset", "ultrafast", 0);
    av_opt_set(hwAccelCtx.video_enc_ctx->priv_data, "b_strategy", "0", 0);
    av_opt_set(hwAccelCtx.video_enc_ctx->priv_data, "profile", "main", 0);
    av_opt_set(hwAccelCtx.video_enc_ctx->priv_data, "qp", "23", 0);
    av_opt_set(hwAccelCtx.video_enc_ctx->priv_data, "quality", "5", 0);
    hwAccelCtx.video_enc_ctx->max_b_frames    = 0;
    if (avcodec_open2(hwAccelCtx.video_enc_ctx, outvideo_hw_encode, &enc_opts) < 0) {
        LOG(LOG_INFO, "unable to open hw video encoder, please check");
        freeResources(NULL, NULL, &hwAccelCtx.video_enc_ctx, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        av_dict_free(&enc_opts);
        return ZET_NOK;
    }
    hwAccelCtx.hwEncenabled = ZET_TRUE;
    av_dict_free(&enc_opts);
    return ZET_OK;
}

#if X86_64
static AVFrame* sw_out_pool = NULL;
void zetHlsServerMdl::initVaapiScalingGraph(bool video_needs_transcode, const HWAccelCtx& hwAccelCtx,
                                                     AVCodecContext* in_video_ctx, AVStream* in_video_stream,
                                                     AVCodecContext* out_video_ctx, AVFilterGraph*& va_graph,
                                                     AVFilterContext*& va_src_ctx, AVFilterContext*& va_fmt_in_ctx,
                                                     AVFilterContext*& va_upload_ctx, AVFilterContext*& va_scale_ctx,
                                                     AVFilterContext*& va_download_ctx, AVFilterContext*& va_fmt_out_ctx,
                                                     AVFilterContext*& va_sink_ctx, bool& use_vaapi, bool& va_out_hw,
                                                     SwsContext*& sws_to_nv12, SwsContext*& sws_nv12_to_outfmt,
                                                     AVFrame*& va_tmp_frame) {
    // Initialize VAAPI scaling graph if any HW accel is enabled (decode or encode)
    if (video_needs_transcode && (hwAccelCtx.hwDecenabled || hwAccelCtx.hwEncenabled)) {
        use_vaapi = ZET_TRUE;
        int src_w = in_video_ctx ? in_video_ctx->width  : in_video_stream->codecpar->width;
        int src_h = in_video_ctx ? in_video_ctx->height : in_video_stream->codecpar->height;
        int dst_w = out_video_ctx ? out_video_ctx->width  : in_video_stream->codecpar->width;
        int dst_h = out_video_ctx ? out_video_ctx->height : in_video_stream->codecpar->height;

        // If encoder is HW and expects VAAPI frames, keep frames in HW
        va_out_hw = (hwAccelCtx.hwEncenabled && out_video_ctx == hwAccelCtx.video_enc_ctx &&
                     hwAccelCtx.video_enc_ctx && hwAccelCtx.video_enc_ctx->pix_fmt == hwAccelCtx.hw_pix_fmt);

        va_graph = avfilter_graph_alloc();
        if (!va_graph) {
            LOG(LOG_ERROR, "VAAPI graph alloc failed, fallback to swscale");
            use_vaapi = ZET_FALSE;
        }
        if (use_vaapi) {
            char args[256];
            AVRational tb  = in_video_stream ? in_video_stream->time_base : (AVRational){1,25};
            AVRational sar = (AVRational){1,1};

            const AVFilter *buffersrc    = avfilter_get_by_name("buffer");
            const AVFilter *buffersink   = avfilter_get_by_name("buffersink");
            const AVFilter *fmt_filter   = avfilter_get_by_name("format");
            const AVFilter *hwupload_f   = avfilter_get_by_name("hwupload");
            const AVFilter *hwdownload_f = avfilter_get_by_name("hwdownload");
            const AVFilter *va_scale     = avfilter_get_by_name("scale_vaapi");
            if (!buffersrc || !buffersink || !fmt_filter || !hwupload_f || !va_scale || (!va_out_hw && !hwdownload_f)) {
                LOG(LOG_ERROR, "Missing filters for VAAPI chain, fallback to swscale");
                use_vaapi = ZET_FALSE;
            }

            // Prefer a device from decoder, else encoder
            AVBufferRef *va_hw_device = NULL;
            if (use_vaapi) {
                if (hwAccelCtx.video_dec_ctx && hwAccelCtx.video_dec_ctx->hw_device_ctx) {
                    va_hw_device = av_buffer_ref(hwAccelCtx.video_dec_ctx->hw_device_ctx);
                } else if (hwAccelCtx.video_enc_ctx && hwAccelCtx.video_enc_ctx->hw_device_ctx) {
                    va_hw_device = av_buffer_ref(hwAccelCtx.video_enc_ctx->hw_device_ctx);
                }
            }

            // X86_64 optimization: determine if decoder outputs HW frames for zero-copy path
            // Check both hwDecenabled flag (may be set to false by hw_get_format callback on failure)
            // and hw_frames_ctx existence
            bool va_in_hw = (hwAccelCtx.hwDecenabled && hwAccelCtx.video_dec_ctx && hwAccelCtx.video_dec_ctx->hw_frames_ctx != NULL);
            LOG(LOG_INFO, "VAAPI path detection: hwDecenabled=%d, video_dec_ctx=%p, hw_frames_ctx=%p, va_in_hw=%d",
                           hwAccelCtx.hwDecenabled, hwAccelCtx.video_dec_ctx,
                           hwAccelCtx.video_dec_ctx ? hwAccelCtx.video_dec_ctx->hw_frames_ctx : NULL, va_in_hw);

            if (use_vaapi) {
                if (va_in_hw) {
                    // HW decoder path: buffersrc accepts VAAPI frames directly
                    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                             src_w, src_h, hwAccelCtx.hw_pix_fmt, tb.num, tb.den, sar.num, sar.den);
                    if (avfilter_graph_create_filter(&va_src_ctx, buffersrc, "in", args, NULL, va_graph) < 0) {
                        LOG(LOG_ERROR, "create buffersrc(HW) failed, fallback to swscale");
                        use_vaapi = ZET_FALSE;
                    } else if (hwAccelCtx.video_dec_ctx && hwAccelCtx.video_dec_ctx->hw_frames_ctx) {
                        // Set hw_frames_ctx for HW input
                        AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
                        if (par) {
                            par->format        = hwAccelCtx.hw_pix_fmt;
                            par->time_base     = tb;
                            par->width         = src_w;
                            par->height        = src_h;
                            par->hw_frames_ctx = av_buffer_ref(hwAccelCtx.video_dec_ctx->hw_frames_ctx);
                            if (par->hw_frames_ctx) {
                                av_buffersrc_parameters_set(va_src_ctx, par);
                            } else {
                                LOG(LOG_ERROR, "Failed to ref hw_frames_ctx");
                                use_vaapi = ZET_FALSE;
                            }
                            av_freep(&par);
                        } else {
                            LOG(LOG_ERROR, "Failed to allocate AVBufferSrcParameters");
                            use_vaapi = ZET_FALSE;
                        }
                    }
                } else {
                    // SW decoder path: buffersrc accepts decoder SW format directly (e.g. yuv420p)
                    AVPixelFormat sw_dec_fmt = in_video_ctx ? (AVPixelFormat)in_video_ctx->pix_fmt: (AVPixelFormat)in_video_stream->codecpar->format;
                    if (sw_dec_fmt == AV_PIX_FMT_NONE) {
                        sw_dec_fmt = AV_PIX_FMT_YUV420P;
                    }
                    snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                             src_w, src_h, sw_dec_fmt, tb.num, tb.den, sar.num, sar.den);
                    if (avfilter_graph_create_filter(&va_src_ctx, buffersrc, "in", args, NULL, va_graph) < 0) {
                        LOG(LOG_ERROR, "create buffersrc(SW) failed, fallback to swscale");
                        use_vaapi = ZET_FALSE;
                    }
                }
            }

            // For SW decoder input: create hwupload; only create fmt_in when SW encoder is used
            // This avoids an extra per-frame CPU format conversion on SW-dec + HW-enc path.
            if (use_vaapi && !va_in_hw) {
                if (!va_out_hw) {
                    if (avfilter_graph_create_filter(&va_fmt_in_ctx, fmt_filter, "fmt_in", "pix_fmts=nv12", NULL, va_graph) < 0) {
                        LOG(LOG_ERROR, "create format(nv12) failed, fallback to swscale");
                        use_vaapi = ZET_FALSE;
                    }
                }
                if (use_vaapi && avfilter_graph_create_filter(&va_upload_ctx, hwupload_f, "hwupload", NULL, NULL, va_graph) < 0) {
                    LOG(LOG_ERROR, "create hwupload failed, fallback to swscale");
                    use_vaapi = ZET_FALSE;
                } else if (use_vaapi && va_hw_device) {
                    AVBufferRef* ref = av_buffer_ref(va_hw_device);
                    if (ref) {
                        va_upload_ctx->hw_device_ctx = ref;
                    } else {
                        LOG(LOG_ERROR, "Failed to ref va_hw_device for upload");
                        use_vaapi = ZET_FALSE;
                    }
                }
            }
            if (use_vaapi) {
                char scale_args[64];
                // Match ffmpeg CLI: scale_vaapi=w=dst_w:h=dst_h:format=nv12
                snprintf(scale_args, sizeof(scale_args), "w=%d:h=%d:format=nv12:extra_hw_frames=24", dst_w, dst_h);
                if (avfilter_graph_create_filter(&va_scale_ctx, va_scale, "scale_vaapi", scale_args, NULL, va_graph) < 0) {
                    LOG(LOG_ERROR, "create scale_vaapi failed, fallback to swscale");
                    use_vaapi = ZET_FALSE;
                } else if (va_hw_device) {
                    AVBufferRef* ref = av_buffer_ref(va_hw_device);
                    if (ref) {
                        va_scale_ctx->hw_device_ctx = ref;
                    } else {
                        LOG(LOG_ERROR, "Failed to ref va_hw_device for scale");
                    }
                }
            }

            const int sink_pix_fmts_hw[] = { (int)hwAccelCtx.hw_pix_fmt, AV_PIX_FMT_NONE };
            const int sink_pix_fmts_sw[] = { AV_PIX_FMT_NV12, AV_PIX_FMT_NONE };
            if (use_vaapi) {
                if (!va_out_hw) {
                    if (avfilter_graph_create_filter(&va_download_ctx, hwdownload_f, "hwdownload", NULL, NULL, va_graph) < 0) {
                        LOG(LOG_ERROR, "create hwdownload failed, fallback to swscale");
                        use_vaapi = ZET_FALSE;
                    }
                    if (use_vaapi && avfilter_graph_create_filter(&va_fmt_out_ctx, fmt_filter, "fmt_out", "pix_fmts=nv12", NULL, va_graph) < 0) {
                        LOG(LOG_ERROR, "create format(nv12) out failed, fallback to swscale");
                        use_vaapi = ZET_FALSE;
                    }
                }
            }
            if (use_vaapi) {
                if (avfilter_graph_create_filter(&va_sink_ctx, buffersink, "out", NULL, NULL, va_graph) < 0) {
                    LOG(LOG_ERROR, "create buffersink failed, fallback to swscale");
                    use_vaapi = ZET_FALSE;
                } else {
                    av_opt_set_int_list(va_sink_ctx, "pix_fmts", va_out_hw ? sink_pix_fmts_hw : sink_pix_fmts_sw, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
                }
            }
            if (use_vaapi) {
                int link_ok = 0;
                // X86_64 optimization: 4 filter chain variants based on hw_dec and hw_enc
                if (va_in_hw && va_out_hw) {
                    // Case 1: HW dec + HW enc (zero-copy optimal path)
                    // buffersrc(VAAPI) ---> scale_vaapi ---> buffersink(VAAPI)
                    link_ok = (avfilter_link(va_src_ctx,  0, va_scale_ctx, 0) >= 0 &&
                               avfilter_link(va_scale_ctx, 0, va_sink_ctx,  0) >= 0);
                    LOG(LOG_INFO, "VAAPI filter chain: HW-in ---> scale ---> HW-out (zero-copy)");
                } else if (va_in_hw && !va_out_hw) {
                    // Case 2: HW dec + SW enc
                    // buffersrc(VAAPI) ---> scale_vaapi ---> hwdownload ---> format(nv12)---> buffersink(NV12)
                    link_ok = (avfilter_link(va_src_ctx,      0, va_scale_ctx,    0) >= 0 &&
                               avfilter_link(va_scale_ctx,    0, va_download_ctx, 0) >= 0 &&
                               avfilter_link(va_download_ctx, 0, va_fmt_out_ctx,  0) >= 0 &&
                               avfilter_link(va_fmt_out_ctx,  0, va_sink_ctx,     0) >= 0);
                    LOG(LOG_INFO, "VAAPI filter chain: HW-in ---> scale ---> download ---> SW-out");
                } else if (!va_in_hw && va_out_hw) {
                    // Case 3: SW dec + HW enc
                    // buffersrc(SW fmt) ---> hwupload ---> scale_vaapi ---> buffersink(VAAPI)
                    link_ok = (avfilter_link(va_src_ctx,    0, va_upload_ctx, 0) >= 0 &&
                               avfilter_link(va_upload_ctx, 0, va_scale_ctx,  0) >= 0 &&
                               avfilter_link(va_scale_ctx,  0, va_sink_ctx,   0) >= 0);
                    LOG(LOG_INFO, "VAAPI filter chain: SW-in ---> upload ---> scale ---> HW-out");
                } else {
                    // Case 4: SW dec + SW enc
                    // buffersrc(NV12) ---> format(nv12) ---> hwupload ---> scale_vaapi ---> hwdownload ----> format(nv12) ---> buffersink(NV12)
                    link_ok = (avfilter_link(va_src_ctx,      0, va_fmt_in_ctx,   0) >= 0 &&
                               avfilter_link(va_fmt_in_ctx,   0, va_upload_ctx,   0) >= 0 &&
                               avfilter_link(va_upload_ctx,   0, va_scale_ctx,    0) >= 0 &&
                               avfilter_link(va_scale_ctx,    0, va_download_ctx, 0) >= 0 &&
                               avfilter_link(va_download_ctx, 0, va_fmt_out_ctx,  0) >= 0 &&
                               avfilter_link(va_fmt_out_ctx,  0, va_sink_ctx,     0) >= 0);
                    LOG(LOG_INFO, "VAAPI filter chain: SW-in ---> upload ---> scale ---> download ---> SW-out");
                }
                if (!link_ok) {
                    LOG(LOG_ERROR, "link VAAPI graph failed, fallback to swscale");
                    use_vaapi = ZET_FALSE;
                }
            }
            if (use_vaapi) {
                if (avfilter_graph_config(va_graph, NULL) < 0) {
                    LOG(LOG_ERROR, "VAAPI graph config failed, fallback to swscale");
                    use_vaapi = ZET_FALSE;
                } else {
                    LOG(LOG_INFO, "VAAPI scaling enabled (%s), va_hw_device: %p", va_out_hw ? "HW out" : "SW out", va_hw_device);
                }
            }
            if (va_hw_device) {
                av_buffer_unref(&va_hw_device);
            }
        }
    }
}

void zetHlsServerMdl::processVaapiScaling(bool& scaled, bool& use_vaapi, AVFilterGraph* va_graph,
                                          AVFilterContext* va_src_ctx, AVFilterContext* va_sink_ctx,
                                          const HWAccelCtx& hwAccelCtx, AVFrame* frame, AVFrame* va_tmp_frame,
                                          SwsContext*& sws_to_nv12, AVFrame* processed_frame,
                                          AVCodecContext* out_codec_ctx, bool va_out_hw,
                                          SwsContext*& sws_nv12_to_outfmt, AVFrame*& target_frame) {
    scaled = ZET_FALSE;
    if (use_vaapi && va_graph && va_src_ctx && va_sink_ctx) {
        AVFrame* work_frame = frame;
        // X86_64 optimization: Runtime format validation to prevent mismatch
        // Check if graph expects HW input but frame is SW (or vice versa)
        AVFilterLink* src_output = va_src_ctx->outputs[0];
        if (src_output && src_output->format != AV_PIX_FMT_NONE) {
            bool graph_expects_hw = (src_output->format == hwAccelCtx.hw_pix_fmt);
            bool frame_is_hw = (frame->format == hwAccelCtx.hw_pix_fmt);
            if (graph_expects_hw != frame_is_hw) {
                LOG(LOG_ERROR, "VAAPI graph format mismatch: graph expects %s, frame is %s. Fallback to swscale.",
                                av_get_pix_fmt_name((AVPixelFormat)src_output->format),
                                av_get_pix_fmt_name((AVPixelFormat)frame->format));
                use_vaapi = ZET_FALSE;
                return;
            }
        }
        // X86_64 optimization: HW decoder outputs VAAPI frames directly
        if (hwAccelCtx.hwDecenabled && frame->format == hwAccelCtx.hw_pix_fmt) {
            // HW input path: pass hardware frame directly into graph (zero-copy)
            work_frame = frame;
        } else { //LOG(LOG_INFO, "processVaapiScaling SW decoder + vaapi scale");
            // SW decoder path: feed decoder output frame directly into VAAPI graph.
            // Color format conversion (e.g. yuv420p -> nv12) will be handled by the filter chain.
            work_frame = frame;
        }

        if (use_vaapi) {
            int add_ret = av_buffersrc_add_frame_flags(va_src_ctx, work_frame, AV_BUFFERSRC_FLAG_KEEP_REF);
            if (add_ret < 0) {
                LOG(LOG_ERROR, "buffersrc add frame failed: %d, fallback to swscale", add_ret);
                use_vaapi = ZET_FALSE;
            }
        }

        if (use_vaapi) {
            av_frame_unref(processed_frame);
            int sink_ret = av_buffersink_get_frame(va_sink_ctx, processed_frame);
            if (sink_ret < 0) {
                if (sink_ret == AVERROR(EAGAIN)) {
                    return; // try next iteration
                }
                LOG(LOG_ERROR, "buffersink get frame failed: %d, fallback to swscale", sink_ret);
                use_vaapi = ZET_FALSE;
            } else {
                if (!va_out_hw && out_codec_ctx->pix_fmt != AV_PIX_FMT_NV12) { //vaapi scale failed and sw encoder, use swscale and change format to YUV420P
                    // Convert NV12 -> encoder SW pix_fmt (e.g., yuv420p)
                    static int c_w = 0, c_h = 0, c_dst = -1;
                    if (!sws_nv12_to_outfmt || c_w != processed_frame->width || c_h != processed_frame->height || c_dst != out_codec_ctx->pix_fmt) {
                        if (sws_nv12_to_outfmt) sws_freeContext(sws_nv12_to_outfmt);
                        sws_nv12_to_outfmt = sws_getContext(processed_frame->width, processed_frame->height, AV_PIX_FMT_NV12,
                                                            out_codec_ctx->width, out_codec_ctx->height, (AVPixelFormat)out_codec_ctx->pix_fmt,
                                                            SWS_BILINEAR, NULL, NULL, NULL);
                        c_w = processed_frame->width; c_h = processed_frame->height; c_dst = out_codec_ctx->pix_fmt;
                    }
                    if (!sws_nv12_to_outfmt) {
                        LOG(LOG_ERROR, "sws_nv12_to_outfmt create failed");
                        use_vaapi = ZET_FALSE;
                    } else {
                        // X86_64 optimization: reuse pooled frame to avoid repeated allocation
                        //static AVFrame* sw_out_pool = NULL;
                        static int pool_w = 0, pool_h = 0, pool_fmt = -1;
                        // Allocate or reuse pooled frame
                        if (!sw_out_pool || pool_w != out_codec_ctx->width ||
                            pool_h != out_codec_ctx->height || pool_fmt != out_codec_ctx->pix_fmt) {
                            //if (sw_out_pool) av_frame_free(&sw_out_pool);
                            if (!sw_out_pool) sw_out_pool = av_frame_alloc();
                            if (!sw_out_pool) {
                                LOG(LOG_ERROR, "alloc sw_out_pool frame failed");
                                return;
                            }
                            sw_out_pool->format = out_codec_ctx->pix_fmt;
                            sw_out_pool->width  = out_codec_ctx->width;
                            sw_out_pool->height = out_codec_ctx->height;
                            if (av_frame_get_buffer(sw_out_pool, 32) < 0) {
                                LOG(LOG_ERROR, "alloc sw_out_pool buffer failed");
                                av_frame_free(&sw_out_pool);
                                return;
                            }
                            pool_w = out_codec_ctx->width;
                            pool_h = out_codec_ctx->height;
                            pool_fmt = out_codec_ctx->pix_fmt;
                        }

                        sws_scale(sws_nv12_to_outfmt, (const uint8_t* const*)processed_frame->data, processed_frame->linesize, 0, processed_frame->height,
                                  sw_out_pool->data, sw_out_pool->linesize);
                        sw_out_pool->pts      = processed_frame->pts;
                        sw_out_pool->pkt_dts  = processed_frame->pkt_dts;
                        sw_out_pool->duration = processed_frame->duration;
                        target_frame          = sw_out_pool;
                        av_frame_unref(processed_frame);
                        scaled = ZET_TRUE;
                    }
                } else {
                    target_frame = processed_frame;
                    scaled = ZET_TRUE;
                }
            }
        }
    }
}

INT32 zetHlsServerMdl::processVideoWithX86_64(bool video_needs_transcode, HWAccelCtx hwAccelCtx, AVFrame *frame, AVFilterGraph **va_graph,
                                                        bool *use_hw_scale, AVCodecContext *in_video_ctx, struct _zetHlsGenInfo* hlsGenInfo, AVStream *in_video_stream,
                                                        AVCodecContext *out_video_ctx, AVFilterContext **va_src_ctx, AVFilterContext **va_fmt_in_ctx,
                                                        AVFilterContext **va_upload_ctx, AVFilterContext **va_scale_ctx, AVFilterContext **va_download_ctx,
                                                        AVFilterContext **va_fmt_out_ctx, AVFilterContext **va_sink_ctx, bool *out_hw, SwsContext **sws_to_nv12,
                                                        SwsContext **sws_nv12_to_out, AVFrame *tmp_hw_sw, double *prof_video_hw_time, AVFrame *processed_frame,
                                                        AVCodecContext *out_codec_ctx, AVFrame **target_frame, SwsContext **sws_ctx, AVFrame **temp_frame_pool,
                                                        int *temp_pool_w, int *temp_pool_h) {
    static bool va_graph_initialized = ZET_FALSE;
    if (!va_graph_initialized && video_needs_transcode) {
        // Check actual decoder output format after first decode
        bool actual_hw_decode = (hwAccelCtx.hwDecenabled && frame->format == hwAccelCtx.hw_pix_fmt);
        LOG(LOG_INFO, "First frame decoded: format=%s (%d), hw_pix_fmt=%s (%d), hwDecenabled=%d, actual_hw=%d",
                       av_get_pix_fmt_name((AVPixelFormat)frame->format), frame->format,
                       av_get_pix_fmt_name(hwAccelCtx.hw_pix_fmt), hwAccelCtx.hw_pix_fmt,
                       hwAccelCtx.hwDecenabled, actual_hw_decode);

        // If hw decode failed (sw fallback), rebuild graph
        if (*va_graph && !actual_hw_decode && *use_hw_scale) {
            LOG(LOG_WARNING, "HW decode failed, rebuilding VAAPI graph for SW input");
            if (in_video_ctx) {
                zet_configure_decoder_threads(in_video_ctx, hlsGenInfo);
            }
            avfilter_graph_free(va_graph);
            *va_graph     = NULL;
            *use_hw_scale = ZET_FALSE;
            initVaapiScalingGraph(video_needs_transcode, hwAccelCtx, in_video_ctx, in_video_stream,
                                  out_video_ctx, *va_graph, *va_src_ctx, *va_fmt_in_ctx, *va_upload_ctx,
                                  *va_scale_ctx, *va_download_ctx, *va_fmt_out_ctx, *va_sink_ctx,
                                  *use_hw_scale, *out_hw, *sws_to_nv12, *sws_nv12_to_out, tmp_hw_sw);
        }
        va_graph_initialized = ZET_TRUE;
    }

    bool   scaled        = ZET_FALSE;
    double prof_hw_start = zet_prof_now_sec();
    processVaapiScaling(scaled, *use_hw_scale, *va_graph, *va_src_ctx, *va_sink_ctx, hwAccelCtx, frame, tmp_hw_sw, *sws_to_nv12, processed_frame,
                        out_codec_ctx, *out_hw, *sws_nv12_to_out, *target_frame);
    *prof_video_hw_time += (zet_prof_now_sec() - prof_hw_start);
    if (!scaled && *sws_ctx) {
        av_frame_unref(processed_frame);
        processed_frame->width  = out_codec_ctx->width;
        processed_frame->height = out_codec_ctx->height;
        // For HW encoder (VAAPI), fallback SW frame must use encoder sw_format (NV12) to avoid color corruption
        processed_frame->format = (hwAccelCtx.hwEncenabled && out_codec_ctx == hwAccelCtx.video_enc_ctx) ? AV_PIX_FMT_NV12 : out_codec_ctx->pix_fmt;
        
        if (av_frame_get_buffer(processed_frame, 0) < 0) {
            LOG(LOG_ERROR, "unable to alloc frame buffer, please check");
            return ZET_ERR_MALLOC;
        }

        if (hwAccelCtx.hwDecenabled) {
            // Allocate or reuse pooled frame
            if (!*temp_frame_pool || *temp_pool_w != out_video_ctx->width || *temp_pool_h != out_video_ctx->height) {
                if (!*temp_frame_pool) *temp_frame_pool = av_frame_alloc();
                if (!*temp_frame_pool) {
                    LOG(LOG_ERROR, "fail to alloc temp_frame_pool, please check...");
                    return  ZET_NOK;
                }
                (*temp_frame_pool)->format = AV_PIX_FMT_YUV420P;
                (*temp_frame_pool)->width  = out_video_ctx->width;
                (*temp_frame_pool)->height = out_video_ctx->height;
                if (av_frame_get_buffer(*temp_frame_pool, 0) < 0) {
                    LOG(LOG_ERROR, "fail to alloc temp_frame_pool buffer");
                    av_frame_free(temp_frame_pool);
                    *temp_frame_pool = NULL;
                    return  ZET_NOK;
                }
                *temp_pool_w = out_video_ctx->width;
                *temp_pool_h = out_video_ctx->height;
            }

            int ret = av_hwframe_transfer_data(*temp_frame_pool, frame, 0);
            if (ret < 0) {
                char err_buf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(ret, err_buf, sizeof(err_buf));
                LOG(LOG_ERROR, "Failed to do hw frame to sw frame transfer: %s, format: %d", err_buf, (AVPixelFormat)frame->format);
                return ZET_ERR_MALLOC;
            }

            static bool swsCtxEnabled    = ZET_FALSE;
            (*temp_frame_pool)->pts      = frame->pts;
            (*temp_frame_pool)->pkt_dts  = frame->pkt_dts;
            (*temp_frame_pool)->duration = frame->duration;
            if (!swsCtxEnabled) {
                SwsContext* tmp = sws_getContext((*temp_frame_pool)->width, (*temp_frame_pool)->height, (AVPixelFormat)(*temp_frame_pool)->format,
                                                 processed_frame->width, processed_frame->height, (AVPixelFormat)processed_frame->format,
                                                 SWS_BICUBIC, NULL, NULL, NULL);
                if (!tmp) {
                    LOG(LOG_ERROR, "unable to create swscale ctx!");
                    return  ZET_NOK;
                }
                *sws_ctx       = tmp;
                swsCtxEnabled = ZET_TRUE;
            }

            ret = sws_scale(*sws_ctx, (const uint8_t * const*)(*temp_frame_pool)->data, (*temp_frame_pool)->linesize, 0, (*temp_frame_pool)->height,
                            processed_frame->data, processed_frame->linesize);
            if (ret < 0) {
                LOG(LOG_ERROR, "sws_scale execute failed....");
                return ret;
            }

            processed_frame->pts      = frame->pts;
            processed_frame->pkt_dts  = frame->pkt_dts;
            processed_frame->duration = frame->duration;
        } else {
            sws_scale(*sws_ctx, frame->data, frame->linesize, 0, frame->height, processed_frame->data, processed_frame->linesize);
            processed_frame->pts      = frame->pts;
            processed_frame->pkt_dts  = frame->pkt_dts;
        }
        *target_frame = processed_frame; //  SW YUV420P in CPU memory
    }
    return ZET_OK;
}

void zetHlsServerMdl::processX86HwUpload(int is_video, HWAccelCtx* hwAccelCtx, AVCodecContext* out_codec_ctx, 
                                                     AVFrame** target_frame, AVFrame** enc_hw_pool_x86)
{
    AVFrame* enc_hw_x86 = NULL;
    if (is_video && hwAccelCtx->hwEncenabled && out_codec_ctx->hw_frames_ctx && *target_frame &&
        (*target_frame)->format != hwAccelCtx->hw_pix_fmt) { // SW frame used by swscale and hw encoder needs upload

        // Allocate pool frame once on first use
        if (!*enc_hw_pool_x86) {
            *enc_hw_pool_x86 = av_frame_alloc();
            if (!*enc_hw_pool_x86) {
                LOG(LOG_ERROR, "Failed to allocate enc_hw_pool_x86 frame");
            }
        }

        if (*enc_hw_pool_x86) {
            // Reuse the pooled frame: unref old data, get new buffer
            av_frame_unref(*enc_hw_pool_x86);
            if (av_hwframe_get_buffer(out_codec_ctx->hw_frames_ctx, *enc_hw_pool_x86, 0) == 0) {
                int tr = av_hwframe_transfer_data(*enc_hw_pool_x86, *target_frame, 0);  // CPU -> GPU
                if (tr == 0) {
                    (*enc_hw_pool_x86)->pts       = (*target_frame)->pts;
                    (*enc_hw_pool_x86)->pkt_dts   = (*target_frame)->pkt_dts;
                    (*enc_hw_pool_x86)->duration  = (*target_frame)->duration;
                    enc_hw_x86                    = *enc_hw_pool_x86;
                    *target_frame                 = enc_hw_x86;
                } else {
                    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                    av_strerror(tr, errbuf, sizeof(errbuf));
                    LOG(LOG_ERROR, "SW->HW upload (CPU -> GPU) failed: %s", errbuf);
                }
            } else {
                LOG(LOG_ERROR, "av_hwframe_get_buffer failed for enc_hw_pool_x86");
            }
        }
    } else if (is_video && hwAccelCtx->hwEncenabled && *target_frame &&
                (*target_frame)->format == hwAccelCtx->hw_pix_fmt) { // X86_64: HW-out zero-copy path detected (frame already in GPU memory)
        // LOG(LOG_VERBOSE, "Zero-copy HW path: VAAPI output (GPU memory) -> HW encoder directly");
    } else if (is_video && !hwAccelCtx->hwEncenabled && *target_frame) {
        //LOG(LOG_VERBOSE, "SW encoding path: VAAPI output or sws_ctx (format=%s, CPU memory) -> SW encoder directly",
                         // av_get_pix_fmt_name((AVPixelFormat)(*target_frame)->format));
    }
}

#elif ARM
static bool saveFrameToFile(AVFrame* frame, const char* filename, FILE* file) {
    if (!frame || !frame->data[0]) {
        LOG(LOG_ERROR, "Invalid frame for saving: %s", filename);
        return ZET_FALSE;
    }

    if (!file) {
        file = fopen(filename, "wb");
        if (!file) {
            LOG(LOG_ERROR, "Failed to open file for writing: %s", filename);
            return ZET_FALSE;
        }
    }

    AVPixelFormat pix_fmt = static_cast<AVPixelFormat>(frame->format);
    LOG(LOG_VERBOSE, "open filename :%s for writing: YUV_FRAME width=%d height=%d format=%s\n", filename, frame->width, frame->height, av_get_pix_fmt_name((AVPixelFormat)frame->format));

    switch (pix_fmt) {
        case AV_PIX_FMT_YUV420P:
            // YUV420P: Planar format, Y first, then U, then V
            for (int i = 0; i < frame->height; i++) {
                fwrite(frame->data[0] + i * frame->linesize[0], 1, frame->width, file);fflush(file);
            }
            for (int i = 0; i < frame->height / 2; i++) {
                fwrite(frame->data[1] + i * frame->linesize[1], 1, frame->width / 2, file);fflush(file);
            }
            for (int i = 0; i < frame->height / 2; i++) {
                fwrite(frame->data[2] + i * frame->linesize[2], 1, frame->width / 2, file);fflush(file);
            }
            break;
        case AV_PIX_FMT_NV12:
            // NV12: Y-plane + UV-interleaved plane
            for (int i = 0; i < frame->height; i++) {
                fwrite(frame->data[0] + i * frame->linesize[0], 1, frame->width, file);fflush(file);
            }
            for (int i = 0; i < frame->height / 2; i++) {
                fwrite(frame->data[1] + i * frame->linesize[1], 1, frame->width, file);fflush(file);
            }
            break;
        case AV_PIX_FMT_NV21:
            // NV21: Y-plane + VU-interleaved plane
            for (int i = 0; i < frame->height; i++) {
                fwrite(frame->data[0] + i * frame->linesize[0], 1, frame->width, file);fflush(file);
            }
            for (int i = 0; i < frame->height / 2; i++) {
                fwrite(frame->data[1] + i * frame->linesize[1], 1, frame->width, file);fflush(file);
            }
            break;
        default:
            // Universal Method: Save All Planes
            LOG(LOG_WARNING, "Using generic save method for format: %s", av_get_pix_fmt_name((AVPixelFormat)frame->format));
            for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
                if (frame->data[i]) {
                    int height = frame->height;
                    if (i > 0) {
                        // Half the height of the chromaticity plane
                        height = frame->height / 2;
                    }
                    for (int j = 0; j < height; j++) {
                        fwrite(frame->data[i] + j * frame->linesize[i], 1, frame->linesize[i], file);fflush(file);
                    }
                }
            }
            break;
    }

    return ZET_TRUE;
}

static bool saveDri_PrimeFrameToFile(AVFrame* frame, const char* filename, FILE* file) {
    if (!frame || !frame->data[0] && frame->format != AV_PIX_FMT_DRM_PRIME) {
        LOG(LOG_ERROR, "Invalid frame for saving: %s", filename);
        return ZET_FALSE;
    }

    AVFrame* sw_frame = NULL;
    AVPixelFormat orig_fmt = static_cast<AVPixelFormat>(frame->format);

    if (orig_fmt == AV_PIX_FMT_DRM_PRIME) {
        sw_frame = av_frame_alloc();
        if (!sw_frame) {
            LOG(LOG_ERROR, "Failed to alloc sw_frame for DRM_PRIME");
            return ZET_FALSE;
        }

        int ret = av_hwframe_transfer_data(sw_frame, frame, 0);
        if (ret < 0) {
            LOG(LOG_ERROR, "Failed to transfer DRM_PRIME to SW frame: %d", ret);
            av_frame_free(&sw_frame);
            return ZET_FALSE;
        }

        LOG(LOG_INFO, "DRM_PRIME transfer result: format=%s (%d), size=%dx%d",
                       av_get_pix_fmt_name((AVPixelFormat)sw_frame->format), sw_frame->format, sw_frame->width, sw_frame->height);

        frame = sw_frame;
    }

    if (!file) {
        file = fopen(filename, "wb");
        if (!file) {
            LOG(LOG_ERROR, "Failed to open file for writing: %s", filename);
            if (sw_frame) av_frame_free(&sw_frame);
            return ZET_FALSE;
        }
    }

    AVPixelFormat pix_fmt = static_cast<AVPixelFormat>(frame->format);
    LOG(LOG_VERBOSE, "Writing frame to %s (format: %s, %dx%d)", filename, av_get_pix_fmt_name(pix_fmt), frame->width, frame->height);

    switch (pix_fmt) {
        case AV_PIX_FMT_YUV420P:
            for (int i = 0; i < frame->height; i++) {
                fwrite(frame->data[0] + i * frame->linesize[0], 1, frame->width, file);
            }
            for (int i = 0; i < frame->height / 2; i++) {
                fwrite(frame->data[1] + i * frame->linesize[1], 1, frame->width / 2, file);
            }
            for (int i = 0; i < frame->height / 2; i++) {
                fwrite(frame->data[2] + i * frame->linesize[2], 1, frame->width / 2, file);
            }
            break;
        case AV_PIX_FMT_NV12:
            for (int i = 0; i < frame->height; i++) {
                fwrite(frame->data[0] + i * frame->linesize[0], 1, frame->width, file);
            }
            for (int i = 0; i < frame->height / 2; i++) {
                fwrite(frame->data[1] + i * frame->linesize[1], 1, frame->width, file);
            }
            break;
        case AV_PIX_FMT_NV21:
            for (int i = 0; i < frame->height; i++) {
                fwrite(frame->data[0] + i * frame->linesize[0], 1, frame->width, file);
            }
            for (int i = 0; i < frame->height / 2; i++) {
                fwrite(frame->data[1] + i * frame->linesize[1], 1, frame->width, file);
            }
            break;
        case AV_PIX_FMT_NV15:
        case AV_PIX_FMT_P010LE:
        case AV_PIX_FMT_P010BE:
            // 10-bit formats: 2 bytes per pixel component
            for (int i = 0; i < frame->height; i++) {
                fwrite(frame->data[0] + i * frame->linesize[0], 1, frame->linesize[0], file);
            }
            for (int i = 0; i < frame->height / 2; i++) {
                fwrite(frame->data[1] + i * frame->linesize[1], 1, frame->linesize[1], file);
            }
            break;
        default:
            LOG(LOG_WARNING, "Using generic method for format: %s", av_get_pix_fmt_name(pix_fmt));
            for (int i = 0; i < AV_NUM_DATA_POINTERS; i++) {
                if (frame->data[i] && frame->linesize[i] > 0) {
                    int plane_height = (i == 0) ? frame->height : (frame->height / 2);
                    for (int j = 0; j < plane_height; j++) {
                        fwrite(frame->data[i] + j * frame->linesize[i], 1, frame->linesize[i], file);
                    }
                }
            }
            break;
    }

    if (sw_frame) {
        av_frame_unref(sw_frame);
        av_frame_free(&sw_frame);
    }
    return ZET_TRUE;
}

void zetHlsServerMdl::initRgaFilterGraph(AVFilterGraph **rga_graph, AVFilterContext **rga_src_ctx, AVFilterContext **rga_fmt_in_ctx,
                                         AVFilterContext **rga_upload_ctx, AVFilterContext **rga_scale_ctx, AVFilterContext **rga_download_ctx,
                                         AVFilterContext **rga_fmt_out_ctx, AVFilterContext **rga_sink_ctx, bool *use_rga,
                                         bool *rga_out_hw, SwsContext **sws_to_nv12, SwsContext **sws_nv12_to_yuv420,
                                         AVFrame *temp_frame, bool video_needs_transcode, const HWAccelCtx *hwAccelCtx,
                                         const AVCodecContext *in_video_ctx, const AVStream *in_video_stream,
                                         const AVCodecContext *out_video_ctx, bool *rga_in_hw, bool force_sw_out) {
    *use_rga    = ZET_FALSE;
    *rga_out_hw = ZET_FALSE;
    if (rga_in_hw) *rga_in_hw = ZET_FALSE;

    LOG(LOG_INFO, "initRgaFilterGraph: video_needs_transcode=%d, hwDecenabled=%d, hwEncenabled=%d",
                   video_needs_transcode, hwAccelCtx->hwDecenabled, hwAccelCtx->hwEncenabled);

    if (video_needs_transcode && (hwAccelCtx->hwDecenabled || hwAccelCtx->hwEncenabled)) {
        *use_rga  = ZET_TRUE;
        int src_w = in_video_ctx ? in_video_ctx->width : in_video_stream->codecpar->width;
        int src_h = in_video_ctx ? in_video_ctx->height : in_video_stream->codecpar->height;
        int dst_w = out_video_ctx ? out_video_ctx->width : in_video_stream->codecpar->width;
        int dst_h = out_video_ctx ? out_video_ctx->height : in_video_stream->codecpar->height;

        *rga_out_hw = (hwAccelCtx->hwEncenabled && out_video_ctx == hwAccelCtx->video_enc_ctx &&
                       hwAccelCtx->video_enc_ctx && hwAccelCtx->video_enc_ctx->pix_fmt == hwAccelCtx->hw_pix_fmt);
        if (force_sw_out) {
            *rga_out_hw = ZET_FALSE;
        }

        bool hw_input = hwAccelCtx->hwDecenabled && (!in_video_ctx || in_video_ctx->pix_fmt == hwAccelCtx->hw_pix_fmt);
        if (rga_in_hw) *rga_in_hw = hw_input;

        LOG(LOG_DEBUG, "RGA filter graph config: src=%dx%d, dst=%dx%d, hw_input=%d, rga_out_hw=%d",
                       src_w, src_h, dst_w, dst_h, hw_input, *rga_out_hw);

        *rga_graph = avfilter_graph_alloc();
        if (!*rga_graph) {
            LOG(LOG_ERROR, "RGA graph alloc failed, fallback to swscale");
            *use_rga = ZET_FALSE;
            return;
        }

        char args[256];
        AVRational tb  = in_video_stream ? in_video_stream->time_base : (AVRational){1,25};
        AVRational sar = (AVRational){1,1};

        const AVFilter *buffersrc    = avfilter_get_by_name("buffer");
        const AVFilter *buffersink   = avfilter_get_by_name("buffersink");
        const AVFilter *fmt_filter   = avfilter_get_by_name("format");
        const AVFilter *hwupload_f   = avfilter_get_by_name("hwupload");
        const AVFilter *hwdownload_f = avfilter_get_by_name("hwdownload");
        const AVFilter *rga_scale    = avfilter_get_by_name("scale_rkrga");

        if (!buffersrc || !buffersink || !rga_scale || (!*rga_out_hw && !hwdownload_f)) {
            LOG(LOG_ERROR, "Missing filters for RGA chain, fallback to swscale");
            *use_rga = ZET_FALSE;
            return;
        }

        AVBufferRef *dec_hw_device = (hwAccelCtx->video_dec_ctx && hwAccelCtx->video_dec_ctx->hw_device_ctx)
                                      ? av_buffer_ref(hwAccelCtx->video_dec_ctx->hw_device_ctx) : NULL;
        AVBufferRef *enc_hw_device = (hwAccelCtx->video_enc_ctx && hwAccelCtx->video_enc_ctx->hw_device_ctx)
                                      ? av_buffer_ref(hwAccelCtx->video_enc_ctx->hw_device_ctx) : NULL;
        AVBufferRef *use_hw_device = NULL;
        if (*rga_out_hw && enc_hw_device) {
            use_hw_device = av_buffer_ref(enc_hw_device);
        } else if (dec_hw_device) {
            use_hw_device = av_buffer_ref(dec_hw_device);
        }
        if (!use_hw_device && (enc_hw_device || dec_hw_device)) {
            LOG(LOG_ERROR, "Failed to ref hw_device for RGA");
            *use_rga = ZET_FALSE;
            if (dec_hw_device) av_buffer_unref(&dec_hw_device);
            if (enc_hw_device) av_buffer_unref(&enc_hw_device);
            return;
        }

        if (hw_input) {
            // buffersrc for HW input frames (drm_prime)
            snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                     src_w, src_h, hwAccelCtx->hw_pix_fmt, tb.num, tb.den, sar.num, sar.den);
            if (avfilter_graph_create_filter(rga_src_ctx, buffersrc, "in", args, NULL, *rga_graph) < 0) {
                LOG(LOG_ERROR, "create buffersrc(HW) failed, fallback to swscale");
                *use_rga = ZET_FALSE;
            } else if (hwAccelCtx->video_dec_ctx && hwAccelCtx->video_dec_ctx->hw_frames_ctx) {
                AVBufferSrcParameters *par = av_buffersrc_parameters_alloc();
                if (!par) { LOG(LOG_ERROR, "alloc AVBufferSrcParameters failed"); *use_rga = ZET_FALSE; }
                if (*use_rga) {
                    par->format        = hwAccelCtx->hw_pix_fmt;
                    par->time_base     = tb;
                    par->width         = src_w;
                    par->height        = src_h;
                    par->hw_frames_ctx = av_buffer_ref(hwAccelCtx->video_dec_ctx->hw_frames_ctx);

                    // Log hw_frames_ctx details for debugging
                    if (par->hw_frames_ctx) {
                        AVHWFramesContext* fc = (AVHWFramesContext*)par->hw_frames_ctx->data;
                        LOG(LOG_DEBUG, "RGA buffersrc hw_frames_ctx: format=%s, sw_format=%s, %dx%d",
                                       av_get_pix_fmt_name(fc->format), av_get_pix_fmt_name(fc->sw_format), fc->width, fc->height);
                    }

                    int pr = av_buffersrc_parameters_set(*rga_src_ctx, par);
                    if (pr < 0) {
                        LOG(LOG_ERROR, "set buffersrc parameters failed: %d", pr);
                        *use_rga = ZET_FALSE;
                    }
                }
                av_freep(&par);
            }
        } else {
            // buffersrc for SW input -> format(nv12) -> hwupload
            // Use actual decoder output pixel format as buffersrc input to avoid duplicate SW conversion here.
            if (!fmt_filter || !hwupload_f) {
                LOG(LOG_ERROR, "Missing SW path filters, fallback to swscale");
                *use_rga = ZET_FALSE;
            } else {
                AVPixelFormat src_fmt = AV_PIX_FMT_YUV420P;
                if (in_video_ctx && in_video_ctx->pix_fmt != AV_PIX_FMT_NONE) {   
                    src_fmt = (AVPixelFormat)in_video_ctx->pix_fmt;
                } else if (in_video_stream && in_video_stream->codecpar &&
                           in_video_stream->codecpar->format != AV_PIX_FMT_NONE) {
                    src_fmt = (AVPixelFormat)in_video_stream->codecpar->format;
                }

                snprintf(args, sizeof(args), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                         src_w, src_h, src_fmt, tb.num, tb.den, sar.num, sar.den);
                if (avfilter_graph_create_filter(rga_src_ctx, buffersrc, "in", args, NULL, *rga_graph) < 0) {
                    LOG(LOG_ERROR, "create buffersrc(SW) failed, fallback to swscale");
                    *use_rga = ZET_FALSE;
                }
                if (*use_rga && avfilter_graph_create_filter(rga_fmt_in_ctx, fmt_filter, "fmt_in", "pix_fmts=nv12", NULL, *rga_graph) < 0) {
                    LOG(LOG_ERROR, "create format(nv12) failed, fallback to swscale");
                    *use_rga = ZET_FALSE;
                }
                if (*use_rga && avfilter_graph_create_filter(rga_upload_ctx, hwupload_f, "hwupload", NULL, NULL, *rga_graph) < 0) {
                    LOG(LOG_ERROR, "create hwupload failed, fallback to swscale");
                    *use_rga = ZET_FALSE;
                } else if (*use_rga && use_hw_device) {
                    AVBufferRef* ref = av_buffer_ref(use_hw_device);
                    if (ref) {
                        (*rga_upload_ctx)->hw_device_ctx = ref;
                    } else {
                        LOG(LOG_ERROR, "Failed to ref hw device for rga upload");
                        *use_rga = ZET_FALSE;
                    }
                }
            }
        }

        if (*use_rga) {
            char scale_args[128];
            // Match ffmpeg CLI: scale_rkrga=w=...:h=...:format=nv12:force_original_aspect_ratio=disable
            snprintf(scale_args, sizeof(scale_args), "w=%d:h=%d:format=nv12:force_original_aspect_ratio=disable", dst_w, dst_h);
            if (avfilter_graph_create_filter(rga_scale_ctx, rga_scale, "scale_rkrga", scale_args, NULL, *rga_graph) < 0) {
                LOG(LOG_ERROR, "create scale_rkrga failed, args=%s, fallback to swscale", scale_args);
                *use_rga = ZET_FALSE;
            } else if (use_hw_device) {
                AVBufferRef* ref = av_buffer_ref(use_hw_device);
                if (ref) {
                    (*rga_scale_ctx)->hw_device_ctx = ref;
                } else {
                    LOG(LOG_ERROR, "Failed to ref hw device for rga scale");
                }
            }
        }

        if (*use_rga && !*rga_out_hw) {
            if (avfilter_graph_create_filter(rga_download_ctx, hwdownload_f, "hwdownload", NULL, NULL, *rga_graph) < 0) {
                LOG(LOG_ERROR, "create hwdownload failed, fallback to swscale");
                *use_rga = ZET_FALSE;
            } else if (use_hw_device) {
                AVBufferRef* ref = av_buffer_ref(use_hw_device);
                if (ref) {
                    (*rga_download_ctx)->hw_device_ctx = ref;
                } else {
                    LOG(LOG_ERROR, "Failed to ref hw device for rga download");
                }
            }
            if (*use_rga && avfilter_graph_create_filter(rga_fmt_out_ctx, fmt_filter, "fmt_out", "pix_fmts=nv12", NULL, *rga_graph) < 0) {
                LOG(LOG_ERROR, "create format(nv12) out failed, fallback to swscale");
                *use_rga = ZET_FALSE;
            }
        }

        const int sink_pix_fmts_hw[] = { (int)hwAccelCtx->hw_pix_fmt, AV_PIX_FMT_NONE };
        const int sink_pix_fmts_sw[] = { AV_PIX_FMT_NV12, AV_PIX_FMT_NONE };

        if (*use_rga && avfilter_graph_create_filter(rga_sink_ctx, buffersink, "out", NULL, NULL, *rga_graph) < 0) {
            LOG(LOG_ERROR, "create buffersink failed, fallback to swscale");
            *use_rga = ZET_FALSE;
        } else if (*use_rga) {
            av_opt_set_int_list(*rga_sink_ctx, "pix_fmts", *rga_out_hw ? sink_pix_fmts_hw : sink_pix_fmts_sw, AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN);
        }

        if (*use_rga) {
            int link_ok = 0;
            if (hw_input) {
                if (*rga_out_hw) {
                    link_ok = (avfilter_link(*rga_src_ctx, 0, *rga_scale_ctx, 0) >= 0 &&
                               avfilter_link(*rga_scale_ctx, 0, *rga_sink_ctx, 0) >= 0);
                } else {
                    link_ok = (avfilter_link(*rga_src_ctx, 0, *rga_scale_ctx, 0) >= 0 &&
                               avfilter_link(*rga_scale_ctx, 0, *rga_download_ctx, 0) >= 0 &&
                               avfilter_link(*rga_download_ctx, 0, *rga_fmt_out_ctx, 0) >= 0 &&
                               avfilter_link(*rga_fmt_out_ctx, 0, *rga_sink_ctx, 0) >= 0);
                }
            } else {
                if (*rga_out_hw) {
                    link_ok = (avfilter_link(*rga_src_ctx, 0, *rga_fmt_in_ctx, 0) >= 0 &&
                               avfilter_link(*rga_fmt_in_ctx, 0, *rga_upload_ctx, 0) >= 0 &&
                               avfilter_link(*rga_upload_ctx, 0, *rga_scale_ctx, 0) >= 0 &&
                               avfilter_link(*rga_scale_ctx, 0, *rga_sink_ctx, 0) >= 0);
                } else {
                    link_ok = (avfilter_link(*rga_src_ctx, 0, *rga_fmt_in_ctx, 0) >= 0 &&
                               avfilter_link(*rga_fmt_in_ctx, 0, *rga_upload_ctx, 0) >= 0 &&
                               avfilter_link(*rga_upload_ctx, 0, *rga_scale_ctx, 0) >= 0 &&
                               avfilter_link(*rga_scale_ctx, 0, *rga_download_ctx, 0) >= 0 &&
                               avfilter_link(*rga_download_ctx, 0, *rga_fmt_out_ctx, 0) >= 0 &&
                               avfilter_link(*rga_fmt_out_ctx, 0, *rga_sink_ctx, 0) >= 0);
                }
            }
            if (!link_ok) {
                LOG(LOG_ERROR, "link RGA graph failed, fallback to swscale");
                *use_rga = ZET_FALSE;
            }
        }

        if (*use_rga && avfilter_graph_config(*rga_graph, NULL) < 0) {
            LOG(LOG_ERROR, "RGA graph config failed, fallback to swscale");
            *use_rga = ZET_FALSE;
        } else if (*use_rga) {
            LOG(LOG_INFO, "RGA scaling enabled (%s, %s-in): %dx%d -> %dx%d",
                          *rga_out_hw ? "HW out" : "SW out", hw_input ? "HW" : "SW",
                          src_w, src_h, dst_w, dst_h);
        }

        if (dec_hw_device) av_buffer_unref(&dec_hw_device);
        if (enc_hw_device) av_buffer_unref(&enc_hw_device);
        if (use_hw_device) av_buffer_unref(&use_hw_device);
    }
}

// Debug file handles - ensure these are closed when done
static FILE*scale_in_src_file    = NULL; 
static FILE*scale_in_dst_file    = NULL;
static FILE*after_rgascale_file  = NULL;
static FILE*final_rgaOutput_file = NULL;
static AVFrame* yuv420_pool      = NULL;

// Cleanup function for debug files
static void cleanup_debug_files() {
    if (scale_in_src_file) { fclose(scale_in_src_file); scale_in_src_file = NULL; }
    if (scale_in_dst_file) { fclose(scale_in_dst_file); scale_in_dst_file = NULL; }
    if (after_rgascale_file) { fclose(after_rgascale_file); after_rgascale_file = NULL; }
    if (final_rgaOutput_file) { fclose(final_rgaOutput_file); final_rgaOutput_file = NULL; }
}

int zetHlsServerMdl::processWithRgaScaling(AVFrame *frame, AVFrame **target_frame, AVFrame *processed_frame,
                                           AVFrame *temp_frame, bool use_rga, bool rga_out_hw,
                                           AVFilterContext *rga_src_ctx, AVFilterContext *rga_sink_ctx,
                                           const HWAccelCtx *hwAccelCtx, SwsContext **sws_to_nv12,
                                           SwsContext **sws_nv12_to_yuv420, const AVCodecContext *out_codec_ctx,
                                           bool rga_in_hw) {
    if (!use_rga) return 0;
    // If graph expects HW-in but got SW (or vice versa), signal caller to rebuild graph
    bool is_hw_frame = (frame && frame->format == hwAccelCtx->hw_pix_fmt);
    if ((rga_in_hw && !is_hw_frame) || (!rga_in_hw && is_hw_frame)) {
        return AVERROR_INVALIDDATA;
    }
    if (!use_rga) {
        return 0;
    }

#if ARM
    // Debug: save input frame before RGA processing (must dri_prime format)
    static int dbg_frame_count = 0;
    if (dbg_frame_count < 5) {
        //saveDri_PrimeFrameToFile(frame, "./scale_in_src_file.yuv", scale_in_src_file); // only HW frame can use this function
        dbg_frame_count++;
    }
#endif

    // Handle format conversion for RGA: DRM_PRIME input uses HW frames directly; SW input relies on filtergraph format(nv12).
    AVFrame* work_frame = frame;
    if (rga_in_hw) {
        // HW input path: pass hardware frame directly into graph
        work_frame = frame;
    } else {
        // SW input path: let RGA filter graph handle pixel format conversion via format(nv12)
        work_frame = frame;
    }

    // Feed frame to RGA filter graph
    int add_ret = av_buffersrc_add_frame_flags(rga_src_ctx, work_frame, 0);
    if (add_ret < 0) {
        LOG(LOG_ERROR, "buffersrc add frame failed: %d", add_ret);
        return ZET_NOK;
    }

    av_frame_unref(processed_frame);
    int sink_ret = av_buffersink_get_frame(rga_sink_ctx, processed_frame);
    if (sink_ret < 0) {
        if (sink_ret == AVERROR(EAGAIN)) return AVERROR(EAGAIN);
        LOG(LOG_VERBOSE, "rga buffersink get frame failed: %d", sink_ret);
        return sink_ret;
    }

    //saveFrameToFile(processed_frame, "./after_rgascale_file.yuv", after_rgascale_file); // must SW Frame can use this function
    // Debug: report actual output pixel format (HW sw_format vs SW fmt)
#if 0
    if (processed_frame->format == hwAccelCtx->hw_pix_fmt && processed_frame->hw_frames_ctx) {
        AVHWFramesContext* ofc = (AVHWFramesContext*)processed_frame->hw_frames_ctx->data;
        if (ofc) {
            LOG(LOG_VERBOSE, "RGA out HW sw_format=%s w=%d h=%d ctx=%p enc_ctx=%p",
                              av_get_pix_fmt_name(ofc->sw_format), processed_frame->width, processed_frame->height,
                              (void*)processed_frame->hw_frames_ctx, (void*)out_codec_ctx->hw_frames_ctx);
        }
    } else {
        LOG(LOG_VERBOSE, "RGA out SW format=%s w=%d h=%d", av_get_pix_fmt_name((AVPixelFormat)processed_frame->format), processed_frame->width, processed_frame->height);
    }
#endif
    // hw decoder: out_codec_ctx->pix_fmt drm_prime, hwAccelCtx->hw_pix_fmt drm_prime, processed_frame->format: nv12, target_frame->format nv12
    // sw decoder: out_codec_ctx->pix_fmt drm_prime, hwAccelCtx->hw_pix_fmt drm_prime, processed_frame->format: nv12,
    if (!rga_out_hw && out_codec_ctx->pix_fmt != AV_PIX_FMT_NV12 && out_codec_ctx->pix_fmt != hwAccelCtx->hw_pix_fmt) {
        static int c_w = 0, c_h = 0, c_dst = -1;
        // ARM optimization: reuse pooled frame to avoid repeated allocation
        static int pool_w = 0, pool_h = 0, pool_fmt = -1;

        if (!*sws_nv12_to_yuv420 || c_w != processed_frame->width || c_h != processed_frame->height || c_dst != out_codec_ctx->pix_fmt) {
            if (*sws_nv12_to_yuv420) sws_freeContext(*sws_nv12_to_yuv420);
            *sws_nv12_to_yuv420 = sws_getContext(processed_frame->width, processed_frame->height, AV_PIX_FMT_NV12,
                                                 out_codec_ctx->width, out_codec_ctx->height, (AVPixelFormat)out_codec_ctx->pix_fmt,
                                                 SWS_BILINEAR, NULL, NULL, NULL);
            c_w   = processed_frame->width;
            c_h   = processed_frame->height;
            c_dst = out_codec_ctx->pix_fmt;
            // Reset pool when parameters change
            if (yuv420_pool) {
                av_frame_free(&yuv420_pool);
                yuv420_pool = NULL;
                pool_w = 0; pool_h = 0; pool_fmt = -1;
            }
        }
        if (!*sws_nv12_to_yuv420) {
            LOG(LOG_ERROR, "sws_nv12_to_yuv420 create failed");
            return ZET_NOK;
        }

        // Allocate or reuse pooled frame
        if (!yuv420_pool || pool_w != out_codec_ctx->width || pool_h != out_codec_ctx->height || pool_fmt != out_codec_ctx->pix_fmt) {
            if (!yuv420_pool) yuv420_pool = av_frame_alloc();
            if (!yuv420_pool) {
                LOG(LOG_ERROR, "alloc yuv420_pool frame failed"); 
                return ZET_NOK;
            }
            yuv420_pool->format = out_codec_ctx->pix_fmt;
            yuv420_pool->width  = out_codec_ctx->width;
            yuv420_pool->height = out_codec_ctx->height;
            if (av_frame_get_buffer(yuv420_pool, 32) < 0) {
                LOG(LOG_ERROR, "alloc yuv420_pool buffer failed"); 
                av_frame_free(&yuv420_pool); 
                return ZET_NOK;
            }
            pool_w   = out_codec_ctx->width;
            pool_h   = out_codec_ctx->height;
            pool_fmt = out_codec_ctx->pix_fmt;
        }

        sws_scale(*sws_nv12_to_yuv420,  (const uint8_t* const*)processed_frame->data, processed_frame->linesize,
                   0, processed_frame->height, yuv420_pool->data, yuv420_pool->linesize);

        yuv420_pool->pts      = processed_frame->pts;
        yuv420_pool->pkt_dts  = processed_frame->pkt_dts;
        yuv420_pool->duration = processed_frame->duration;
        *target_frame         = yuv420_pool;
        av_frame_unref(processed_frame);
    } else {
        *target_frame = processed_frame;
    }
    //saveFrameToFile(*target_frame, "./final_rgaOutput_file.yuv", final_rgaOutput_file); // only SW Frame can use this function
    return 0;
}

INT32 zetHlsServerMdl::processVideoWithARM(bool video_needs_transcode, HWAccelCtx hwAccelCtx, AVFrame *frame, AVFilterGraph **rga_graph,
                                                   bool *use_hw_scale, bool *in_hw, AVCodecContext *in_video_ctx, struct _zetHlsGenInfo* hlsGenInfo,
                                                   AVStream *in_video_stream, AVCodecContext *out_video_ctx, AVFilterContext **rga_src_ctx,
                                                   AVFilterContext **rga_fmt_in_ctx, AVFilterContext **rga_upload_ctx, AVFilterContext **rga_scale_ctx,
                                                   AVFilterContext **rga_download_ctx, AVFilterContext **rga_fmt_out_ctx, AVFilterContext **rga_sink_ctx,
                                                   bool *out_hw, SwsContext **sws_to_nv12, SwsContext **sws_nv12_to_yuv, AVFrame *tmp_hw_sw,
                                                   double *prof_video_hw_time, AVFrame **target_frame, AVFrame *processed_frame, SwsContext **sws_ctx, AVCodecContext* out_codec_ctx) {
    // Build or rebuild RGA graph lazily
    // ARM optimization: use HW-out (drm_prime) when encoder is HW-enabled for zero-copy
    const bool hw_involved = (hwAccelCtx.hwDecenabled || hwAccelCtx.hwEncenabled);
    if (!*use_hw_scale && video_needs_transcode && hw_involved) { //Fault tolerance handling for h264 while sar not 1:1, h263, and Mjpeg
        bool force_sw_out = !hwAccelCtx.hwEncenabled;
        if (*rga_graph) { avfilter_graph_free(rga_graph); *rga_graph = NULL; }
        *in_hw = (frame->format == hwAccelCtx.hw_pix_fmt);
        LOG(LOG_DEBUG, "Building RGA graph: frame_format=%s (%d), hw_pix_fmt=%s (%d), in_hw=%d, hwEncenabled=%d",
                       av_get_pix_fmt_name((AVPixelFormat)frame->format), frame->format,
                       av_get_pix_fmt_name(hwAccelCtx.hw_pix_fmt), hwAccelCtx.hw_pix_fmt, *in_hw, hwAccelCtx.hwEncenabled);

        if (!*in_hw && !hwAccelCtx.hwDecenabled && in_video_ctx) {
            zet_configure_decoder_threads(in_video_ctx, hlsGenInfo);
        }

        initRgaFilterGraph(rga_graph, rga_src_ctx, rga_fmt_in_ctx, rga_upload_ctx,
                           rga_scale_ctx, rga_download_ctx, rga_fmt_out_ctx,
                           rga_sink_ctx, use_hw_scale, out_hw, sws_to_nv12, sws_nv12_to_yuv, tmp_hw_sw,
                           video_needs_transcode, &hwAccelCtx, in_video_ctx, in_video_stream, out_video_ctx,
                           in_hw, force_sw_out);
    }

    bool tried_rga = ZET_FALSE;
    if (*use_hw_scale) {
        tried_rga  = ZET_TRUE;
        double prof_hw_start = zet_prof_now_sec();
        int r = processWithRgaScaling(frame, target_frame, processed_frame, tmp_hw_sw, *use_hw_scale,
                                      *out_hw, *rga_src_ctx, *rga_sink_ctx, &hwAccelCtx, sws_to_nv12, sws_nv12_to_yuv, out_codec_ctx, *in_hw);
        *prof_video_hw_time += (zet_prof_now_sec() - prof_hw_start);
        
        if (r == AVERROR_INVALIDDATA) {
            // Input type changed (HW <---> SW), rebuild graph
            if (*rga_graph) { avfilter_graph_free(rga_graph); *rga_graph = NULL; }
            *use_hw_scale = ZET_FALSE;
            *in_hw        = (frame->format == hwAccelCtx.hw_pix_fmt);
            bool force_sw_out = !hwAccelCtx.hwEncenabled;
            initRgaFilterGraph(rga_graph, rga_src_ctx, rga_fmt_in_ctx, rga_upload_ctx,
                               rga_scale_ctx, rga_download_ctx, rga_fmt_out_ctx,
                               rga_sink_ctx, use_hw_scale, out_hw, sws_to_nv12, sws_nv12_to_yuv, tmp_hw_sw,
                               video_needs_transcode, &hwAccelCtx, in_video_ctx, in_video_stream, out_video_ctx,
                               in_hw, force_sw_out);
            if (*use_hw_scale) {
                r = processWithRgaScaling(frame, target_frame, processed_frame, tmp_hw_sw, *use_hw_scale,
                                          *out_hw, *rga_src_ctx, *rga_sink_ctx, &hwAccelCtx, sws_to_nv12, sws_nv12_to_yuv, out_codec_ctx, *in_hw);
            }
        }

        if (*use_hw_scale && r < 0) {
            if (r == AVERROR(EAGAIN)) {
                return ZET_ERR_CONTINUE;
            } else {
                LOG(LOG_ERROR, "processWithRgaScaling failed: %d", r);
                tried_rga    = ZET_FALSE;
                *use_hw_scale = ZET_FALSE;
            }
        }
    }

    if (!tried_rga && *sws_ctx) {
        av_frame_unref(processed_frame);
        processed_frame->width  = out_codec_ctx->width;
        processed_frame->height = out_codec_ctx->height;
        processed_frame->format = (hwAccelCtx.hwEncenabled && out_codec_ctx == hwAccelCtx.video_enc_ctx) ? AV_PIX_FMT_NV12 : out_codec_ctx->pix_fmt;

        if (av_frame_get_buffer(processed_frame, 32) < 0) {
            LOG(LOG_ERROR, "unable to alloc frame buffer, please check");
            return ZET_ERR_MALLOC;
        }

        if (hwAccelCtx.hwDecenabled && frame->format == hwAccelCtx.hw_pix_fmt) { // transfer hw dec frame to sw frame
            if (!tmp_hw_sw) {
                LOG(LOG_ERROR, "failed to alloc tmp_hw_sw frame");
                return ZET_NOK;
            }
            av_frame_unref(tmp_hw_sw);

            // Get actual sw_format from decoder context (may be P010LE for 10-bit)
            AVPixelFormat transfer_fmt = AV_PIX_FMT_NV12;  // Default
            if (hwAccelCtx.video_dec_ctx && hwAccelCtx.video_dec_ctx->hw_frames_ctx) {
                AVHWFramesContext* dec_frames = (AVHWFramesContext*)hwAccelCtx.video_dec_ctx->hw_frames_ctx->data;
                transfer_fmt = dec_frames->sw_format;
            }

            tmp_hw_sw->format = transfer_fmt;
            tmp_hw_sw->width  = frame->width;
            tmp_hw_sw->height = frame->height;
            if (av_frame_get_buffer(tmp_hw_sw, 32) < 0) {
                LOG(LOG_ERROR, "failed to alloc temp_frame buffer");
                return ZET_ERR_MALLOC;
            }
            int tr = av_hwframe_transfer_data(tmp_hw_sw, frame, 0);
            if (tr < 0) {
                LOG(LOG_ERROR, "HW->SW transfer failed: %d", tr);
                return ZET_ERR_MALLOC;
            }
            sws_scale(*sws_ctx, (const uint8_t* const*)tmp_hw_sw->data, tmp_hw_sw->linesize, 0, tmp_hw_sw->height,
                      processed_frame->data, processed_frame->linesize);
        } else {
            sws_scale(*sws_ctx, frame->data, frame->linesize, 0, frame->height,
                      processed_frame->data, processed_frame->linesize);
        }
        processed_frame->pts      = frame->pts;
        processed_frame->pkt_dts  = frame->pkt_dts;
        processed_frame->duration = frame->duration;
        *target_frame             = processed_frame;
    }
    return ZET_OK;
}

void zetHlsServerMdl::processArmHwUpload(int is_video,  HWAccelCtx* hwAccelCtx, AVCodecContext* out_codec_ctx, 
                                                     AVFrame** target_frame, AVFrame** enc_hw_pool)
{
    AVFrame* enc_hw      = NULL;
    if (is_video && hwAccelCtx->hwEncenabled && out_codec_ctx->hw_frames_ctx && *target_frame &&
        (*target_frame)->format != hwAccelCtx->hw_pix_fmt) { // SW frame (NV12 in CPU memory), especially when rga process failed!!!
        // Allocate pool frame once on first use
        if (!*enc_hw_pool) {
            *enc_hw_pool = av_frame_alloc();
            if (!*enc_hw_pool) {
                LOG(LOG_ERROR, "Failed to allocate enc_hw_pool frame");
            }
        }
        if (*enc_hw_pool) {
            // Reuse the pooled frame: unref old data, get new buffer
            av_frame_unref(*enc_hw_pool);
            if (av_hwframe_get_buffer(out_codec_ctx->hw_frames_ctx, *enc_hw_pool, 0) == 0) {
                int tr = av_hwframe_transfer_data(*enc_hw_pool, *target_frame, 0);
                if (tr == 0) {
                    (*enc_hw_pool)->pts      = (*target_frame)->pts;
                    (*enc_hw_pool)->pkt_dts  = (*target_frame)->pkt_dts;
                    (*enc_hw_pool)->duration = (*target_frame)->duration;
                    enc_hw                   = *enc_hw_pool;  // Point to pooled frame
                    *target_frame            = enc_hw;
                } else {
                    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                    av_strerror(tr, errbuf, sizeof(errbuf));
                    LOG(LOG_ERROR, "SW->HW upload (CPU->GPU) failed: %s", errbuf);
                }
            } else {
                LOG(LOG_ERROR, "av_hwframe_get_buffer failed for enc_hw_pool");
            }
        }
    } else if (is_video && hwAccelCtx->hwEncenabled && *target_frame &&
               (*target_frame)->format == hwAccelCtx->hw_pix_fmt) {
        LOG(LOG_VERBOSE, "Zero-copy HW path: RGA output (drm_prime, GPU memory) -> HW encoder directly");
    } else if (is_video && !hwAccelCtx->hwEncenabled && *target_frame) { // Software encoding path: RGA SW-out (NV12, CPU memory) -> SW encoder directly
        LOG(LOG_VERBOSE, "SW encoding path: RGA output (format=%s, CPU memory) -> SW encoder directly",
                          av_get_pix_fmt_name((AVPixelFormat)(*target_frame)->format));
    }
}

#endif
// Profiling accumulators (seconds)
double prof_video_decode_time = 0.0;  // avcodec_receive_frame for video
double prof_video_hw_time     = 0.0;  // VAAPI/RGA hardware scaling
double prof_video_encode_time = 0.0;  // video encode send_frame/receive_packet
double prof_audio_time        = 0.0;  // full audio chain: decode+resample+AAC agg+encode
double prof_mux_time          = 0.0;  // all av_interleaved_write_frame calls

bool zetHlsServerMdl::DirectWriteToHLS(bool is_video, bool input_is_mpegts,
                                              AVPacket* pkt, AVStream* in_stream, AVStream* out_stream,
                                              bool hdmv_multi_video, INT64& last_video_dts, INT64& last_video_pts,
                                              AVFormatContext* out_fmt_ctx, double& prof_mux_time) {
                                          
    if (!is_video || !input_is_mpegts) {
        static INT64 fisrt_audio_pts_normal = pkt->pts;
        if (!is_video/* && input_is_mpegts && is_bd_iso_source &&
            pkt->pts != AV_NOPTS_VALUE*/ && fisrt_audio_pts_normal != 0) {
            if (directcopy_audio_base_pts == AV_NOPTS_VALUE) {
                directcopy_audio_base_pts = pkt->pts;
                LOG(LOG_INFO,
                    "BD ISO audio direct-copy: set base PTS=%ld (time_base=%d/%d)",
                    (long)directcopy_audio_base_pts,
                    in_stream->time_base.num, in_stream->time_base.den);
            }
            INT64 adj_pts = pkt->pts - directcopy_audio_base_pts;
            INT64 adj_dts = (pkt->dts != AV_NOPTS_VALUE)
                              ? (pkt->dts - directcopy_audio_base_pts)
                              : adj_pts;
            if (adj_pts < 0) adj_pts = 0;
            if (adj_dts < 0) adj_dts = 0;
            pkt->pts = adj_pts;
            pkt->dts = adj_dts;
        }
        LOG(LOG_DEBUG, "no resample, %s pts: %ld, dts: %ld (in_tb=%d/%d)",
                       is_video ? "video" : "audio",
                       (long)pkt->pts, (long)pkt->dts,
                       in_stream->time_base.num, in_stream->time_base.den);
        av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
    }

    if (is_video && input_is_mpegts) {
        if (hdmv_multi_video) {
            av_packet_rescale_ts(pkt, in_stream->time_base, out_stream->time_base);
    	} else {
            static INT64 copy_video_step  = 0;
            if (copy_video_step == 0) {
                AVRational fr = {0, 1};
                if (in_stream->avg_frame_rate.num > 0 && in_stream->avg_frame_rate.den > 0) {
                    fr = in_stream->avg_frame_rate;
                } else if (in_stream->r_frame_rate.num > 0 && in_stream->r_frame_rate.den > 0) {
                    fr = in_stream->r_frame_rate;
                } else {
                    fr = (AVRational){25, 1}; // fallback
                }
                copy_video_step = av_rescale_q(1, av_inv_q(fr), out_stream->time_base);
                if (copy_video_step <= 0) {
                    copy_video_step = 1; // safety fallback
                }
            }
            if (last_video_dts == AV_NOPTS_VALUE) {
                pkt->dts = 0;
                pkt->pts = 0;
            } else {
                pkt->dts = last_video_dts + copy_video_step;
                pkt->pts = last_video_pts + copy_video_step;
            }
            last_video_dts = pkt->dts;
            last_video_pts = pkt->pts;
       }
    }

    pkt->stream_index     = out_stream->index;
    double prof_mux_start = zet_prof_now_sec();
    int    write_ret      = av_interleaved_write_frame(out_fmt_ctx, pkt);
    prof_mux_time        += (zet_prof_now_sec() - prof_mux_start);
    if (write_ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(write_ret, errbuf, sizeof(errbuf));
        LOG(LOG_ERROR, "%s direct-copy write failed, pts: %lld, dts: %lld, ret=%d (%s)",
                        is_video ? "Video" : "Audio", (long long)pkt->pts, (long long)pkt->dts, write_ret, errbuf);
    }
    av_packet_unref(pkt);
    return ZET_TRUE;
}

INT32 zetHlsServerMdl::processAudioResample(AVFrame* processed_frame, AVFrame* frame, AVCodecContext* out_codec_ctx, SwrContext* swr_ctx,
                                                     bool audio_input_is_8k, AVFrame*& aac_frame, AVPacket*& aac_out_pkt, AVStream* out_audio_stream,
                                                     AVFormatContext* out_fmt_ctx, std::atomic<bool>& process_stop_requested, INT64& last_audio_pts,
                                                     INT64& last_audio_dts, double& prof_audio_time, double prof_audio_start, int& errNo,
                                                     INT64& audio8k_in_samples_total, double& prof_mux_time) {
    av_frame_unref(processed_frame);
    processed_frame->sample_rate = out_codec_ctx->sample_rate;
    processed_frame->format      = out_codec_ctx->sample_fmt;
    if (audio_input_is_8k && frame && frame->nb_samples > 0) {
        audio8k_in_samples_total += frame->nb_samples;
    }

    av_channel_layout_copy(&processed_frame->ch_layout, &out_codec_ctx->ch_layout);
    int channels       = processed_frame->ch_layout.nb_channels;
    int frame_size     = out_codec_ctx->frame_size > 0 ? out_codec_ctx->frame_size : 1024;
    int sample_size    = av_get_bytes_per_sample(out_codec_ctx->sample_fmt);

    if (frame->nb_samples <= 0) {
        LOG(LOG_ERROR, "Invalid input audio frame samples: %d", frame->nb_samples);
        av_frame_unref(frame);
        return ZET_ERR_CONTINUE;
    }

    INT64 delay          = swr_get_delay(swr_ctx, frame->sample_rate);
    INT64 dst_nb_samples = av_rescale_rnd(delay + frame->nb_samples, out_codec_ctx->sample_rate, frame->sample_rate, AV_ROUND_UP);
    if (dst_nb_samples <= 0) {
        LOG(LOG_ERROR, "Invalid dst_nb_samples after rescale: %ld (delay=%ld, in_nb_samples=%d)",
                        dst_nb_samples, delay, frame->nb_samples);
        av_frame_unref(processed_frame);
        return ZET_ERR_CONTINUE;
    }

    if (dst_nb_samples < frame_size) dst_nb_samples = frame_size;

    if (audio_input_is_8k) {
        if (dst_nb_samples > out_codec_ctx->sample_rate) {
            dst_nb_samples = out_codec_ctx->sample_rate;
        }
    } else {
        if (dst_nb_samples > frame_size * 4) {
            dst_nb_samples = frame_size * 4;
        }
    }
    processed_frame->nb_samples = (int)dst_nb_samples;

    if (av_frame_get_buffer(processed_frame, 32) < 0) {
        LOG(LOG_ERROR, "Failed to allocate audio frame buffer");
        av_frame_unref(processed_frame);
        return ZET_ERR_CONTINUE;
    }

    int ret = swr_convert(swr_ctx, processed_frame->data, processed_frame->nb_samples, (const uint8_t **)frame->data, frame->nb_samples);
    if (ret < 0) {
        LOG(LOG_ERROR, "swr_convert failed: %d", ret);
        av_frame_unref(processed_frame);
        return ZET_ERR_CONTINUE;
    }
    if (ret == 0) {
        av_frame_unref(processed_frame);
        return ZET_ERR_CONTINUE;
    }
    processed_frame->nb_samples = ret;
    if (processed_frame->format != out_codec_ctx->sample_fmt) {
        LOG(LOG_ERROR, "Sample format mismatch after resample: frame %d vs encoder %d",
                        processed_frame->format, out_codec_ctx->sample_fmt);
        return ZET_FALSE;
    }

    if (out_codec_ctx->codec_id == AV_CODEC_ID_AAC) {
        static std::vector<uint8_t> aac_residual_buffer;// Ring buffer indices in samples (per channel): [head, tail)
        static INT64 aac_residual_head    = 0;
        static INT64 aac_residual_tail    = 0;
        static INT64 aac_residual_samples = 0;
        static INT64 aac_pts_samples      = 0;

        if (channels <= 0 || sample_size <= 0) {
            LOG(LOG_ERROR, "Invalid AAC encoder layout: channels=%d, sample_size=%d", channels, sample_size);
            av_frame_unref(processed_frame);
            return ZET_ERR_CONTINUE;
        }
        // Append current resampled frame (planar) into an interleaved residual buffer: [s0ch0][s0ch1]...[s1ch0]...
        INT64 append_samples = ret;
        if (append_samples <= 0) {
            av_frame_unref(processed_frame);
            prof_audio_time += (zet_prof_now_sec() - prof_audio_start);
            return ZET_ERR_CONTINUE;
        }
        INT64  needed_tail_samples = aac_residual_tail + append_samples;
        size_t needed_bytes        = (size_t)needed_tail_samples * channels * sample_size;
        if (aac_residual_buffer.size() < needed_bytes) {
            aac_residual_buffer.resize(needed_bytes);
        }

        for (INT64 i = 0; i < append_samples; i++) {
            for (int ch = 0; ch < channels; ch++) {
                uint8_t *dst = aac_residual_buffer.data() +
                               ((aac_residual_tail + i) * channels + ch) * sample_size;
                const uint8_t *src = processed_frame->data[ch] + i * sample_size;
                memcpy(dst, src, sample_size);
            }
        }
        aac_residual_tail    += append_samples;
        aac_residual_samples  = aac_residual_tail - aac_residual_head;
        INT64 processed_samples = 0;// how many samples from residual have been consumed this round

        if (!aac_frame) {
            aac_frame = av_frame_alloc();
            if (!aac_frame) {
                LOG(LOG_ERROR, "Failed to allocate AAC frame");
                av_frame_unref(processed_frame);
                prof_audio_time += (zet_prof_now_sec() - prof_audio_start);
                return ZET_ERR_CONTINUE;
            }
            aac_frame->format      = out_codec_ctx->sample_fmt;
            aac_frame->sample_rate = out_codec_ctx->sample_rate;
            aac_frame->nb_samples  = frame_size;
            av_channel_layout_copy(&aac_frame->ch_layout, &out_codec_ctx->ch_layout);
            if (av_frame_get_buffer(aac_frame, 0) < 0) {
                LOG(LOG_ERROR, "Failed to allocate AAC frame buffer");
                av_frame_free(&aac_frame);
                av_frame_unref(processed_frame);
                prof_audio_time += (zet_prof_now_sec() - prof_audio_start);
                return ZET_ERR_CONTINUE;
            }
        }

        while (aac_residual_samples >= frame_size) { // TODO: consider the last audio frame processing in the future
            if (av_frame_make_writable(aac_frame) < 0) {
                LOG(LOG_ERROR, "Failed to make AAC frame writable");
                break;
            }

            for (int i = 0; i < frame_size; i++) {
                INT64 sample_index = aac_residual_head + processed_samples + i;
                for (int ch = 0; ch < channels; ch++) {
                    const uint8_t *src = aac_residual_buffer.data() + (sample_index * channels + ch) * sample_size;
                    uint8_t *dst       = aac_frame->data[ch] + i * sample_size;
                    memcpy(dst, src, sample_size);
                }
            }
			// PTS in samples, using encoder time_base = 1/sample_rate
            aac_frame->pts   = aac_pts_samples;
            aac_pts_samples += frame_size;
            int send_ret     = avcodec_send_frame(out_codec_ctx, aac_frame);
            if (send_ret < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(send_ret, errbuf, sizeof(errbuf));
                LOG(LOG_ERROR, "Audio (AAC) encode failed when sending aggregated frame, nb_samples: %d, fmt: %d, sample_rate: %d, err: %s",
                                 aac_frame->nb_samples, aac_frame->format, aac_frame->sample_rate, errbuf);
                break;
            }
            if (!aac_out_pkt) {
                aac_out_pkt = av_packet_alloc();
                if (!aac_out_pkt) {
                    LOG(LOG_ERROR, "Failed to allocate audio output packet");
                    break;
                }
            }
            while ((errNo = avcodec_receive_packet(out_codec_ctx, aac_out_pkt)) >= 0 && !process_stop_requested.load()) {
                aac_out_pkt->stream_index = out_audio_stream->index;
                if (aac_out_pkt->pts == AV_NOPTS_VALUE) {
                    if (last_audio_pts == AV_NOPTS_VALUE) {
                        aac_out_pkt->pts = 0;
                    } else {
                        INT64 samples_per_packet = out_codec_ctx->frame_size;
                        INT64 inc                = av_rescale_q(samples_per_packet, av_make_q(1, out_codec_ctx->sample_rate), out_codec_ctx->time_base);
                        aac_out_pkt->pts           = last_audio_pts + inc;
                    }
                    last_audio_pts = aac_out_pkt->pts;
                }
                if (aac_out_pkt->dts == AV_NOPTS_VALUE) {
                    aac_out_pkt->dts = aac_out_pkt->pts;
                }
                if (aac_out_pkt->pts < aac_out_pkt->dts) {
                    aac_out_pkt->dts = aac_out_pkt->pts;
                }
                if (aac_out_pkt->duration == AV_NOPTS_VALUE && out_codec_ctx->frame_size > 0) {
                    aac_out_pkt->duration = av_rescale_q(out_codec_ctx->frame_size, av_make_q(1, out_codec_ctx->sample_rate), out_codec_ctx->time_base);
                }

                av_packet_rescale_ts(aac_out_pkt, out_codec_ctx->time_base, out_audio_stream->time_base);

                if (aac_out_pkt->pts <= last_audio_pts) {
                    aac_out_pkt->pts = last_audio_pts + 1;
                }
                if (aac_out_pkt->dts <= last_audio_dts) {
                    aac_out_pkt->dts = last_audio_dts + 1;
                }
                last_audio_pts = aac_out_pkt->pts;
                last_audio_dts = aac_out_pkt->dts;

                double prof_mux_start = zet_prof_now_sec();
                if (!g_first_video_written && clip_duration_sec > 0.0) {
                    LOG(LOG_DEBUG, "Drop pre-roll audio packet before first video, pts=%lld dts=%lld", 
                                      (long long)aac_out_pkt->pts, (long long)aac_out_pkt->dts);
                    av_packet_unref(aac_out_pkt);
                    continue;
                }

                if (av_interleaved_write_frame(out_fmt_ctx, aac_out_pkt) < 0) {
                    LOG(LOG_ERROR, "Audio write failed after encode, please check, pts: %lld, dts: %lld",
                                    (long long)aac_out_pkt->pts, (long long)aac_out_pkt->dts);
                    av_packet_unref(aac_out_pkt);
                    break;
                }
                prof_mux_time += (zet_prof_now_sec() - prof_mux_start);
                av_packet_unref(aac_out_pkt);
            }
            processed_samples    += frame_size;
            aac_residual_samples -= frame_size;
        }
        // Advance head pointer and occasionally compact the residual buffer
        if (processed_samples > 0) {
            aac_residual_head    += processed_samples;
            aac_residual_samples  = aac_residual_tail - aac_residual_head;
            if (aac_residual_samples <= 0) {
                aac_residual_head    = 0;
                aac_residual_tail    = 0;
                aac_residual_samples = 0;
                aac_residual_buffer.clear();
            } else { // Compact only when head grows too large to reduce memmove frequency
                const INT64 compact_threshold = frame_size * 128;
                if (aac_residual_head > compact_threshold) {
                    size_t remaining_bytes = (size_t)aac_residual_samples * channels * sample_size;
                    memmove(aac_residual_buffer.data(),
                            aac_residual_buffer.data() + (size_t)aac_residual_head * channels * sample_size,
                            remaining_bytes);
                    aac_residual_head = 0;
                    aac_residual_tail = aac_residual_samples;
                    aac_residual_buffer.resize(remaining_bytes);
                }
            }
        }
        av_frame_unref(processed_frame);
        prof_audio_time += (zet_prof_now_sec() - prof_audio_start);
        return ZET_ERR_CONTINUE;
    } else {
        static INT64 encoded_audio_samples = 0;
        processed_frame->pts               = encoded_audio_samples;
        encoded_audio_samples             += ret;
        if (processed_frame->nb_samples <= 0) {
            LOG(LOG_WARNING, "Invalid sample count after resample: %d", processed_frame->nb_samples);
            av_frame_unref(processed_frame);
            return ZET_ERR_CONTINUE;
        }
        if (avcodec_send_frame(out_codec_ctx, processed_frame) < 0) {
            LOG(LOG_ERROR, "Audio encode failed for non-AAC codec");
            return ZET_ERR_CONTINUE;
        }
        return ZET_OK;
    }
    return ZET_OK;
}

 INT32 zetHlsServerMdl::adjustVideoTimestamp(bool is_interlaced, bool is_mbaff_h264_ts, bool video_needs_transcode, bool input_is_mpegts,
                                                    bool hdmv_multi_video, int64_t& interlaced_frame_index, AVFrame* frame, HWAccelCtx& hwAccelCtx,
                                                    bool use_hw_scale, int64_t& arm_rga_frame_index, AVFrame* target_frame, AVStream* in_stream,
                                                    AVCodecContext* out_codec_ctx, struct _zetHlsGenInfo* hlsGenInfo, AVStream* out_stream, int64_t last_video_pts,
                                                    std::atomic<bool>& process_stop_requested) {
#if ARM
    // ARM: only drop every second decoded frame for MBAFF H.264 TS sources.
    if (is_interlaced && is_mbaff_h264_ts && video_needs_transcode && input_is_mpegts && !hdmv_multi_video) {
        interlaced_frame_index++;
        if ((interlaced_frame_index & 1) == 0) {
            LOG(LOG_VERBOSE, "ARM MBAFF H.264 TS: dropping every second decoded frame, index=%lld",
                (long long)interlaced_frame_index);
            av_frame_unref(frame);
            return ZET_ERR_CONTINUE;
        }
    }
    // For non-TS inputs on ARM, do not override PTS when using RGA+HW encode; rely
    // on decoder PTS (rescaled below) just like x86, so that HLS segmentation sees
    // the same timeline on both architectures.
#endif
    if (input_is_mpegts) {
        static INT64 mpegts_encoded_frame_count = 0;
        if (hdmv_multi_video && target_frame->pts != AV_NOPTS_VALUE && in_stream) {
            target_frame->pts = av_rescale_q(target_frame->pts,
                                             in_stream->time_base,
                                             out_codec_ctx->time_base);
        } else {
            target_frame->pts = mpegts_encoded_frame_count++;
        }
    } else {
        // Non-TS: use decoder PTS with a simple rescale when available.
#if ARM
        if (g_arm_rga_use_index_ts && video_needs_transcode && hwAccelCtx.hwEncenabled && !hwAccelCtx.hwDecenabled &&
            use_hw_scale && !input_is_mpegts) {
            // ARM: for SW-decode + RGA + HW-encode on MEncoder-generated sources,
            // use a local frame index to drive a clean, strictly monotonic
            // timeline. Other sources keep decoder-based PTS to match x86
            // behavior and avoid the 7s tail segment issue.
            target_frame->pts = arm_rga_frame_index++;
        } else
#endif
        {
            if (target_frame->pts != AV_NOPTS_VALUE && target_frame->pts > 0 && in_stream) {
                target_frame->pts = av_rescale_q(target_frame->pts, in_stream->time_base, out_codec_ctx->time_base);
            } else {
                static INT64 encoded_frame_count = 0;
                target_frame->pts = encoded_frame_count++;
            }
        }
    }
    return ZET_OK;
}

INT32 zetHlsServerMdl::getMediaMsg(AVFormatContext* in_fmt_ctx, AVStream* in_video_stream, AVStream* in_audio_stream,
                                    AVCodecID curVideoCodecID, AVCodecID curAudioCodecID, double file_duration_sec) {

    if (!in_video_stream || !in_audio_stream) {
        LOG(LOG_ERROR, "ffmpeg transcode probe failed due to null ptr, in_video_stream: %p, in_audio_stream: %p",
                        in_video_stream, in_audio_stream);
        return ZET_NOK;
    }

    int width   = in_video_stream->codecpar->width;
    int height  = in_video_stream->codecpar->height;
    int bitrate = in_video_stream->codecpar->bit_rate;

    AVRational frameRate;

    struct stat file_stat;
    int fileSize = 0;
    if (!stat(in_fmt_ctx->url, &file_stat)) {
        fileSize= file_stat.st_size;
    }

    if (in_video_stream->codecpar->bit_rate > 0) {
        bitrate = in_video_stream->codecpar->bit_rate;
    } else if (in_fmt_ctx && in_fmt_ctx->bit_rate > 0 && in_audio_stream->codecpar->bit_rate > 0) {
        bitrate = in_fmt_ctx->bit_rate - in_audio_stream->codecpar->bit_rate;
        bitrate = bitrate < 0 ? 0 : bitrate;
    }

    if (in_video_stream->r_frame_rate.num > 0 && in_video_stream->r_frame_rate.den > 0) {
        frameRate = in_video_stream->r_frame_rate;
    } else if (in_video_stream->avg_frame_rate.num > 0 && in_video_stream->avg_frame_rate.den > 0) {
        frameRate = in_video_stream->avg_frame_rate;
    } else {
        frameRate = (AVRational){25, 1};
    }

    int audSampleRate = in_audio_stream->codecpar->sample_rate;
    int audBitrate    = in_audio_stream->codecpar->bit_rate;
    int channels      = in_audio_stream->codecpar->ch_layout.nb_channels;

    LOG(LOG_ERROR, "ffmpeg transcode probe successfully, fileSize : %.3f Mb, file durtion: %.3fs, Video msg: "
                    "width: %d, heght: %d, bitrate: %.3f Mbps, frameRate:  %.3fFPS, codec name: %s, "
                    "Audio msg: audSampleRate: %d khz,  audBitrate: %d kb, channels :%d, codec name: %s",
                    double(fileSize/(1024*1024)), file_duration_sec, width, height, static_cast<double>(bitrate) / 1000000.0, 
                    double(frameRate.num/frameRate.den), avcodec_get_name(curVideoCodecID),
                    audSampleRate, audBitrate/1000, channels, avcodec_get_name(curAudioCodecID));
    return ZET_OK;
}

INT32 zetHlsServerMdl::processWithApi(void* msg) {
    ZETCHECK_PTR_IS_NULL(hlsGenInfo);
    avformat_network_init();
    this->bindCurrentInstance((void*)0x123);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = zetHlsServerMdl::signalHandlerForward;
    sa.sa_flags     = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1 || sigaction(SIGQUIT, &sa, NULL) == -1) {
        LOG(LOG_ERROR, "Failed to set signal handler");
        this->bindCurrentInstance(NULL);
        return ZET_NOK;
    }

    process_stop_requested.store(ZET_FALSE, std::memory_order_relaxed);

    clip_start_pts            = AV_NOPTS_VALUE;
    clip_duration_sec         = (hlsGenInfo->seekEndTime > hlsGenInfo->seekTime && hlsGenInfo->seekEndTime > 0.0)
                                  ? (hlsGenInfo->seekEndTime - hlsGenInfo->seekTime) : 0.0;
    base_video_pts            = AV_NOPTS_VALUE;
    bool stop_due_to_clip     = ZET_FALSE;
    g_first_video_written     = ZET_FALSE;
    g_first_video_pts         = AV_NOPTS_VALUE;
    g_first_video_time_sec    = 0.0;
    g_first_video_time_valid  = ZET_FALSE;

    last_video_pts            = AV_NOPTS_VALUE;
    last_video_dts            = AV_NOPTS_VALUE;
    last_audio_pts            = AV_NOPTS_VALUE;
    last_audio_dts            = AV_NOPTS_VALUE;

    is_bd_iso_source          = ZET_FALSE;
    directcopy_audio_base_pts = AV_NOPTS_VALUE;

    AVFormatContext* in_fmt_ctx = NULL;
    AVDictionary*    fmt_opts   = NULL;
    av_dict_set(&fmt_opts, "probesize", "50000000", 0);
    av_dict_set(&fmt_opts, "analyzeduration", "3000000", 0);

    // Initialize FFmpeg log callback once
    static bool ffmpeg_log_inited = ZET_FALSE;
    if (!ffmpeg_log_inited) {
        av_log_set_level(AV_LOG_INFO);
        av_log_set_callback(zet_ffmpeg_log_callback);
        // av_log_set_level(AV_LOG_INFO);        
        ffmpeg_log_inited = true;
    }

    if (avformat_open_input(&in_fmt_ctx, hlsGenInfo->file_input, NULL, &fmt_opts) < 0) {
        LOG(LOG_ERROR, "ffmpeg transcode start failed, due to unable to open input url %s", hlsGenInfo->file_input);
        av_dict_free(&fmt_opts);
        freeResources(&in_fmt_ctx, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        return ZET_NOK;
    }
    av_dict_free(&fmt_opts);
    av_dump_format(in_fmt_ctx, 0, hlsGenInfo->file_input, 0);

    int primary_video_idx = -1;
    for (unsigned int i = 0; i < in_fmt_ctx->nb_streams; i++) {
        if (in_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            if (primary_video_idx < 0) {
                primary_video_idx = i;  // Keep first video stream
            } else {
                in_fmt_ctx->streams[i]->discard = AVDISCARD_ALL;
                LOG(LOG_INFO, "Pre-disabled extra video stream %d to prevent SPS conflicts", i);
            }
        } else if (in_fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            in_fmt_ctx->streams[i]->discard = AVDISCARD_ALL;
            LOG(LOG_DEBUG, "Pre-disabled subtitle stream %d (codec=%s) to avoid unnecessary probing",
                           i, avcodec_get_name(in_fmt_ctx->streams[i]->codecpar->codec_id));
        }
    }

    if (avformat_find_stream_info(in_fmt_ctx, NULL) < 0 || in_fmt_ctx->nb_streams <= 0) {
        LOG(LOG_ERROR, "ffmpeg transcode start failed, due to unable to get stream info, nb_streams=%d", in_fmt_ctx->nb_streams);
        freeResources(&in_fmt_ctx, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        return ZET_NOK;
    }

#if ARM
    g_arm_rga_use_index_ts = ZET_FALSE;
    AVDictionaryEntry* sw_tag = av_dict_get(in_fmt_ctx->metadata, "software", NULL, 0);
    if (sw_tag && sw_tag->value && strstr(sw_tag->value, "MEncoder") != NULL) {
        LOG(LOG_INFO, "ARM: detected MEncoder-generated source (%s), enabling RGA index-based PTS mode",
                       sw_tag->value);
        g_arm_rga_use_index_ts = ZET_TRUE;
    }
#endif

    int video_stream_idx = findBestStream(in_fmt_ctx, AVMEDIA_TYPE_VIDEO, "Video");
    if (video_stream_idx < 0) {
        LOG(LOG_ERROR, "ffmpeg transcode start failed, due to video stream idx is invalid, please check");
        freeResources(&in_fmt_ctx, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
        return ZET_NOK;
    }
    int audio_stream_idx = -1;
    if (hlsGenInfo->audIndex == -1) {
        LOG(LOG_DEBUG, "not select specified audio index, use default index");
        audio_stream_idx = findBestStream(in_fmt_ctx, AVMEDIA_TYPE_AUDIO, "Audio");
    } else {
        LOG(LOG_INFO, "Use audio stream index parsed from -map parameter: %d", hlsGenInfo->audIndex);
		std::vector<int> audio_global_indices;
		for (unsigned int i = 0; i < in_fmt_ctx->nb_streams; ++i) {
            AVStream* stream = in_fmt_ctx->streams[i];
            if (stream && stream->codecpar && stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_global_indices.push_back(i);   
                LOG(LOG_DEBUG, "Collect audio stream: global index=%d, codec=%s", i, avcodec_get_name(stream->codecpar->codec_id));
            }
        }
        if (audio_global_indices.empty()) {
            LOG(LOG_ERROR, "No valid audio stream found in the input file");
            audio_stream_idx = findBestStream(in_fmt_ctx, AVMEDIA_TYPE_AUDIO, "Audio");
        } else if (hlsGenInfo->audIndex >= (int)audio_global_indices.size()) {
            LOG(LOG_ERROR, "Parsed audio stream type sequence %d is out of range (total audio streams: %d)",
                            hlsGenInfo->audIndex, (int)audio_global_indices.size());
            audio_stream_idx = findBestStream(in_fmt_ctx, AVMEDIA_TYPE_AUDIO, "Audio");
        } else { 
            audio_stream_idx              = audio_global_indices[hlsGenInfo->audIndex];
            AVStream* target_audio_stream = in_fmt_ctx->streams[audio_stream_idx];
            if (target_audio_stream && target_audio_stream->codecpar && target_audio_stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                LOG(LOG_INFO, "Successfully select audio stream: type sequence=%d, global index=%d, codec=%s",
                               hlsGenInfo->audIndex, audio_stream_idx, avcodec_get_name(target_audio_stream->codecpar->codec_id)); 
            } else {
                LOG(LOG_ERROR, "Mapped audio stream global index %d is not a valid audio stream", audio_stream_idx);
                audio_stream_idx = findBestStream(in_fmt_ctx, AVMEDIA_TYPE_AUDIO, "Audio");
            }
        }
    }
    double    file_duration_sec = getMediaFileDuration(in_fmt_ctx, video_stream_idx);
    AVStream* in_video_stream	= in_fmt_ctx->streams[video_stream_idx];
    AVStream* in_audio_stream	= (audio_stream_idx >= 0) ? in_fmt_ctx->streams[audio_stream_idx] : NULL;
    LOG(LOG_DEBUG, "Now Primary video stream: %d, total streams: %d, get file duartion: %f, in_video_stream: %p, \
    			   in_audio_stream : %p, video_stream_idx: %d, audio_stream_idx: %d", 
    			   video_stream_idx, in_fmt_ctx->nb_streams, file_duration_sec, in_video_stream, in_audio_stream, video_stream_idx, audio_stream_idx);

    is_bd_iso_source = ZET_FALSE;
    if (in_fmt_ctx && in_fmt_ctx->iformat && !strcmp(in_fmt_ctx->iformat->name, "mpegts") &&
        in_video_stream && in_video_stream->codecpar &&
        in_video_stream->codecpar->codec_id == AV_CODEC_ID_H264 &&
        in_video_stream->codecpar->codec_tag == MKTAG('H','D','M','V') &&
        hlsGenInfo && hlsGenInfo->file_input[0] != '\0') {
        const char *in_path = hlsGenInfo->file_input;
        const char *dot     = strrchr(in_path, '.');
        if (dot && (!strcasecmp(dot, ".iso"))) {
            is_bd_iso_source = ZET_TRUE;
            LOG(LOG_INFO,
                "Detected Blu-ray ISO HDMV H.264 source, enabling ISO-specific clip duration guard, file_duration=%.3f",
                file_duration_sec);
        }
    }

    if (is_bd_iso_source && file_duration_sec > 0.0 &&
        hlsGenInfo->seekTime <= 0.0 && (hlsGenInfo->seekEndTime <= 0.0 || hlsGenInfo->seekEndTime <= hlsGenInfo->seekTime)) {
        clip_duration_sec = file_duration_sec;
        LOG(LOG_INFO,
            "Blu-ray ISO guard: set clip_duration_sec to file_duration_sec=%.3f (seekTime=%.3f, seekEndTime=%.3f)",
            clip_duration_sec, hlsGenInfo->seekTime, hlsGenInfo->seekEndTime);
    }

    bool    is_interlaced          = ZET_FALSE;
    INT64   interlaced_frame_index = 0;
    if (in_video_stream && in_video_stream->codecpar && in_video_stream->codecpar->field_order != AV_FIELD_PROGRESSIVE &&
                        in_video_stream->codecpar->field_order != AV_FIELD_UNKNOWN) {
        is_interlaced = ZET_TRUE;
        LOG(LOG_INFO, "check frame is_interlaced, field_order: %d", in_video_stream->codecpar->field_order);
    }

    bool is_mbaff_h264_ts = ZET_FALSE;
    if (is_interlaced && in_video_stream && in_video_stream->codecpar &&
        in_video_stream->codecpar->codec_id == AV_CODEC_ID_H264 &&
        in_fmt_ctx->iformat && !strcmp(in_fmt_ctx->iformat->name, "mpegts")) {
        is_mbaff_h264_ts = isH264StreamMbaff(in_video_stream->codecpar);
        LOG(LOG_INFO, "MBAFF H.264 TS detection: %d", (int)is_mbaff_h264_ts);
    }

    bool hdmv_multi_video = ZET_FALSE;
    if (in_fmt_ctx && in_fmt_ctx->iformat && !strcmp(in_fmt_ctx->iformat->name, "mpegts")) {
        int hdmv_video_count = 0;
        for (unsigned int si = 0; si < in_fmt_ctx->nb_streams; ++si) {
            AVStream *st = in_fmt_ctx->streams[si];
            if (!st || !st->codecpar) {
                continue;
            }
            AVCodecParameters *cp = st->codecpar;
            if (cp->codec_type == AVMEDIA_TYPE_VIDEO && cp->codec_id   == AV_CODEC_ID_H264 && cp->codec_tag  == MKTAG('H','D','M','V')) {
                hdmv_video_count++;
            }
        }
        hdmv_multi_video = (hdmv_video_count > 1);
        LOG(LOG_INFO, "HDMV H.264 TS video stream count: %d, hdmv_multi_video: %d", hdmv_video_count, (int)hdmv_multi_video);
    }

    bool   audio_input_is_8k = ZET_FALSE;
    if (in_audio_stream /*&& in_audio_stream->codecpar->codec_id == AV_CODEC_ID_AAC */&& in_audio_stream->codecpar->sample_rate == 8000) {
        audio_input_is_8k = ZET_TRUE;
        LOG(LOG_INFO, "Input audio is AAC 8kHz, will use special AAC resample path");
    }

    curVideoCodecID            = findTargetCodec(hlsGenInfo, ZET_TRUE);
    curAudioCodecID            = findTargetCodec(hlsGenInfo, ZET_FALSE);
    bool video_needs_transcode = isNeedTranscode(in_video_stream->codecpar, hlsGenInfo, curVideoCodecID);
    bool audio_needs_transcode = (in_audio_stream != NULL) && isNeedTranscode(in_audio_stream->codecpar, hlsGenInfo, curAudioCodecID);

    AVCodecContext* in_video_ctx = NULL;
    AVCodecContext* in_audio_ctx = NULL;

    if (video_needs_transcode) {
        if (initHWDecoder(in_fmt_ctx, in_video_stream, in_audio_ctx, hwAccelCtx) != ZET_OK) {
            LOG(LOG_INFO, "init hw decoder failed, Now switch to use software decoder...");
            hwAccelCtx.hwDecenabled = ZET_FALSE;
            const AVCodec* in_video_codec = avcodec_find_decoder(in_video_stream->codecpar->codec_id);
            in_video_ctx                  = avcodec_alloc_context3(in_video_codec);
            avcodec_parameters_to_context(in_video_ctx, in_video_stream->codecpar);
            if (in_video_stream->codecpar->codec_id == AV_CODEC_ID_AV1) {
                LOG(LOG_INFO, "av1 codec found!!!");
                in_video_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
            }
            if (in_video_ctx) {
                zet_configure_decoder_threads(in_video_ctx, hlsGenInfo);
            }
            if (avcodec_open2(in_video_ctx, in_video_codec, NULL) < 0) {
                LOG(LOG_ERROR, "ffmpeg transcode start failed, due to unable to open viodeo decoder, please check");
                freeResources(&in_fmt_ctx, NULL, &in_video_ctx, &in_audio_ctx, NULL, NULL, NULL, NULL, NULL, NULL);
                return ZET_NOK;
            }
        }
    }

    if (audio_needs_transcode && in_audio_stream) {
        const AVCodec* in_audio_codec = avcodec_find_decoder(in_audio_stream->codecpar->codec_id);
        in_audio_ctx                  = avcodec_alloc_context3(in_audio_codec);
        avcodec_parameters_to_context(in_audio_ctx, in_audio_stream->codecpar);
        if (avcodec_open2(in_audio_ctx, in_audio_codec, NULL) < 0) {
            LOG(LOG_ERROR, "ffmpeg transcode start failed, due to unable to open viodeo decoder, please check");
            freeResources(&in_fmt_ctx, NULL, &in_video_ctx, &in_audio_ctx, NULL, NULL, NULL, NULL, NULL, NULL);
            return ZET_NOK;
        }
    }

    if (hlsGenInfo->need_probe) {
        getMediaMsg(in_fmt_ctx, in_video_stream, in_audio_stream, curVideoCodecID, curAudioCodecID, file_duration_sec);
        // Release all FFmpeg contexts before returning to avoid leaks on probe-only runs.
        freeResources(&in_fmt_ctx, NULL, &in_video_ctx, &in_audio_ctx, NULL, NULL, NULL, NULL, NULL, NULL);
        return ZET_OK;
    }

    // check seek command before transcode
    if (hlsGenInfo->seekTime > 0.0) {
        int64_t seek_ts = (int64_t)(hlsGenInfo->seekTime /av_q2d(in_video_stream->time_base));
        if (avformat_seek_file(in_fmt_ctx, video_stream_idx, INT64_MIN, seek_ts, INT64_MAX, AVSEEK_FLAG_BACKWARD) < 0) {
            LOG(LOG_WARNING, "Initial seek failed");
        }
        if (in_video_ctx) avcodec_flush_buffers(in_video_ctx);
        if (in_audio_ctx) avcodec_flush_buffers(in_audio_ctx);
    }

    AVCodecContext* out_video_ctx = NULL;
    AVCodecContext* out_audio_ctx = NULL;
    SwsContext*     sws_ctx       = NULL;
    SwrContext*     swr_ctx       = NULL;

    // Static resource cleanup tracking
    static bool static_resources_allocated = false;
    if (video_needs_transcode) {
        in_video_ctx  = hwAccelCtx.hwDecenabled ? hwAccelCtx.video_dec_ctx : in_video_ctx;
        if (initHWEncoder(hlsGenInfo, in_fmt_ctx, in_video_stream, in_video_ctx, in_audio_ctx, hwAccelCtx, curVideoCodecID) == ZET_OK) {
            LOG(LOG_INFO, "init hw encoder successfully, hwAccelCtx width: %d, height: %d, pix_fmt: %s, hwDecenabled: %d, hwencEnabled: %d", 
                           hwAccelCtx.video_enc_ctx->width, hwAccelCtx.video_enc_ctx->height,
                           av_get_pix_fmt_name(hwAccelCtx.video_enc_ctx->pix_fmt), hwAccelCtx.hwDecenabled, hwAccelCtx.hwEncenabled);

            out_video_ctx = hwAccelCtx.video_enc_ctx;
            AVPixelFormat src_pix_fmt;
            AVPixelFormat dst_pix_fmt;
#if X86_64
            src_pix_fmt = hwAccelCtx.hwDecenabled ? AV_PIX_FMT_YUV420P : in_video_ctx->pix_fmt;
            dst_pix_fmt = AV_PIX_FMT_YUV420P;
#elif ARM
            // ARM: Handle both 8-bit and 10-bit formats
            if (hwAccelCtx.hwDecenabled && hwAccelCtx.video_dec_ctx && hwAccelCtx.video_dec_ctx->hw_frames_ctx) {
                AVHWFramesContext* dec_frames = (AVHWFramesContext*)hwAccelCtx.video_dec_ctx->hw_frames_ctx->data;
                src_pix_fmt = dec_frames->sw_format;  // Use actual decoder sw_format (may be P010LE for 10-bit)
                LOG(LOG_DEBUG, "Using decoder sw_format: %s for sws_ctx", av_get_pix_fmt_name(src_pix_fmt));
            } else {
                src_pix_fmt = in_video_ctx->pix_fmt;
            }
            dst_pix_fmt = (hwAccelCtx.hwEncenabled && out_video_ctx == hwAccelCtx.video_enc_ctx) ? AV_PIX_FMT_NV12 : out_video_ctx->pix_fmt;
#endif
            sws_ctx = sws_getContext(in_video_ctx->width, in_video_ctx->height, src_pix_fmt,
                                     out_video_ctx->width, out_video_ctx->height, dst_pix_fmt,
                                     SWS_BICUBIC, NULL, NULL, NULL);
            if (!sws_ctx) {
                LOG(LOG_ERROR, "ffmpeg transcode start failed, due to unable to init swscale context for encoder");
                freeResources(&in_fmt_ctx, NULL, &in_video_ctx, &in_audio_ctx, NULL, &out_audio_ctx, NULL, NULL, NULL, NULL);
                return ZET_NOK;
            }
        } else {
#if X86_64
            if (in_video_ctx) in_video_ctx->pix_fmt = AV_PIX_FMT_YUV420P; // need to further check again
#endif
            LOG(LOG_WARNING, "Hardware encoder initialization failed, falling back to software encoder, decenabled: %d, Encenabled: %d", hwAccelCtx.hwDecenabled, hwAccelCtx.hwEncenabled);

            out_video_ctx = initOutputCodecCtx(in_video_ctx, in_video_stream, curVideoCodecID, hlsGenInfo);
            if (!out_video_ctx) {
                LOG(LOG_ERROR, "ffmpeg transcode start failed, due to unable to init output video context");
                freeResources(&in_fmt_ctx, NULL, &in_video_ctx, &in_audio_ctx, &out_video_ctx, &out_audio_ctx, NULL, NULL, NULL, NULL);
                return ZET_NOK;
            }
            if (hlsGenInfo->width != in_video_stream->codecpar->width || hlsGenInfo->height != in_video_stream->codecpar->height) {
                sws_ctx = initScaleContext(in_video_ctx, out_video_ctx);
                if (!sws_ctx) {
                    LOG(LOG_ERROR, "ffmpeg transcode start failed, due to unable to init swscale context for software encoder");
                    freeResources(&in_fmt_ctx, NULL, &in_video_ctx, &in_audio_ctx, &out_video_ctx, &out_audio_ctx, NULL, NULL, NULL, NULL);
                    return ZET_NOK;
                }
            }
        }
    }

    if (audio_needs_transcode && in_audio_stream) {
        out_audio_ctx = initOutputCodecCtx(in_audio_ctx, in_audio_stream, curAudioCodecID, hlsGenInfo);
        if (!out_audio_ctx) {
            LOG(LOG_ERROR, "ffmpeg transcode start failed, due to unable to init output audio context");
            freeResources(&in_fmt_ctx, NULL, &in_video_ctx, &in_audio_ctx, &out_video_ctx, &out_audio_ctx, &sws_ctx, NULL, NULL, NULL);
            return ZET_NOK;
        }
        if (hlsGenInfo->sampleRate!= in_audio_stream->codecpar->sample_rate || in_audio_stream->codecpar->codec_id!= curAudioCodecID) {
            swr_ctx = initResampleContext(in_audio_ctx, out_audio_ctx);
            if (!swr_ctx) {
                LOG(LOG_ERROR, "ffmpeg transcode start failed, due to unable to init resample context");
                freeResources(&in_fmt_ctx, NULL, &in_video_ctx, &in_audio_ctx, &out_video_ctx, &out_audio_ctx, &sws_ctx, NULL, NULL, NULL);
                return ZET_NOK;
            }
        }
    }

    AVFormatContext* out_fmt_ctx = NULL;
    AVDictionary*    hls_opts    = NULL;
    std::string      hls_playlist_path;
    std::string      hls_segment_pattern;
    if (initHlsCtx(hlsGenInfo, in_fmt_ctx, in_video_ctx, in_audio_ctx,
                   out_video_ctx, out_audio_ctx, sws_ctx, swr_ctx,
                   &out_fmt_ctx, &hls_opts, hls_playlist_path, hls_segment_pattern) != ZET_OK || !out_fmt_ctx) {
        LOG(LOG_ERROR, "ffmpeg transcode start failed, due to unable to create init hls ctx!");
        return ZET_NOK;
    }

    AVStream* out_video_stream = avformat_new_stream(out_fmt_ctx, NULL);
    if (!out_video_stream) {
        LOG(LOG_ERROR, "ffmpeg transcode start failed, due to unable to create new video output stream");
        freeResources(&in_fmt_ctx, &out_fmt_ctx, &in_video_ctx, &in_audio_ctx, &out_video_ctx, &out_audio_ctx, &sws_ctx, &swr_ctx, NULL, NULL);
        return ZET_NOK;
    }
    if (video_needs_transcode) {
        if (!hwAccelCtx.hwEncenabled) {
            avcodec_parameters_from_context(out_video_stream->codecpar, out_video_ctx);
            out_video_stream->time_base = out_video_ctx->time_base;
        } else {
            avcodec_parameters_from_context(out_video_stream->codecpar, hwAccelCtx.video_enc_ctx);
            out_video_stream->time_base =  hwAccelCtx.video_enc_ctx->time_base;
        }
    } else {
        avcodec_parameters_copy(out_video_stream->codecpar, in_video_stream->codecpar);
        out_video_stream->time_base = in_video_stream->time_base;
        if (out_video_stream->codecpar->format == AV_PIX_FMT_NONE) {
            LOG(LOG_WARNING, "Input video pix_fmt %s, forced to YUV420P", av_get_pix_fmt_name((AVPixelFormat)out_video_stream->codecpar->format));
            out_video_stream->codecpar->format = AV_PIX_FMT_YUV420P;
        }
    }

    AVStream* out_audio_stream = NULL;
    if (in_audio_stream) {
        out_audio_stream = avformat_new_stream(out_fmt_ctx, NULL);
        if (!out_audio_stream) {
            LOG(LOG_ERROR, "ffmpeg transcode start failed, due to unable to create new audio output stream");
            freeResources(&in_fmt_ctx, &out_fmt_ctx, &in_video_ctx, &in_audio_ctx, &out_video_ctx, &out_audio_ctx, &sws_ctx, &swr_ctx, NULL, NULL);
            return ZET_NOK;
        }
        if (audio_needs_transcode) {
            avcodec_parameters_from_context(out_audio_stream->codecpar, out_audio_ctx);
            out_audio_stream->time_base = out_audio_ctx->time_base;
        } else {
            avcodec_parameters_copy(out_audio_stream->codecpar, in_audio_stream->codecpar);
            out_audio_stream->time_base = in_audio_stream->time_base;
        }
    }

    if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&out_fmt_ctx->pb, hls_playlist_path.c_str(), AVIO_FLAG_WRITE) < 0) {
            LOG(LOG_ERROR, "ffmpeg transcode start failed, due to unable to open output file");
            av_dict_free(&hls_opts);
            freeResources(in_fmt_ctx, out_fmt_ctx, in_video_ctx, in_audio_ctx, out_video_ctx, out_audio_ctx, sws_ctx, swr_ctx, NULL, NULL);
            return ZET_NOK;
        }
    }

	// Will adjust out video/audio timebase based on output format, for example, hls will be 1/90000
    if (avformat_write_header(out_fmt_ctx, &hls_opts) < 0) {
        LOG(LOG_ERROR, "ffmpeg transcode start failed, due to unable to write into output file");
        av_dict_free(&hls_opts);
        freeResources(&in_fmt_ctx, &out_fmt_ctx, &in_video_ctx, &in_audio_ctx, &out_video_ctx, &out_audio_ctx, &sws_ctx, &swr_ctx, NULL, NULL);
        return ZET_NOK;
    }
    av_dict_free(&hls_opts);

    AVPacket* pkt             = av_packet_alloc();
    AVFrame*  frame           = av_frame_alloc();
    AVFrame*  processed_frame = av_frame_alloc();
    AVFrame*  hw_frame;
    int       errNo;
#if X86_64
    AVFilterGraph   *va_graph        = NULL;
    AVFilterContext *va_src_ctx      = NULL;
    AVFilterContext *va_fmt_in_ctx   = NULL;
    AVFilterContext *va_upload_ctx   = NULL;
    AVFilterContext *va_scale_ctx    = NULL;
    AVFilterContext *va_download_ctx = NULL;
    AVFilterContext *va_fmt_out_ctx  = NULL;
    AVFilterContext *va_sink_ctx     = NULL;
    SwsContext      *sws_to_nv12     = NULL;
    SwsContext      *sws_nv12_to_out = NULL;
    static AVFrame  *temp_frame_pool = NULL;
    static AVFrame*  enc_hw_pool_x86 = NULL;
    bool             use_hw_scale    = ZET_FALSE;
    bool             out_hw          = ZET_FALSE;
    AVFrame         *tmp_hw_sw       = av_frame_alloc();
    initVaapiScalingGraph(video_needs_transcode, hwAccelCtx, in_video_ctx, in_video_stream,
                          out_video_ctx, va_graph, va_src_ctx, va_fmt_in_ctx, va_upload_ctx,
                          va_scale_ctx, va_download_ctx, va_fmt_out_ctx, va_sink_ctx,
                          use_hw_scale, out_hw, sws_to_nv12, sws_nv12_to_out, tmp_hw_sw);
#elif ARM
    AVFilterGraph   *rga_graph       = NULL;
    AVFilterContext *rga_src_ctx     = NULL;
    AVFilterContext *rga_fmt_in_ctx  = NULL;
    AVFilterContext *rga_upload_ctx  = NULL;
    AVFilterContext *rga_scale_ctx   = NULL;
    AVFilterContext *rga_download_ctx= NULL;
    AVFilterContext *rga_fmt_out_ctx = NULL;
    AVFilterContext *rga_sink_ctx    = NULL;
    SwsContext      *sws_to_nv12     = NULL;
    SwsContext      *sws_nv12_to_yuv = NULL;
    static AVFrame  *enc_hw_pool     = NULL;
    bool             use_hw_scale    = ZET_FALSE;
    bool             out_hw          = ZET_FALSE;
    bool             in_hw           = ZET_FALSE;
    AVFrame         *tmp_hw_sw       = av_frame_alloc();
    // Delay RGA graph init until first decoded frame so we know real input format (HW vs SW)
#endif
    INT64			 arm_rga_frame_index = 0;
    //pthread_mutex_lock(&hls_mux_mutex);
    msgType = Transcode_start; //Transcode_working;
    //pthread_mutex_unlock(&hls_mux_mutex);
    LOG(LOG_DEBUG, "ffmpeg transcode prepared already, start to work now");
    static INT64     audio8k_in_samples_total = 0;
    static AVFrame  *aac_frame                = NULL;
    static AVPacket *aac_out_pkt              = NULL;
    static AVPacket *out_pkt                  = NULL;
    while (!process_stop_requested.load(std::memory_order_relaxed)) {
        int rf_ret = av_read_frame(in_fmt_ctx, pkt);
        if (rf_ret == AVERROR(EAGAIN) || rf_ret == AVERROR(EINTR)) {
            // For complex TS/ISO inputs or non-blocking IO, EAGAIN/EINTR are transient
            // conditions; retry reading instead of treating them as fatal errors.
            const char *fmt_name = (in_fmt_ctx && in_fmt_ctx->iformat && in_fmt_ctx->iformat->name)
                                   ? in_fmt_ctx->iformat->name : "unknown";
            int64_t cur_pos = (in_fmt_ctx && in_fmt_ctx->pb) ? avio_tell(in_fmt_ctx->pb) : -1;
            LOG(LOG_WARNING,
                "av_read_frame returned %d (%s), retrying, format=%s, pb_pos=%lld",
                rf_ret,
                (rf_ret == AVERROR(EAGAIN) ? "EAGAIN" : "EINTR"),
                fmt_name,
                (long long)cur_pos);
            av_packet_unref(pkt);
            continue;
        }
        if (rf_ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(rf_ret, errbuf, sizeof(errbuf));
            int64_t cur_pos = (in_fmt_ctx && in_fmt_ctx->pb) ? avio_tell(in_fmt_ctx->pb) : -1;
            const char *fmt_name = (in_fmt_ctx && in_fmt_ctx->iformat && in_fmt_ctx->iformat->name)
                                   ? in_fmt_ctx->iformat->name : "unknown";
            LOG(LOG_ERROR, "av_read_frame end or failed, ret=%d (%s), format=%s, pb_pos=%lld",
                           rf_ret, errbuf, fmt_name, (long long)cur_pos);
            av_frame_free(&processed_frame);
            this->bindCurrentInstance(NULL);
            break;
        }

        if (pkt->stream_index != video_stream_idx && !(in_audio_stream && pkt->stream_index == audio_stream_idx)) {
            av_packet_unref(pkt);
            continue;
        }
        double     prof_decode_start = zet_prof_now_sec();
        AVStream*  in_stream         = in_fmt_ctx->streams[pkt->stream_index];
        AVStream*  out_stream        = (pkt->stream_index == video_stream_idx) ? out_video_stream : out_audio_stream;
        bool       is_video          = (pkt->stream_index == video_stream_idx);
        bool       needs_transcode   = is_video ? video_needs_transcode : audio_needs_transcode;
        AVRational fr                = in_stream->avg_frame_rate.num > 0 ? in_stream->avg_frame_rate : (AVRational){30, 1};
        INT64      frame_step        = av_rescale_q(1, av_inv_q(fr), out_stream->time_base);
        if (frame_step <= 0) frame_step = 1;

        bool input_is_mpegts = (in_fmt_ctx->iformat && strcmp(in_fmt_ctx->iformat->name, "mpegts") == 0);
        if (!needs_transcode) { // no transcode, direct write to HLS
			if (DirectWriteToHLS(is_video, input_is_mpegts, pkt, in_stream, out_stream, hdmv_multi_video,
                                 last_video_dts, last_video_pts, out_fmt_ctx, prof_mux_time)) {
                continue;
            }
        }

        AVCodecContext* in_codec_ctx  = is_video ? in_video_ctx : in_audio_ctx;
        AVCodecContext* out_codec_ctx = is_video ? out_video_ctx : out_audio_ctx;
        if (pkt && avcodec_send_packet(in_codec_ctx, pkt) < 0) {
            LOG(LOG_ERROR, "%s decode failed, please check", is_video ? "Video" : "Audio");
            if (is_video && hwAccelCtx.hwDecenabled) {
                LOG(LOG_ERROR, "ready to switch to sw decoder, mark");
                hwAccelCtx.hwDecenabled       = ZET_FALSE;
                const AVCodec* in_video_codec = avcodec_find_decoder(in_video_stream->codecpar->codec_id);
                in_video_ctx                  = avcodec_alloc_context3(in_video_codec);
                avcodec_parameters_to_context(in_video_ctx, in_video_stream->codecpar);
                // Enable multithread software decode on fallback as well
                if (in_video_ctx) {
                    zet_configure_decoder_threads(in_video_ctx, hlsGenInfo);
                }
                if (avcodec_open2(in_video_ctx, in_video_codec, NULL) < 0) {
                    LOG(LOG_ERROR, "Failed to open software video decoder");
                    av_packet_unref(pkt);
                    continue;
                }
                in_codec_ctx = in_video_ctx;
                if (avcodec_send_packet(in_codec_ctx, pkt) < 0) {
                    LOG(LOG_ERROR, "Software decode also failed");
                    av_packet_unref(pkt);
                    continue;
                }
            } else {
                av_packet_unref(pkt);
                continue;
            }
        }

        while ((errNo = avcodec_receive_frame(in_codec_ctx, frame)) >= 0 && !process_stop_requested.load()) {
            double prof_decode_end = zet_prof_now_sec();
            if (is_video) {
                prof_video_decode_time += (prof_decode_end - prof_decode_start);
            }

            static bool seekInfoDbg = ZET_FALSE;
            // Skip decoded frames that are earlier than the requested seek time.
            if (hlsGenInfo && hlsGenInfo->seekTime > 0 && in_stream) {
                double frame_time = -1.0;
                if (frame->pts != AV_NOPTS_VALUE) {
                    if (in_stream->start_time != AV_NOPTS_VALUE) {
                        frame->pts -= in_stream->start_time;
                    }
                    frame_time = av_q2d(in_stream->time_base) * frame->pts;
                } else if (frame->pkt_dts != AV_NOPTS_VALUE) {
                    if (in_stream->start_time != AV_NOPTS_VALUE) {
                        frame->pkt_dts -= in_stream->start_time;
                    }
                    frame_time = av_q2d(in_stream->time_base) * frame->pkt_dts;
                }
                if (!seekInfoDbg)
                    LOG(LOG_VERBOSE, "pre frame msg, time: %f, target seekTime: %f, in_stream->time_base num: %d, den: %d, pts: %ld, dts: %ld, stat time: %ld, isKey: %d", 
                               frame_time, hlsGenInfo->seekTime, in_stream->time_base.num, in_stream->time_base.den, 
                               frame->pts, frame->pkt_dts, in_stream->start_time, (frame->pict_type == AV_PICTURE_TYPE_I) ? 1 : 0);
                if (frame_time >= 0.0 && frame_time + 1e-6 < hlsGenInfo->seekTime) {
                    av_frame_unref(frame);
                    prof_decode_start = zet_prof_now_sec();
                    continue;
                }
                if (!seekInfoDbg) {
                    LOG(LOG_INFO, "seek frame msg, time: %f, target seekTime: %f, in_stream->time_base num: %d, den: %d, pts: %ld, dts: %ld, stat time: %ld, isKey: %d", 
                                   frame_time, hlsGenInfo->seekTime, in_stream->time_base.num, in_stream->time_base.den, frame->pts, frame->pkt_dts, in_stream->start_time, (frame->pict_type == AV_PICTURE_TYPE_I) ? 1 : 0);
                    seekInfoDbg = ZET_TRUE;
                }
            }

            AVFrame* target_frame  = frame;
            if (!target_frame) continue;
            if (is_video && (frame->width <= 0 || frame->height <= 0)) {
                LOG(LOG_WARNING, "Invalid decoded frame dimensons: %d x %d, skip frame", frame->width, frame->height);
                av_frame_unref(frame);
                continue;
            }
            if (is_video) {
#if X86_64
                static int temp_pool_w = 0, temp_pool_h = 0;
                static AVFrame *temp_frame_pool = NULL;
                int ret_x86 = processVideoWithX86_64(video_needs_transcode, hwAccelCtx, frame, &va_graph, &use_hw_scale,
                                                      in_video_ctx, hlsGenInfo, in_video_stream, out_video_ctx,
                                                      &va_src_ctx, &va_fmt_in_ctx, &va_upload_ctx,
                                                      &va_scale_ctx, &va_download_ctx, &va_fmt_out_ctx,
                                                      &va_sink_ctx, &out_hw, &sws_to_nv12, &sws_nv12_to_out,
                                                      tmp_hw_sw, &prof_video_hw_time, processed_frame, out_codec_ctx,
                                                      &target_frame, &sws_ctx, &temp_frame_pool, &temp_pool_w, &temp_pool_h);
                if (ret_x86 == ZET_ERR_MALLOC) {
                    break;
                } else if (ret_x86 == ZET_NOK) {
                    return ZET_NOK;
                } else if (ret_x86 < 0) {
                    return ret_x86;
                }
#elif ARM
                int ret_arm = processVideoWithARM(video_needs_transcode, hwAccelCtx, frame, &rga_graph, &use_hw_scale,
                                                  &in_hw, in_video_ctx, hlsGenInfo, in_video_stream, out_video_ctx,
                                                  &rga_src_ctx, &rga_fmt_in_ctx, &rga_upload_ctx,
                                                  &rga_scale_ctx, &rga_download_ctx, &rga_fmt_out_ctx,
                                                  &rga_sink_ctx, &out_hw, &sws_to_nv12, &sws_nv12_to_yuv,
                                                  tmp_hw_sw, &prof_video_hw_time, &target_frame, processed_frame, &sws_ctx, out_codec_ctx);
                if (ret_arm == ZET_ERR_MALLOC) {
                    break;
                } else if (ret_arm == ZET_NOK) {
                    return ZET_NOK;
                } else if (ret_arm == ZET_ERR_CONTINUE) {
                    continue;
                }
#endif
            } else if (!is_video && swr_ctx) {
                double prof_audio_start = zet_prof_now_sec();
                bool continue_loop = processAudioResample(processed_frame, frame, out_codec_ctx, swr_ctx,
                                                           audio_input_is_8k, aac_frame, aac_out_pkt, out_audio_stream,
                                                           out_fmt_ctx, process_stop_requested, last_audio_pts,
                                                           last_audio_dts, prof_audio_time, prof_audio_start, errNo,
                                                           audio8k_in_samples_total, prof_mux_time);
                if (!continue_loop) {
                    break;
                } else if (continue_loop == ZET_ERR_CONTINUE) {
                    continue;
                }
            }

            if (is_video) {
                if (ZET_ERR_CONTINUE == adjustVideoTimestamp(is_interlaced, is_mbaff_h264_ts, video_needs_transcode,
                                                             input_is_mpegts, hdmv_multi_video, interlaced_frame_index, frame, hwAccelCtx, use_hw_scale,
                                                             arm_rga_frame_index, target_frame, in_stream, out_codec_ctx, hlsGenInfo, out_stream, last_video_pts,
                                                             process_stop_requested)) {
                    continue;
                }

            }
#if X86_64
            processX86HwUpload(is_video, &hwAccelCtx, out_codec_ctx, &target_frame, &enc_hw_pool_x86);
#elif ARM
            processArmHwUpload(is_video, &hwAccelCtx, out_codec_ctx, &target_frame, &enc_hw_pool);         
#endif
            double prof_enc_start = 0.0;
            if (is_video)  prof_enc_start = zet_prof_now_sec();
            if (is_video && avcodec_send_frame(out_codec_ctx, target_frame) < 0) {
                LOG(LOG_ERROR, "%s encode failed, please check", is_video ? "Video" : "Audio");
                break;
            }
            if (!out_pkt) {
                out_pkt = av_packet_alloc();
            }
            if (!out_pkt) {
                LOG(LOG_ERROR, "Failed to allocate output packet");
                break;
            }
            while ((errNo = avcodec_receive_packet(out_codec_ctx, out_pkt)) >= 0 && !process_stop_requested.load()) {
                out_pkt->stream_index = out_stream->index;
                if (is_video) {
                    AVRational fr = (out_codec_ctx->framerate.num > 0 && out_codec_ctx->framerate.den > 0)
                                     ? out_codec_ctx->framerate
                                     : (AVRational){25, 1};
                    INT64 frame_step = av_rescale_q(1, av_inv_q(fr), out_codec_ctx->time_base);
                    if (frame_step <= 0) frame_step = 1;
                    out_pkt->duration = frame_step;

                    if (out_pkt->dts == AV_NOPTS_VALUE) {
                        out_pkt->dts = out_pkt->pts;
                    }
                    // First rescale into the muxer's time base
                    av_packet_rescale_ts(out_pkt, out_codec_ctx->time_base, out_stream->time_base);

                    // Establish a 0-based timeline for the current clip so that HLS
                    // segmentation sees a clean range [0, clip_duration].
                    if (base_video_pts == AV_NOPTS_VALUE && clip_duration_sec > 0.0) {
                        base_video_pts           = out_pkt->pts;
                        g_first_video_time_sec   = av_q2d(out_stream->time_base) * (double)base_video_pts;
                        g_first_video_time_valid = ZET_TRUE;
                    }
                    if (base_video_pts != AV_NOPTS_VALUE && clip_duration_sec > 0.0) {
                        out_pkt->pts -= base_video_pts;
                        out_pkt->dts -= base_video_pts;
                        if (out_pkt->pts < 0) out_pkt->pts = 0;
                        if (out_pkt->dts < 0) out_pkt->dts = 0;
                    }

                    // Keep timestamps strictly monotonic in the normalized timeline.
                    if (out_pkt->pts <= last_video_pts) {
                        out_pkt->pts = last_video_pts + 1; // keep simple tick-based monotonic fix
                    }
                    if (out_pkt->dts <= last_video_dts) {
                        out_pkt->dts = last_video_dts + 1;
                    }
                    last_video_pts      = out_pkt->pts;
                    last_video_dts      = out_pkt->dts;
                    double out_time_sec = av_q2d(out_stream->time_base) * out_pkt->pts;
                    int    is_key       = (out_pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
                    LOG(LOG_VERBOSE, "Video out pkt: pts=%lld dts=%lld dur=%lld sec=%.3f key=%d",
                                     (long long)out_pkt->pts, (long long)out_pkt->dts, (long long)out_pkt->duration, out_time_sec, is_key);

                    if (!g_first_video_written && clip_duration_sec > 0.0) {
                        g_first_video_written = ZET_TRUE;
                        g_first_video_pts 	  = out_pkt->pts;
                        LOG(LOG_DEBUG, "First video packet written at sec=%.3f (PTS=%ld), corresponding real time: %f", out_time_sec, g_first_video_pts, g_first_video_time_sec);
                    }

                    // Clip stop logic: stop after writing enough video for (seekEnd - seekTime).
                    if (clip_duration_sec > 0.0) {
                        if (clip_start_pts == AV_NOPTS_VALUE) {
                            clip_start_pts = out_pkt->pts;
                        }
                        INT64  delta_pts   = out_pkt->pts - clip_start_pts;
                        double clip_time   = av_q2d(out_stream->time_base) * delta_pts;
                        if (clip_time >= clip_duration_sec - 0.5) {
                            LOG(LOG_INFO, "clip duration reached: clip_time=%.3f sec, duration=%.3f sec, final pts=%lld, start pts: %ld",
                                           clip_time, clip_duration_sec, (long long)out_pkt->pts, clip_start_pts);
                            process_stop_requested.store(ZET_TRUE, std::memory_order_relaxed);
                            stop_due_to_clip = ZET_TRUE;
                        }
                    }
					//if (is_key && out_time_sec > 20.0) { LOG(LOG_INFO, "ARM HWENC keyframe at %.3f sec (PTS=%lld)", out_time_sec, (long long)out_pkt->pts);}
                } else if (!is_video && out_codec_ctx->codec_id != AV_CODEC_ID_AAC) {
                    if (out_pkt->pts == AV_NOPTS_VALUE) {
                        if (last_audio_pts == AV_NOPTS_VALUE) out_pkt->pts = 0;
                        else {
                            INT64 samples_per_packet = out_codec_ctx->frame_size;
                            INT64 inc                = av_rescale_q(samples_per_packet, av_make_q(1, out_codec_ctx->sample_rate), out_codec_ctx->time_base);
                            out_pkt->pts             = last_audio_pts + inc;
                        }
                        last_audio_pts = out_pkt->pts;
                    }
                    if (out_pkt->dts == AV_NOPTS_VALUE) { 
                        out_pkt->dts = out_pkt->pts;
                    }
                    if (out_pkt->pts < out_pkt->dts) {
                        out_pkt->dts = out_pkt->pts;
                    }
                    if (out_pkt->duration == AV_NOPTS_VALUE && out_codec_ctx->frame_size > 0) {
                        out_pkt->duration = av_rescale_q(out_codec_ctx->frame_size, av_make_q(1, out_codec_ctx->sample_rate), out_codec_ctx->time_base);
                    }
                    av_packet_rescale_ts(out_pkt, out_codec_ctx->time_base, out_stream->time_base);

                    if (out_pkt->pts <= last_audio_pts) {
                        out_pkt->pts = last_audio_pts + 1;
                    }
                    if (out_pkt->dts <= last_audio_dts) {
                        out_pkt->dts = last_audio_dts + 1;
                    }
                    last_audio_pts = out_pkt->pts;
                    last_audio_dts = out_pkt->dts;
                }
                double prof_mux_start2 = zet_prof_now_sec();
                if (!g_first_video_written && clip_duration_sec > 0.0) {
                    LOG(LOG_DEBUG, "Drop pre-roll audio packet before first video, pts=%lld dts=%lld", 
                                      (long long)out_pkt->pts, (long long)out_pkt->dts);
                    av_packet_unref(out_pkt);
                    continue;
                }
                if (av_interleaved_write_frame(out_fmt_ctx, out_pkt) < 0) {
                    LOG(LOG_ERROR, "%s write failed after encode, please check, pts: %lld, dts: %lld", is_video ? "Video" : "Audio",
                                    (long long)out_pkt->pts, (long long)out_pkt->dts);
                    av_packet_unref(out_pkt);
                    break;
                }
                prof_mux_time += (zet_prof_now_sec() - prof_mux_start2);
                av_packet_unref(out_pkt);
            }
            if (is_video) {
                prof_video_encode_time += (zet_prof_now_sec() - prof_enc_start);
            }
            //av_packet_free(&out_pkt);
            if (errNo != AVERROR(EAGAIN) && errNo != AVERROR_EOF) {
                LOG(LOG_ERROR, "%s encode failed, please check", is_video ? "Video" : "Audio");
                break;
            }
            av_frame_unref(frame);
            prof_decode_start = zet_prof_now_sec();
        }
        av_packet_unref(pkt);
        if (process_stop_requested.load()) {
            LOG(LOG_INFO, "transcode was ended or interruptted by outter message");
            break;
        }
    }

    if (audio_input_is_8k) {
        double in_duration_sec = (double)audio8k_in_samples_total / 8000.0;
        LOG(LOG_VERBOSE, "AAC8k input decoded total samples=%lld (~%.3f sec at 8kHz)",
                      (long long)audio8k_in_samples_total, in_duration_sec);
    }

    flushEncodersAndFinalize(out_fmt_ctx, video_needs_transcode, out_video_ctx, out_video_stream,
                             audio_needs_transcode, out_audio_ctx, out_audio_stream, in_audio_stream,
                             pkt, hls_playlist_path, file_duration_sec, stop_due_to_clip, clip_duration_sec);

    if (out_video_stream) {
        avcodec_parameters_free(&out_video_stream->codecpar);
        out_video_stream = NULL;
    }

    if (out_audio_stream) {
        avcodec_parameters_free(&out_audio_stream->codecpar);
        out_audio_stream = NULL;
    }

    //pthread_mutex_lock(&hls_mux_mutex);
    msgType = Transcode_finished;
    //pthread_mutex_unlock(&hls_mux_mutex);

    if (!hwAccelCtx.hwEncenabled && !hwAccelCtx.hwDecenabled) {
        freeResources(&in_fmt_ctx, &out_fmt_ctx, &in_video_ctx, &in_audio_ctx, &out_video_ctx, &out_audio_ctx, &sws_ctx, &swr_ctx, &frame, &pkt);
    } else {
        freeResources(&in_fmt_ctx, &out_fmt_ctx, NULL, &in_audio_ctx, NULL, &out_audio_ctx, &sws_ctx, &swr_ctx, &frame, &pkt);
    }

#if X86_64
    if (va_graph) {
        avfilter_graph_free(&va_graph);
        va_graph = NULL;
        va_src_ctx = va_fmt_in_ctx = va_upload_ctx = va_scale_ctx = va_download_ctx = va_fmt_out_ctx = va_sink_ctx = NULL; 
    }
    if (sws_to_nv12) { 
        sws_freeContext(sws_to_nv12);
        sws_to_nv12 = NULL;
    }
    if (sws_nv12_to_out) {
        sws_freeContext(sws_nv12_to_out);
        sws_nv12_to_out = NULL;
    }
    // X86_64: Cleanup fallback SW->HW upload static frame pool
    SAFE_RELEASE_FRAME(enc_hw_pool_x86);
    SAFE_RELEASE_FRAME(temp_frame_pool);
    SAFE_RELEASE_FRAME(sw_out_pool);
#elif ARM
    if (rga_graph) {
        avfilter_graph_free(&rga_graph);
        rga_graph = NULL;
        rga_src_ctx = rga_scale_ctx = rga_sink_ctx = NULL; rga_fmt_in_ctx = rga_upload_ctx = rga_download_ctx = rga_fmt_out_ctx = NULL;
    }
    if (sws_to_nv12) {
        sws_freeContext(sws_to_nv12); 
        sws_to_nv12 = NULL;
    }

    if (sws_nv12_to_yuv) { 
        sws_freeContext(sws_nv12_to_yuv);
        sws_nv12_to_yuv = NULL;
    }
    cleanup_debug_files();
    SAFE_RELEASE_FRAME(yuv420_pool);
    SAFE_RELEASE_FRAME(enc_hw_pool);
#endif
    SAFE_RELEASE_FRAME(tmp_hw_sw);
    SAFE_RELEASE_FRAME(aac_frame);
    SAFE_RELEASE_FRAME(processed_frame);
    SAFE_RELEASE_PACKET(aac_out_pkt);
    SAFE_RELEASE_PACKET(out_pkt);
    this->bindCurrentInstance(NULL);

    LOG(LOG_DEBUG, "profile: prof_video_decode_time=%.3f s, prof_video_hw_time=%.3f s, prof_video_encode_time=%.3f s, prof_audio_time=%.3f s, prof_mux_time=%.3f s, total time: %.3f s",
                    prof_video_decode_time, prof_video_hw_time, prof_video_encode_time, prof_audio_time, prof_mux_time,
                    prof_video_decode_time + prof_video_hw_time + prof_video_encode_time + prof_audio_time + prof_mux_time);
    LOG(LOG_DEBUG, "processWithApi finished, total execution time is: %.2f s", prof_video_decode_time + prof_video_hw_time + prof_video_encode_time + prof_audio_time + prof_mux_time);
    return ZET_OK;
}

void zetHlsServerMdl::flushEncodersAndFinalize(AVFormatContext* out_fmt_ctx,  bool video_needs_transcode, AVCodecContext* out_video_ctx, AVStream* out_video_stream,
                                               bool audio_needs_transcode,  AVCodecContext* out_audio_ctx, AVStream* out_audio_stream, AVStream* in_audio_stream,
                                               AVPacket* pkt, const std::string& hls_playlist_path, double file_duration_sec, bool stop_due_to_clip, double clip_duration_sec) {

    if (video_needs_transcode && out_video_ctx && out_video_stream && last_video_pts != AV_NOPTS_VALUE) {
        if (stop_due_to_clip && clip_duration_sec > 0.0) {
            LOG(LOG_INFO,  "Skipping video flush packets because clip stop was requested (seekEndTime-based clip_duration_sec=%.3f)",
                            clip_duration_sec);
        } else {
            avcodec_send_frame(out_video_ctx, NULL);
            AVPacket* flush_pkt = av_packet_alloc();
            double flush_duration = 0.0;
            int ret;

            while ((ret = avcodec_receive_packet(out_video_ctx, flush_pkt)) >= 0) {
                if (flush_pkt->size == 0 || !flush_pkt->data) {
                    LOG(LOG_ERROR, "Invalid flush packet (empty data)");
                    av_packet_unref(flush_pkt);
                    continue;
                }
    
                // Normalize flush packet duration to avoid 0-duration video
                // packets that confuse the HLS muxer, especially on some
                // encoders that may emit duration=0 for trailing frames.
                if (flush_pkt->duration <= 0) {
                    AVRational fr = (out_video_ctx->framerate.num > 0 && out_video_ctx->framerate.den > 0)
                                     ? out_video_ctx->framerate
                                     : (AVRational){25, 1};
                    int64_t frame_step = av_rescale_q(1, av_inv_q(fr), out_video_ctx->time_base);
                    if (frame_step <= 0) frame_step = 1;
                    flush_pkt->duration = frame_step;
                }
    
                av_packet_rescale_ts(flush_pkt, out_video_ctx->time_base, out_video_stream->time_base);
                flush_pkt->stream_index = out_video_stream->index;
    
                if (flush_pkt->pts == AV_NOPTS_VALUE) {
                    INT64 increment = (flush_pkt->duration > 0)
                                      ? flush_pkt->duration
                                      : av_rescale_q(1, out_video_stream->time_base, out_video_stream->time_base);
                    flush_pkt->pts = last_video_pts + increment;
                }
                if (flush_pkt->dts == AV_NOPTS_VALUE) {
                    INT64 increment = (flush_pkt->duration > 0)
                                      ? flush_pkt->duration
                                      : av_rescale_q(1, out_video_stream->time_base, out_video_stream->time_base);
                    flush_pkt->dts = last_video_dts + increment;
                }
    
                if (flush_pkt->pts <= last_video_pts) {
                    flush_pkt->pts = last_video_pts + 1;
                }
                if (flush_pkt->dts <= last_video_dts) {
                    flush_pkt->dts = last_video_dts + 1;
                }

                last_video_pts = flush_pkt->pts;
                last_video_dts = flush_pkt->dts;
                flush_duration += av_q2d(out_video_stream->time_base) * flush_pkt->duration;

                double flush_sec = av_q2d(out_video_stream->time_base) * flush_pkt->pts;
                int    flush_key = (flush_pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0;
                LOG(LOG_DEBUG, "ARM flush video pkt: pts=%lld dts=%lld dur=%lld sec=%.3f key=%d",
                               (long long)flush_pkt->pts, (long long)flush_pkt->dts,
                               (long long)flush_pkt->duration, flush_sec, flush_key);
    
                double prof_mux_start3 = zet_prof_now_sec();
                if (av_interleaved_write_frame(out_fmt_ctx, flush_pkt) < 0) {
                    LOG(LOG_ERROR, "Failed to write video flush packet");
                }
                prof_mux_time += (zet_prof_now_sec() - prof_mux_start3);
                av_packet_unref(flush_pkt);
            }

            if (ret != AVERROR_EOF) {
                LOG(LOG_ERROR, "Video flush failed (ret=%d, expected EOF)", ret);
            } else {
                //LOG(LOG_VERBOSE, "Video flush completed (total duration: %.3f sec)", flush_duration);
            }
            av_packet_free(&flush_pkt);
        }
    }    

    if (audio_needs_transcode && in_audio_stream && out_audio_ctx && out_audio_stream && pkt && last_video_pts != AV_NOPTS_VALUE) {
        avcodec_send_frame(out_audio_ctx, NULL);
        double audio_flush_duration = 0.0;
        int ret;
        while ((ret = avcodec_receive_packet(out_audio_ctx, pkt)) >= 0) {
            av_packet_rescale_ts(pkt, out_audio_ctx->time_base, out_audio_stream->time_base);
            pkt->stream_index = out_audio_stream->index;

            if (pkt->pts == AV_NOPTS_VALUE) {    
                INT64 frame_interval = av_rescale_q(1, out_audio_ctx->time_base, out_audio_stream->time_base);                
                pkt->pts = (last_audio_pts == AV_NOPTS_VALUE) ? 0 : (last_audio_pts + frame_interval);
            }
            if (pkt->dts == AV_NOPTS_VALUE) {
                pkt->dts = pkt->pts;
            }
            if (last_audio_pts != AV_NOPTS_VALUE && pkt->pts <= last_audio_pts) {
                pkt->pts = last_audio_pts + 1;
            }
            if (last_audio_dts != AV_NOPTS_VALUE && pkt->dts <= last_audio_dts) {
                pkt->dts = last_audio_dts + 1;
            }
            last_audio_pts = pkt->pts;
            last_audio_dts = pkt->dts;
            audio_flush_duration += av_q2d(out_audio_stream->time_base) * pkt->duration;
            double prof_mux_start4 = zet_prof_now_sec();
            if (!g_first_video_written && clip_duration_sec > 0.0) {
                LOG(LOG_INFO, "Drop pre-roll audio packet before first video, pts=%lld dts=%lld", 
                                  (long long)pkt->pts, (long long)pkt->dts);
                av_packet_unref(pkt);
                continue;
            }
            if (av_interleaved_write_frame(out_fmt_ctx, pkt) < 0) {
                LOG(LOG_ERROR, "Failed to write audio flush packet");
            }
            prof_mux_time += (zet_prof_now_sec() - prof_mux_start4);
            av_packet_unref(pkt);
        }        
        if (ret != AVERROR_EOF) {
            LOG(LOG_ERROR, "Audio flush failed (ret=%d, expected EOF)", ret);
        } else {
            LOG(LOG_VERBOSE, "Audio flush completed (total duration: %.3f sec)", audio_flush_duration);
        }
    }    

    if (out_video_stream) {
        bool has_video_pts = (last_video_pts != AV_NOPTS_VALUE);
        bool has_audio_pts = (last_audio_pts != AV_NOPTS_VALUE);

        // If no encoded A/V packets were observed, avoid using invalid PTS to
        // compute duration. Fall back to the probed file duration when
        // available, otherwise use 0.
        if (!has_video_pts && !has_audio_pts) {
            double final_duration = (file_duration_sec > 0.0) ? file_duration_sec : 0.0;
            LOG(LOG_WARNING,
                "No encoded A/V packets observed, using file_duration_sec=%.3f as final duration",
                final_duration);

            av_interleaved_write_frame(out_fmt_ctx, NULL);
            av_write_trailer(out_fmt_ctx);
            if (final_duration > 0.0) {
                fixM3U8Msg(hls_playlist_path, final_duration, hlsGenInfo->need_scale);
            }
            //pthread_mutex_lock(&hls_mux_mutex);
            msgType = Transcode_lastSegment_generated;
            //pthread_mutex_unlock(&hls_mux_mutex);
            return;
        }

        INT64 max_pts = FFMAX(last_video_pts, last_audio_pts);
        if (max_pts == AV_NOPTS_VALUE) {
            max_pts = has_video_pts ? last_video_pts : last_audio_pts;
        }

        out_fmt_ctx->duration  = av_rescale_q(max_pts, out_video_stream->time_base, AV_TIME_BASE_Q);
        double total_duration  = max_pts * av_q2d(out_video_stream->time_base);
        double final_duration  = total_duration;
        if (stop_due_to_clip && clip_duration_sec > 0.0) {
            LOG(LOG_DEBUG, "Using clip_duration_sec=%.3f sec as final duration instead of encoder-derived %.3f sec",
                           clip_duration_sec, total_duration);
            final_duration = clip_duration_sec;
        }
#if ARM
        // ARM safety guard: if computed output duration is significantly larger than input file duration,
        // trust the input duration to avoid doubled HLS durations on problematic sources (e.g. some H263 AVIs).
        if (file_duration_sec > 0.0 &&
            total_duration > file_duration_sec * 1.5 &&
            total_duration < file_duration_sec * 10.0) {
            LOG(LOG_INFO, "ARM: adjusting HLS final duration from %.3f sec to input duration %.3f sec", total_duration, file_duration_sec);
            final_duration = file_duration_sec;
        }
#endif
        LOG(LOG_DEBUG, "Final duration: %.3f sec (max_pts=%ld)", final_duration, max_pts);
        av_interleaved_write_frame(out_fmt_ctx, NULL);
        av_write_trailer(out_fmt_ctx);
        fixM3U8Msg(hls_playlist_path, final_duration, hlsGenInfo->need_scale);
        //pthread_mutex_lock(&hls_mux_mutex);
        msgType = Transcode_lastSegment_generated;
        //pthread_mutex_unlock(&hls_mux_mutex);
    } else {
        LOG(LOG_WARNING, "No valid video stream for final duration calculation");
        av_write_trailer(out_fmt_ctx);
    }
}

static void initCommandMsg() {
    memset(zet_cmd_msg.cmdType, 0, sizeof(zet_cmd_msg.cmdType));
    zet_cmd_msg.seekTime = -1;
    //pthread_mutex_init(&zet_cmd_msg.mutex, NULL);
    //pthread_cond_init(&zet_cmd_msg.cond, NULL);
}

static void refreshCommand(const char* params, double seekTime = -1) {
    //pthread_mutex_lock(&zet_cmd_msg.mutex);
    if (seekTime >= 0) {
        zet_cmd_msg.seekTime= seekTime;
    }

    if (ZET_ARRAY_ELEMS(zet_cmd_msg.cmdType) >  strlen(params)) {
        memcpy(zet_cmd_msg.cmdType, params, strlen(params));
    } else {
        LOG(LOG_ERROR, "src command len : %d longer than target len: %lu", (INT32)strlen(params), ZET_ARRAY_ELEMS(zet_cmd_msg.cmdType));
    }
    //pthread_cond_signal(&zet_cmd_msg.cond);
   // pthread_mutex_unlock(&zet_cmd_msg.mutex);
}

#endif

zetHlsServerMdl::zetHlsServerMdl():hlsGenInfo(NULL), hlsCmdInfo(NULL) {
   this->init();
}

void zetHlsServerMdl::init() {
    hlsGenInfo = (zetHlsGenInfo*)calloc(1, sizeof(zetHlsGenInfo));
    if (!hlsGenInfo) {
        LOG(LOG_ERROR, "Memory allocation failed!!!");
        return;
    }
    hlsCmdInfo            = NULL;
    hlsGenInfo->audIndex  = -1;
#if FFMPEG_EXE_WITH_CMDLINE
    ffmpeg_pid            = -1;
#else
    curVideoCodecID       = AV_CODEC_ID_H264;
    curAudioCodecID       = AV_CODEC_ID_AAC;
    is_running            = ZET_FALSE;
    transcode_thread      = 0;
    process_stop_requested.store(ZET_FALSE);
    pthread_mutex_init(&hls_mux_mutex, NULL);
    initCommandMsg();
    stopdemuxerThreads    = ZET_FALSE;
    stopVideoThreads      = ZET_FALSE;
    stopAudioThreads      = ZET_FALSE;
    memset(&hwAccelCtx, 0, sizeof(hwAccelCtx));
#endif
    msgType = Transcode_unInitialed;	
    LOG(LOG_DEBUG, " called, and exit!!!");
}

void zetHlsServerMdl::setCmdNum(INT32& args) {
    cmdNum = args;
}

bool zetHlsServerMdl::parseCmd(void* cmd) {
    ZETCHECK_PTR_IS_NULL(cmd);
    const char *opt;
    const char *next;
    INT32 optindex = 1;
    bool  err      = ZET_FALSE;
    if (cmdNum < 2) {
        LOG(LOG_ERROR, " parseCmd error, please refer to this info");
        optionsExplain();
    	return err;
    }

    char** inputInfo = reinterpret_cast<char**>(cmd);
    ZETCHECK_PTR_IS_NULL(*inputInfo);

    showHlsServerCmdInfo(cmdNum, inputInfo);
    const char* ffmpegPath = FFMPEG_BIN_PATH;

    char hlsCmdInfoBuffer[MAX_READING_LENGTH];
    snprintf(hlsCmdInfoBuffer, sizeof(hlsCmdInfoBuffer), "%s -y", ffmpegPath);

    char* curPtr = hlsCmdInfoBuffer + strlen(hlsCmdInfoBuffer);

    while (optindex < cmdNum) {
        opt  = (const char*)inputInfo[optindex++];
        if (!strcmp(opt, "-hls")) {
            hlsGenInfo->need_hls = ZET_TRUE;
            *curPtr++ = ' ';
            continue;
        }

        if (!strcmp(opt, "-live")) {
            hlsGenInfo->isLive= ZET_TRUE;
            *curPtr++ = ' ';
            continue;
        }

        if (optindex >= cmdNum) {
            LOG(LOG_ERROR, " cmd line %s has no value, warning ...", opt);
            optionsExplain();
            goto PARSE_OPINIONS_OUT;
        }

        if(!strcmp(opt, "-probe")) {
            hlsGenInfo->need_probe = ZET_TRUE;
            continue;
        }

        if (!strcmp(opt, "-scale")) {
            hlsGenInfo->need_scale = ZET_TRUE;
            continue;
        }

        next = (const char*)inputInfo[optindex];

        if (!strcmp(opt, "-ab")) {
            hlsGenInfo->audBitrate = atoi(next);
            appendOption(curPtr, "-b:a");
            *curPtr++ = ' ';
            curPtr += sprintf(curPtr, "%dk", hlsGenInfo->audBitrate);
            optindex++;
            continue;
        }

        if(!strcmp(opt, "-ss")) {
            double      seekTime     = atof(next);
            hlsGenInfo->seekTime     = seekTime;
            appendOption(curPtr, "-ss");
            *curPtr++ = ' ';
            curPtr += snprintf(curPtr, hlsCmdInfoBuffer + sizeof(hlsCmdInfoBuffer) - curPtr, "%g", hlsGenInfo->seekTime);
            hlsGenInfo->seek_requested = ZET_TRUE;
            zet_cmd_msg.seekTime = seekTime >= 0 ? seekTime : 0;
            if (ZET_ARRAY_ELEMS(zet_cmd_msg.cmdType) >  strlen("seek")) {
                memcpy(zet_cmd_msg.cmdType, "seek", strlen("seek"));
            }
            optindex++;
            continue;
        }

        if(!strcmp(opt, "-to")) {
            double      seekEndTime  = atof(next);
            hlsGenInfo->seekEndTime  = seekEndTime;
            appendOption(curPtr, "-to");
            *curPtr++ = ' ';
            curPtr += snprintf(curPtr, hlsCmdInfoBuffer + sizeof(hlsCmdInfoBuffer) - curPtr, "%g", hlsGenInfo->seekEndTime);
            hlsGenInfo->seek_requested = ZET_TRUE;
            zet_cmd_msg.seekTime =  hlsGenInfo->seekEndTime > 0 ? hlsGenInfo->seekEndTime : 0 ;
            optindex++;
            continue;
        }

        if(!strcmp(opt, "-start_number")) {
            INT32      startNumber   = atof(next);
            hlsGenInfo->startNumber  = startNumber;
            appendOption(curPtr, "-start_number");
            *curPtr++ = ' ';
            curPtr += snprintf(curPtr, hlsCmdInfoBuffer + sizeof(hlsCmdInfoBuffer) - curPtr, "%d", hlsGenInfo->startNumber);
            optindex++;
            continue;
        }

        if(!strcmp(opt, "-hls_time")) {
            INT32      hls_timeSec   = atof(next);
            hlsGenInfo->hls_timeSec  = hls_timeSec;
            appendOption(curPtr, "-hls_time");
            *curPtr++ = ' ';
            curPtr += snprintf(curPtr, hlsCmdInfoBuffer + sizeof(hlsCmdInfoBuffer) - curPtr, "%d", hlsGenInfo->startNumber);
            optindex++;
            continue;
        }

        if(!strcmp(opt, "-map")) {
            if (!next || strlen(next) == 0) {
                LOG(LOG_ERROR, "-map parameter requires a value (e.g., 0:a:0)");
                optindex++;
                continue;
            }
            int  input_file_idx   = -1;
            char stream_type      = 0;
            int  audio_stream_idx = -1;
            int  parse_ret        = sscanf(next, "%d:%c:%d", &input_file_idx, &stream_type, &audio_stream_idx);
            if (parse_ret != 3 || input_file_idx != 0 || stream_type != 'a' || audio_stream_idx < 0) {
                LOG(LOG_ERROR, "Invalid -map format, required: 0:a:index (e.g., 0:a:0), current: %s", next);
                optindex++;
                continue;
            }
            hlsGenInfo->audIndex = audio_stream_idx;
            LOG(LOG_INFO, "Successfully parse -map parameter, target audio stream index: %d", audio_stream_idx);
            appendOption(curPtr, "-map");
            *curPtr++ = ' ';
            curPtr += snprintf(curPtr, hlsCmdInfoBuffer + sizeof(hlsCmdInfoBuffer) - curPtr, "%s", next);
            optindex++;
            continue;
        }

        if (opt[0] == '-' ) {
            switch (opt[1]) {
                case 'i':
                    strncpy(hlsGenInfo->file_input, next, MAX_FILE_NAME_LENGTH - 1);
                    hlsGenInfo->need_input = ZET_TRUE;
                    appendOption(curPtr, "-re");
                    appendOption(curPtr, "-i");
                    *curPtr++ = ' ';
                    curPtr += snprintf(curPtr, hlsCmdInfoBuffer + sizeof(hlsCmdInfoBuffer) - curPtr, "%s", next);
                    optindex++;
                    break;
                case 'o':
                    strncpy(hlsGenInfo->file_output, next, MAX_FILE_NAME_LENGTH - 1);
                    hlsGenInfo->need_output = ZET_TRUE;
                    optindex++;
            	    break;
                case 'b':
                    hlsGenInfo->bitrate = atoi(next);
                    appendOption(curPtr, "-b:v");
                    *curPtr++ = ' ';
                    curPtr += snprintf(curPtr, hlsCmdInfoBuffer + sizeof(hlsCmdInfoBuffer) - curPtr, "%dk", hlsGenInfo->bitrate);
                    optindex++;
                    break;
                case 'w':
                    hlsGenInfo->width = atoi(next);
                    optindex++;
                    break;
                case 'h':
                    hlsGenInfo->height = atoi(next);
                    if (hlsGenInfo->height > 0) {
                        appendOption(curPtr, "-s");
                        *curPtr++ = ' ';
                         curPtr += snprintf(curPtr, hlsCmdInfoBuffer + sizeof(hlsCmdInfoBuffer) - curPtr, "%dx%d",
                                             hlsGenInfo->width, hlsGenInfo->height);
                    }
                    if (hlsGenInfo->width <= 0 || hlsGenInfo->height <= 0) {
                        LOG(LOG_ERROR, " invalid input width or height, please check...");
                    }
                    optindex++;
                    break;
                case 'f':
                    hlsGenInfo->framerate= atof(next);
                    appendOption(curPtr, "-r");
                    *curPtr++ = ' ';
                    curPtr += snprintf(curPtr, hlsCmdInfoBuffer + sizeof(hlsCmdInfoBuffer) - curPtr, "%g", hlsGenInfo->framerate);                        
                    optindex++;
            	    break;
                case 's':
                    hlsGenInfo->sampleRate= atoi(next);
                    appendOption(curPtr, "-ar");
                    *curPtr++ = ' ';
                    curPtr += snprintf(curPtr, hlsCmdInfoBuffer + sizeof(hlsCmdInfoBuffer) - curPtr, "%u", hlsGenInfo->sampleRate);
                    optindex++;
                    break;
                case 'n':
                    hlsGenInfo->threads = atoi(next);
                    appendOption(curPtr, "-threads");
                    *curPtr++ = ' ';
                    curPtr += sprintf(curPtr, "%d", hlsGenInfo->threads);
                    optindex++;
                    break;
                case 'c':
                    hlsGenInfo->channels = atoi(next);
                    appendOption(curPtr, "-ac");
                    *curPtr++ = ' ';
                    curPtr += snprintf(curPtr, hlsCmdInfoBuffer + sizeof(hlsCmdInfoBuffer) - curPtr, "%u", hlsGenInfo->channels); 
                    optindex++;
                    break;
                case 'a':
                    strncpy(hlsGenInfo->audCodingType, next, MAX_TYPE_NAME_LENGTH - 1);
                    appendOption(curPtr, "-c:a");
                    *curPtr++ = ' ';
                    curPtr += sprintf(curPtr, "%s", next);
                    optindex++;
                    break;
                case 'v':
                    strncpy(hlsGenInfo->vidCodingType, next, MAX_TYPE_NAME_LENGTH - 1);
                    appendOption(curPtr, "-c:v");
                    *curPtr++ = ' ';
                    curPtr += snprintf(curPtr, hlsCmdInfoBuffer + sizeof(hlsCmdInfoBuffer) - curPtr, "%s", next);
                    optindex++;
                    break;
                default:
                    LOG(LOG_ERROR, " unkonw parameters: %s ...", opt);
                    optionsExplain();
                    goto PARSE_OPINIONS_OUT;
            }
        } else {
            LOG(LOG_ERROR, " invalid parameters: %s, must start as '-', hlsCmdInfoBuffer: %s", opt, hlsCmdInfoBuffer);
            optionsExplain();
            goto PARSE_OPINIONS_OUT; 
        }
    }
 {
    if (!hlsGenInfo->need_input || !hlsGenInfo->need_hls ) {
        LOG(LOG_ERROR, " invalid input file or output format, please specified the file");
        goto PARSE_OPINIONS_OUT;
    }

    appendOption(curPtr, "-bf 0");
   // appendOption(curPtr, "-flush_packets 0");
    appendOption(curPtr, "-f hls");

    if (hlsGenInfo->isLive) {

        appendOption(curPtr, "-hls_time 2");

        appendOption(curPtr, "-start_number 0");

        appendOption(curPtr, "-hls_list_size 10");

        appendOption(curPtr, "-hls_flags delete_segments+append_list+omit_endlist");

        appendOption(curPtr, "-preset ultrafast");
    } else {

        appendOption(curPtr, "-hls_time 2");

        appendOption(curPtr, "-start_number 0");

        appendOption(curPtr, "-hls_list_size 20");

        appendOption(curPtr, "-hls_flags delete_segments+append_list+independent_segments");

        appendOption(curPtr, "-use_wallclock_as_timestamps 0");

        appendOption(curPtr, "-preset ultrafast");

        appendOption(curPtr, "-fflags nobuffer -flags low_delay -avioflags direct");

        appendOption(curPtr, "-force_key_frames");

        *curPtr++ = ' ';
        const char* keyframe_settings = "\"expr:gte(n,n_forced*2)\" -g 60 -keyint_min 60";
        size_t remaining_space        = MAX_READING_LENGTH - (curPtr - hlsCmdInfoBuffer);
        if (strlen(keyframe_settings) < remaining_space) {
            strcpy(curPtr, keyframe_settings);
            curPtr += strlen(keyframe_settings);
        } else {
            LOG(LOG_ERROR, "transCmdInfoBuffer is too small to append keyframe_settings!");
            goto PARSE_OPINIONS_OUT;
        }

    }

    *curPtr++                 = ' ';
    const char* fullInputPath = hlsGenInfo->file_input;
    const char* fileName      = strrchr(fullInputPath, '/');
   // fileName == NULL ? fullInputPath : fileName++;
    if (fileName == NULL) {
        fileName = fullInputPath;
    } else {
        fileName++;
    }

    if (fileName == NULL || *fileName == '\0') {
        LOG(LOG_ERROR, "Invalid fileName extracted from path: %s", fullInputPath);
        goto PARSE_OPINIONS_OUT;
    }
#if 0
    const char* dotPos        = strrchr(fileName, '.');
    int         baseLen       = (dotPos != NULL) ? (dotPos - fileName) : strlen(fileName);
    char        baseName[MAX_FILE_NAME_LENGTH];
    snprintf(baseName, sizeof(baseName), "%.*s", baseLen, fileName);

    if (access(HLS_PATH, F_OK) == -1) {
        mkdir(HLS_PATH, 0755);
    }

    INT64 dir_len = strlen(HLS_PATH) + strlen(baseName) + 10;

    char hlsSubDir [dir_len];
    snprintf (hlsSubDir, sizeof (hlsSubDir), "%s/%s-out", HLS_PATH, baseName);

    if (access(hlsSubDir, F_OK) == 0) {
        char rmCmd[MAX_READING_LENGTH];
        if (chmod(hlsSubDir, 0777) == -1) {
            LOG(LOG_ERROR, "Failed to set permissions for hlsSubDir: %s, %s", strerror(errno), hlsSubDir);
        }
        snprintf(rmCmd, sizeof(rmCmd), "rm -rf %s/*", hlsSubDir);
        int ret = system(rmCmd);
        if (ret != 0) {
            LOG(LOG_WARNING, "Failed to clear directory '%s', system() returned %d", hlsSubDir, ret);
        } else {
            LOG(LOG_DEBUG, "clear the old data inside this file: %s", hlsSubDir);
        }
    } else {
        LOG(LOG_DEBUG, "target file not exist, create it: %s", hlsSubDir);
        mkdir(hlsSubDir, 0777);
    }

    curPtr += snprintf(curPtr, hlsCmdInfoBuffer + sizeof(hlsCmdInfoBuffer) - curPtr,
                         "-hls_segment_filename %s/%s-out_%%03d.ts",
                           hlsSubDir, baseName);

    *curPtr++ = ' ';

	int path_len = snprintf(NULL, 0, "%s/%s-output.m3u8", hlsSubDir, baseName);
    if (path_len < 0) {
        LOG(LOG_ERROR, "Failed to calculate path length");
        return false;
    }

    if (path_len + 1 > MAX_FILE_NAME_LENGTH) {
        LOG(LOG_ERROR, "Output file path is too long: %d, max: %d", 
        path_len, MAX_FILE_NAME_LENGTH - 1);
        return false;
    }

    if (snprintf(hlsGenInfo->file_output, MAX_FILE_NAME_LENGTH,
                 "%s/%s-output.m3u8", hlsSubDir, baseName) >= MAX_FILE_NAME_LENGTH) {
        LOG(LOG_ERROR, "Output path was truncated");
        return false;
    }

    curPtr += snprintf(curPtr, hlsCmdInfoBuffer + sizeof(hlsCmdInfoBuffer) - curPtr,
                       "%s", hlsGenInfo->file_output);
#endif

    if (currentLogLevel == LOG_INFO) {
        appendOption(curPtr, "-progress pipe:1");
        *curPtr++ = ' ';
        const char* grep_filter = "2>&1 | grep -E \"time|Opening|Initializing|Encoding\"";
        size_t remaining_space  = MAX_READING_LENGTH - (curPtr - hlsCmdInfoBuffer);
        if (strlen(grep_filter) < remaining_space) {
            strcpy(curPtr, grep_filter);
            curPtr += strlen(grep_filter);
        } else {
            LOG(LOG_ERROR, "transCmdInfoBuffer is too small to append grep filter!");
            goto PARSE_OPINIONS_OUT;
        }
    }

    *curPtr = '\0';

    INT32 needed_size = strlen(hlsCmdInfoBuffer) + 1;
    if (needed_size > MAX_READING_LENGTH) {
        LOG(LOG_ERROR, " hlsCmdInfoBuffer is longer than default size, quit...");
        goto PARSE_OPINIONS_OUT;
    }

    // Free old hlsCmdInfo if exists
    if (hlsCmdInfo) {
        free(hlsCmdInfo);
        hlsCmdInfo = NULL;
    }

    hlsCmdInfo = (char*)malloc(strlen(hlsCmdInfoBuffer) + 1);
    if (!hlsCmdInfo) {
        LOG(LOG_ERROR, "try to generate ffmpeg command info, but failed...");
        goto PARSE_OPINIONS_OUT; 
    }

    strcpy(hlsCmdInfo, hlsCmdInfoBuffer);
    LOG(LOG_DEBUG, "full ffmpeg command is: %s", hlsCmdInfo);
    return ZET_TRUE;
 }	
PARSE_OPINIONS_OUT:
    if (hlsCmdInfo) {
        free(hlsCmdInfo);
        hlsCmdInfo = NULL;
    }
    return ZET_FALSE;
}

INT32 zetHlsServerMdl::process(void* msg) {
    INT32 ret = ZET_NOK;

#if FFMPEG_EXE_WITH_CMDLINE
    ret = this->processWithCmdLine(msg);
#else

    if (is_running) {
        LOG(LOG_DEBUG, "Transcode is already running");
        return ZET_OK;
    }

    is_running = ZET_TRUE;
    process_stop_requested.store(ZET_FALSE);
    struct timespec start_ts, end_ts;
    if (clock_gettime(CLOCK_MONOTONIC, &start_ts) != 0) {
        LOG(LOG_ERROR, "clock_gettime start failed");
        is_running = ZET_FALSE;
        return ZET_NOK;
    }

    ret = this->processWithApi(msg);

    if (clock_gettime(CLOCK_MONOTONIC, &end_ts) != 0) {
        LOG(LOG_ERROR, "clock_gettime end failed");
        is_running = ZET_FALSE;
        return ZET_NOK;
    }

    double duration_ms = (end_ts.tv_sec - start_ts.tv_sec) * 1000.0
						  + (end_ts.tv_nsec - start_ts.tv_nsec) / 1000000.0;
    double duration_s  = duration_ms / 1000.0;
    //LOG(LOG_WARNING, "call processWithApi finished, total execution time is: %.2f s", duration_s);

    if (ret != ZET_OK) {
        //pthread_mutex_lock(&hls_mux_mutex);
        msgType = Transcode_error;
        //pthread_mutex_unlock(&hls_mux_mutex);
    }

	//ret = this->processWithMultiThreadApi(msg);
    return ret;
#endif

}

INT32 zetHlsServerMdl::processUpLayerCmd(const char* params, double seekTime) {
    INT32 ret = ZET_NOK;

    if (!params || !hlsCmdInfo) {
        LOG(LOG_DEBUG, " NULL ptr found, please check params: %s and cmdInfo: %s", params, hlsCmdInfo);
        return ret;        
    }

    stopFFmpegProcess();

#if FFMPEG_EXE_WITH_CMDLINE
    if (!strcasecmp(params, "seek")) {
        INT32 target_seekTime = (INT32)seekTime;
        LOG(LOG_DEBUG, " called, current seekTime: %d", target_seekTime);
        generateNewCmd(hlsCmdInfo, target_seekTime);
        ret = this->process(NULL);
    } else if (!strcasecmp(params, "stop")) {
        this->release();
        LOG(LOG_DEBUG, " called, execute stop command");
        return ZET_OK;
    }
    return ret;
#else
    refreshCommand(params, seekTime);
    return ZET_OK;
#endif
}

void zetHlsServerMdl::stopFFmpegProcess() {
#if FFMPEG_EXE_WITH_CMDLINE

    if (ffmpeg_pid == -1) return;

    if (ffmpeg_pid > 0) {
        kill(-ffmpeg_pid, SIGKILL);
        waitpid(ffmpeg_pid, NULL, 0);
        ffmpeg_pid = -1;
    }
     LOG(LOG_DEBUG, "called, force to kill ffmpeg process..");
#else
     process_stop_requested.store(ZET_TRUE);
     LOG(LOG_DEBUG, "called, force to kill ffmpeg process..");
#endif
}

hlsMsgType zetHlsServerMdl::getHLSMsgType() {
    return msgType;
}

void zetHlsServerMdl::resetHLSMsg() {
#if !FFMPEG_EXE_WITH_CMDLINE
    //pthread_mutex_lock(&hls_mux_mutex);
    msgType = Transcode_unInitialed;
    //pthread_mutex_unlock(&hls_mux_mutex);
#endif
}

void zetHlsServerMdl::release() {
    SAFE_FREE(hlsCmdInfo);
    SAFE_FREE(hlsGenInfo);

#if !FFMPEG_EXE_WITH_CMDLINE
    memset(zet_cmd_msg.cmdType, 0, sizeof(zet_cmd_msg.cmdType));
    zet_cmd_msg.seekTime = -1;
    pthread_mutex_destroy(&zet_cmd_msg.mutex);
    pthread_cond_destroy(&zet_cmd_msg.cond);
    pthread_mutex_destroy(&hls_mux_mutex);
    last_video_pts  = AV_NOPTS_VALUE;
    last_video_dts  = AV_NOPTS_VALUE;
    cleanupHardwareContext(hwAccelCtx);
#endif
    LOG(LOG_DEBUG, "called, and exit!!!");
}

zetHlsServerMdl::~zetHlsServerMdl() {
    this->release();
    LOG(LOG_DEBUG, " called, and exit!!!");
}

