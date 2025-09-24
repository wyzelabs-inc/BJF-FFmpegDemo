/*
1.增加启动超时机制，30秒内音视频编码器未初始化成功，打印错误日志后退出
2.在视频处理线程 video_processing_thread 的末尾，增加了冲刷编码器的逻辑。它通过向编码器发送一个 nullptr 帧来告知数据流结束，然后循环接收编码器输出的所有剩余数据包。
3.增加了运行时状态监控日志，每秒打印各个处理队列的当前大小
4.改进资源清理逻辑，为PcmFrameObserver和YuvFrameObserver添加了独立的join(),在main函数中使用do-while(false)结构，将所有资源清理代码集中到循环体之后。无论程序是正常结束还是中途出错break，都能保证清理逻辑被完整执行，防止资源泄漏
*/

#include <csignal>
#include <cstring>
#include <sstream>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <atomic>
#include <chrono>

#include "AgoraRefCountedObject.h"
#include "IAgoraService.h"
#include "NGIAgoraRtcConnection.h"
#include "common/log.h"
#include "common/opt_parser.h"
#include "common/sample_common.h"
#include "common/sample_connection_observer.h"
#include "common/sample_local_user_observer.h"
#include "common/helper.h"

#include "NGIAgoraAudioTrack.h"
#include "NGIAgoraLocalUser.h"
#include "NGIAgoraMediaNodeFactory.h"
#include "NGIAgoraMediaNode.h"
#include "NGIAgoraVideoTrack.h"

extern "C" {
#include "libavcodec/avcodec.h"
#include "libavformat/avformat.h"
#include "libavutil/avutil.h"
#include "libavutil/opt.h"
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
#include "libavutil/time.h"
#include "libavutil/audio_fifo.h"
}

#define DEFAULT_SAMPLE_RATE 16000
#define DEFAULT_NUM_OF_CHANNELS 1
#define STREAM_TYPE_HIGH "high"
#define STREAM_TYPE_LOW "low"

// ================== Global Shared Data ==================

// Struct to hold encoded packets for the muxer
struct MediaPacket {
    AVPacket* pkt;
    enum AVMediaType type;
};

// Thread-safe queue for media packets
template<typename T>
class ThreadSafeQueue {
public:
    void push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(value));
        cond_.notify_one();
    }

    bool pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] { return !queue_.empty() || exit_flag_; });
        if (exit_flag_ && queue_.empty()) {
            return false;
        }
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }
    
    int size() {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    void notify_exit() {
        exit_flag_ = true;
        cond_.notify_all();
    }

    bool is_exiting() const {
        return exit_flag_.load();
    }

private:
    std::queue<T> queue_;
    std::mutex mutex_;
    std::condition_variable cond_;
    std::atomic<bool> exit_flag_{false};
};

// Global queues
ThreadSafeQueue<MediaPacket> muxer_queue;
ThreadSafeQueue<agora::media::base::VideoFrame*> video_queue;
ThreadSafeQueue<std::vector<int16_t>> audio_queue;

// ================== Command Line Options ==================

struct SampleOptions {
    std::string token;
    std::string channelId;
    std::string userId;
    std::string remoteUserId;
    std::string rtmpUrl;
    std::string streamType = STREAM_TYPE_HIGH;
    struct {
        int sampleRate = DEFAULT_SAMPLE_RATE;
        int numOfChannels = DEFAULT_NUM_OF_CHANNELS;
    } audio;
};

// ================== Muxer Thread ==================

