//  Agora RTC/MEDIA SDK
//
//  Created by Jay Zhang in 2020-04.
//  Copyright (c) 2020 Agora.io. All rights reserved.
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
#include <thread>

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
#include "libswscale/swscale.h"
#include "libswresample/swresample.h"
}


#define DEFAULT_SAMPLE_RATE 16000
#define DEFAULT_NUM_OF_CHANNELS 1
#define DEFAULT_AUDIO_FILE "received_audio.pcm"
#define DEFAULT_VIDEO_FILE "received_video.h264"
#define DEFAULT_FILE_LIMIT (100 * 1024 * 1024)
#define STREAM_TYPE_HIGH "high"
#define STREAM_TYPE_LOW "low"

struct SampleOptions {
  std::string appId;
  std::string channelId;
  std::string userId;
  std::string remoteUserId;
  std::string streamType = STREAM_TYPE_HIGH;
  std::string audioFile = DEFAULT_AUDIO_FILE;
  std::string videoFile = DEFAULT_VIDEO_FILE;

  struct {
    int sampleRate = DEFAULT_SAMPLE_RATE;
    int numOfChannels = DEFAULT_NUM_OF_CHANNELS;
  } audio;
};

struct PcmChunk {
    std::vector<int16_t> data;
    // No timestamp needed as we generate it locally
};

class PcmFrameObserver : public agora::media::IAudioFrameObserverBase {
 public:
  PcmFrameObserver(const std::string& outputFilePath, int sampleRate, int numChannels,agora::agora_refptr<agora::rtc::IAudioEncodedFrameSender> audioSender)
      : outputFilePath_(outputFilePath),
        audioSender_(audioSender),
        sampleRate_(sampleRate),
        numChannels_(numChannels),
        swr_ctx_(nullptr) {
          sendAudioThread_ = std::thread(&PcmFrameObserver::sendAudioTask, this);
        }

  ~PcmFrameObserver() {
    exitFlag_ = true;
    queueCond_.notify_one();
    if (sendAudioThread_.joinable()) {
      sendAudioThread_.join();
    }
  }

  bool onPlaybackAudioFrame(const char* channelId,AudioFrame& audioFrame) override { return true; };

  bool onRecordAudioFrame(const char* channelId,AudioFrame& audioFrame) override { return true; };

  bool onMixedAudioFrame(const char* channelId,AudioFrame& audioFrame) override { return true; };

  bool onPlaybackAudioFrameBeforeMixing(const char* channelId, agora::media::base::user_id_t userId, AudioFrame& audioFrame) override {
    PcmChunk chunk;
    const int16_t* pcm_buffer = reinterpret_cast<const int16_t*>(audioFrame.buffer);
    size_t num_samples = audioFrame.samplesPerChannel * audioFrame.channels;
    chunk.data.assign(pcm_buffer, pcm_buffer + num_samples);
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        pcmQueue_.push(std::move(chunk));
    }
    queueCond_.notify_one();
    return true;
  }

  bool onEarMonitoringAudioFrame(AudioFrame& audioFrame) override {return true;};

  AudioParams getEarMonitoringAudioParams()override {return  AudioParams();};

  int getObservedAudioFramePosition() override {return 0;};

  AudioParams getPlaybackAudioParams() override {return  AudioParams();};

  AudioParams getRecordAudioParams()  override {return  AudioParams();};

  AudioParams getMixedAudioParams() override {return  AudioParams();};


 private:
  std::string outputFilePath_;
  int sampleRate_;
  int numChannels_;
  agora::agora_refptr<agora::rtc::IAudioEncodedFrameSender> audioSender_;

  //resample members
  SwrContext* swr_ctx_;

  // For dedicated sending thread
  std::thread sendAudioThread_;
  std::atomic<bool> exitFlag_{false};
  std::queue<PcmChunk> pcmQueue_;
  std::mutex queueMutex_;
  std::condition_variable queueCond_;

  void sendAudioTask();
  bool initResampler();
  void cleanupResampler();
  static uint8_t linear2ulaw(int16_t pcm_val);
};

