#ifndef PLAYER_DEV_ZETMEDIA_ZETBUSMODULE_ZETHLSSERVERMDL_H
#define PLAYER_DEV_ZETMEDIA_ZETBUSMODULE_ZETHLSSERVERMDL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>
#include <vector>
#include <queue>

#include "iZetModule.h"
#include "commonDefine.h"
#include "zetFFmpegProcess.h"

typedef enum _hlsMsgType {
    Transcode_unInitialed,
    Transcode_error,
    Transcode_start,
    Transcode_working,
    Transcode_lastSegment_generated,
    Transcode_finished,
} hlsMsgType;

struct _zetHlsGenInfo;
//struct  HWAccelCtx;

template<typename T>
class ThreadSafeQueue {
private:
    std::queue<T> queue_;
    pthread_mutex_t mutex_;
    pthread_cond_t cond_;
    bool stopped_;

public:
    ThreadSafeQueue() : stopped_(false) {
        pthread_mutex_init(&mutex_, NULL);
        pthread_cond_init(&cond_, NULL);
    }

    ~ThreadSafeQueue() {
        pthread_mutex_destroy(&mutex_);
        pthread_cond_destroy(&cond_);
    }

    void push(const T& item) {
        pthread_mutex_lock(&mutex_);
        if (!stopped_) {
            queue_.push(item);
            pthread_cond_signal(&cond_);
        }
        pthread_mutex_unlock(&mutex_);
    }

    bool pop(T& item, bool wait = true) {
        pthread_mutex_lock(&mutex_);
        
        if (stopped_) {
            pthread_mutex_unlock(&mutex_);
            return false;
        }

        if (wait) {
            while (!stopped_ && queue_.empty()) {
                pthread_cond_wait(&cond_, &mutex_);
            }

            if (stopped_) {
                pthread_mutex_unlock(&mutex_);
                return false;
            }
        }

        if (queue_.empty()) {
            pthread_mutex_unlock(&mutex_);
            return false;
        }

        item = queue_.front();
        queue_.pop();
        pthread_mutex_unlock(&mutex_);
        return true;
    }

    bool isStopped() {
        pthread_mutex_lock(&mutex_);
        bool res = stopped_;
        pthread_mutex_unlock(&mutex_);
        return res;
    }

    void stop() {
        pthread_mutex_lock(&mutex_);
        stopped_ = true;
        pthread_cond_broadcast(&cond_);
        pthread_mutex_unlock(&mutex_);
    }

    bool empty() {
        pthread_mutex_lock(&mutex_);
        bool res = queue_.empty();
        pthread_mutex_unlock(&mutex_);
        return res;
    }
};

class zetHlsServerMdl : public iZetModule {
 public:
    zetHlsServerMdl();
    zetHlsServerMdl(const zetHlsServerMdl&)            = delete;
    zetHlsServerMdl& operator=(const zetHlsServerMdl&) = delete;
    void        init();
    bool        parseCmd(void* cmd);
    INT32       process(void* msg);

#if FFMPEG_EXE_WITH_CMDLINE
    INT32       processWithCmdLine(void* msg);
#else

    INT32       processWithApi(void* msg);

#if X86_64
    INT32       processWithX86Api(void*msg);
    void        initVaapiScalingGraph(bool video_needs_transcode, const HWAccelCtx& hwAccelCtx, AVCodecContext* in_video_ctx,
                                               AVStream* in_video_stream, AVCodecContext* out_video_ctx, AVFilterGraph*& va_graph,
                                               AVFilterContext*& va_src_ctx, AVFilterContext*& va_fmt_in_ctx, AVFilterContext*& va_upload_ctx,
                                               AVFilterContext*& va_scale_ctx, AVFilterContext*& va_download_ctx, AVFilterContext*& va_fmt_out_ctx,
                                               AVFilterContext*& va_sink_ctx, bool& use_vaapi, bool& va_out_hw, SwsContext*& sws_to_nv12,
                                               SwsContext*& sws_nv12_to_outfmt, AVFrame*& va_tmp_frame);

    void        processVaapiScaling(bool& scaled, bool& use_vaapi, AVFilterGraph* va_graph, 
                                            AVFilterContext* va_src_ctx, AVFilterContext* va_sink_ctx,
                                            const HWAccelCtx& hwAccelCtx, AVFrame* frame, AVFrame* va_tmp_frame,
                                            SwsContext*& sws_to_nv12, AVFrame* processed_frame,
                                            AVCodecContext* out_codec_ctx, bool va_out_hw,
                                            SwsContext*& sws_nv12_to_outfmt, AVFrame*& target_frame);