void muxing_thread_func(const char* rtmp_url, AVCodecContext* video_codec_ctx, AVCodecContext* audio_codec_ctx) {
    AVFormatContext* out_ctx = nullptr;
    AVStream* video_stream = nullptr;
    AVStream* audio_stream = nullptr;
    int ret = 0;

    // Real-time pacing control
    int64_t start_wall_us = av_gettime_relative();
    int64_t v_base_pts = AV_NOPTS_VALUE;
    int64_t a_base_pts = AV_NOPTS_VALUE;

    AG_LOG(INFO, "Muxer thread started. Pushing to: %s", rtmp_url);

    // Allocate output format context
    avformat_alloc_output_context2(&out_ctx, nullptr, "flv", rtmp_url);
    if (!out_ctx) {
        AG_LOG(ERROR, "Failed to allocate output context");
        return;
    }

    // Add video stream
    video_stream = avformat_new_stream(out_ctx, video_codec_ctx->codec);
    if (!video_stream) {
        AG_LOG(ERROR, "Failed to create video stream");
        goto cleanup;
    }
    avcodec_parameters_from_context(video_stream->codecpar, video_codec_ctx);
    video_stream->time_base = video_codec_ctx->time_base;


    // Add audio stream
    audio_stream = avformat_new_stream(out_ctx, audio_codec_ctx->codec);
    if (!audio_stream) {
        AG_LOG(ERROR, "Failed to create audio stream");
        goto cleanup;
    }
    avcodec_parameters_from_context(audio_stream->codecpar, audio_codec_ctx);
    audio_stream->time_base = audio_codec_ctx->time_base;

    // Open output URL
    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&out_ctx->pb, rtmp_url, AVIO_FLAG_WRITE);
        if (ret < 0) {
            AG_LOG(ERROR, "Could not open output URL '%s'", rtmp_url);
            goto cleanup;
        }
    }

    // Write stream header
    ret = avformat_write_header(out_ctx, nullptr);
    if (ret < 0) {
        AG_LOG(ERROR, "Error occurred when opening output URL");
        goto cleanup;
    }

    AG_LOG(INFO, "RTMP header written successfully.");

    while (true) {
        MediaPacket packet;
        if (!muxer_queue.pop(packet)) {
            break; // Exit flag was set and queue is empty
        }

        AVStream* out_stream = (packet.type == AVMEDIA_TYPE_VIDEO) ? video_stream : audio_stream;
        AVPacket* pkt = packet.pkt;

        // Rescale timestamps
        av_packet_rescale_ts(pkt, (packet.type == AVMEDIA_TYPE_VIDEO) ? video_codec_ctx->time_base : audio_codec_ctx->time_base, out_stream->time_base);
        pkt->stream_index = out_stream->index;

        // Real-time pacing control - ensure we don't send faster than real-time
        if (packet.type == AVMEDIA_TYPE_VIDEO) {
            if (v_base_pts == AV_NOPTS_VALUE) {
                v_base_pts = pkt->pts;
            }
            int64_t pts_offset = pkt->pts - v_base_pts;
            int64_t target_wall_us = start_wall_us + av_rescale_q(pts_offset, out_stream->time_base, AV_TIME_BASE_Q);
            int64_t current_wall_us = av_gettime_relative();

            if (current_wall_us < target_wall_us) {
                int64_t sleep_us = target_wall_us - current_wall_us;
                if (sleep_us > 0 && sleep_us < 1000000) { // Max 1 second sleep
                    av_usleep(sleep_us);
                }
            }
        } else { // Audio
            if (a_base_pts == AV_NOPTS_VALUE) {
                a_base_pts = pkt->pts;
            }
            int64_t pts_offset = pkt->pts - a_base_pts;
            int64_t target_wall_us = start_wall_us + av_rescale_q(pts_offset, out_stream->time_base, AV_TIME_BASE_Q);
            int64_t current_wall_us = av_gettime_relative();

            if (current_wall_us < target_wall_us) {
                int64_t sleep_us = target_wall_us - current_wall_us;
                if (sleep_us > 0 && sleep_us < 1000000) { // Max 1 second sleep
                    av_usleep(sleep_us);
                }
            }
        }

        // Write the frame
        ret = av_interleaved_write_frame(out_ctx, pkt);
        if (ret < 0) {
            AG_LOG(ERROR, "Error muxing packet");
            av_packet_free(&pkt);
            break;
        }
        av_packet_free(&pkt);
    }

    AG_LOG(INFO, "Muxer thread finishing.");

    // Write trailer and clean up
    av_write_trailer(out_ctx);

cleanup:
    if (out_ctx && !(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&out_ctx->pb);
    }
    if (out_ctx) {
        avformat_free_context(out_ctx);
    }
}


