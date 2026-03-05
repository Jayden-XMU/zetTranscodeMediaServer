#include <iostream>
#include <sys/stat.h> 
#include <unistd.h>
#include <sys/types.h>
#include <cmath>
#include <stdlib.h>
#include <fstream>
#include <sstream>
#include <iomanip>
#include "zetFFmpegProcess.h"

std::vector<uint8_t> audioResidualBuffer;
int audioResidualSamples       = 0;
int audioChannels              = 0;
int audioSampleSize            = 0;
AVPixelFormat hw_pix_fmt       = AV_PIX_FMT_YUV420P;
AVBufferRef  * hw_device_ctx   = NULL;

INT32 findBestStream(AVFormatContext* fmt_ctx, AVMediaType type, const std::string& type_name) {

    int stream_idx = av_find_best_stream(fmt_ctx, type, -1, -1, NULL, 0);

    if (stream_idx < 0) {
        LOG(LOG_ERROR, "do not find %s stream", type_name.c_str());
        return -1;
    }
    LOG(LOG_DEBUG, " find %s stream, stream_idx: %d", type_name.c_str(), stream_idx);
    return stream_idx;
}

SwsContext* initScaleContext(AVCodecContext* in_codec_ctx, AVCodecContext* out_codec_ctx) {
    if (in_codec_ctx->pix_fmt == AV_PIX_FMT_NONE) {
        LOG(LOG_ERROR, "Invalid input pixel format");
        return NULL;
    }
    return sws_getContext(in_codec_ctx->width, in_codec_ctx->height, in_codec_ctx->pix_fmt,
                          out_codec_ctx->width, out_codec_ctx->height, out_codec_ctx->pix_fmt,
                          SWS_BICUBIC, NULL, NULL, NULL);
}

SwrContext* initResampleContext(AVCodecContext* in_codec_ctx, AVCodecContext* out_codec_ctx) {
    SwrContext* swr_ctx = swr_alloc();
    if (!swr_ctx) {
        return NULL;
    }

    AVSampleFormat out_sample_fmt = out_codec_ctx->codec_id == AV_CODEC_ID_AAC ? AV_SAMPLE_FMT_FLTP : out_codec_ctx->sample_fmt;
    av_opt_set_chlayout(swr_ctx, "in_chlayout", &in_codec_ctx->ch_layout, 0);
    av_opt_set_chlayout(swr_ctx, "out_chlayout", &out_codec_ctx->ch_layout, 0);
    av_opt_set_int(swr_ctx, "in_sample_rate", in_codec_ctx->sample_rate, 0);
    av_opt_set_int(swr_ctx, "out_sample_rate", out_codec_ctx->sample_rate, 0);
    av_opt_set_sample_fmt(swr_ctx, "in_sample_fmt", in_codec_ctx->sample_fmt, 0);
    av_opt_set_sample_fmt(swr_ctx, "out_sample_fmt", out_sample_fmt, 0);

    if (swr_init(swr_ctx) < 0) {
        swr_free(&swr_ctx);
        return NULL;
    }

    if (out_codec_ctx->codec_id == AV_CODEC_ID_AAC) {
        //out_codec_ctx->frame_size = (in_codec_ctx->ch_layout.nb_channels > 1) ? 2048 : 1024;
        if (out_codec_ctx->codec_id == AV_CODEC_ID_AAC) {
            LOG(LOG_DEBUG, "AAC encoder reports frame_size=%d (do not override)", out_codec_ctx->frame_size);
        }
    }
    LOG(LOG_DEBUG, "Audio resampler initialized: %dHz %s -> %dHz %s",
                    in_codec_ctx->sample_rate, av_get_sample_fmt_name(in_codec_ctx->sample_fmt),
                    out_codec_ctx->sample_rate, av_get_sample_fmt_name(out_codec_ctx->sample_fmt));
    return swr_ctx;
}

/*
INT32 initHlsCtx(struct _zetHlsGenInfo* info, AVFormatContext* in_fmt_ctx, AVCodecContext* in_video_ctx,
                      AVCodecContext* in_audio_ctx, AVCodecContext* out_video_ctx, AVCodecContext* out_audio_ctx,
                      SwsContext* sws_ctx, SwrContext* swr_ctx, AVFormatContext**out_fmt_ctx,
                      AVDictionary** hls_opts, std::string& hls_playlist_path, std::string& hls_segment_pattern) {
    hls_playlist_path   = info->file_output;
    size_t last_slash   = hls_playlist_path.find_last_of("/");
    std::string hls_dir = (last_slash != std::string::npos) ? hls_playlist_path.substr(0, last_slash): ".";
    hls_segment_pattern = hls_dir + "/segment_%03d.ts";

    *out_fmt_ctx = NULL;
    if (avformat_alloc_output_context2(out_fmt_ctx, NULL, "hls", hls_playlist_path.c_str()) < 0) {
        LOG(LOG_ERROR, "unable to alloc output context");
        freeResources(&in_fmt_ctx, out_fmt_ctx, &in_video_ctx, &in_audio_ctx,
        &out_video_ctx, &out_audio_ctx, &sws_ctx, &swr_ctx, NULL, NULL);
        return ZET_NOK;
    }    

    *hls_opts = NULL;
    info->need_scale ? av_dict_set(hls_opts, "hls_time", "3600*5", 0) : av_dict_set(hls_opts, "hls_time", "2", 0);
    //av_dict_set(hls_opts, "hls_time", "2", 0);
    av_dict_set(hls_opts, "hls_segment_filename", hls_segment_pattern.c_str(), 0);
    av_dict_set(hls_opts, "hls_list_size", "0", 0);
    av_dict_set(hls_opts, "hls_start_number", "0", 0);
    av_dict_set(hls_opts, "hls_flags", "split_by_time", 0);
    av_dict_set(hls_opts, "hls_segment_type", "mpegts", 0);
    return ZET_OK;
}
*/

