#ifndef PLAYER_DEV_ZETMEDIA_ZETBUSMODULE_ZETFFMPEGPROCESS_H
#define PLAYER_DEV_ZETMEDIA_ZETBUSMODULE_ZETFFMPEGPROCESS_H

#include <vector>
#include "commonDefine.h"

#if defined(__arm__) || defined(__aarch64__)
#ifdef __linux__
//#include <rga/RgaApi.h>
#else
//#include "rga/RgaApi.h"
#endif
#endif
extern "C" {
    #include "libavformat/avformat.h"      // NOLINT
    #include "libavformat/version.h"       // NOLINT
    #include <libavcodec/avcodec.h>
    #include <libavutil/timestamp.h>
    #include <libavutil/cpu.h>
    #include <libavdevice/avdevice.h>
    #include "libavutil/avutil.h"          // NOLINT
    #include "libavutil/imgutils.h"        // NOLINT
    #include "libavutil/opt.h"             // NOLINT
    #include "libswresample/swresample.h"  // NOLINT
    #include "libswscale/swscale.h"        // NOLINT

#if X86_64
//    #include <libavutil/hwcontext.h>
//    #include <libavutil/hwcontext_vaapi.h>   // Intel VAAPI
#endif
    #include <libavutil/error.h>
   // #include <libavutil/hwcontext_cuda.h>    // NVIDIA CUDA
   // #include <libavutil/hwcontext_qsv.h>     // Intel Quick Sync Video
    #include <libavfilter/avfilter.h>
    #include <libavfilter/buffersrc.h>
    #include <libavfilter/buffersink.h>
    #include <libavutil/opt.h>
}

typedef struct {
    enum AVCodecID          av_codec_id;
    char                    mine[16];
} zetCodingTypeInfo;

typedef enum {
    NONE,
    RKMPP,
    VAAPI,
    NVENC,
    QSV,
} ZetHWAccelType;

typedef struct {
    ZetHWAccelType  hw_accel_type;
    bool            hw_accel_supported;
    bool            use_afbc;
    char            vaapi_device[64];
    char            rk_mpp_device[64];
} ZetHWAccelInfo;

struct HWAccelCtx {
    enum AVPixelFormat hw_pix_fmt;
    ZetHWAccelType     hw_accel_type;
    bool               use_afbc;
    char               vaapi_device[64];
    char               rk_mpp_device[64];
    bool               hwDecenabled;
    bool               hwEncenabled;
    bool               hwScaEnabled;
    AVBufferRef*       hw_dec_frames_ctx;
    AVBufferRef*       hw_enc_frames_ctx;
    AVCodecContext*    video_enc_ctx;
    AVCodecContext*    video_dec_ctx;
};

SwsContext*         initScaleContext(AVCodecContext* in_codec_ctx, AVCodecContext* out_codec_ctx);
SwrContext*         initResampleContext(AVCodecContext* in_codec_ctx, AVCodecContext* out_codec_ctx);
/*
INT32               initHlsCtx(struct _zetHlsGenInfo* info, AVFormatContext* in_fmt_ctx, AVCodecContext* in_video_ctx,
                                   AVCodecContext* in_audio_ctx, AVCodecContext* out_video_ctx, AVCodecContext* out_audio_ctx,
                                   SwsContext* sws_ctx, SwrContext* swr_ctx, AVFormatContext**out_fmt_ctx,
                                   AVDictionary** hls_opts, std::string& hls_playlist_path, std::string& hls_segment_pattern);
*/
INT32               findBestStream(AVFormatContext* fmt_ctx, AVMediaType type, const std::string& type_name);

void                freeResources(AVFormatContext* in_fmt_ctx, AVFormatContext* out_fmt_ctx,
                                        AVCodecContext* in_video_ctx, AVCodecContext* in_audio_ctx,
                                        AVCodecContext* out_video_ctx, AVCodecContext* out_audio_ctx,
                                        SwsContext* sws_ctx, SwrContext* swr_ctx,
                                        AVFrame* frame, AVPacket* pkt);

void                freeResources(AVFormatContext** in_fmt_ctx, AVFormatContext** out_fmt_ctx, AVCodecContext** in_video_ctx,
                                        AVCodecContext** in_audio_ctx, AVCodecContext** out_video_ctx, AVCodecContext** out_audio_ctx,
                                        SwsContext** sws_ctx, SwrContext** swr_ctx, AVFrame** frame, AVPacket** pkt);