// ================== Audio Processing Thread ==================

class PcmFrameObserver : public agora::media::IAudioFrameObserverBase {
public:
    PcmFrameObserver(int sampleRate, int numChannels)
        : sampleRate_(sampleRate),
          numChannels_(numChannels),
          aac_encoder_ctx_(nullptr),
          audio_frame_(nullptr),
          swr_ctx_(nullptr),
          audio_fifo_(nullptr),
          pts_ (0)
          {
        audio_thread_ = std::thread(&PcmFrameObserver::audio_processing_thread, this);
    }

    ~PcmFrameObserver() {
        // The thread must be joined via the public join() method before this destructor is called
        // to ensure a clean shutdown.
        cleanup_aac_encoder();
    }

    bool onPlaybackAudioFrame(const char* channelId, AudioFrame& audioFrame) override { return true; }
    bool onRecordAudioFrame(const char* channelId, AudioFrame& audioFrame) override { return true; }
    bool onMixedAudioFrame(const char* channelId, AudioFrame& audioFrame) override { return true; }
    bool onEarMonitoringAudioFrame(AudioFrame& audioFrame) override { return true; }
    AudioParams getEarMonitoringAudioParams() override { return AudioParams(); }
    int getObservedAudioFramePosition() override { return 0; }
    AudioParams getPlaybackAudioParams() override { return AudioParams(); }
    AudioParams getRecordAudioParams() override { return AudioParams(); }
    AudioParams getMixedAudioParams() override { return AudioParams(); }

    bool onPlaybackAudioFrameBeforeMixing(const char* channelId, agora::media::base::user_id_t userId, AudioFrame& audioFrame) override {
        if (audio_queue.is_exiting()) return true;
        const int16_t* pcm_buffer = reinterpret_cast<const int16_t*>(audioFrame.buffer);
        size_t num_samples = audioFrame.samplesPerChannel * audioFrame.channels;
        std::vector<int16_t> chunk(pcm_buffer, pcm_buffer + num_samples);
        audio_queue.push(std::move(chunk));
        return true;
    }

    AVCodecContext* get_codec_context() { return aac_encoder_ctx_; }

    void join() {
        if (audio_thread_.joinable()) {
            audio_thread_.join();
        }
    }

private:
    void audio_processing_thread();
    bool init_aac_encoder();
    void cleanup_aac_encoder();
    void encode_and_push_audio_frame(AVFrame* frame);

    int sampleRate_;
    int numChannels_;
    std::thread audio_thread_;
    
    AVCodecContext* aac_encoder_ctx_;
    AVFrame* audio_frame_;
    SwrContext* swr_ctx_;
    AVAudioFifo* audio_fifo_;
    int64_t pts_;
};

class YuvFrameObserver : public agora::rtc::IVideoFrameObserver2 {
public:
    YuvFrameObserver() : encoder_ctx_(nullptr), frame_(nullptr), pts_(0), start_time_(0) {
        video_thread_ = std::thread(&YuvFrameObserver::video_processing_thread, this);
    }

    ~YuvFrameObserver() {
        // The thread must be joined via the public join() method before this destructor is called
        // to ensure a clean shutdown.
        cleanup_h264_encoder();
    }

    void onFrame(const char* channelId, agora::user_id_t remoteUid, const agora::media::base::VideoFrame* frame) override {
        if (video_queue.is_exiting()) return;
        auto new_frame = new agora::media::base::VideoFrame(*frame);
        new_frame->yBuffer = new uint8_t[frame->yStride * frame->height];
        new_frame->uBuffer = new uint8_t[frame->uStride * frame->height / 2];
        new_frame->vBuffer = new uint8_t[frame->vStride * frame->height / 2];
        memcpy(new_frame->yBuffer, frame->yBuffer, frame->yStride * frame->height);
        memcpy(new_frame->uBuffer, frame->uBuffer, frame->uStride * frame->height / 2);
        memcpy(new_frame->vBuffer, frame->vBuffer, frame->vStride * frame->height / 2);
        video_queue.push(new_frame);
    }