class YuvFrameObserver : public agora::rtc::IVideoFrameObserver2 {
 public:
  YuvFrameObserver(const std::string& outputFilePath,agora::agora_refptr<agora::rtc::IVideoEncodedImageSender> videoSender)
      : outputFilePath_(outputFilePath),
        videoSender_(videoSender),
        h264File_(nullptr),
        fileCount(0),
        fileSize_(0) {
    // 初始化FFmpeg编码器
    // 注意：在第一次接收到视频帧时才会真正初始化编码器，因为那时才知道视频尺寸
  }

  void onFrame(const char* channelId, agora::user_id_t remoteUid, const agora::media::base::VideoFrame* frame) override;
  void writeH264Frame(AVPacket* packet, FILE* f);
  
  ~YuvFrameObserver() {
    // 清理FFmpeg资源
    cleanupFFmpegEncoder();
    
    // 关闭文件
    if (h264File_) {
      fclose(h264File_);
      h264File_ = nullptr;
    }
  }

 private:
  // FFmpeg相关成员变量
  AVCodecContext* encoder_ctx_ = nullptr; // 初始化为nullptr
  AVFrame* frame_ = nullptr;
  AVPacket* packet_ = nullptr;
  
  std::string outputFilePath_;
  agora::agora_refptr<agora::rtc::IVideoEncodedImageSender> videoSender_;
  FILE* h264File_;
  int fileCount;
  int fileSize_;
  
  // 初始化FFmpeg编码器
  bool initFFmpegEncoder(int width, int height) {
    // 查找H264编码器
    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!encoder) {
      AG_LOG(ERROR, "Cannot find H264 encoder");
      return false;
    }
  
    // 分配编码器上下文
    encoder_ctx_ = avcodec_alloc_context3(encoder);
    if (!encoder_ctx_) {
      AG_LOG(ERROR, "Failed to allocate encoder context");
      return false;
    }
  
    // 设置编码器参数
    encoder_ctx_->bit_rate = 2000000; // 调整码率为 2Mbps，对于实时流更合理
    encoder_ctx_->width = width;
    encoder_ctx_->height = height;
    encoder_ctx_->time_base = (AVRational){1, 15};
    encoder_ctx_->framerate = (AVRational){15, 1};
    encoder_ctx_->gop_size = 20;
    encoder_ctx_->pix_fmt = AV_PIX_FMT_YUV420P;
    // 对于实时流，禁用B帧以降低延迟
    encoder_ctx_->max_b_frames = 0;

    
  
    // 打开编码器
    if (avcodec_open2(encoder_ctx_, encoder, nullptr) < 0) {
      AG_LOG(ERROR, "Failed to open encoder");
      return false;
    }
  
