//  Agora RTC/MEDIA SDK
//
//  Created by Jay Zhang in 2020-04.
//  Copyright (c) 2020 Agora.io. All rights reserved.
//
//  This sample demonstrates how to receive audio and video streams from a remote user,
//  transcode them using FFmpeg, and push them to an RTMP server like YouTube.
//
//  The application is structured into three main threads:
//  1. Audio Thread: Receives PCM audio, encodes it into AAC.
//  2. Video Thread: Receives YUV video, encodes it into H.264.
//  3. Muxing Thread: Takes encoded AAC and H.264 packets and sends them to the RTMP server.
//

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
        if (audio_thread_.joinable()) {
            audio_thread_.join();
        }
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
    YuvFrameObserver() : encoder_ctx_(nullptr), frame_(nullptr), sws_ctx_(nullptr), pts_(0) {
        video_thread_ = std::thread(&YuvFrameObserver::video_processing_thread, this);
    }

    ~YuvFrameObserver() {
        if (video_thread_.joinable()) {
            video_thread_.join();
        }
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

private:
    void video_processing_thread();
    bool init_h264_encoder(int width, int height);
    void cleanup_h264_encoder();

    std::thread video_thread_;
    AVCodecContext* encoder_ctx_;
    AVFrame* frame_;
    SwsContext* sws_ctx_;
    int64_t pts_;
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
        return false;
    }

    swr_ctx_ = swr_alloc_set_opts(nullptr,
                                  aac_encoder_ctx_->channel_layout, aac_encoder_ctx_->sample_fmt, aac_encoder_ctx_->sample_rate,
                                  (numChannels_ == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO, AV_SAMPLE_FMT_S16, sampleRate_,
                                  0, nullptr);
    if (!swr_ctx_ || swr_init(swr_ctx_) < 0) {
        AG_LOG(ERROR, "Failed to initialize resampler");
        return false;
    }

    audio_frame_ = av_frame_alloc();
    audio_frame_->nb_samples = aac_encoder_ctx_->frame_size;
    audio_frame_->format = aac_encoder_ctx_->sample_fmt;
    audio_frame_->channel_layout = aac_encoder_ctx_->channel_layout;
    audio_frame_->sample_rate = aac_encoder_ctx_->sample_rate;
    if (av_frame_get_buffer(audio_frame_, 0) < 0) {
        AG_LOG(ERROR, "Failed to allocate audio frame buffer");
        return false;
    }

    audio_fifo_ = av_audio_fifo_alloc(aac_encoder_ctx_->sample_fmt, aac_encoder_ctx_->channels, 1);
    if (!audio_fifo_) {
        AG_LOG(ERROR, "Failed to allocate audio FIFO");
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
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!encoder) {
        AG_LOG(ERROR, "Cannot find H264 encoder");
        return false;
    }
    encoder_ctx_ = avcodec_alloc_context3(encoder);
    if (!encoder_ctx_) {
        AG_LOG(ERROR, "Failed to allocate encoder context");
        return false;
    }

    encoder_ctx_->bit_rate = 2000000;
    encoder_ctx_->width = width;
    encoder_ctx_->height = height;
    encoder_ctx_->time_base = {1, 20}; // Common timebase for video
    encoder_ctx_->framerate = {20, 1};
    encoder_ctx_->gop_size = 40;
    encoder_ctx_->max_b_frames = 0; // No B-frames for low latency
    encoder_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    av_opt_set(encoder_ctx_->priv_data, "preset", "veryfast", 0);
    av_opt_set(encoder_ctx_->priv_data, "tune", "zerolatency", 0);

    if (avcodec_open2(encoder_ctx_, encoder, nullptr) < 0) {
        AG_LOG(ERROR, "Failed to open H264 encoder");
        return false;
    }

    frame_ = av_frame_alloc();
    frame_->format = encoder_ctx_->pix_fmt;
    frame_->width = encoder_ctx_->width;
    frame_->height = encoder_ctx_->height;
    if (av_frame_get_buffer(frame_, 32) < 0) {
        AG_LOG(ERROR, "Failed to allocate frame buffer");
        return false;
    }
    return true;
}