INT32 handleSeekRequest(AVFormatContext* in_fmt_ctx, AVFormatContext*& out_fmt_ctx, AVStream*& out_video_stream,
                               AVStream*& out_audio_stream, const std::string& hls_playlist_path,
                               const std::string& hls_segment_pattern, bool video_needs_transcode,
                               bool audio_needs_transcode, AVStream* in_video_stream,
                               AVStream* in_audio_stream, AVCodecContext* out_video_ctx,
                               AVCodecContext* out_audio_ctx) {
    LOG(LOG_DEBUG, "Handling seek request, reinitializing output");

    if (out_fmt_ctx) {
        av_write_trailer(out_fmt_ctx);
        if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&out_fmt_ctx->pb);
        }
        if (out_video_stream) {
            avcodec_parameters_free(&out_video_stream->codecpar);
            out_video_stream = NULL;
        }
        if (out_audio_stream) {
            avcodec_parameters_free(&out_audio_stream->codecpar);
            out_audio_stream = NULL;
        }
        avformat_free_context(out_fmt_ctx);
        out_fmt_ctx = NULL;
    }

    if (avformat_alloc_output_context2(&out_fmt_ctx, NULL, "hls", hls_playlist_path.c_str()) < 0) {
        LOG(LOG_ERROR, "unable to re-alloc output context after seek");
        return ZET_NOK;
    }

    AVDictionary* new_hls_opts = NULL;
    av_dict_set(&new_hls_opts, "hls_time", "2", 0);
    av_dict_set(&new_hls_opts, "hls_segment_filename", hls_segment_pattern.c_str(), 0);
    av_dict_set(&new_hls_opts, "hls_list_size", "0", 0);
    av_dict_set(&new_hls_opts, "hls_start_number", "1", 0);
    av_dict_set(&new_hls_opts, "hls_flags", "split_by_time", 0);
    av_dict_set(&new_hls_opts, "hls_segment_type", "mpegts", 0);

    out_video_stream = avformat_new_stream(out_fmt_ctx, NULL);
    if (!out_video_stream) {
        LOG(LOG_ERROR, "unable to create new video output stream");
        av_dict_free(&new_hls_opts);
        avformat_free_context(out_fmt_ctx);
        out_fmt_ctx = NULL;
        return ZET_NOK;
    }

    if (video_needs_transcode) {
        avcodec_parameters_from_context(out_video_stream->codecpar, out_video_ctx);
        out_video_stream->time_base = out_video_ctx->time_base;
    } else {
        avcodec_parameters_copy(out_video_stream->codecpar, in_video_stream->codecpar);
        out_video_stream->time_base = in_video_stream->time_base;
    }

    if (in_audio_stream) {
        out_audio_stream = avformat_new_stream(out_fmt_ctx, NULL);
        if (!out_audio_stream) {
            LOG(LOG_ERROR, "unable to create new audio output stream");
            avcodec_parameters_free(&out_video_stream->codecpar);
            out_video_stream = NULL;
            av_dict_free(&new_hls_opts);
            avformat_free_context(out_fmt_ctx);
            out_fmt_ctx = NULL;
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
            LOG(LOG_ERROR, "unable to open output file");
            av_dict_free(&new_hls_opts);

            if (out_video_stream) {
                avcodec_parameters_free(&out_video_stream->codecpar);
                out_video_stream = NULL;
            }
            if (out_audio_stream) {
                avcodec_parameters_free(&out_audio_stream->codecpar);
                out_audio_stream = NULL;
            }
            avformat_free_context(out_fmt_ctx);
            out_fmt_ctx = NULL;
            return ZET_NOK;
        }
    }

    if (avformat_write_header(out_fmt_ctx, &new_hls_opts) < 0) {
        LOG(LOG_ERROR, "unable to write into new output file");
        av_dict_free(&new_hls_opts);

        if (!(out_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&out_fmt_ctx->pb);
        }
        if (out_video_stream) {
            avcodec_parameters_free(&out_video_stream->codecpar);
            out_video_stream = NULL;
        }
        if (out_audio_stream) {
            avcodec_parameters_free(&out_audio_stream->codecpar);
            out_audio_stream = NULL;
        }
        avformat_free_context(out_fmt_ctx);
        out_fmt_ctx = NULL;
        return ZET_NOK;
    }

    av_dict_free(&new_hls_opts);
    return ZET_OK;
}