    // 分配帧和包
    frame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!frame_ || !packet_) {
      AG_LOG(ERROR, "Failed to allocate frame or packet");
      return false;
    }
  
    // 设置帧参数
    frame_->format = encoder_ctx_->pix_fmt;
    frame_->width = encoder_ctx_->width;
    frame_->height = encoder_ctx_->height;
    
    // 分配帧缓冲区
    if (av_frame_get_buffer(frame_, 32) < 0) {
      AG_LOG(ERROR, "Failed to allocate frame buffer");
      return false;
    }
  
    return true;
  }


  // 清理FFmpeg资源
  void cleanupFFmpegEncoder() {
    if (frame_) {
      av_frame_free(&frame_);
      frame_ = nullptr;
    }
    
    if (packet_) {
      av_packet_free(&packet_);
      packet_ = nullptr;
    }
    
    if (encoder_ctx_) {
      avcodec_free_context(&encoder_ctx_);
      encoder_ctx_ = nullptr;
    }
  }

  // 编码YUV数据并保存为H264
  bool encodeAndSendH264(const agora::media::base::VideoFrame* videoFrame) {
    // 如果编码器未初始化或尺寸不匹配，则重新初始化
    if (!encoder_ctx_ || encoder_ctx_->width != videoFrame->width || encoder_ctx_->height != videoFrame->height) {
      if (encoder_ctx_) {
        cleanupFFmpegEncoder();
      }
      if (!initFFmpegEncoder(videoFrame->width, videoFrame->height)) {
        AG_LOG(ERROR, "Failed to initialize encoder");
        return false;
      }
    }
    
    // 设置帧的PTS
    frame_->pts = encoder_ctx_->frame_number;
    
    // 将视频帧数据复制到AVFrame
    // Y平面
    for (int y = 0; y < videoFrame->height; y++) {
      memcpy(frame_->data[0] + y * frame_->linesize[0], 
             videoFrame->yBuffer + y * videoFrame->yStride, 
             videoFrame->width);
    }
    // U平面
    for (int y = 0; y < videoFrame->height / 2; y++) {
      memcpy(frame_->data[1] + y * frame_->linesize[1], 
             videoFrame->uBuffer + y * videoFrame->uStride, 
             videoFrame->width / 2);
    }
    // V平面
    for (int y = 0; y < videoFrame->height / 2; y++) {
      memcpy(frame_->data[2] + y * frame_->linesize[2], 
             videoFrame->vBuffer + y * videoFrame->vStride, 
             videoFrame->width / 2);
    }
    
    // 发送帧到编码器
    if (avcodec_send_frame(encoder_ctx_, frame_) < 0) {
      AG_LOG(ERROR, "Failed to send frame to encoder");
      return false;
    }
    
    // 接收编码后的包
    while (avcodec_receive_packet(encoder_ctx_, packet_) == 0) {
      // writeH264Frame(packet_, h264File_);
      //发送h264数据
      agora::rtc::EncodedVideoFrameInfo videoEncodedFrameInfo;
      videoEncodedFrameInfo.rotation = agora::rtc::VIDEO_ORIENTATION_0;
      videoEncodedFrameInfo.codecType = agora::rtc::VIDEO_CODEC_H264;
      videoEncodedFrameInfo.framesPerSecond = 20; // 根据实际情况设置帧率
      videoEncodedFrameInfo.frameType =
          (packet_->flags & AV_PKT_FLAG_KEY) ? agora::rtc::VIDEO_FRAME_TYPE::VIDEO_FRAME_TYPE_KEY_FRAME
                                   : agora::rtc::VIDEO_FRAME_TYPE::VIDEO_FRAME_TYPE_DELTA_FRAME;
      
      videoSender_->sendEncodedVideoImage(packet_->data, packet_->size, videoEncodedFrameInfo);
      
      // 同时保存到文件
      // if (h264File_) {
      //   if (fwrite(packet_->data, 1, packet_->size, h264File_) != packet_->size) {
      //     AG_LOG(ERROR, "Error writing H264 data: %s", std::strerror(errno));
      //   }
      //   fileSize_ += packet_->size;
      // }
      
      av_packet_unref(packet_);
    }
    
    return true;
  }



};

uint8_t PcmFrameObserver::linear2ulaw(int16_t pcm_val) {
    static const int cBias = 0x84;
    static const int cClip = 32635;
    int sign = (pcm_val >> 8) & 0x80;
    if (pcm_val < 0) pcm_val = -pcm_val;
    if (pcm_val > cClip) pcm_val = cClip;
    pcm_val = pcm_val + cBias;
    int exponent = 7;
    for (int expMask = 0x4000; (pcm_val & expMask) == 0 && exponent > 0; exponent--, expMask >>= 1) {}
    int mantissa = (pcm_val >> ((exponent == 0) ? 4 : (exponent + 3))) & 0x0F;
    uint8_t ulaw = ~(sign | (exponent << 4) | mantissa);
    return ulaw;
}

bool PcmFrameObserver::initResampler() {
  //初始化重采样器
  swr_ctx_ = swr_alloc_set_opts(
      nullptr,
      AV_CH_LAYOUT_MONO,    // 目标声道布局
      AV_SAMPLE_FMT_S16,    // 目标采样格式
      8000,                 // 目标采样率 (8000Hz)
      (numChannels_ == 1) ? AV_CH_LAYOUT_MONO : AV_CH_LAYOUT_STEREO, // 输入声道布局
      AV_SAMPLE_FMT_S16,                   // 输入格式
      sampleRate_,                         // 输入采样率 (16000Hz)
      0, nullptr);

  if (!swr_ctx_ || swr_init(swr_ctx_) < 0) {
    AG_LOG(ERROR, "Failed to init resampler");
    return false;
  }

  return true;
}

void PcmFrameObserver::cleanupResampler() {
  if (swr_ctx_) {
    swr_free(&swr_ctx_);
    swr_ctx_ = nullptr;
  }
}