void YuvFrameObserver::cleanup_h264_encoder() {
    if (encoder_ctx_) avcodec_free_context(&encoder_ctx_);
    if (frame_) av_frame_free(&frame_);
    if (sws_ctx_) sws_freeContext(sws_ctx_);
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
            // Create SwsContext
            sws_ctx_ = sws_getContext(videoFrame->width, videoFrame->height, AV_PIX_FMT_YUV420P, 
                                      encoder_ctx_->width, encoder_ctx_->height, encoder_ctx_->pix_fmt, 
                                      SWS_BILINEAR, NULL, NULL, NULL);
            if (!sws_ctx_) {
                AG_LOG(ERROR, "Failed to create SwsContext");
                break;
            }
            encoder_initialized = true;
        }

        if (av_frame_make_writable(frame_) < 0) continue;

        // Use sws_scale for robust conversion
        const uint8_t* const src_data[4] = { videoFrame->yBuffer, videoFrame->uBuffer, videoFrame->vBuffer, NULL };
        const int src_linesize[4] = { (int)videoFrame->yStride, (int)videoFrame->uStride, (int)videoFrame->vStride, 0 };

        sws_scale(sws_ctx_, src_data, src_linesize, 0, videoFrame->height, frame_->data, frame_->linesize);
        
        frame_->pts = pts_++;

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
        return -1;
    }

    std::signal(SIGQUIT, SignalHandler);
    std::signal(SIGABRT, SignalHandler);
    std::signal(SIGINT, SignalHandler);

    auto service = createAndInitAgoraService(false, true, true);
    if (!service) {
        AG_LOG(ERROR, "Failed to create Agora service!");
        return -1;
    }

    agora::rtc::RtcConnectionConfiguration ccfg;
    ccfg.clientRoleType = agora::rtc::CLIENT_ROLE_BROADCASTER;
    ccfg.autoSubscribeAudio = false;
    ccfg.autoSubscribeVideo = false;

    auto connection = service->createRtcConnection(ccfg);
    if (!connection) {
        AG_LOG(ERROR, "Failed to create Agora connection!");
        return -1;
    }

    auto connObserver = std::make_shared<SampleConnectionObserver>();
    connection->registerObserver(connObserver.get());
    
    auto localUserObserver = std::make_shared<SampleLocalUserObserver>(connection->getLocalUser());

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

    // Create observers
    auto pcmFrameObserver = std::make_shared<PcmFrameObserver>(options.audio.sampleRate, options.audio.numOfChannels);
    auto yuvFrameObserver = std::make_shared<YuvFrameObserver>();

    // Register observers
    if (connection->getLocalUser()->setPlaybackAudioFrameBeforeMixingParameters(options.audio.numOfChannels, options.audio.sampleRate)) {
        AG_LOG(ERROR, "Failed to set audio frame parameters!");
    }
    localUserObserver->setAudioFrameObserver(pcmFrameObserver.get());
    localUserObserver->setVideoFrameObserver(yuvFrameObserver.get());

    if (connection->connect(options.token.c_str(), options.channelId.c_str(), options.userId.c_str())) {
        AG_LOG(ERROR, "Failed to connect to Agora channel!");
        return -1;
    }

    AG_LOG(INFO, "Connected to Agora channel. Waiting for media streams...");

    // Wait for encoders to be initialized
    AVCodecContext* video_codec_ctx = nullptr;
    AVCodecContext* audio_codec_ctx = nullptr;
    while (!muxer_queue.is_exiting() && (!video_codec_ctx || !audio_codec_ctx)) {
        if (!video_codec_ctx) video_codec_ctx = yuvFrameObserver->get_codec_context();
        if (!audio_codec_ctx) audio_codec_ctx = pcmFrameObserver->get_codec_context();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::thread muxer_thread;
    if (!muxer_queue.is_exiting()) {
        muxer_thread = std::thread(muxing_thread_func, options.rtmpUrl.c_str(), video_codec_ctx, audio_codec_ctx);
    }

    // Main loop
    while (!muxer_queue.is_exiting()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    AG_LOG(INFO, "Exiting...");

    // Cleanup
    if (muxer_thread.joinable()) muxer_thread.join();
    
    connection->unregisterObserver(connObserver.get());
    localUserObserver->unsetAudioFrameObserver();
    localUserObserver->unsetVideoFrameObserver();

    if (connection->disconnect()) {
        AG_LOG(ERROR, "Failed to disconnect from Agora channel!");
    }

    service->release();
    return 0;
}