INT32 handleAacFrames(AVCodecContext* codec_ctx, AVFrame* frame,
                             AVFormatContext* fmt_ctx, AVStream* stream) {
    int frame_size  = codec_ctx->frame_size;
    int channels    = codec_ctx->ch_layout.nb_channels;
    int sample_size = av_get_bytes_per_sample(codec_ctx->sample_fmt);

    if (!codec_ctx || !fmt_ctx || !stream) {
        LOG(LOG_ERROR, "Invalid parameters in handleAacFrames");
        return ZET_NOK;
    }

    if (audioChannels == 0) {
        audioChannels        = channels;
        audioSampleSize      = sample_size;
        audioResidualBuffer.clear();
        audioResidualSamples = 0;
    }

    LOG(LOG_DEBUG, "Handling AAC frames: nb_samples=%d, channels=%d, frame_size: %d, sample_size: %d, frame->format: %d", 
                   frame ? frame->nb_samples : 0, 
                   codec_ctx->ch_layout.nb_channels,
                   frame_size, sample_size, frame->format);


    // add new data to ResidualBuffer
    if (frame && frame->nb_samples > 0) {
        size_t current_size = audioResidualBuffer.size();
        size_t add_size     = frame->nb_samples * channels * sample_size;
        audioResidualBuffer.resize(current_size + add_size);

        for (int ch = 0; ch < channels; ch++) {
            uint8_t* dst = audioResidualBuffer.data() + current_size + ch * frame->nb_samples * sample_size;
            memcpy(dst, frame->data[ch], frame->nb_samples * sample_size);
        }
        audioResidualSamples += frame->nb_samples; // update residual samples
    }

    int          processed_samples         = 0;
    static INT64 last_audio_pts            = AV_NOPTS_VALUE;

    while (audioResidualSamples >= frame_size) { // make sure to have enough sample to create aac frame
        // create the frame meet aac demand
        AVFrame* aac_frame     = av_frame_alloc();
        aac_frame->format      = codec_ctx->sample_fmt;//frame->format;
        aac_frame->sample_rate = codec_ctx->sample_rate;//codec_ctx->sample_rate;//
        aac_frame->nb_samples  = frame_size;
        av_channel_layout_copy(&aac_frame->ch_layout, &codec_ctx->ch_layout);

        if (av_frame_get_buffer(aac_frame, 0) < 0) {
            av_frame_free(&aac_frame);
            return ZET_NOK;
        }

        // copy residual buffer to aac frame
        for (int ch = 0; ch < channels; ch++) {
            const UINT8* src = audioResidualBuffer.data() + (processed_samples * channels + ch) * sample_size;

            size_t copy_bytes = frame_size * sample_size;

            // need to check boundary
            if (copy_bytes > aac_frame->linesize[ch]) {
                LOG(LOG_DEBUG, "send aac frame but failed, copy_bytes: %d, aac_frame->linesize[ch]: %d, frame_size : %d, sample_size: %d", (int)copy_bytes, aac_frame->linesize[ch], frame_size, sample_size);
                av_frame_free(&aac_frame);
                return ZET_NOK;
            }
            memcpy(aac_frame->data[ch], src, frame_size * sample_size);
        }

        // set the pts properly !!!
        if (frame && frame->pts != AV_NOPTS_VALUE) {
            // Calculate timestamp based on sample size
            INT64 sample_duration = av_rescale_q(1, av_make_q(1, codec_ctx->sample_rate), codec_ctx->time_base);
            aac_frame->pts        = frame->pts + processed_samples * sample_duration;

            // Ensure that the timestamp monotonically increases
            if (last_audio_pts != AV_NOPTS_VALUE && aac_frame->pts <= last_audio_pts) {
                aac_frame->pts = last_audio_pts + sample_duration * frame_size;
                LOG(LOG_DEBUG, "Adjusted audio PTS to maintain monotonicity");
            }
            last_audio_pts = aac_frame->pts;
        } else {
            // create the rightly timestamp
            if (last_audio_pts == AV_NOPTS_VALUE) {
                aac_frame->pts = 0;
            } else {
                INT64 sample_duration = av_rescale_q(1, av_make_q(1, codec_ctx->sample_rate), codec_ctx->time_base);
                aac_frame->pts = last_audio_pts + sample_duration * frame_size;
            }
            last_audio_pts = aac_frame->pts;
        }

        if (avcodec_send_frame(codec_ctx, aac_frame) < 0) {
            LOG(LOG_ERROR, "Failed to send frame to AAC encoder");
            av_frame_free(&aac_frame);
            return ZET_NOK;
        }

        AVPacket* out_pkt = av_packet_alloc();
        while (avcodec_receive_packet(codec_ctx, out_pkt) >= 0) {
            // Ensure that the timestamp monotonically increases
            if (out_pkt->pts == AV_NOPTS_VALUE) {
                out_pkt->pts = aac_frame->pts;
            }
            if (out_pkt->dts == AV_NOPTS_VALUE) {
                out_pkt->dts = out_pkt->pts;
            }

            out_pkt->duration = av_rescale_q(frame_size, av_make_q(1, codec_ctx->sample_rate), stream->time_base);

            av_packet_rescale_ts(out_pkt, codec_ctx->time_base, stream->time_base);
            out_pkt->stream_index = stream->index;

            if (av_interleaved_write_frame(fmt_ctx, out_pkt) < 0) {
                LOG(LOG_ERROR, "Audio packet write failed - PTS: %ld, DTS: %ld", out_pkt->pts, out_pkt->dts);
            }
            av_packet_unref(out_pkt);
        }
        av_packet_free(&out_pkt);
        av_frame_free(&aac_frame);
        processed_samples    += frame_size;
        audioResidualSamples -= frame_size;
    }

    // update ResidualBuffer
    if (processed_samples > 0) {
        size_t remaining_size = audioResidualSamples * channels * sample_size;
        if (remaining_size > 0) {
            // move residual data to the head
            std::vector<uint8_t> temp_buffer(audioResidualBuffer.begin() + processed_samples * channels * sample_size, audioResidualBuffer.end());
            audioResidualBuffer = std::move(temp_buffer);
        } else {
            audioResidualBuffer.clear();
        }
    }
    return ZET_OK;
}

INT32 handlehMultiThreadAacFrames(AVCodecContext* codec_ctx, AVFrame* frame, AVFormatContext* fmt_ctx, AVStream* stream, pthread_mutex_t hls_mux_mutex) {
  // pthread_mutex_lock(&audio_residual_mutex);
    int frame_size  = codec_ctx->frame_size;
    int channels    = codec_ctx->ch_layout.nb_channels;
    int sample_size = av_get_bytes_per_sample(codec_ctx->sample_fmt);

    if (audioChannels == 0) {
        audioChannels   = channels;
        audioSampleSize = sample_size;
        audioResidualBuffer.clear();
        audioResidualSamples = 0;
    }

    //add new data to ResidualBuffer
    if (frame && frame->nb_samples > 0) {
        INT32 current_size = audioResidualBuffer.size();
        INT32 add_size     = frame->nb_samples * channels * sample_size;

        if (frame->nb_samples <= 0 || add_size > SIZE_MAX - current_size) {
            LOG(LOG_ERROR, "Invalid frame samples: %d, add_size: %d", frame->nb_samples, add_size);
            return ZET_NOK;
        }
        audioResidualBuffer.resize(current_size + add_size);
        for (int ch = 0; ch < channels; ch++) {
            uint8_t* dst = audioResidualBuffer.data() + current_size + ch * frame->nb_samples * sample_size;
            memcpy(dst, frame->data[ch], frame->nb_samples * sample_size);
        }
        audioResidualSamples += frame->nb_samples;
    }

    int            processed_samples = 0;
    static INT64   last_audio_pts    = AV_NOPTS_VALUE;

    while (audioResidualSamples >= frame_size) {
        // create the frame meet aac demand
        AVFrame* aac_frame     = av_frame_alloc();
        aac_frame->format      = codec_ctx->sample_fmt;
        aac_frame->sample_rate = codec_ctx->sample_rate;
        aac_frame->nb_samples  = frame_size;
        av_channel_layout_copy(&aac_frame->ch_layout, &codec_ctx->ch_layout);

        if (av_frame_get_buffer(aac_frame, 0) < 0) {
            av_frame_free(&aac_frame);
            return ZET_NOK;
        }

        for (int ch = 0; ch < channels; ch++) {
            const UINT8* src = audioResidualBuffer.data() + (processed_samples * channels + ch) * sample_size;
            memcpy(aac_frame->data[ch], src, frame_size * sample_size);
        }

        // set the pts properly !!!
        if (frame && frame->pts != AV_NOPTS_VALUE) {
            // Calculate timestamp based on sample size
            INT64 sample_duration = av_rescale_q(1, av_make_q(1, codec_ctx->sample_rate), codec_ctx->time_base);
            aac_frame->pts = frame->pts + processed_samples * sample_duration;

            // Ensure that the timestamp monotonically increases
            if (last_audio_pts != AV_NOPTS_VALUE && aac_frame->pts <= last_audio_pts) {
                aac_frame->pts = last_audio_pts + sample_duration * frame_size;
                LOG(LOG_DEBUG, "Adjusted audio PTS to maintain monotonicity");
            }
            last_audio_pts = aac_frame->pts;
        } else {
            // create the rightly timestamp
            if (last_audio_pts == AV_NOPTS_VALUE) {
                aac_frame->pts = 0;
            } else {
                int64_t sample_duration = av_rescale_q(1, av_make_q(1, codec_ctx->sample_rate), codec_ctx->time_base);
                aac_frame->pts = last_audio_pts + sample_duration * frame_size;
            }
            last_audio_pts = aac_frame->pts;
        }

        if (avcodec_send_frame(codec_ctx, aac_frame) < 0) {
            LOG(LOG_ERROR, "Failed to send frame to AAC encoder");
            av_frame_free(&aac_frame);
            return ZET_NOK;
        } 

        AVPacket* out_pkt = av_packet_alloc();
        while (avcodec_receive_packet(codec_ctx, out_pkt) >= 0) {
            // Ensure that the timestamp monotonically increases
            if (out_pkt->pts == AV_NOPTS_VALUE) {
                out_pkt->pts = aac_frame->pts;
            }
            if (out_pkt->dts == AV_NOPTS_VALUE) {
                out_pkt->dts = out_pkt->pts;
            }

            out_pkt->duration     = av_rescale_q(frame_size, av_make_q(1, codec_ctx->sample_rate), stream->time_base);            
            av_packet_rescale_ts(out_pkt, codec_ctx->time_base, stream->time_base);
            out_pkt->stream_index = stream->index;

            int retry_count       = 0;
            // need to use try lock in case of deadlock while writing info file
            while (retry_count < 5) {
                if (pthread_mutex_trylock(&hls_mux_mutex) == 0) {       
                    if (av_interleaved_write_frame(fmt_ctx, out_pkt) < 0) {
                        LOG(LOG_ERROR, "Audio packet write failed - PTS: %ld, DTS: %ld", 
                        out_pkt->pts, out_pkt->dts);
                    }
                    pthread_mutex_unlock(&hls_mux_mutex); 
                    break;
                }
                usleep(1000);
                retry_count++;
            }
            av_packet_unref(out_pkt); 
        }
        av_packet_free(&out_pkt);
        av_frame_free(&aac_frame);
        processed_samples    += frame_size;
        audioResidualSamples -= frame_size;
        //pthread_mutex_unlock(&audio_residual_mutex);
    }

    // update ResidualBuffer
    if (processed_samples > 0) {
        size_t remaining_size = audioResidualSamples * channels * sample_size;
        if (remaining_size > 0) {
            // move residual data to the head
            std::vector<uint8_t> temp_buffer(audioResidualBuffer.begin() +
                                           processed_samples * channels * sample_size,
                                           audioResidualBuffer.end());
            audioResidualBuffer = std::move(temp_buffer);
        } else {
            audioResidualBuffer.clear();
        }
    }
    return ZET_OK;
}