    AVCodecContext* get_codec_context() { return encoder_ctx_; }

    void join() {
        if (video_thread_.joinable()) {
            video_thread_.join();
        }
    }


private:
    void video_processing_thread();
    bool init_h264_encoder(int width, int height);
    void cleanup_h264_encoder();

    std::thread video_thread_;
    AVCodecContext* encoder_ctx_;
    AVFrame* frame_;
    int64_t pts_;
    int64_t start_time_;
};

// ================== Observer Implementations ==================

bool PcmFrameObserver::init_aac_encoder() {
    const int TARGET_SAMPLE_RATE = 44100;
    const int TARGET_BIT_RATE = 128000;

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_AAC);
    if (!encoder) {
        AG_LOG(ERROR, "Cannot find AAC encoder");
        return false;
    }

    aac_encoder_ctx_ = avcodec_alloc_context3(encoder);
    if (!aac_encoder_ctx_) {
        AG_LOG(ERROR, "Failed to allocate AAC encoder context");
        cleanup_aac_encoder();
        return false;
    }

    aac_encoder_ctx_->codec_id = AV_CODEC_ID_AAC;
    aac_encoder_ctx_->codec_type = AVMEDIA_TYPE_AUDIO;
    aac_encoder_ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    aac_encoder_ctx_->sample_rate = 44100;
    aac_encoder_ctx_->channel_layout = (numChannels_ == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO;
    aac_encoder_ctx_->channels = numChannels_;
    aac_encoder_ctx_->bit_rate = 128000;
    aac_encoder_ctx_->time_base = {1, TARGET_SAMPLE_RATE};
    if (encoder->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)
        aac_encoder_ctx_->frame_size = 1024;

    if (avcodec_open2(aac_encoder_ctx_, encoder, nullptr) < 0) {
        AG_LOG(ERROR, "Failed to open AAC encoder");
        cleanup_aac_encoder();
        return false;
    }

    swr_ctx_ = swr_alloc_set_opts(nullptr,
                                  aac_encoder_ctx_->channel_layout, aac_encoder_ctx_->sample_fmt, aac_encoder_ctx_->sample_rate,
                                  (numChannels_ == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, sampleRate_,
                                  0, nullptr);
    if (!swr_ctx_ || swr_init(swr_ctx_) < 0) {
        AG_LOG(ERROR, "Failed to initialize resampler");
        cleanup_aac_encoder();
        return false;
    }

    audio_frame_ = av_frame_alloc();
    audio_frame_->nb_samples = aac_encoder_ctx_->frame_size;
    audio_frame_->format = aac_encoder_ctx_->sample_fmt;
    audio_frame_->channel_layout = aac_encoder_ctx_->channel_layout;
    audio_frame_->sample_rate = aac_encoder_ctx_->sample_rate;
    if (av_frame_get_buffer(audio_frame_, 0) < 0) {
        AG_LOG(ERROR, "Failed to allocate audio frame buffer");
        cleanup_aac_encoder();
        return false;
    }

    audio_fifo_ = av_audio_fifo_alloc(aac_encoder_ctx_->sample_fmt, aac_encoder_ctx_->channels, 1);
    if (!audio_fifo_) {
        AG_LOG(ERROR, "Failed to allocate audio FIFO");
        cleanup_aac_encoder();
        return false;
    }
    return true;
}

void PcmFrameObserver::cleanup_aac_encoder() {
    if (aac_encoder_ctx_) avcodec_free_context(&aac_encoder_ctx_);
    if (audio_frame_) av_frame_free(&audio_frame_);
    if (swr_ctx_) swr_free(&swr_ctx_);
    if (audio_fifo_) av_audio_fifo_free(audio_fifo_);
}

void PcmFrameObserver::encode_and_push_audio_frame(AVFrame* frame) {
    if (!frame) {
        AG_LOG(INFO, "Flushing audio encoder");
    }

    if (avcodec_send_frame(aac_encoder_ctx_, frame) < 0) {
        AG_LOG(ERROR, "Failed to send frame to AAC encoder");
        return;
    }

    while (true) {
        AVPacket* pkt = av_packet_alloc();
        int ret = avcodec_receive_packet(aac_encoder_ctx_, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            av_packet_free(&pkt);
            break;
        } else if (ret < 0) {
            AG_LOG(ERROR, "Failed to receive packet from AAC encoder");
            av_packet_free(&pkt);
            break;
        }
        muxer_queue.push({pkt, AVMEDIA_TYPE_AUDIO});
    }
}

void PcmFrameObserver::audio_processing_thread() {
    if (!init_aac_encoder()) {
        AG_LOG(ERROR, "Failed to initialize AAC encoder");
        return;
    }
    
    const int frame_size = aac_encoder_ctx_->frame_size;

    while (true) {
        std::vector<int16_t> chunk;
        if (!audio_queue.pop(chunk)) break;

        const uint8_t* in_data[] = { reinterpret_cast<const uint8_t*>(chunk.data()) };
        int in_samples = chunk.size() / numChannels_;

        int max_out_samples = av_rescale_rnd(in_samples, aac_encoder_ctx_->sample_rate, sampleRate_, AV_ROUND_UP);
        uint8_t** converted_input_samples = NULL;
        av_samples_alloc_array_and_samples(&converted_input_samples, NULL, aac_encoder_ctx_->channels, max_out_samples, aac_encoder_ctx_->sample_fmt, 0);

        int out_samples = swr_convert(swr_ctx_, converted_input_samples, max_out_samples, in_data, in_samples);
        if (out_samples < 0) {
            AG_LOG(ERROR, "Error during resampling");
            av_freep(&converted_input_samples[0]);
            continue;
        }

        av_audio_fifo_write(audio_fifo_, (void**)converted_input_samples, out_samples);
        av_freep(&converted_input_samples[0]);

        while (av_audio_fifo_size(audio_fifo_) >= frame_size) {
            if (av_frame_make_writable(audio_frame_) < 0) break;
            av_audio_fifo_read(audio_fifo_, (void**)audio_frame_->data, frame_size);
            
            audio_frame_->pts = pts_;
            pts_ += frame_size;

            encode_and_push_audio_frame(audio_frame_);
        }
    }

    // Flush any remaining samples from the FIFO buffer
    int remaining_samples = av_audio_fifo_size(audio_fifo_);
    if (remaining_samples > 0) {
        AG_LOG(INFO, "Flushing %d remaining audio samples from FIFO", remaining_samples);
        if (av_frame_make_writable(audio_frame_) >= 0) {
            av_audio_fifo_read(audio_fifo_, (void**)audio_frame_->data, remaining_samples);
            // Pad the rest of the frame with silence
            av_samples_set_silence(&audio_frame_->data[0], remaining_samples, frame_size - remaining_samples, aac_encoder_ctx_->channels, aac_encoder_ctx_->sample_fmt);
            audio_frame_->nb_samples = frame_size;

            audio_frame_->pts = pts_;
            pts_ += audio_frame_->nb_samples;
            encode_and_push_audio_frame(audio_frame_);
        }
    }

    // Flush the encoder
    encode_and_push_audio_frame(nullptr);
}


bool YuvFrameObserver::init_h264_encoder(int width, int height) {
    const AVCodec* encoder = avcodec_find_encoder_by_name("libx264");
    if (!encoder) {
        AG_LOG(ERROR, "Cannot find libx264 encoder");
        return false;
    }
    encoder_ctx_ = avcodec_alloc_context3(encoder);
    if (!encoder_ctx_) {
        AG_LOG(ERROR, "Failed to allocate encoder context");
        cleanup_h264_encoder();
        return false;
    }

    // 720p@30fps, ~1.5 Mbps, GOP=2s
    encoder_ctx_->bit_rate = 1200000; // 1.2 Mbps for more stable streaming
    encoder_ctx_->width = width;
    encoder_ctx_->height = height;
    encoder_ctx_->time_base = (AVRational){1, 15};
    encoder_ctx_->framerate = (AVRational){15, 1};
    encoder_ctx_->gop_size = 30;       // 2s at 15fps
    encoder_ctx_->max_b_frames = 0; // No B-frames for low latency
    encoder_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    av_opt_set(encoder_ctx_->priv_data, "preset", "ultrafast", 0);
    av_opt_set(encoder_ctx_->priv_data, "tune", "zerolatency", 0);

    if (avcodec_open2(encoder_ctx_, encoder, nullptr) < 0) {
        AG_LOG(ERROR, "Failed to open H264 encoder");
        cleanup_h264_encoder();
        return false;
    }

    frame_ = av_frame_alloc();
    frame_->format = encoder_ctx_->pix_fmt;
    frame_->width = encoder_ctx_->width;
    frame_->height = encoder_ctx_->height;
    if (av_frame_get_buffer(frame_, 32) < 0) {
        AG_LOG(ERROR, "Failed to allocate frame buffer");
        cleanup_h264_encoder();
        return false;
    }
    return true;
}

void YuvFrameObserver::cleanup_h264_encoder() {
    if (encoder_ctx_) avcodec_free_context(&encoder_ctx_);
    if (frame_) av_frame_free(&frame_);
}

void YuvFrameObserver::video_processing_thread() {
    bool encoder_initialized = false;

    while (true) {
        agora::media::base::VideoFrame* videoFrame;
        if (!video_queue.pop(videoFrame)) break;

        if (!encoder_initialized || encoder_ctx_->width != videoFrame->width || encoder_ctx_->height != videoFrame->height) {
            if (encoder_ctx_) cleanup_h264_encoder();
            if (!init_h264_encoder(videoFrame->width, videoFrame->height)) {
                AG_LOG(ERROR, "Failed to initialize H264 encoder");
                delete videoFrame->yBuffer;
                delete videoFrame->uBuffer;
                delete videoFrame->vBuffer;
                delete videoFrame;
                continue;
            }
            encoder_initialized = true;
        }

        if (av_frame_make_writable(frame_) < 0) continue;

        // Since Agora provides YUV420P frames and our encoder uses the same format without resizing,
        // we can copy the planes directly, handling potential stride differences.
        // This avoids an unnecessary sws_scale operation.
        // Y plane
        for (int i = 0; i < frame_->height; i++) {
            memcpy(frame_->data[0] + i * frame_->linesize[0], videoFrame->yBuffer + i * videoFrame->yStride, frame_->width);
        }
        // U plane
        for (int i = 0; i < frame_->height / 2; i++) {
            memcpy(frame_->data[1] + i * frame_->linesize[1], videoFrame->uBuffer + i * videoFrame->uStride, frame_->width / 2);
        }
        // V plane
        for (int i = 0; i < frame_->height / 2; i++) {
            memcpy(frame_->data[2] + i * frame_->linesize[2], videoFrame->vBuffer + i * videoFrame->vStride, frame_->width / 2);
        }

        // Calculate PTS based on actual time to ensure correct playback speed
        // Use the renderTimeMs from Agora if available, otherwise use system time
        int64_t current_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();

        if (start_time_ == 0) {
            start_time_ = current_time;
            pts_ = 0;
        } else {
            // Convert milliseconds to time_base units (1/15 second)
            // time_base = {1, 15}, so 1 second = 15 time_base units
            // 1 ms = 15/1000 = 0.015 time_base units
            int64_t elapsed_ms = current_time - start_time_;
            pts_ = (elapsed_ms * 15) / 1000;  // Convert to time_base units
        }

        frame_->pts = pts_;

        if (avcodec_send_frame(encoder_ctx_, frame_) < 0) continue;

        while (true) {
            AVPacket* pkt = av_packet_alloc();
            int ret = avcodec_receive_packet(encoder_ctx_, pkt);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                av_packet_free(&pkt);
                break;
            } else if (ret < 0) {
                av_packet_free(&pkt);
                break;
            }
            muxer_queue.push({pkt, AVMEDIA_TYPE_VIDEO});
        }
        
        delete videoFrame->yBuffer;
        delete videoFrame->uBuffer;
        delete videoFrame->vBuffer;
        delete videoFrame;
    }

    // Flush the encoder to get any remaining packets
    if (encoder_initialized) {
        AG_LOG(INFO, "Flushing video encoder...");
        // Send a null frame to the encoder to signal end of stream
        if (avcodec_send_frame(encoder_ctx_, nullptr) >= 0) {
            while (true) {
                AVPacket* pkt = av_packet_alloc();
                int ret = avcodec_receive_packet(encoder_ctx_, pkt);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    av_packet_free(&pkt);
                    break;
                } else if (ret < 0) {
                    av_packet_free(&pkt);
                    break;
                }
                muxer_queue.push({pkt, AVMEDIA_TYPE_VIDEO});
            }
        }
    }
}