void PcmFrameObserver::sendAudioTask() {
    if (!initResampler()) {
        AG_LOG(ERROR, "Failed to initialize resampler in worker thread");
        return;
    }

    std::vector<int16_t> pcm8k_fifo;
    const int PCMU_FRAME_DURATION_MS = 10;
    const int PCMU_SAMPLES_PER_FRAME = 8000 * PCMU_FRAME_DURATION_MS / 1000; // Now 80

    // Buffer for resampled data, sized for one 10ms chunk
    std::vector<int16_t> resampled_buffer(PCMU_SAMPLES_PER_FRAME); // Now sized to 80

    int64_t nextCaptureMs = 0;

    while (!exitFlag_) {
        PcmChunk chunk;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCond_.wait(lock, [this] { return !pcmQueue_.empty() || exitFlag_; });

            if (exitFlag_) break;

            chunk = std::move(pcmQueue_.front());
            pcmQueue_.pop();
        }

        // Resample 16k -> 8k
        const uint8_t* inData[1] = { reinterpret_cast<const uint8_t*>(chunk.data.data()) };
        int in_samples = chunk.data.size() / numChannels_;

        uint8_t* outData[1] = { reinterpret_cast<uint8_t*>(resampled_buffer.data()) };
        int out_samples = swr_convert(swr_ctx_, outData, resampled_buffer.size(), inData, in_samples);

        if (out_samples > 0) {
            pcm8k_fifo.insert(pcm8k_fifo.end(), resampled_buffer.begin(), resampled_buffer.begin() + out_samples);
        }

        // Process FIFO if it has enough data for a 20ms frame
        while (pcm8k_fifo.size() >= PCMU_SAMPLES_PER_FRAME) {
            if (nextCaptureMs == 0) {
                nextCaptureMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
            } else {
                auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                auto wait_ms = nextCaptureMs - now_ms;
                if (wait_ms > 0) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
                }
            }

            std::vector<uint8_t> pcmu_packet(PCMU_SAMPLES_PER_FRAME);
            for (int i = 0; i < PCMU_SAMPLES_PER_FRAME; ++i) {
                pcmu_packet[i] = linear2ulaw(pcm8k_fifo[i]);
            }

            agora::rtc::EncodedAudioFrameInfo audioFrameInfo;
            audioFrameInfo.codec = agora::rtc::AUDIO_CODEC_PCMU;
            audioFrameInfo.sampleRateHz = 8000;
            audioFrameInfo.numberOfChannels = 1;
            audioFrameInfo.samplesPerChannel = PCMU_SAMPLES_PER_FRAME;
            audioFrameInfo.captureTimeMs = nextCaptureMs;

            if (audioSender_) {
                audioSender_->sendEncodedAudioFrame(pcmu_packet.data(), pcmu_packet.size(), audioFrameInfo);
            }

            nextCaptureMs += PCMU_FRAME_DURATION_MS;
            pcm8k_fifo.erase(pcm8k_fifo.begin(), pcm8k_fifo.begin() + PCMU_SAMPLES_PER_FRAME);

            // Reset pacer if we are falling too far behind to prevent bursting
            auto current_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (current_time_ms > nextCaptureMs + 100) { // 100ms threshold
                nextCaptureMs = current_time_ms;
            }
        }
    }
    cleanupResampler();
}

void YuvFrameObserver::writeH264Frame(AVPacket* packet, FILE* f) {
  if (fwrite(packet->data, 1, packet->size, f) != packet->size) {
    AG_LOG(ERROR, "Error writing H264 data: %s", std::strerror(errno));
  }
  fileSize_ += packet->size;
}