double getMediaFileDuration(AVFormatContext* in_fmt_ctx, int video_stream_idx) {
    if (!in_fmt_ctx) {
        LOG(LOG_ERROR, "in_fmt_ctx is NULL, cannot calculate duration");
        return -1;
    }

    if (in_fmt_ctx->duration != AV_NOPTS_VALUE) {
        double duration_sec = in_fmt_ctx->duration / (double)AV_TIME_BASE;
        LOG(LOG_INFO, "Duration from fmt_ctx: %.3f sec", duration_sec);
        return duration_sec;
    }

    int64_t max_stream_duration = 0;
    for (int i = 0; i < in_fmt_ctx->nb_streams; i++) {
        AVStream* stream = in_fmt_ctx->streams[i];
        if (stream->duration != AV_NOPTS_VALUE) {
            int64_t duration_us = av_rescale_q(stream->duration, stream->time_base, AV_TIME_BASE_Q);
            if (duration_us > max_stream_duration) {
                max_stream_duration = duration_us;
            }
        }
    }

    if (max_stream_duration > 0) {
        double duration_sec = max_stream_duration / (double)AV_TIME_BASE;
        LOG(LOG_INFO, "Duration from max stream: %.3f sec", duration_sec);
        return duration_sec;
    }

    if (video_stream_idx >= 0 && video_stream_idx < in_fmt_ctx->nb_streams) {
        AVStream* video_stream = in_fmt_ctx->streams[video_stream_idx];
        if (video_stream->duration != AV_NOPTS_VALUE) {
            int64_t duration_us = av_rescale_q(video_stream->duration, video_stream->time_base, AV_TIME_BASE_Q);
            if (duration_us > 0) {
                double duration_sec = duration_us / (double)AV_TIME_BASE;
                LOG(LOG_INFO, "Duration from video stream: %.3f sec", duration_sec);
                return duration_sec;
            }
        }
    }

    double  last_pkt_duration = -1;
    INT64   current_pos       = avio_tell(in_fmt_ctx->pb);
    bool    support_seek      = (in_fmt_ctx->pb && (in_fmt_ctx->pb->seekable & AVIO_SEEKABLE_NORMAL));

    if (current_pos < 0) {
        LOG(LOG_WARNING, "Cannot get current file position, skip last packet check");
    } else {
        AVPacket*  pkt          = av_packet_alloc();
        int64_t    last_pts     = AV_NOPTS_VALUE;
        AVRational last_time_base;
        while (av_read_frame(in_fmt_ctx, pkt) == 0) {
            if (pkt->pts != AV_NOPTS_VALUE) {
                last_pts       = pkt->pts;
                last_time_base = in_fmt_ctx->streams[pkt->stream_index]->time_base;
            }
            av_packet_unref(pkt);
        }

        if (last_pts != AV_NOPTS_VALUE && last_time_base.den > 0) {
            int64_t duration_us = av_rescale_q(last_pts, last_time_base, AV_TIME_BASE_Q);
            last_pkt_duration   = duration_us / (double)AV_TIME_BASE;
        }
        av_packet_free(&pkt);

        //avio_seek(in_fmt_ctx->pb, current_pos, SEEK_SET);
        if (support_seek) {
            if (avformat_seek_file(in_fmt_ctx, -1, INT64_MIN, current_pos, INT64_MAX, AVSEEK_FLAG_BYTE) < 0) {
                LOG(LOG_WARNING, "Failed to seek back to original position after duration probe, byte pos: %lld", (long long)current_pos);
            }
        } else {
            LOG(LOG_ERROR, "Input source does not support seek, skip seek back normal for streams/pipes");
        }
    }

    if (last_pkt_duration > 0) {
        LOG(LOG_INFO, "using last pkt to get duraioin: %f, current_pos: %ld, support_seek: %d", last_pkt_duration, current_pos, support_seek);
        return last_pkt_duration; 
    }

    double bitrate_estimate = -1;
    {
        struct stat file_stat;
        if (stat(in_fmt_ctx->url, &file_stat) == 0 && file_stat.st_size > 0) {
            int64_t total_bitrate = 0;
            for (int i = 0; i < in_fmt_ctx->nb_streams; i++) {
                if (in_fmt_ctx->streams[i]->codecpar->bit_rate > 0) {
                    total_bitrate += in_fmt_ctx->streams[i]->codecpar->bit_rate;
                }
            }
            if (total_bitrate == 0 && in_fmt_ctx->bit_rate > 0) {
                total_bitrate = in_fmt_ctx->bit_rate;
            }

            if (total_bitrate > 0) {
                bitrate_estimate = (file_stat.st_size * 8.0) / total_bitrate;
                LOG(LOG_INFO, "Duration from file size+bitrate: %.3f sec", bitrate_estimate);
            }
        }
    }

    if (bitrate_estimate > 0) {
        LOG(LOG_INFO, "using bitrate estimate to get duration: %f", bitrate_estimate);
        return bitrate_estimate;
    }

    LOG(LOG_WARNING, "All methods failed to get duration");
    return -1;
}