    INT32       processVideoWithX86_64(bool video_needs_transcode, HWAccelCtx hwAccelCtx, AVFrame *frame, AVFilterGraph **va_graph,
                                                bool *use_hw_scale, AVCodecContext *in_video_ctx, struct _zetHlsGenInfo* hlsGenInfo, AVStream *in_video_stream,
                                                AVCodecContext *out_video_ctx, AVFilterContext **va_src_ctx, AVFilterContext **va_fmt_in_ctx,
                                                AVFilterContext **va_upload_ctx, AVFilterContext **va_scale_ctx, AVFilterContext **va_download_ctx,
                                                AVFilterContext **va_fmt_out_ctx, AVFilterContext **va_sink_ctx, bool *out_hw, SwsContext **sws_to_nv12,
                                                SwsContext **sws_nv12_to_out, AVFrame *tmp_hw_sw, double *prof_video_hw_time, AVFrame *processed_frame,
                                                AVCodecContext *out_codec_ctx, AVFrame **target_frame, SwsContext **sws_ctx, AVFrame **temp_frame_pool,
                                                int *temp_pool_w, int *temp_pool_h);
    void        processX86HwUpload(int is_video, HWAccelCtx* hwAccelCtx, AVCodecContext* out_codec_ctx, 
                                                     AVFrame** target_frame, AVFrame** enc_hw_pool_x86);
#elif ARM
    INT32       processWithArmApi(void*msg);
    void        initRgaFilterGraph(AVFilterGraph **rga_graph, AVFilterContext **rga_src_ctx, AVFilterContext **rga_fmt_in_ctx,
                                          AVFilterContext **rga_upload_ctx, AVFilterContext **rga_scale_ctx, AVFilterContext **rga_download_ctx,
                                          AVFilterContext **rga_fmt_out_ctx, AVFilterContext **rga_sink_ctx, bool *use_rga,
                                          bool *rga_out_hw, SwsContext **sws_to_nv12, SwsContext **sws_nv12_to_yuv420,
                                          AVFrame *temp_frame, bool video_needs_transcode, const HWAccelCtx *hwAccelCtx,
                                          const AVCodecContext *in_video_ctx, const AVStream *in_video_stream, const AVCodecContext *out_video_ctx,
                                          bool *rga_in_hw, bool force_sw_out);

    int         processWithRgaScaling(AVFrame *frame, AVFrame **target_frame, AVFrame *processed_frame,
                                               AVFrame *temp_frame, bool use_rga, bool rga_out_hw,
                                               AVFilterContext *rga_src_ctx, AVFilterContext *rga_sink_ctx, const HWAccelCtx *hwAccelCtx,
                                               SwsContext **sws_to_nv12, SwsContext **sws_nv12_to_yuv420, const AVCodecContext *out_codec_ctx,
                                              bool rga_in_hw);
    INT32       processVideoWithARM(bool video_needs_transcode, HWAccelCtx hwAccelCtx, AVFrame *frame, AVFilterGraph **rga_graph,
                                            bool *use_hw_scale, bool *in_hw, AVCodecContext *in_video_ctx, struct _zetHlsGenInfo* hlsGenInfo,
                                            AVStream *in_video_stream, AVCodecContext *out_video_ctx, AVFilterContext **rga_src_ctx,
                                            AVFilterContext **rga_fmt_in_ctx, AVFilterContext **rga_upload_ctx, AVFilterContext **rga_scale_ctx,
                                            AVFilterContext **rga_download_ctx, AVFilterContext **rga_fmt_out_ctx, AVFilterContext **rga_sink_ctx,
                                            bool *out_hw, SwsContext **sws_to_nv12, SwsContext **sws_nv12_to_yuv, AVFrame *tmp_hw_sw,
                                            double *prof_video_hw_time, AVFrame **target_frame, AVFrame *processed_frame, SwsContext **sws_ctx, AVCodecContext* out_codec_ctx);

    void        processArmHwUpload(int is_video,  HWAccelCtx* hwAccelCtx, AVCodecContext* out_codec_ctx, 
                                           AVFrame** target_frame, AVFrame** enc_hw_pool);

#endif

    INT32		initHlsCtx(struct _zetHlsGenInfo* info, AVFormatContext* in_fmt_ctx, AVCodecContext* in_video_ctx,
                            AVCodecContext* in_audio_ctx, AVCodecContext* out_video_ctx, AVCodecContext* out_audio_ctx,
                            SwsContext* sws_ctx, SwrContext* swr_ctx, AVFormatContext**out_fmt_ctx,
                            AVDictionary** hls_opts, std::string& hls_playlist_path, std::string& hls_segment_pattern);


    AVCodecID   findTargetCodec(struct _zetHlsGenInfo* hlsGenInfo, bool isVideo);

    void        bindCurrentInstance(void* ptr) {
                s_current_instance= (!ptr) ? NULL : this;
    }

    static void signalHandlerForward(int signum, siginfo_t* info, void* ucontext);
    bool        checkAndProcessCommands(AVFormatContext* in_fmt_ctx);