void YuvFrameObserver::onFrame(const char* channelId, agora::user_id_t remoteUid, const agora::media::base::VideoFrame* frame) {
  // 如果是第一次接收数据，或者文件大小超过10MB，则创建新文件
  // if (h264File_ == nullptr || fileSize_ > 100 * 1024 * 1024) {
  //   if (h264File_ != nullptr) {
  //     fclose(h264File_);
  //     h264File_ = nullptr;
  //   }
  //   
  //   // 构造文件名
  //   std::string filename = outputFilePath_ + "_" + std::string(remoteUid) + "_" + std::to_string(fileCount++) + ".h264";
  //   
  //   // 打开文件
  //   h264File_ = fopen(filename.c_str(), "wb");
  //   if (h264File_ == nullptr) {
  //     AG_LOG(ERROR, "Failed to open file %s: %s", filename.c_str(), strerror(errno));
  //     return;
  //   }
  //   fileSize_ = 0;
  // }
  
  // 编码并保存H264数据
  if (!encodeAndSendH264(frame)) {
    AG_LOG(ERROR, "Failed to encode and send H264 data");
    return;
  }

  // Close the file if size limit is reached
  // if (fileSize_ >= DEFAULT_FILE_LIMIT) {
  //   fclose(h264File_);
  //   h264File_ = nullptr;
  //   fileSize_ = 0;
  // }
  return ;
}



  

static bool exitFlag = false;
static void SignalHandler(int sigNo) { exitFlag = true; }