enum AVPixelFormat getHardwarePixelFormat() {
    AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;

#if X86_64
    pix_fmt = AV_PIX_FMT_VAAPI;
#elif ARM
    pix_fmt = AV_PIX_FMT_DRM_PRIME;
#endif
    return pix_fmt;
}

enum AVPixelFormat get_hw_format(AVCodecContext* ctx, const enum AVPixelFormat* pix_fmts) {

    enum AVPixelFormat hw_pix_fmt = AV_PIX_FMT_NONE;
#if X86_64
    hw_pix_fmt = AV_PIX_FMT_VAAPI;
#elif ARM
    hw_pix_fmt = AV_PIX_FMT_DRM_PRIME;
#endif

    const enum AVPixelFormat* p;
    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == hw_pix_fmt) {
            LOG(LOG_DEBUG, "Selected HW pixel format: %s", av_get_pix_fmt_name(*p));
            return *p;
        }

        if (hw_pix_fmt == AV_PIX_FMT_CUDA && *p == AV_PIX_FMT_NV12) {
            return AV_PIX_FMT_NV12;
        } else if (hw_pix_fmt == AV_PIX_FMT_VAAPI && *p == AV_PIX_FMT_VAAPI) {
            return AV_PIX_FMT_VAAPI;
        } else if (hw_pix_fmt == AV_PIX_FMT_QSV && *p == AV_PIX_FMT_QSV) {
            return AV_PIX_FMT_QSV;
        }
    }
    LOG(LOG_ERROR, "Failed to get HW surface format, now use: %s", av_get_pix_fmt_name(*p));
    return AV_PIX_FMT_NONE;
}

enum AVPixelFormat get_hwAccelFrame_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VAAPI)
            return *p;
    }

    fprintf(stderr, "Unable to decode this file using VA-API.\n");
    return AV_PIX_FMT_NONE;
}

bool isHWCodecSupport(AVCodecID codecID, const AVCodec** video_ctx, bool enc) {
    bool ret = ZET_FALSE;
#if X86_64
    LOG(LOG_DEBUG, "NOW x86_86 check hw codec enter, codecID : %s, enc: %d", avcodec_get_name(codecID), enc);
  #if 0
    for (int i = 0; i < ZET_ARRAY_ELEMS(VAAPIMappingList); i++) {
        if (VAAPIMappingList[i].av_codec_id == codecID) {
            LOG(LOG_INFO, "@@@found target codec is: %s, vaapi ID: %d, input ID: %d", VAAPIMappingList[i].mine, VAAPIMappingList[i].av_codec_id, codecID);
            *video_ctx = enc ? avcodec_find_encoder_by_name(VAAPIMappingList[i].mine) : avcodec_find_decoder_by_name(VAAPIMappingList[i].mine);
            LOG(LOG_INFO, ".....NOW found target codec is: %s, video_ctx: %p", VAAPIMappingList[i].mine, *video_ctx);
            ret = *video_ctx ? ZET_TRUE: ZET_FALSE;
            return ret;
        }
    }
    return ZET_FALSE;
  #else
    AVHWDeviceType hw_type = AV_HWDEVICE_TYPE_VAAPI;
    *video_ctx             = hw_accel_find_codec_by_hw_type(hw_type, codecID, enc ? 1 : 0);
    ret                    = (*video_ctx != NULL) ? ZET_TRUE : ZET_FALSE;
  #endif

#elif ARM
    LOG(LOG_DEBUG, "NOW ARM check hw codec enter,codecID : %s, enc: %d", avcodec_get_name(codecID), enc);
    for (int i = 0; i < ZET_ARRAY_ELEMS(RKMPPMappingList); i++) {
        if (RKMPPMappingList[i].av_codec_id == codecID) {
            *video_ctx = enc ? avcodec_find_encoder_by_name(RKMPPMappingList[i].mine): avcodec_find_decoder_by_name(RKMPPMappingList[i].mine);
            ret = *video_ctx ? ZET_TRUE: ZET_FALSE;
            return ret;
        }
    }
    return ZET_FALSE;
#endif
    return ret;
}

const AVCodec* hw_accel_find_codec_by_hw_type(enum AVHWDeviceType device_type, enum AVCodecID codec_id, int is_encoder) {
    void *iter = NULL;
    const AVCodec *codec;

    const char *hw_suffix = NULL;
    switch (device_type) {
        case AV_HWDEVICE_TYPE_RKMPP: hw_suffix = "_rkmpp"; break;
        case AV_HWDEVICE_TYPE_VAAPI: hw_suffix = "_vaapi"; break;
        case AV_HWDEVICE_TYPE_CUDA: hw_suffix  = "_nvenc"; break;
        case AV_HWDEVICE_TYPE_QSV: hw_suffix   = "_qsv"; break;
        case AV_HWDEVICE_TYPE_VIDEOTOOLBOX: hw_suffix = "_videotoolbox"; break;
        default: return NULL;
    }

    LOG(LOG_VERBOSE, "hw_suffix : %s", hw_suffix);

    while ((codec = is_encoder ? av_codec_iterate(&iter) : av_codec_iterate(&iter))) {
        if (codec->id != codec_id) continue;
        if ((is_encoder && !av_codec_is_encoder(codec)) || (!is_encoder && !av_codec_is_decoder(codec))) continue;
        if (strstr(codec->name, hw_suffix)) {
            if (is_encoder) {
#if LIBAVCODEC_VERSION_INT >= AV_VERSION_INT(58, 0, 0)
                int config_idx = 0;
                const AVCodecHWConfig *config;
                while ((config = avcodec_get_hw_config(codec, config_idx++))) {
                        if (config->device_type == device_type) {
                            return codec;
                        }
                }
#else
                if (codec->pix_fmts) { 	LOG(LOG_ERROR, "enter 1, %s" hw_type);
                    for (int i = 0; codec->pix_fmts[i] != AV_PIX_FMT_NONE; i++) {
                        enum AVPixelFormat fmt = codec->pix_fmts[i];
                        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt);
                        if (desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) {
                            LOG(LOG_INFO, "hw codec found %s: %s support hw foramt\n",  is_encoder ? "encoder" : "decoder", codec->name);
                            return codec;
                        }
                    }
                }
#endif
            } else {
                LOG(LOG_INFO, "hw codec found %s: %s support hw foramt %s\n",  is_encoder ? "encoder" : "decoder", codec->name, av_hwdevice_get_type_name(device_type));
                return codec;
            }
        }
    }
    return NULL;
}