INT32               handleSeekRequest(AVFormatContext* in_fmt_ctx, AVFormatContext*& out_fmt_ctx, AVStream*& out_video_stream,
                                             AVStream*& out_audio_stream, const std::string& hls_playlist_path,
                                             const std::string& hls_segment_pattern, bool video_needs_transcode,
                                             bool audio_needs_transcode, AVStream* in_video_stream,
                                             AVStream* in_audio_stream, AVCodecContext* out_video_ctx,
                                             AVCodecContext* out_audio_ctx);

INT32               handleAacFrames(AVCodecContext* codec_ctx, AVFrame* frame, AVFormatContext* fmt_ctx, AVStream* stream);

INT32               handlehMultiThreadAacFrames(AVCodecContext* codec_ctx, AVFrame* frame, AVFormatContext* fmt_ctx, AVStream* stream, pthread_mutex_t hls_mux_mutex);

double              getMediaFileDuration(AVFormatContext* in_fmt_ctx, int video_stream_idx);

enum AVPixelFormat  getHardwarePixelFormat();

enum AVPixelFormat  get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts);

enum AVPixelFormat  get_hwAccelFrame_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts);

const AVCodec*      hw_accel_find_codec_by_hw_type(enum AVHWDeviceType device_type, enum AVCodecID codec_id, int is_encoder);

bool                initHWAccel(AVCodecContext* ctx, AVHWDeviceType& hw_type);

bool                isHWCodecSupport(AVCodecID codecID, const AVCodec** video_ctx, bool enc);

#if  ARM
INT32               initRKMPPContext(AVCodecContext* codec_ctx, HWAccelCtx& hwAccelCtx, bool enc);
INT32               initRGAContext(HWAccelCtx &hwAccelCtx, bool enc);

#elif X86_64
INT32               initVAAPIContext(AVCodecContext* codec_ctx, HWAccelCtx& hwAccelCtx, bool enc);
#endif

INT32               cleanupHardwareContext(HWAccelCtx& hw_ctx_);
bool                fixM3U8Msg(const std::string& m3u8_path, double expected_total_duration, bool need_scale);

static pthread_mutex_t      audio_residual_mutex = PTHREAD_MUTEX_INITIALIZER;

extern std::vector<uint8_t> audioResidualBuffer;
extern int                  audioResidualSamples;
extern int                  audioChannels;
extern int                  audioSampleSize;