int main(int argc, char* argv[]) {
  SampleOptions options;
  opt_parser optParser;

  optParser.add_long_opt("token", &options.appId,
                         "The token for authentication");
  optParser.add_long_opt("channelId", &options.channelId, "Channel Id");
  optParser.add_long_opt("userId", &options.userId, "User Id / default is 0");
  optParser.add_long_opt("remoteUserId", &options.remoteUserId,
                         "The remote user to receive stream from");
  optParser.add_long_opt("audioFile", &options.audioFile, "Output audio file");
  optParser.add_long_opt("videoFile", &options.videoFile, "Output video file");
  optParser.add_long_opt("sampleRate", &options.audio.sampleRate,
                         "Sample rate for received audio");
  optParser.add_long_opt("numOfChannels", &options.audio.numOfChannels,
                         "Number of channels for received audio");
  optParser.add_long_opt("streamtype", &options.streamType, "the stream type");

  if ((argc <= 1) || !optParser.parse_opts(argc, argv)) {
    std::ostringstream strStream;
    optParser.print_usage(argv[0], strStream);
    std::cout << strStream.str() << std::endl;
    return -1;
  }

  if (options.appId.empty()) {
    AG_LOG(ERROR, "Must provide appId!");
    return -1;
  }

  if (options.channelId.empty()) {
    AG_LOG(ERROR, "Must provide channelId!");
    return -1;
  }

  std::signal(SIGQUIT, SignalHandler);
  std::signal(SIGABRT, SignalHandler);
  std::signal(SIGINT, SignalHandler);

  // Create Agora service
  auto service = createAndInitAgoraService(false, true, true);
  if (!service) {
    AG_LOG(ERROR, "Failed to creating Agora service!");
  }

  // Create Agora connection
  agora::rtc::RtcConnectionConfiguration ccfg;
  ccfg.clientRoleType = agora::rtc::CLIENT_ROLE_BROADCASTER;
  ccfg.autoSubscribeAudio = false;
  ccfg.autoSubscribeVideo = false;
  ccfg.enableAudioRecordingOrPlayout =
      false;  // Subscribe audio but without playback

  agora::agora_refptr<agora::rtc::IRtcConnection> connection =
      service->createRtcConnection(ccfg);
  if (!connection) {
    AG_LOG(ERROR, "Failed to creating Agora connection!");
    return -1;
  }

  // Subcribe streams from all remote users or specific remote user
  agora::rtc::VideoSubscriptionOptions subscriptionOptions;
  if (options.streamType == STREAM_TYPE_HIGH) {
    subscriptionOptions.type = agora::rtc::VIDEO_STREAM_HIGH;
  } else if(options.streamType==STREAM_TYPE_LOW){
    subscriptionOptions.type = agora::rtc::VIDEO_STREAM_LOW;
  } else{
    AG_LOG(ERROR, "It is a error stream type");
    return -1;
  }
  if (options.remoteUserId.empty()) {
    AG_LOG(INFO, "Subscribe streams from all remote users");
    connection->getLocalUser()->subscribeAllAudio();
    connection->getLocalUser()->subscribeAllVideo(subscriptionOptions);

  } else {
    connection->getLocalUser()->subscribeAudio(options.remoteUserId.c_str());
    connection->getLocalUser()->subscribeVideo(options.remoteUserId.c_str(),
                                               subscriptionOptions);
  }
  // Register connection observer to monitor connection event
  auto connObserver = std::make_shared<SampleConnectionObserver>();
  connection->registerObserver(connObserver.get());

  // Create local user observer
  auto localUserObserver =
      std::make_shared<SampleLocalUserObserver>(connection->getLocalUser());

  // Create media node factory
  agora::agora_refptr<agora::rtc::IMediaNodeFactory> factory = service->createMediaNodeFactory();
  if (!factory) {
    AG_LOG(ERROR, "Failed to create media node factory!");
  }

  // Create audio frame sender
  agora::agora_refptr<agora::rtc::IAudioEncodedFrameSender> audioFrameSender =
      factory->createAudioEncodedFrameSender();
  if (!audioFrameSender) {
    AG_LOG(ERROR, "Failed to create audio frame sender!");
    return -1;
  }

  // Create video frame sender
  agora::agora_refptr<agora::rtc::IVideoEncodedImageSender> videoFrameSender =
      factory->createVideoEncodedImageSender();
  if (!videoFrameSender) {
    AG_LOG(ERROR, "Failed to create video frame sender!");
    return -1;
  }

  // Register audio frame observer to receive audio stream
  auto pcmFrameObserver = std::make_shared<PcmFrameObserver>(options.audioFile, options.audio.sampleRate, options.audio.numOfChannels, audioFrameSender);  // G.711u固定使用8000Hz采样率
  if (connection->getLocalUser()->setPlaybackAudioFrameBeforeMixingParameters(
          options.audio.numOfChannels, options.audio.sampleRate)) {
    AG_LOG(ERROR, "Failed to set audio frame parameters!");
    return -1;
  }
  localUserObserver->setAudioFrameObserver(pcmFrameObserver.get());

  // Connect to Agora channel
  if (connection->connect(options.appId.c_str(), options.channelId.c_str(),
                          options.userId.c_str())) {
    AG_LOG(ERROR, "Failed to connect to Agora channel!");
    return -1;
  }

  // Register video frame observer to receive video stream and transcode
  std::string yuvFileName = options.videoFile.substr(0, options.videoFile.find_last_of('.')) + ".h264";
  std::shared_ptr<YuvFrameObserver> yuvFrameObserver =
      std::make_shared<YuvFrameObserver>(yuvFileName, videoFrameSender);
  localUserObserver->setVideoFrameObserver(yuvFrameObserver.get());


  agora::rtc::SenderOptions option;
  option.ccMode = agora::rtc::TCcMode::CC_ENABLED;
  // Create video track
  agora::agora_refptr<agora::rtc::ILocalVideoTrack> customVideoTrack =
      service->createCustomVideoTrack(videoFrameSender, option);
  // Create audio track
  agora::agora_refptr<agora::rtc::ILocalAudioTrack> customAudioTrack =
      service->createCustomAudioTrack(audioFrameSender, agora::base::MIX_ENABLED);


// Publish video track
connection->getLocalUser()->publishVideo(customVideoTrack);
// Publish audio track
connection->getLocalUser()->publishAudio(customAudioTrack);


  // Start receiving incoming media data
  AG_LOG(INFO, "Start receiving audio & video data ...");

  // Periodically check exit flag
  while (!exitFlag) {
    usleep(10000);
  }

  // Unpublish audio & video track
  connection->getLocalUser()->unpublishAudio(customAudioTrack);
  connection->getLocalUser()->unpublishVideo(customVideoTrack);

  // Unregister audio & video frame observers
  localUserObserver->unsetAudioFrameObserver();
  localUserObserver->unsetVideoFrameObserver();

 // Unregister connection observer
  connection->unregisterObserver(connObserver.get());

  // Disconnect from Agora channel
  if (connection->disconnect()) {
    AG_LOG(ERROR, "Failed to disconnect from Agora channel!");
    return -1;
  }
  AG_LOG(INFO, "Disconnected from Agora channel successfully");

  // Destroy Agora connection and related resources
  localUserObserver.reset();
  pcmFrameObserver.reset();
  yuvFrameObserver.reset();
  audioFrameSender = nullptr;
  videoFrameSender = nullptr;
  customAudioTrack = nullptr;
  customVideoTrack = nullptr;
  connection = nullptr;

  // Destroy Agora Service
  service->release();
  service = nullptr;

  return 0;
}