bool initHWAccel(AVCodecContext* ctx, AVHWDeviceType& hw_type) {
#if X86_64
    hw_type = AV_HWDEVICE_TYPE_VAAPI;
#elif ARM
    hw_type = AV_HWDEVICE_TYPE_RKMPP;
#endif
    AVBufferRef* hw_device_ctx = NULL;

#if X86_64
    hw_type = AV_HWDEVICE_TYPE_VAAPI;
    const char* vaapi_device = "/dev/dri/renderD128";
#elif ARM
    hw_type = AV_HWDEVICE_TYPE_RKMPP;
    const char* vaapi_device = NULL;
#endif

    if (av_hwdevice_ctx_create(&hw_device_ctx, hw_type, vaapi_device, NULL, 0) < 0) {
        LOG(LOG_ERROR, "Failed to create %d device", hw_type);
        return ZET_FALSE;
    }

    ctx->hw_device_ctx = av_buffer_ref(hw_device_ctx);

    if (!ctx->hw_device_ctx) {
        LOG(LOG_ERROR, "Failed to reference HW device context");
        return ZET_FALSE;
    }
    ctx->opaque     = NULL; // this must be use for callback!!!
    ctx->get_format = get_hw_format;
    return ZET_TRUE;
}

#ifdef ARM
INT32 initRKMPPContext(AVCodecContext* codec_ctx, HWAccelCtx &hwAccelCtx, bool enc) {

    //  get RKMPP device context
 /*   AVHWDeviceContext* device_ctx   = enc ? (AVHWDeviceContext*)hwAccelCtx.video_enc_ctx : (AVHWDeviceContext*)hwAccelCtx.video_dec_ctx;
    AVRKMPPDeviceContext* rkmpp_ctx = (AVRKMPPDeviceContext*)device_ctx->hwctx;
*/
    // set RKMPP parameters
    codec_ctx->pix_fmt    = AV_PIX_FMT_DRM_PRIME;
    codec_ctx->sw_pix_fmt = AV_PIX_FMT_NV12;

    // configure codec parameters
    av_opt_set(codec_ctx->priv_data, "preset", "ultrafast", 0);
    //av_opt_set(codec_ctx->priv_data, "tune", "zerolatency", 0);
    av_opt_set(codec_ctx->priv_data, "profile", "main", 0);
    av_opt_set(codec_ctx->priv_data, "b_strategy", "0", 0);
    LOG(LOG_VERBOSE, "RKMPP context initialized");
    return ZET_OK;
}

INT32 initRGAContext(HWAccelCtx &hwAccelCtx) {
 /*   int ret = c_RkRgaInit();
    if (ret != 0) {
        LOG(LOG_ERROR, "Failed to initialize RGA: %d", ret);
        return ZET_NOK;
    }
    
    hwAccelCtx.rga_handle = NULL;

    memset(&hwAccelCtx.rga_src_info, 0, sizeof(rga_info_t));
    memset(&hwAccelCtx.rga_dst_info, 0, sizeof(rga_info_t));

    hwAccelCtx.rga_src_info.fd = -1;
    hwAccelCtx.rga_dst_info.fd = -1;
*/
    LOG(LOG_VERBOSE, "RGA context initialized");
    return ZET_OK;
}

#elif X86_64
INT32 initVAAPIContext(AVCodecContext* codec_ctx, HWAccelCtx &hwAccelCtx, bool enc) {
    // get VAAPI device context
//    AVHWDeviceContext* device_ctx   = enc ? (AVHWDeviceContext*)hwAccelCtx.video_enc_ctx : (AVHWDeviceContext*)hwAccelCtx.video_dec_ctx;
//    AVVAAPIDeviceContext* vaapi_ctx = (AVVAAPIDeviceContext*)device_ctx->hwctx;

    // set VAAPI parameters
    codec_ctx->pix_fmt    = AV_PIX_FMT_VAAPI;
    codec_ctx->sw_pix_fmt = AV_PIX_FMT_NV12;

    // configure codec parameters
    av_opt_set(codec_ctx->priv_data, "qp", "23", 0);
    av_opt_set(codec_ctx->priv_data, "quality", "5", 0);

    LOG(LOG_DEBUG, "~VAAPI context initialized");
    return ZET_OK;
}
#endif

void freeResources(AVFormatContext* in_fmt_ctx, AVFormatContext* out_fmt_ctx, AVCodecContext* in_video_ctx, AVCodecContext* in_audio_ctx,
                         AVCodecContext* out_video_ctx, AVCodecContext* out_audio_ctx, SwsContext* sws_ctx,
                         SwrContext* swr_ctx, AVFrame* frame, AVPacket* pkt) {
    if (in_fmt_ctx) {
        avformat_close_input(&in_fmt_ctx);
        in_fmt_ctx = NULL;
    }

    if (out_fmt_ctx) {
        if (!(out_fmt_ctx->oformat && !(out_fmt_ctx->oformat->flags & AVFMT_NOFILE))) {
            avio_closep(&out_fmt_ctx->pb);
        }
        avformat_free_context(out_fmt_ctx);
        out_fmt_ctx = NULL;
    }

    if (in_video_ctx) avcodec_free_context(&in_video_ctx);
    if (in_audio_ctx) avcodec_free_context(&in_audio_ctx);
    if (out_video_ctx) avcodec_free_context(&out_video_ctx);
    if (out_audio_ctx) avcodec_free_context(&out_audio_ctx);
    if (sws_ctx) sws_freeContext(sws_ctx);
    if (swr_ctx) swr_free(&swr_ctx);
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt); 
    avformat_network_deinit();
}

    // for more robust way to release memory