// ================== Main Function ==================

static void SignalHandler(int sigNo) { 
    AG_LOG(INFO, "Signal %d received, preparing to exit.", sigNo);
    muxer_queue.notify_exit();
    video_queue.notify_exit();
    audio_queue.notify_exit();
}

int main(int argc, char* argv[]) {
    SampleOptions options;
    int ret = 0;

    // --- Resource Declarations ---
    agora::base::IAgoraService* service = nullptr;
    agora::agora_refptr<agora::rtc::IRtcConnection> connection = nullptr;
    std::shared_ptr<SampleConnectionObserver> connObserver;
    std::shared_ptr<SampleLocalUserObserver> localUserObserver;
    std::shared_ptr<PcmFrameObserver> pcmFrameObserver;
    std::shared_ptr<YuvFrameObserver> yuvFrameObserver;
    std::thread muxer_thread;
    opt_parser optParser;

    optParser.add_long_opt("token", &options.token, "Token for authentication");
    optParser.add_long_opt("channelId", &options.channelId, "Channel ID");
    optParser.add_long_opt("userId", &options.userId, "User ID");
    optParser.add_long_opt("remoteUserId", &options.remoteUserId, "The remote user to subscribe to");
    optParser.add_long_opt("rtmpUrl", &options.rtmpUrl, "RTMP URL for streaming");
    optParser.add_long_opt("sampleRate", &options.audio.sampleRate, "Sample rate for received audio");
    optParser.add_long_opt("numOfChannels", &options.audio.numOfChannels, "Number of channels for received audio");
    optParser.add_long_opt("streamType", &options.streamType, "Stream type (high/low)");

    if ((argc <= 1) || !optParser.parse_opts(argc, argv)) {
        std::ostringstream strStream;
        optParser.print_usage(argv[0], strStream);
        std::cout << strStream.str() << std::endl;
        return -1;
    }

    if (options.token.empty() || options.channelId.empty() || options.rtmpUrl.empty()) {
        AG_LOG(ERROR, "Must provide token, channelId, and rtmpUrl!");
        ret = -1;
    }

    std::signal(SIGQUIT, SignalHandler);
    std::signal(SIGABRT, SignalHandler);
    std::signal(SIGINT, SignalHandler);

    // Use a do-while(false) loop for centralized resource cleanup
    do {
        if (ret != 0) break;

        service = createAndInitAgoraService(false, true, true);
        if (!service) {
            AG_LOG(ERROR, "Failed to create Agora service!");
            ret = -1; break;
        }

        agora::rtc::RtcConnectionConfiguration ccfg;
        ccfg.clientRoleType = agora::rtc::CLIENT_ROLE_BROADCASTER;
        ccfg.autoSubscribeAudio = false;
        ccfg.autoSubscribeVideo = false;

        connection = service->createRtcConnection(ccfg);
        if (!connection) {
            AG_LOG(ERROR, "Failed to create Agora connection!");
            ret = -1; break;
        }

        connObserver = std::make_shared<SampleConnectionObserver>();
        connection->registerObserver(connObserver.get());
        
        localUserObserver = std::make_shared<SampleLocalUserObserver>(connection->getLocalUser());

        // Subscribe to remote user
        agora::rtc::VideoSubscriptionOptions subscriptionOptions;
        subscriptionOptions.type = (options.streamType == STREAM_TYPE_HIGH) ? agora::rtc::VIDEO_STREAM_HIGH : agora::rtc::VIDEO_STREAM_LOW;
        if (options.remoteUserId.empty()) {
            connection->getLocalUser()->subscribeAllAudio();
            connection->getLocalUser()->subscribeAllVideo(subscriptionOptions);
        } else {
            connection->getLocalUser()->subscribeAudio(options.remoteUserId.c_str());
            connection->getLocalUser()->subscribeVideo(options.remoteUserId.c_str(), subscriptionOptions);
        }

        pcmFrameObserver = std::make_shared<PcmFrameObserver>(options.audio.sampleRate, options.audio.numOfChannels);
        yuvFrameObserver = std::make_shared<YuvFrameObserver>();

        if (connection->getLocalUser()->setPlaybackAudioFrameBeforeMixingParameters(options.audio.numOfChannels, options.audio.sampleRate)) {
            AG_LOG(ERROR, "Failed to set audio frame parameters!");
            ret = -1; break;
        }
        localUserObserver->setAudioFrameObserver(pcmFrameObserver.get());
        localUserObserver->setVideoFrameObserver(yuvFrameObserver.get());

        if (connection->connect(options.token.c_str(), options.channelId.c_str(), options.userId.c_str())) {
            AG_LOG(ERROR, "Failed to connect to Agora channel!");
            ret = -1; break;
        }

        AG_LOG(INFO, "Connected to Agora channel. Waiting for media streams...");

        AVCodecContext* video_codec_ctx = nullptr;
        AVCodecContext* audio_codec_ctx = nullptr;
        const auto timeout_duration = std::chrono::seconds(30);
        auto start_time = std::chrono::steady_clock::now();
        bool timed_out = false;

        while (!muxer_queue.is_exiting() && (!video_codec_ctx || !audio_codec_ctx)) {
            if (!video_codec_ctx) video_codec_ctx = yuvFrameObserver->get_codec_context();
            if (!audio_codec_ctx) audio_codec_ctx = pcmFrameObserver->get_codec_context();

            if (std::chrono::steady_clock::now() - start_time > timeout_duration) {
                AG_LOG(ERROR, "Timeout waiting for media encoders to initialize.");
                if (!video_codec_ctx) AG_LOG(ERROR, "Video encoder failed to initialize.");
                if (!audio_codec_ctx) AG_LOG(ERROR, "Audio encoder failed to initialize.");
                timed_out = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!muxer_queue.is_exiting() && !timed_out) {
            muxer_thread = std::thread(muxing_thread_func, options.rtmpUrl.c_str(), video_codec_ctx, audio_codec_ctx);
        } else {
            SignalHandler(SIGTERM); // Trigger graceful shutdown
            if (timed_out) ret = -1;
        }

        auto last_log_time = std::chrono::steady_clock::now();
        while (!muxer_queue.is_exiting()) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 1) {
                AG_LOG(INFO, "Queue sizes -> Video: %d, Audio: %d, Muxer: %d", 
                       video_queue.size(), audio_queue.size(), muxer_queue.size());
                last_log_time = now;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

    } while (false);

    AG_LOG(INFO, "Exiting...");

    // --- Cleanup ---
    // Explicitly join all threads before destroying observers.
    // This prevents hangs if a thread is stuck, and avoids relying on destructor order.
    if (muxer_thread.joinable()) {
        muxer_thread.join();
    }
    if (pcmFrameObserver) {
        pcmFrameObserver->join();
    }
    if (yuvFrameObserver) {
        yuvFrameObserver->join();
    }

    pcmFrameObserver.reset();
    yuvFrameObserver.reset();

    if (connection) {
        if (connObserver) {
            connection->unregisterObserver(connObserver.get());
        }
        if (localUserObserver) {
            localUserObserver->unsetAudioFrameObserver();
            localUserObserver->unsetVideoFrameObserver();
        }
        if (connection->disconnect()) {
            AG_LOG(ERROR, "Failed to disconnect from Agora channel!");
            if (ret == 0) ret = -1;
        }
    }

    localUserObserver.reset();
    connObserver.reset();
    connection = nullptr;

    if (service) {
        service->release();
    }

    return ret;
}