    INT32       getMediaMsg(AVFormatContext* in_fmt_ctx, AVStream* in_video_stream, AVStream* in_audio_stream,
                            AVCodecID curVideoCodecID, AVCodecID curAudioCodecID, double file_duration_sec);


	//enum AVPixelFormat  hw_get_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts);

    INT32       initHWDecoder(AVFormatContext* in_fmt_ctx, AVStream* in_video_stream, AVCodecContext* in_audio_ctx, HWAccelCtx& hwAccelCtx);

    INT32       initHWEncoder(struct _zetHlsGenInfo* hlsGenInfo, AVFormatContext* in_fmt_ctx, AVStream* in_video_stream,
                                    AVCodecContext* in_video_ctx, AVCodecContext* in_audio_ctx, HWAccelCtx& hwAccelCtx, AVCodecID curVideoCodecID);

    bool        DirectWriteToHLS(bool is_video, bool input_is_mpegts, AVPacket* pkt,
                                        AVStream* in_stream, AVStream* out_stream, bool hdmv_multi_video,
                                        INT64& last_video_dts, INT64& last_video_pts,AVFormatContext* out_fmt_ctx,
                                        double& prof_mux_time);

    INT32       processAudioResample(AVFrame* processed_frame, AVFrame* frame, AVCodecContext* out_codec_ctx, SwrContext* swr_ctx,
                                              bool audio_input_is_8k, AVFrame*& aac_frame, AVPacket*& aac_out_pkt, AVStream* out_audio_stream,
                                              AVFormatContext* out_fmt_ctx, std::atomic<bool>& process_stop_requested, INT64& last_audio_pts,
                                              INT64& last_audio_dts, double& prof_audio_time, double prof_audio_start, int& errNo,
                                              INT64& audio8k_in_samples_total, double& prof_mux_time);


    void        flushEncodersAndFinalize(AVFormatContext* out_fmt_ctx, bool video_needs_transcode, AVCodecContext* out_video_ctx,
                                                   AVStream* out_video_stream, bool audio_needs_transcode, AVCodecContext* out_audio_ctx,
                                                   AVStream* out_audio_stream, AVStream* in_audio_stream, AVPacket* pkt,
                                                   const std::string& hls_playlist_path, double file_duration_sec,
                                                   bool stop_due_to_clip, double clip_duration_sec);
    INT32       adjustVideoTimestamp(bool is_interlaced, bool is_mbaff_h264_ts, bool video_needs_transcode, bool input_is_mpegts,
                                              bool hdmv_multi_video, int64_t& interlaced_frame_index, AVFrame* frame, HWAccelCtx& hwAccelCtx,
                                              bool use_hw_scale, int64_t& arm_rga_frame_index, AVFrame* target_frame, AVStream* in_stream,
                                              AVCodecContext* out_codec_ctx, struct _zetHlsGenInfo* hlsGenInfo, AVStream* out_stream, int64_t last_video_pts,
                                              std::atomic<bool>& process_stop_requested);
#endif
    INT32       processUpLayerCmd(const char* params, double seekTime = -1);
    hlsMsgType  getHLSMsgType();
    void        resetHLSMsg();
    void        release();
    void        setCmdNum(INT32 &args);
    void        stopFFmpegProcess();
    ~zetHlsServerMdl();

 private:
    struct _zetHlsGenInfo* hlsGenInfo;
    char *                 hlsCmdInfo;
    INT32                  cmdNum;
    std::atomic<hlsMsgType> msgType;
#if FFMPEG_EXE_WITH_CMDLINE
    pid_t ffmpeg_pid;
#else
    AVCodecID               curVideoCodecID;
    AVCodecID               curAudioCodecID;
    std::atomic<bool>       process_stop_requested;
    void                    handleSignal(int signum);
    static zetHlsServerMdl* s_current_instance;
    INT64 last_video_pts    {AV_NOPTS_VALUE};
    INT64 last_video_dts    {AV_NOPTS_VALUE};
    INT64 last_audio_pts    {AV_NOPTS_VALUE};
    INT64 last_audio_dts    {AV_NOPTS_VALUE};
    bool                    is_running;
    pthread_t               transcode_thread;
    pthread_mutex_t         hls_mux_mutex;
    HWAccelCtx              hwAccelCtx;
 
    // Narrow fix state for Blu-ray ISO HDMV sources
    bool                    is_bd_iso_source        {ZET_FALSE};
    INT64                   directcopy_audio_base_pts {AV_NOPTS_VALUE};
 
    FILE *                  audio_src_file {NULL};
    FILE *                  audio_file {NULL};
    FILE *                  audio_cvt_file {NULL};

 //   volatile bool  stopdemuxerThreads;
 //   volatile bool  stopVideoThreads;
 //   volatile bool  stopAudioThreads;
#endif
 };
#endif