void freeResources(AVFormatContext** in_fmt_ctx, AVFormatContext** out_fmt_ctx, AVCodecContext** in_video_ctx,
                         AVCodecContext** in_audio_ctx, AVCodecContext** out_video_ctx, AVCodecContext** out_audio_ctx,
                         SwsContext** sws_ctx, SwrContext** swr_ctx, AVFrame** frame, AVPacket** pkt) {
    if (pkt && *pkt) {
        av_packet_free(pkt);
    }

    if (frame && *frame) {
        av_frame_free(frame);
    }

    if (swr_ctx && *swr_ctx) {
        swr_free(swr_ctx);
    }

    if (sws_ctx && *sws_ctx) {
        sws_freeContext(*sws_ctx);
    }

    if (out_audio_ctx && *out_audio_ctx) {
        if ((*out_audio_ctx)->hw_device_ctx) {
            av_buffer_unref(&(*out_audio_ctx)->hw_device_ctx);
        }
        avcodec_free_context(out_audio_ctx);
    }

    if (out_video_ctx && *out_video_ctx) {
        if ((*out_video_ctx)->hw_device_ctx) {
            av_buffer_unref(&(*out_video_ctx)->hw_device_ctx);
        }
        avcodec_free_context(out_video_ctx);
    }

    if (in_audio_ctx && *in_audio_ctx) {
        avcodec_free_context(in_audio_ctx);
    }

    if (in_video_ctx && *in_video_ctx) {
        avcodec_free_context(in_video_ctx);
    }

    if (out_fmt_ctx && *out_fmt_ctx) {
        if ((*out_fmt_ctx)->pb && !((*out_fmt_ctx)->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&(*out_fmt_ctx)->pb);
        }
        avformat_free_context(*out_fmt_ctx);
        *out_fmt_ctx = NULL;
    }

    if (in_fmt_ctx && *in_fmt_ctx) {
        avformat_close_input(in_fmt_ctx); 
    }

    avformat_network_deinit();
}

INT32 cleanupHardwareContext(HWAccelCtx& hw_ctx_) {
    if (hw_ctx_.video_dec_ctx && hw_ctx_.video_dec_ctx->hw_device_ctx) {
        av_buffer_unref(&hw_ctx_.video_dec_ctx->hw_device_ctx);
    }

    if (hw_ctx_.video_enc_ctx && hw_ctx_.video_enc_ctx->hw_device_ctx) {
        av_buffer_unref(&hw_ctx_.video_enc_ctx->hw_device_ctx);
    }

    if (hw_ctx_.video_dec_ctx) {
        if (hw_ctx_.video_dec_ctx->hw_device_ctx) {
            av_buffer_unref(&hw_ctx_.video_dec_ctx->hw_device_ctx);
            hw_ctx_.video_dec_ctx->hw_device_ctx = NULL;
        }
        avcodec_free_context(&hw_ctx_.video_dec_ctx);
        hw_ctx_.video_dec_ctx = NULL;
    }
    
    if (hw_ctx_.video_enc_ctx) {
        if (hw_ctx_.video_enc_ctx->hw_device_ctx) {
            av_buffer_unref(&hw_ctx_.video_enc_ctx->hw_device_ctx);
            hw_ctx_.video_enc_ctx->hw_device_ctx = NULL;
        }
        avcodec_free_context(&hw_ctx_.video_enc_ctx);
        hw_ctx_.video_enc_ctx = NULL;
    }

    if (hw_ctx_.hw_enc_frames_ctx) {
        av_buffer_unref(&hw_ctx_.hw_enc_frames_ctx);
        hw_ctx_.hw_enc_frames_ctx = NULL;
    }
	
    if (hw_ctx_.hw_dec_frames_ctx) {
        av_buffer_unref(&hw_ctx_.hw_dec_frames_ctx);
        hw_ctx_.hw_dec_frames_ctx = NULL;
    }

    hw_ctx_.hwDecenabled = ZET_FALSE;
    hw_ctx_.hwEncenabled = ZET_FALSE;
    hw_ctx_.hwScaEnabled = ZET_FALSE;
    return ZET_OK;
}

/*
bool fixM3U8Msg(const std::string& m3u8_path, double expected_total_duration) {
    std::ifstream file(m3u8_path);
    if (!file.is_open()) {
        LOG(LOG_ERROR, "Failed to open m3u8 file: %s", m3u8_path.c_str());
        return ZET_FALSE;
    }

    std::vector<std::string> lines;
    std::string line;
    std::vector<int> extinf_lines;
    double total_duration_before_last = 0.0;
    int last_extinf_line              = -1;

    while (std::getline(file, line)) {
        lines.push_back(line);
    }
    file.close();

    if (lines.empty()) {
        LOG(LOG_ERROR, "M3U8 file is empty: %s", m3u8_path.c_str());
        return ZET_FALSE;
    }

    for (int line_num = 0; line_num < (int)lines.size(); line_num++) {
        if (lines[line_num].find("#EXTINF:") == 0) {
            extinf_lines.push_back(line_num);
        }
    }

    if (!extinf_lines.empty()) {
        last_extinf_line = extinf_lines.back();
        for (int i = 0; i < (int)extinf_lines.size() - 1; i++) {
            int         line_num     = extinf_lines[i];
            std::string line         = lines[line_num];
            size_t      start        = line.find(":") + 1;
            size_t      end          = line.find(",");

            if (end != std::string::npos) {
                std::string duration_str    = line.substr(start, end - start);
                double      duration        = std::stod(duration_str);
                total_duration_before_last += duration;
                LOG(LOG_VERBOSE, "line_num: %d, Segment duration: %.6fs, accumulated: %.6fs",
                line_num, duration, total_duration_before_last);
            }
        }
    }

    double last_segment_duration = expected_total_duration - total_duration_before_last;
    LOG(LOG_ERROR, "before calculate, last segment duration is : %.6f, expected_total_duration: %f, total_duration_before_last: %f", 
                                     last_segment_duration, expected_total_duration, total_duration_before_last);

    if (last_segment_duration < 0) {
        LOG(LOG_ERROR, "Calculated last segment duration is negative: %.6f", last_segment_duration);
        last_segment_duration = 0.1;
    } else if (last_segment_duration > 10.0) {
        LOG(LOG_ERROR, "Last segment duration seems too long: %.6f, capping to 10s", last_segment_duration);
        last_segment_duration = 2.0;
    }

    LOG(LOG_VERBOSE, "Correcting last segment: previous_total=%.6fs, expected_total=%.6fs, last_segment=%.6fs",
        total_duration_before_last, expected_total_duration, last_segment_duration);

    std::ostringstream new_extinf;
    new_extinf << "#EXTINF:" << std::fixed << std::setprecision(6) << last_segment_duration << ",";

    lines[last_extinf_line]     = new_extinf.str();
    double max_segment_duration = std::min(2.99, last_segment_duration);

    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].find("#EXT-X-TARGETDURATION:") == 0) {
            std::ostringstream new_target;
            new_target << "#EXT-X-TARGETDURATION:" << static_cast<int>(ceil(max_segment_duration));
            lines[i] = new_target.str();
            break;
        }
    }

    std::ofstream out_file(m3u8_path);
    if (!out_file.is_open()) {
        LOG(LOG_ERROR, "Failed to create corrected m3u8 file");
        return false;
    }

    for (const auto& corrected_line : lines) {
        out_file << corrected_line << std::endl;
    }
    out_file.close();

    LOG(LOG_VERBOSE, "Successfully corrected m3u8 file: %s", m3u8_path.c_str());
    return ZET_TRUE;
}
*/