static zetCodingTypeInfo codecMappingList[] = {
    /* video */
    {AV_CODEC_ID_MPEG1VIDEO,  "mpeg1" },
    {AV_CODEC_ID_MPEG2VIDEO,  "mpeg2" },
    {AV_CODEC_ID_H263,  "h263"  },
    {AV_CODEC_ID_MPEG4, "mpeg4"  },
    {AV_CODEC_ID_WMV3,  "wmv3" },
    {AV_CODEC_ID_H264,  "h264" },
    {AV_CODEC_ID_H264,  "libx264" },
    {AV_CODEC_ID_MJPEG, "mjpeg" },
    {AV_CODEC_ID_VP8,   "vp8" },
    {AV_CODEC_ID_VP9,   "vp9" },
    {AV_CODEC_ID_HEVC,  "hevc" },
    {AV_CODEC_ID_VC1,   "vc1" },
    {AV_CODEC_ID_AVS,   "avs" },
    {AV_CODEC_ID_CAVS,  "cavs" },
    {AV_CODEC_ID_CAVS,  "avs+" },
    {AV_CODEC_ID_FLV1,  "flv1" },
    /* audio */
    {AV_CODEC_ID_AAC,   "aac" },
    {AV_CODEC_ID_APE,   "ape" },
    {AV_CODEC_ID_MP3,   "mp3" },
    {AV_CODEC_ID_WMALOSSLESS, "wmalossless" },
    {AV_CODEC_ID_WMAPRO, "wnapro" },
    {AV_CODEC_ID_WMAV1,  "wmav1" },
    {AV_CODEC_ID_WMAV2,  "wmav2"  },
    {AV_CODEC_ID_VORBIS, "vorbis" },
    {AV_CODEC_ID_PCM_S16LE, "pcm" },
    {AV_CODEC_ID_PCM_S24LE, "pcm" },
    {AV_CODEC_ID_PCM_S32LE, "pcm" },
    {AV_CODEC_ID_FLAC,  "flac" },
    {AV_CODEC_ID_MP1,   "mp1" },
    {AV_CODEC_ID_MP2,   "mp2" },
    {AV_CODEC_ID_DTS,   "dts" },
    {AV_CODEC_ID_AC3,   "ac3" },
    {AV_CODEC_ID_EAC3,  "eac3" },
    {AV_CODEC_ID_TRUEHD, "truehd" },
    {AV_CODEC_ID_MLP,   "mlp" },
    {AV_CODEC_ID_G729,         "g729" },
    {AV_CODEC_ID_ADPCM_G722,   "adpcm_g722" },
    {AV_CODEC_ID_ADPCM_G726,   "adpcm_g726" },
    {AV_CODEC_ID_AMR_NB,       "amr_nb" },
    {AV_CODEC_ID_AMR_WB,       "amr_wb" },
    {AV_CODEC_ID_ADPCM_IMA_QT, "adpcm_ima_qt" },

    {AV_CODEC_ID_DVD_SUBTITLE, "dvd" },
    {AV_CODEC_ID_DVB_SUBTITLE, "dvb" },
    {AV_CODEC_ID_TEXT,         "txt" },
    {AV_CODEC_ID_XSUB,         "xsub" },
    {AV_CODEC_ID_SSA,          "ssa" },
    {AV_CODEC_ID_MOV_TEXT,     "mov_txt" },
    {AV_CODEC_ID_HDMV_PGS_SUBTITLE, "pgs" },
    {AV_CODEC_ID_DVB_TELETEXT, "teletext" },
    {AV_CODEC_ID_SRT,          "srt" },
    {AV_CODEC_ID_MICRODVD,     "micodvd" },
    {AV_CODEC_ID_EIA_608,      "eia808" },
    {AV_CODEC_ID_JACOSUB,      "jacosub" },
    {AV_CODEC_ID_SAMI,         "sami" },
    {AV_CODEC_ID_REALTEXT,     "realtext" },
    {AV_CODEC_ID_STL,          "stl" },
    {AV_CODEC_ID_SUBVIEWER1,   "subviewer1" },
    {AV_CODEC_ID_SUBVIEWER,    "subviewer" },
    {AV_CODEC_ID_SUBRIP,       "subrip" },
    {AV_CODEC_ID_WEBVTT,       "webvtt" },
    {AV_CODEC_ID_MPL2,         "mpl2" },
    {AV_CODEC_ID_VPLAYER,      "vplayer" },
    {AV_CODEC_ID_PJS,          "pjs" },
    {AV_CODEC_ID_ASS,          "ass" },
    {AV_CODEC_ID_HDMV_TEXT_SUBTITLE, "hdmv_txt" },

};

#if X86_64
  static zetCodingTypeInfo VAAPIMappingList[] = {
      {AV_CODEC_ID_H264,        "h264_vaapi"},
      {AV_CODEC_ID_HEVC,        "hevc_vaapi" },
      {AV_CODEC_ID_VP8,         "vp8_vaapi" },
      {AV_CODEC_ID_VP9,         "vp9_vaapi" },
      {AV_CODEC_ID_MPEG2VIDEO,  "mpeg2_vaapi"},
      {AV_CODEC_ID_MPEG4,       "mpeg4_vaapi"},
      {AV_CODEC_ID_HEVC,        "hevc_vaapi"},
      {AV_CODEC_ID_MJPEG,       "mjpeg_vaapi"},
      {AV_CODEC_ID_AV1,         "av1_vaapi"},
      {AV_CODEC_ID_VP8,         "vp8_vaapi"},
      {AV_CODEC_ID_VP9,         "vp9_vaapi"},
  };

#elif ARM
  static zetCodingTypeInfo RKMPPMappingList[] = {
      {AV_CODEC_ID_H264,  "h264_rkmpp" },
      {AV_CODEC_ID_HEVC,  "hevc_rkmpp" },
      {AV_CODEC_ID_VP8,   "vp8_rkmpp" },
      {AV_CODEC_ID_VP9,   "vp9_rkmpp" },
  };
#endif

#endif