bool fixM3U8Msg(const std::string& m3u8_path, double expected_total_duration, bool need_scale) {
    std::ifstream file(m3u8_path);
    if (!file.is_open()) {
        LOG(LOG_ERROR, "Failed to open m3u8 file: %s", m3u8_path.c_str());
        return ZET_FALSE;
    }

    std::vector<std::string> lines;
    std::string line;
    std::vector<int> extinf_lines;
    double total_duration_before_last = 0.0;
    int last_extinf_line              = -1;

    while (std::getline(file, line)) {
        lines.push_back(line);
    }
    file.close();

    if (lines.empty()) {
        LOG(LOG_ERROR, "M3U8 file is empty: %s", m3u8_path.c_str());
        return ZET_FALSE;
    }

    for (int line_num = 0; line_num < (int)lines.size(); line_num++) {
        if (lines[line_num].find("#EXTINF:") == 0) {
            extinf_lines.push_back(line_num);
        }
    }

    if (extinf_lines.empty()) {
        LOG(LOG_ERROR, "No EXTINF lines found in m3u8 file: %s", m3u8_path.c_str());
        return ZET_FALSE;
    }

    last_extinf_line = extinf_lines.back();
    if (last_extinf_line < 0 || last_extinf_line >= (int)lines.size()) {
        LOG(LOG_ERROR, "Invalid last EXTINF line index: %d, total lines: %zu", last_extinf_line, lines.size());
        return ZET_FALSE;
    }

    for (int i = 0; i < (int)extinf_lines.size() - 1; i++) {
        int line_num = extinf_lines[i];

        if (line_num < 0 || line_num >= (int)lines.size()) {
            LOG(LOG_ERROR, "Invalid EXTINF line index: %d at position %d", line_num, i);
            continue;
        }

        std::string line_content = lines[line_num];
        size_t      start        = line_content.find(":") + 1;
        size_t      end          = line_content.find(",");

        if (end != std::string::npos && start < line_content.length()) {
            try {
                std::string duration_str    = line_content.substr(start, end - start);
                double duration             = std::stod(duration_str);
                total_duration_before_last += duration;
                LOG(LOG_VERBOSE, "line_num: %d, Segment duration: %.6fs, accumulated: %.6fs",
                                  line_num, duration, total_duration_before_last);
            } catch (const std::exception& e) {
                LOG(LOG_ERROR, "Failed to parse duration at line %d: %s", line_num, e.what());
            }
        } else {
            LOG(LOG_ERROR, "Invalid EXTINF format at line %d: %s", line_num, line_content.c_str());
        }
    }

    double last_segment_duration = expected_total_duration - total_duration_before_last;

    LOG(LOG_VERBOSE, "Before calculation - last segment: %.6f, expected total: %.6f, accumulated: %.6f",
                      last_segment_duration, expected_total_duration, total_duration_before_last);

    if (last_segment_duration < 0) {
        LOG(LOG_WARNING, "Calculated last segment duration is negative: %.6f, setting to 0.1", last_segment_duration);
        last_segment_duration = 0.1;
    } else if (last_segment_duration > 10.0 && !need_scale) {
        LOG(LOG_WARNING, "Last segment duration too long: %.6f, capping to 2.0", last_segment_duration);
        last_segment_duration = 2.0;
    } else if (last_segment_duration < 0.001) {
        LOG(LOG_WARNING, "Last segment duration too short: %.6f, setting to 0.1", last_segment_duration);
        last_segment_duration = 0.1;
    }

    LOG(LOG_VERBOSE, "Correcting last segment: previous_total=%.6fs, expected_total=%.6fs, last_segment=%.6fs",
                      total_duration_before_last, expected_total_duration, last_segment_duration);

    if (last_extinf_line < 0 || last_extinf_line >= (int)lines.size()) {
        LOG(LOG_ERROR, "Invalid last EXTINF line index after calculation: %d", last_extinf_line);
        return ZET_FALSE;
    }

    std::ostringstream new_extinf;
    new_extinf << "#EXTINF:" << std::fixed << std::setprecision(6) << last_segment_duration << ",";

    try {
        lines[last_extinf_line] = new_extinf.str();
    } catch (const std::exception& e) {
        LOG(LOG_ERROR, "Failed to update line %d: %s", last_extinf_line, e.what());
        return ZET_FALSE;
    }

    double max_segment_duration = std::min(10.0, std::max(1.0, last_segment_duration));

    for (size_t i = 0; i < lines.size(); i++) {
        if (lines[i].find("#EXT-X-TARGETDURATION:") == 0) {
            int target_duration = static_cast<int>(ceil(max_segment_duration));
            if (target_duration < 1) target_duration = 1;
            if (target_duration > 10) target_duration = 10;

            std::ostringstream new_target;
            new_target << "#EXT-X-TARGETDURATION:" << target_duration;
            lines[i] = new_target.str();
            break;
        }
    }

    std::ofstream out_file(m3u8_path);
    if (!out_file.is_open()) {
        LOG(LOG_ERROR, "Failed to create corrected m3u8 file: %s", m3u8_path.c_str());
        return ZET_FALSE;
    }

    for (const auto& corrected_line : lines) {
        out_file << corrected_line << std::endl;
    }
    out_file.close();
    LOG(LOG_VERBOSE, "Successfully corrected m3u8 file: %s", m3u8_path.c_str());
    return ZET_TRUE;
}

